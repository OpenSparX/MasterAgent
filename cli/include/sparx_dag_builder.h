#pragma once
/**
 * @file sparx_dag_builder.h
 * @brief Fluent builder for small IntentDAGs, plus JSON/Mermaid/text export.
 *
 * The kernel's IntentDAG / DAGNode / DAGEdge are powerful and precise, but
 * constructing one by hand for simple cases (call A then B, or A+B in
 * parallel → C) takes ~25 lines of boilerplate, mostly duplicating
 * priority/deadline into every node and remembering to list actions in the
 * AdmissionContext. Forgetting any single field causes validateDAG to reject
 * the plan without explaining what was missing — a terrible first experience.
 *
 * This builder is the answer to "how do I use the orchestrator for a simple
 * 2-step task?". It constructs a valid IntentDAG and a matching
 * AdmissionContext, ready for validateDAG() or export.
 *
 * Usage:
 *
 *   auto [dag, admission] = DagBuilder("turn-off-ac")
 *       .node("read_temp", "vehicle.climate.getTemperature")
 *       .node("set_ac", "vehicle.climate.setPower", {{"power", "off"}})
 *           .after("read_temp")
 *       .build();
 *
 *   // dag is a valid IntentDAG, admission is a matching AdmissionContext.
 *   auto result = orchestrator.validateDAG(dag, admission, call);
 *   assert(result.valid);
 *
 * Node count is bounded by AdmissionContext::max_nodes (32 by default), not by
 * this builder.
 *
 * Two things are deliberately NOT defaulted, because a default would amount to
 * asserting a safety property on the caller's behalf:
 *
 *   retries()          requires an explicit idempotency policy and a non-empty
 *                      retryable-error set.
 *   p0Authorization()  is required in addition to priority(P0). Without it a P0
 *                      plan is correctly rejected with
 *                      ORCHESTRATOR_P0_AUTHORIZATION_REQUIRED.
 */

#include "master_agent/orchestrator/orchestrator.h"
#include "master_agent/common/types.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <initializer_list>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace sparx {

using master_agent::TaskPriority;
using master_agent::stableDigest;
using master_agent::secureDigest;
using master_agent::orchestrator::AdmissionContext;
using master_agent::orchestrator::DAGEdge;
using master_agent::orchestrator::DAGNode;
using master_agent::orchestrator::IntentDAG;

/// What build() returns. The AdmissionContext is produced alongside the DAG
/// because the two must agree: every node action has to appear in
/// allowed_capabilities or validateDAG rejects the plan. Returning them as a
/// pair removes the most common cause of a mystifying rejection.
struct BuiltPlan {
    IntentDAG dag;
    AdmissionContext admission;
};

class DagBuilder {
public:
    /// @param plan_id  Used for dag_id, request_id, and the idempotency key.
    ///                 One logical plan, one id — the orchestrator dedupes on it.
    explicit DagBuilder(std::string plan_id)
        : plan_id_(std::move(plan_id)) {}

    /// Sets the deadline for the plan and every node in it.
    ///
    /// Nodes inherit the plan deadline rather than taking their own by default:
    /// a node deadline later than the plan's is unreachable, and hand-setting
    /// both is where the two drift apart.
    DagBuilder& deadline(std::chrono::milliseconds from_now) {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        deadline_mono_ns_ =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() +
            std::chrono::duration_cast<std::chrono::nanoseconds>(from_now).count();
        return *this;
    }

    /// Sets the plan priority, inherited by nodes added afterwards.
    DagBuilder& priority(TaskPriority p) {
        priority_ = p;
        return *this;
    }

    /// Adds a node. `action` is the capability string the executor dispatches on
    /// and is automatically added to the AdmissionContext.
    DagBuilder& node(std::string node_id, std::string action,
                     nlohmann::json params = nlohmann::json::object()) {
        DAGNode n;
        n.node_id = std::move(node_id);
        n.action = std::move(action);
        n.executor = default_executor_;
        n.params = std::move(params);
        n.base_priority = priority_;
        nodes_.push_back(std::move(n));
        return *this;
    }

    /// Declares that the most recently added node depends on `node_ids`.
    ///
    /// Listing several ids fans in: the node waits for all of them, which is the
    /// join half of a parallel step. Nodes with no `after` have no incoming edge
    /// and start together, which is the fan-out half — parallelism is the
    /// default and sequencing is what you opt into.
    DagBuilder& after(const std::vector<std::string>& node_ids) {
        if (nodes_.empty()) return *this;
        auto& current = nodes_.back();
        for (const auto& dep : node_ids) {
            current.dependencies.push_back(dep);
            edges_.push_back(DAGEdge{
                "edge-" + dep + "-" + current.node_id,
                dep, current.node_id, "SUCCESS", true});
        }
        return *this;
    }

    DagBuilder& after(const std::string& node_id) {
        return after(std::vector<std::string>{node_id});
    }

    /// Braced-list overload. Without it, `.after({"a", "b"})` is ambiguous:
    /// a two-element braced list of `const char*` matches both the vector
    /// overload and std::string's (InputIt first, InputIt last) constructor.
    /// An initializer_list parameter is preferred over both, so this is what
    /// makes the documented call form compile.
    DagBuilder& after(std::initializer_list<std::string> node_ids) {
        return after(std::vector<std::string>(node_ids));
    }

    /// Overrides the executor for the most recently added node.
    /// "atomic_service" for MCP tools, "agent_dispatch" for sub-agents.
    DagBuilder& executor(std::string executor_name) {
        if (!nodes_.empty()) nodes_.back().executor = std::move(executor_name);
        return *this;
    }

    /// Sets max_attempts on the most recently added node, and registers the
    /// retry policy the validator demands alongside it.
    ///
    /// `idempotency_policy` must be one of READ_ONLY, TARGET_STATE, or
    /// TRANSACTION_KEY. There is no default: the orchestrator will not retry a
    /// node whose side effects it cannot prove are safe to repeat, and picking a
    /// value on the caller's behalf would be asserting a safety property the
    /// builder cannot know. `retryable_errors` likewise must be non-empty —
    /// "retry on anything" is how a failed write gets duplicated.
    ///
    /// A node with retries and executor "agent_dispatch" is rejected by the
    /// validator regardless of what is passed here, because agent manifests
    /// expose no signed retry contract.
    DagBuilder& retries(std::uint32_t max_attempts,
                        const std::string& idempotency_policy,
                        const std::set<std::string>& retryable_errors,
                        std::chrono::nanoseconds base_backoff =
                            std::chrono::milliseconds(100),
                        std::chrono::nanoseconds max_backoff =
                            std::chrono::seconds(5)) {
        if (nodes_.empty()) return *this;
        nodes_.back().max_attempts = max_attempts;
        if (max_attempts > 1) {
            master_agent::orchestrator::CapabilityRetryPolicy policy;
            policy.idempotency_policy = idempotency_policy;
            policy.retryable_errors = retryable_errors;
            policy.base_backoff_ns = base_backoff.count();
            policy.max_backoff_ns = max_backoff.count();
            retry_policies_[nodes_.back().action] = std::move(policy);
        }
        return *this;
    }

    /// Grants P0 (safety-critical) priority to the listed capabilities.
    ///
    /// Separate from priority() and required in addition to it, because P0 is a
    /// trust grant, not a scheduling hint. The validator requires
    /// authorization_ref to start with "trusted-safety:", so the reference has
    /// to come from whatever actually vouches for the caller. The builder
    /// deliberately does not synthesise one: a builder that could mint its own
    /// safety authorization would make the validator's P0 check unenforceable
    /// for every local caller.
    DagBuilder& p0Authorization(std::string trusted_safety_ref,
                                std::set<std::string> capabilities) {
        p0_authorization_ref_ = std::move(trusted_safety_ref);
        p0_capabilities_ = std::move(capabilities);
        return *this;
    }

    /// Sets the executor used by nodes added after this call.
    DagBuilder& defaultExecutor(std::string executor_name) {
        default_executor_ = std::move(executor_name);
        return *this;
    }

    /// Identity the plan is admitted under. Defaults are fine for local runs;
    /// a real deployment passes the principal from its auth layer.
    DagBuilder& principal(std::string principal_id_hash) {
        principal_ = std::move(principal_id_hash);
        return *this;
    }

    BuiltPlan build() const {
        // A deadline is mandatory for validation, so supply a usable default
        // rather than emitting a plan that is rejected for a field the caller
        // never knew about.
        std::int64_t deadline_ns = deadline_mono_ns_;
        if (deadline_ns == 0) {
            const auto now = std::chrono::steady_clock::now().time_since_epoch();
            deadline_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() +
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    kDefaultDeadline).count();
        }

        IntentDAG dag;
        dag.dag_id = plan_id_;
        dag.request_id = plan_id_ + "-request";
        dag.priority = priority_;
        dag.deadline_mono_ns = deadline_ns;
        dag.schema_version = 2;
        dag.idempotency_key = plan_id_ + "-" + stableDigest(plan_id_);
        dag.nodes = nodes_;
        dag.edges = edges_;
        for (auto& n : dag.nodes) {
            n.deadline_mono_ns = deadline_ns;
        }

        AdmissionContext admission;
        admission.principal_id_hash = principal_;
        admission.granted_priority = priority_;
        admission.policy_snapshot_id = "sparx-local-policy-v1";
        admission.policy_digest = secureDigest(admission.policy_snapshot_id);
        admission.deadline_mono_ns = deadline_ns;
        for (const auto& n : nodes_) {
            admission.allowed_capabilities.insert(n.action);
        }

        // P0 nodes are rejected unless the grant is present and the
        // authorization_ref is the trusted-safety one. When no grant was given,
        // the ordinary synthetic ref is used and a P0 plan fails validation —
        // which is the correct outcome, not an oversight.
        if (!p0_authorization_ref_.empty()) {
            admission.p0_authorization = true;
            admission.authorization_ref = p0_authorization_ref_;
            admission.p0_allowed_capabilities = p0_capabilities_;
        } else {
            admission.authorization_ref = plan_id_ + "-authorization";
        }

        // The digest is over the policies themselves, so it can be computed
        // here; it authenticates nothing that the caller did not already state.
        admission.retry_policies = retry_policies_;
        if (!admission.retry_policies.empty()) {
            admission.retry_policy_digest =
                master_agent::orchestrator::retryPoliciesDigest(
                    admission.retry_policies);
        }

        return BuiltPlan{std::move(dag), std::move(admission)};
    }

private:
    static constexpr std::chrono::seconds kDefaultDeadline{30};

    std::string plan_id_;
    std::string principal_ = "sparx-local-principal";
    std::string default_executor_ = "atomic_service";
    TaskPriority priority_ = TaskPriority::P1;
    std::int64_t deadline_mono_ns_ = 0;
    std::vector<DAGNode> nodes_;
    std::vector<DAGEdge> edges_;
    std::map<std::string,
             master_agent::orchestrator::CapabilityRetryPolicy> retry_policies_;
    std::string p0_authorization_ref_;
    std::set<std::string> p0_capabilities_;
};

/// Serialises a DAG to JSON. Round-trips the fields the builder sets; fields the
/// builder does not touch (trigger conditions, compensation, result bindings)
/// are omitted rather than emitted as empty objects, so the output stays
/// readable and does not imply configuration that is not there.
inline nlohmann::json dagToJson(const IntentDAG& dag) {
    nlohmann::json out;
    out["dag_id"] = dag.dag_id;
    out["request_id"] = dag.request_id;
    out["schema_version"] = dag.schema_version;
    out["priority"] = master_agent::toString(dag.priority);
    out["deadline_mono_ns"] = dag.deadline_mono_ns;
    out["idempotency_key"] = dag.idempotency_key;

    out["nodes"] = nlohmann::json::array();
    for (const auto& n : dag.nodes) {
        nlohmann::json jn;
        jn["node_id"] = n.node_id;
        jn["action"] = n.action;
        jn["executor"] = n.executor;
        jn["priority"] = master_agent::toString(n.base_priority);
        jn["max_attempts"] = n.max_attempts;
        if (!n.params.empty()) jn["params"] = n.params;
        if (!n.dependencies.empty()) jn["dependencies"] = n.dependencies;
        out["nodes"].push_back(std::move(jn));
    }

    out["edges"] = nlohmann::json::array();
    for (const auto& e : dag.edges) {
        nlohmann::json je;
        je["edge_id"] = e.edge_id;
        je["from"] = e.from_node_id;
        je["to"] = e.to_node_id;
        je["type"] = e.edge_type;
        je["required"] = e.required;
        out["edges"].push_back(std::move(je));
    }
    return out;
}

/// Renders a DAG as a Mermaid flowchart. Pasteable into the README, a GitHub
/// comment, or docs/architecture.md — the point is that a developer can see the
/// plan shape without running it.
inline std::string dagToMermaid(const IntentDAG& dag) {
    std::ostringstream out;
    out << "flowchart TD\n";
    for (const auto& n : dag.nodes) {
        // Node label carries the action, since node_id alone rarely says what
        // the step does.
        out << "    " << n.node_id << "[\"" << n.node_id << "<br/><small>"
            << n.action << "</small>\"]\n";
    }
    if (dag.edges.empty() && dag.nodes.size() > 1) {
        out << "    %% no edges: all nodes start in parallel\n";
    }
    for (const auto& e : dag.edges) {
        out << "    " << e.from_node_id
            << (e.required ? " --> " : " -.-> ")
            << e.to_node_id << "\n";
    }
    return out.str();
}

/// Renders a DAG as indented text for terminal output. Roots first, then each
/// node's dependents, so execution order reads top to bottom.
inline std::string dagToText(const IntentDAG& dag) {
    std::ostringstream out;
    out << "  plan " << dag.dag_id << "  ("
        << dag.nodes.size() << " node"
        << (dag.nodes.size() == 1 ? "" : "s") << ", priority="
        << master_agent::toString(dag.priority) << ")\n";

    for (const auto& n : dag.nodes) {
        const bool is_root = n.dependencies.empty();
        out << "    " << (is_root ? "● " : "○ ") << n.node_id
            << "  " << n.action;
        if (n.executor != "atomic_service") out << "  [" << n.executor << "]";
        if (n.max_attempts > 1) out << "  retries=" << n.max_attempts;
        if (!is_root) {
            out << "\n        after: ";
            for (size_t i = 0; i < n.dependencies.size(); ++i) {
                out << (i ? ", " : "") << n.dependencies[i];
            }
            // A node with several dependencies is a join; naming it explains why
            // the step has not started yet when only one input has finished.
            if (n.dependencies.size() > 1) {
                out << "  (join: waits for all " << n.dependencies.size() << ")";
            }
        }
        out << "\n";
    }
    return out.str();
}

}  // namespace sparx

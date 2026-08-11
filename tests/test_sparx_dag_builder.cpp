/**
 * @file test_sparx_dag_builder.cpp
 * @brief Verifies DagBuilder produces plans the real orchestrator accepts.
 *
 * The builder exists for exactly one reason: a developer should be able to
 * express "call A, then B" without filling 18 fields per node and without
 * discovering by trial and error that validateDAG also checks the
 * AdmissionContext. So the test that matters is not "does the struct have the
 * right fields" — it is "does the real Orchestrator::validateDAG accept what
 * came out of the builder".
 *
 * Every shape below is run through the actual orchestrator, not a mock. If the
 * kernel tightens a validation rule, these fail, which is the point: the
 * builder's contract is defined by the validator, not by its own source.
 */

#include <iostream>
#include <memory>
#include <string>

#include "master_agent/agent_dispatch/agent_dispatch.h"
#include "master_agent/atomic_service/atomic_service.h"
#include "master_agent/common/types.h"
#include "master_agent/orchestrator/orchestrator.h"
#include "sparx_dag_builder.h"
#include "test_support.h"

using namespace master_agent;
using master_agent::test_support::expect;
namespace dispatch = master_agent::agent_dispatch;
namespace orchestrator = master_agent::orchestrator;

namespace {

/// Bundles the orchestrator and its dependencies so each test can validate
/// against a real one. Held together because Orchestrator holds shared_ptrs and
/// would dangle if the deps went out of scope first.
struct Harness {
    std::shared_ptr<ManualRuntimeClock> clock;
    std::shared_ptr<IdGenerator> ids;
    std::shared_ptr<atomic_service::AtomicServiceManager> atomic;
    std::shared_ptr<dispatch::AgentDispatch> dispatch_svc;
    std::unique_ptr<orchestrator::Orchestrator> orch;

    Harness() {
        clock = std::make_shared<ManualRuntimeClock>();
        ids = std::make_shared<IdGenerator>("sparx-dag-builder-test");
        atomic = std::make_shared<atomic_service::AtomicServiceManager>(
            clock, ids, 1);
        dispatch_svc = std::make_shared<dispatch::AgentDispatch>(clock, ids);
        orch = std::make_unique<orchestrator::Orchestrator>(
            clock, ids, atomic, dispatch_svc);
    }

    /// Validates a built plan through the real orchestrator.
    orchestrator::ValidationResult validate(const sparx::BuiltPlan& plan) {
        CallContext call{
            CallerModuleId::AgentService, plan.dag.request_id,
            "trace-sparx-dag-builder",
            plan.admission.principal_id_hash,
            plan.admission.granted_priority,
            plan.dag.deadline_mono_ns, {}, 0,
            plan.admission.authorization_ref};
        return orch->validateDAG(plan.dag, plan.admission, call);
    }
};

void testSingleNodePlanValidates() {
    Harness h;
    auto plan = sparx::DagBuilder("single")
                    .node("only", "vehicle.climate.getTemperature")
                    .build();

    const auto result = h.validate(plan);
    expect(result.valid,
           "1-node plan from builder must pass validateDAG");
    expect(plan.dag.nodes.size() == 1,
           "1-node plan has exactly one node");
    expect(plan.dag.edges.empty(),
           "1-node plan has no edges");
    // The builder must derive allowed_capabilities from the node actions;
    // forgetting this is the single most common reason a hand-built plan is
    // rejected, and the reason build() returns both halves.
    expect(plan.admission.allowed_capabilities.count(
               "vehicle.climate.getTemperature") == 1,
           "node action must appear in admission allowed_capabilities");
}

void testSequentialPlanValidates() {
    Harness h;
    auto plan = sparx::DagBuilder("sequential")
                    .node("read", "vehicle.climate.getTemperature")
                    .node("write", "vehicle.climate.setPower",
                          {{"power", "off"}})
                    .after("read")
                    .build();

    const auto result = h.validate(plan);
    expect(result.valid,
           "2-node sequential plan must pass validateDAG");
    expect(plan.dag.edges.size() == 1,
           "one after() call produces one edge");
    expect(plan.dag.edges[0].from_node_id == "read" &&
               plan.dag.edges[0].to_node_id == "write",
           "edge direction follows after(): dependency -> dependent");
    expect(plan.dag.nodes[1].dependencies.size() == 1 &&
               plan.dag.nodes[1].dependencies[0] == "read",
           "dependent node records its dependency");
    expect(plan.dag.nodes[1].params.value<std::string>("power", "") == "off",
           "node params survive into the built DAG");
}

void testFanInFanOutPlanValidates() {
    Harness h;
    // read_temp and read_speed have no dependencies, so they are parallel
    // roots; decide joins them. This is the shape the builder's "parallelism is
    // the default" claim rests on.
    auto plan = sparx::DagBuilder("fan-in")
                    .node("read_temp", "vehicle.climate.getTemperature")
                    .node("read_speed", "vehicle.telemetry.getSpeed")
                    .node("decide", "vehicle.climate.setPower")
                    .after({"read_temp", "read_speed"})
                    .build();

    const auto result = h.validate(plan);
    expect(result.valid,
           "3-node fan-in plan must pass validateDAG");
    expect(plan.dag.edges.size() == 2,
           "after() with two dependencies produces two edges");
    expect(plan.dag.nodes[0].dependencies.empty() &&
               plan.dag.nodes[1].dependencies.empty(),
           "unsequenced nodes stay parallel roots");
    expect(plan.admission.allowed_capabilities.size() == 3,
           "all three distinct actions are permitted");
}

void testRetriesRegisterMatchingPolicy() {
    Harness h;
    auto plan = sparx::DagBuilder("retrying")
                    .node("read", "vehicle.climate.getTemperature")
                    .retries(3, "READ_ONLY", {"TIMEOUT", "UNAVAILABLE"})
                    .build();

    const auto result = h.validate(plan);
    expect(result.valid,
           "retrying node must validate: reject_code=" + result.reject_code);
    expect(plan.dag.nodes[0].max_attempts == 3,
           "retries() sets max_attempts");
    // The validator requires a policy keyed by action for any node with
    // max_attempts > 1. A builder that set max_attempts alone would produce a
    // plan rejected with ORCHESTRATOR_RETRY_POLICY_REQUIRED — the exact class of
    // mystifying rejection this builder exists to prevent.
    expect(plan.admission.retry_policies.count(
               "vehicle.climate.getTemperature") == 1,
           "retries() registers a policy keyed by the node action");
    expect(!plan.admission.retry_policy_digest.empty(),
           "retry policy digest is computed, or admission is rejected");
    expect(plan.admission.retry_policy_digest ==
               orchestrator::retryPoliciesDigest(plan.admission.retry_policies),
           "digest matches the policies it covers");
}

void testRetriesOfOneRegistersNoPolicy() {
    Harness h;
    auto plan = sparx::DagBuilder("single-attempt")
                    .node("read", "vehicle.climate.getTemperature")
                    .retries(1, "READ_ONLY", {"TIMEOUT"})
                    .build();

    expect(h.validate(plan).valid, "max_attempts=1 plan must validate");
    // No retry means no policy: an unused policy in the admission context would
    // still be digested and validated, so it can only add failure modes.
    expect(plan.admission.retry_policies.empty(),
           "max_attempts=1 registers no retry policy");
    expect(plan.admission.retry_policy_digest.empty(),
           "no policies means no digest");
}

void testP0RequiresExplicitAuthorization() {
    Harness h;
    // priority(P0) alone must NOT validate. If it did, any local caller could
    // self-grant safety-critical scheduling by naming it.
    auto ungranted = sparx::DagBuilder("p0-ungranted")
                         .priority(TaskPriority::P0)
                         .node("urgent", "vehicle.safety.brake")
                         .build();
    const auto ungranted_result = h.validate(ungranted);
    expect(!ungranted_result.valid,
           "P0 without an authorization grant must be rejected");
    expect(ungranted_result.reject_code ==
               "ORCHESTRATOR_P0_AUTHORIZATION_REQUIRED",
           "rejection names the missing P0 authorization, got: " +
               ungranted_result.reject_code);

    auto granted = sparx::DagBuilder("p0-granted")
                       .priority(TaskPriority::P0)
                       .p0Authorization("trusted-safety:sparx-test",
                                        {"vehicle.safety.brake"})
                       .node("urgent", "vehicle.safety.brake")
                       .build();
    const auto granted_result = h.validate(granted);
    expect(granted_result.valid,
           "P0 with a trusted-safety grant must validate: reject_code=" +
               granted_result.reject_code);
    expect(granted.dag.priority == TaskPriority::P0,
           "DAG-level priority reflects priority()");
    expect(granted.dag.nodes[0].base_priority == TaskPriority::P0,
           "node inherits the DAG priority rather than defaulting to P1");
    expect(granted.admission.granted_priority == TaskPriority::P0,
           "admission priority matches the DAG, or admission is rejected");
    expect(granted.admission.p0_authorization,
           "grant sets the p0_authorization flag");
}

void testNodeDeadlinesMatchDag() {
    Harness h;
    auto plan = sparx::DagBuilder("deadline")
                    .deadline(std::chrono::seconds(5))
                    .node("a", "svc.a")
                    .node("b", "svc.b")
                    .build();

    expect(h.validate(plan).valid, "explicit deadline must validate");
    // A node deadline later than the DAG's is the classic silent-timeout bug:
    // the DAG expires while a node still believes it has time.
    for (const auto& n : plan.dag.nodes) {
        expect(n.deadline_mono_ns == plan.dag.deadline_mono_ns,
               "node " + n.node_id + " deadline equals the DAG deadline");
    }
    expect(plan.admission.deadline_mono_ns == plan.dag.deadline_mono_ns,
           "admission deadline equals the DAG deadline");
}

void testJsonExportRoundTripsStructure() {
    auto plan = sparx::DagBuilder("export")
                    .node("read", "vehicle.climate.getTemperature")
                    .node("write", "vehicle.climate.setPower",
                          {{"power", "off"}})
                    .after("read")
                    .build();

    const auto json = sparx::dagToJson(plan.dag);
    expect(json["dag_id"] == "export", "JSON carries dag_id");
    expect(json["schema_version"] == 2,
           "JSON carries the frozen schema version");
    expect(json["nodes"].size() == 2, "JSON lists both nodes");
    expect(json["edges"].size() == 1, "JSON lists the edge");
    expect(json["nodes"][1]["dependencies"][0] == "read",
           "JSON preserves dependencies");
    expect(json["nodes"][1]["params"]["power"] == "off",
           "JSON preserves node params");
    // Omission is deliberate: an empty params object in the output would imply
    // the node was configured with parameters when it was not.
    expect(!json["nodes"][0].contains("params"),
           "empty params are omitted rather than emitted as {}");
    expect(json["edges"][0]["from"] == "read" &&
               json["edges"][0]["to"] == "write",
           "JSON edge keeps direction");
}

void testMermaidExportIsRenderable() {
    auto plan = sparx::DagBuilder("diagram")
                    .node("read", "vehicle.climate.getTemperature")
                    .node("write", "vehicle.climate.setPower")
                    .after("read")
                    .build();

    const auto mermaid = sparx::dagToMermaid(plan.dag);
    expect(mermaid.find("flowchart TD") == 0,
           "Mermaid output starts with a flowchart declaration");
    expect(mermaid.find("read --> write") != std::string::npos,
           "Mermaid contains the required edge as a solid arrow");
    expect(mermaid.find("vehicle.climate.getTemperature") !=
               std::string::npos,
           "Mermaid labels nodes with their action");

    // A parallel plan has no edges; the output must say so rather than render
    // as two unexplained orphan boxes.
    auto parallel = sparx::DagBuilder("parallel")
                        .node("a", "svc.a")
                        .node("b", "svc.b")
                        .build();
    expect(sparx::dagToMermaid(parallel.dag).find("parallel") !=
               std::string::npos,
           "edgeless multi-node plan explains that nodes run in parallel");
}

void testTextExportMarksRootsAndJoins() {
    auto plan = sparx::DagBuilder("text")
                    .node("read_temp", "vehicle.climate.getTemperature")
                    .node("read_speed", "vehicle.telemetry.getSpeed")
                    .node("decide", "vehicle.climate.setPower")
                    .after({"read_temp", "read_speed"})
                    .build();

    const auto text = sparx::dagToText(plan.dag);
    expect(text.find("3 nodes") != std::string::npos,
           "text export reports the node count");
    expect(text.find("● read_temp") != std::string::npos,
           "roots are marked with a filled bullet");
    expect(text.find("○ decide") != std::string::npos,
           "dependent nodes are marked with a hollow bullet");
    expect(text.find("after: read_temp, read_speed") != std::string::npos,
           "dependent node lists what it waits for");
    expect(text.find("join: waits for all 2") != std::string::npos,
           "multi-dependency nodes are identified as joins");

    // Singular/plural: "1 nodes" in developer-facing output is the kind of
    // detail that makes a tool feel unfinished.
    auto single = sparx::DagBuilder("one").node("a", "svc.a").build();
    expect(sparx::dagToText(single.dag).find("1 node ") != std::string::npos ||
               sparx::dagToText(single.dag).find("1 node,") !=
                   std::string::npos,
           "single-node count is not pluralised");
}

void testDistinctPlansGetDistinctIds() {
    auto a = sparx::DagBuilder("plan-a").node("n", "svc.a").build();
    auto b = sparx::DagBuilder("plan-b").node("n", "svc.a").build();
    expect(a.dag.dag_id != b.dag.dag_id,
           "different plan names produce different dag_ids");
    expect(!a.dag.idempotency_key.empty() &&
               !b.dag.idempotency_key.empty(),
           "every plan carries a non-empty idempotency key");
    expect(a.dag.idempotency_key != b.dag.idempotency_key,
           "distinct plans get distinct idempotency keys, or the second "
           "submit is silently deduplicated as a retry of the first");
}

}  // namespace

int main() {
    try {
        testSingleNodePlanValidates();
        testSequentialPlanValidates();
        testFanInFanOutPlanValidates();
        testRetriesRegisterMatchingPolicy();
        testRetriesOfOneRegistersNoPolicy();
        testP0RequiresExplicitAuthorization();
        testNodeDeadlinesMatchDag();
        testJsonExportRoundTripsStructure();
        testMermaidExportIsRenderable();
        testTextExportMarksRootsAndJoins();
        testDistinctPlansGetDistinctIds();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "dag builder tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}

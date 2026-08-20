/**
 * @file sparx_formal_verify.cpp
 * @brief Bounded Model Checker for IntentDAG execution plans.
 *
 * Implements CTL model checking via recursive state-space exploration
 * with depth bounding. Produces counterexample traces on violation.
 */

#include "sparx_formal_verify.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <queue>
#include <set>
#include <sstream>

namespace sparx::formal {

// ---------------------------------------------------------------------------
// Formula construction
// ---------------------------------------------------------------------------

FormulaPtr Formula::makeAtom(const std::string& name,
                             const std::string& node_id,
                             const std::string& param) {
    auto f = std::make_shared<Formula>();
    f->op = TemporalOp::Atom;
    f->atom = Atom{name, node_id, param};
    return f;
}

FormulaPtr Formula::makeNot(FormulaPtr sub) {
    auto f = std::make_shared<Formula>();
    f->op = TemporalOp::Not;
    f->children.push_back(std::move(sub));
    return f;
}

FormulaPtr Formula::makeAnd(FormulaPtr a, FormulaPtr b) {
    auto f = std::make_shared<Formula>();
    f->op = TemporalOp::And;
    f->children.push_back(std::move(a));
    f->children.push_back(std::move(b));
    return f;
}

FormulaPtr Formula::makeOr(FormulaPtr a, FormulaPtr b) {
    auto f = std::make_shared<Formula>();
    f->op = TemporalOp::Or;
    f->children.push_back(std::move(a));
    f->children.push_back(std::move(b));
    return f;
}

FormulaPtr Formula::makeImplies(FormulaPtr a, FormulaPtr b) {
    // a → b ≡ ¬a ∨ b
    return makeOr(makeNot(std::move(a)), std::move(b));
}

FormulaPtr Formula::makeAG(FormulaPtr sub) {
    auto f = std::make_shared<Formula>();
    f->op = TemporalOp::AG;
    f->children.push_back(std::move(sub));
    return f;
}

FormulaPtr Formula::makeAF(FormulaPtr sub) {
    auto f = std::make_shared<Formula>();
    f->op = TemporalOp::AF;
    f->children.push_back(std::move(sub));
    return f;
}

FormulaPtr Formula::makeAX(FormulaPtr sub) {
    auto f = std::make_shared<Formula>();
    f->op = TemporalOp::AX;
    f->children.push_back(std::move(sub));
    return f;
}

FormulaPtr Formula::makeAU(FormulaPtr phi, FormulaPtr psi) {
    auto f = std::make_shared<Formula>();
    f->op = TemporalOp::AU;
    f->children.push_back(std::move(phi));
    f->children.push_back(std::move(psi));
    return f;
}

FormulaPtr Formula::makeEF(FormulaPtr sub) {
    auto f = std::make_shared<Formula>();
    f->op = TemporalOp::EF;
    f->children.push_back(std::move(sub));
    return f;
}

FormulaPtr Formula::makeBounded(FormulaPtr sub, std::uint32_t k) {
    auto f = std::make_shared<Formula>();
    f->op = TemporalOp::ABounded;
    f->children.push_back(std::move(sub));
    f->bound = k;
    return f;
}

// ---------------------------------------------------------------------------
// Kripke Model construction from plan
// ---------------------------------------------------------------------------

KripkeModel KripkeModel::fromPlan(const std::vector<PlanNode>& nodes) {
    KripkeModel model;
    model.nodes = nodes;

    if (nodes.empty()) {
        model.initial_state = "empty";
        model.states["empty"] = KripkeState{"empty", {{"plan.empty"}}, {}};
        return model;
    }

    // Build state for each execution stage:
    // For each node: "before_X", "executing_X", "completed_X", "failed_X"
    // Plus global states: "init", "all_completed"

    model.initial_state = "init";
    KripkeState init{"init", {{"plan.started"}}, {}};

    // Find root nodes (no dependencies)
    std::set<std::string> all_ids;
    std::map<std::string, const PlanNode*> node_map;
    for (const auto& n : nodes) {
        all_ids.insert(n.id);
        node_map[n.id] = &n;
    }

    std::vector<std::string> roots;
    for (const auto& n : nodes) {
        if (n.deps.empty()) roots.push_back(n.id);
    }

    // Init transitions to executing roots
    for (const auto& root : roots) {
        init.successors.push_back("executing_" + root);
    }
    model.states["init"] = init;

    // Build states for each node
    std::uint32_t max_depth = 0;
    for (const auto& n : nodes) {
        // Executing state
        KripkeState exec;
        exec.id = "executing_" + n.id;
        exec.labels.propositions.insert("node.executing");
        exec.labels.propositions.insert("node." + n.id + ".executing");
        if (n.is_destructive)
            exec.labels.propositions.insert("node.destructive");
        if (n.requires_auth)
            exec.labels.propositions.insert("node.requires_auth");
        for (const auto& res : n.resources)
            exec.labels.propositions.insert("resource.held." + res);
        exec.successors.push_back("completed_" + n.id);
        exec.successors.push_back("failed_" + n.id);
        model.states[exec.id] = exec;

        // Completed state
        KripkeState comp;
        comp.id = "completed_" + n.id;
        comp.labels.propositions.insert("node.completed");
        comp.labels.propositions.insert("node." + n.id + ".completed");
        // Find nodes that depend on this one
        for (const auto& other : nodes) {
            if (other.id == n.id) continue;
            bool depends_on_n = false;
            for (const auto& dep : other.deps) {
                if (dep == n.id) { depends_on_n = true; break; }
            }
            if (depends_on_n) {
                // Check if all other deps are also satisfied
                // (simplified: assume sequential for now)
                comp.successors.push_back("executing_" + other.id);
            }
        }
        if (comp.successors.empty()) {
            comp.successors.push_back("all_completed");
        }
        model.states[comp.id] = comp;

        // Failed state
        KripkeState fail;
        fail.id = "failed_" + n.id;
        fail.labels.propositions.insert("node.failed");
        fail.labels.propositions.insert("node." + n.id + ".failed");
        fail.labels.propositions.insert("plan.has_failure");
        if (n.is_idempotent) {
            fail.successors.push_back("executing_" + n.id);  // retry
        }
        fail.successors.push_back("plan_failed");
        model.states[fail.id] = fail;
    }

    // Terminal states
    KripkeState all_done{"all_completed",
        {{"plan.completed", "plan.success"}}, {}};
    all_done.successors.push_back("all_completed");  // self-loop (terminal)
    model.states["all_completed"] = all_done;

    KripkeState plan_fail{"plan_failed",
        {{"plan.failed"}}, {}};
    plan_fail.successors.push_back("plan_failed");  // self-loop
    model.states["plan_failed"] = plan_fail;

    // Compute depth via BFS from init
    std::queue<std::pair<std::string, uint32_t>> bfs;
    std::set<std::string> visited;
    bfs.push({model.initial_state, 0});
    while (!bfs.empty()) {
        auto [sid, d] = bfs.front();
        bfs.pop();
        if (visited.count(sid)) continue;
        visited.insert(sid);
        max_depth = std::max(max_depth, d);
        auto it = model.states.find(sid);
        if (it != model.states.end()) {
            for (const auto& succ : it->second.successors) {
                if (!visited.count(succ)) bfs.push({succ, d + 1});
            }
        }
    }
    model.depth = max_depth;
    return model;
}

// ---------------------------------------------------------------------------
// Built-in safety properties
// ---------------------------------------------------------------------------

namespace properties {

FormulaPtr authBeforeDestructive() {
    // AG(destructive → previously_authorized)
    // Simplified: AG(¬(destructive ∧ ¬authorized))
    auto destructive = Formula::makeAtom("node.destructive");
    auto auth = Formula::makeAtom("node.authorized");
    auto violation = Formula::makeAnd(destructive, Formula::makeNot(auth));
    return Formula::makeAG(Formula::makeNot(violation));
}

FormulaPtr noConflictingDestructive() {
    // AG(¬(two_destructive_same_resource))
    return Formula::makeAG(
        Formula::makeNot(Formula::makeAtom("conflict.destructive")));
}

FormulaPtr allNodesTerminate() {
    // AF(plan.completed ∨ plan.failed)
    auto done = Formula::makeOr(
        Formula::makeAtom("plan.completed"),
        Formula::makeAtom("plan.failed"));
    return Formula::makeAF(done);
}

FormulaPtr deadlineSafety(std::uint32_t global_deadline_ms) {
    auto completed = Formula::makeAtom("plan.completed");
    return Formula::makeBounded(completed, global_deadline_ms / 100);
}

FormulaPtr noResourceDeadlock() {
    // AG(resource.held → AF(resource.released))
    auto held = Formula::makeAtom("resource.held");
    auto released = Formula::makeAtom("resource.released");
    return Formula::makeAG(Formula::makeImplies(held, Formula::makeAF(released)));
}

FormulaPtr retryIsSafe() {
    // AG(node.failed ∧ node.idempotent → EF(node.completed))
    auto failed = Formula::makeAtom("node.failed");
    auto idempotent = Formula::makeAtom("node.idempotent");
    auto completed = Formula::makeAtom("node.completed");
    auto can_retry = Formula::makeAnd(failed, idempotent);
    return Formula::makeAG(
        Formula::makeImplies(can_retry, Formula::makeEF(completed)));
}

FormulaPtr dataFlowIntegrity() {
    return Formula::makeAG(
        Formula::makeNot(Formula::makeAtom("dataflow.backward")));
}

}  // namespace properties

// ---------------------------------------------------------------------------
// PlanVerifier — Bounded Model Checker
// ---------------------------------------------------------------------------

PlanVerifier::PlanVerifier(VerifierConfig config)
    : config_(std::move(config)) {}

PlanVerification PlanVerifier::verify(
    const std::vector<PlanNode>& plan) const {

    auto start = std::chrono::steady_clock::now();
    PlanVerification result;
    result.plan_id = plan.empty() ? "empty" : plan[0].id;

    // Build Kripke model
    auto model = KripkeModel::fromPlan(plan);

    // Determine properties to check
    std::vector<std::pair<std::string, FormulaPtr>> props;

    if (config_.properties.empty()) {
        // Check all built-in properties
        props.emplace_back("auth-before-destructive",
                           properties::authBeforeDestructive());
        props.emplace_back("no-conflicting-destructive",
                           properties::noConflictingDestructive());
        props.emplace_back("all-nodes-terminate",
                           properties::allNodesTerminate());
        props.emplace_back("no-resource-deadlock",
                           properties::noResourceDeadlock());
        props.emplace_back("data-flow-integrity",
                           properties::dataFlowIntegrity());
    } else {
        for (const auto& name : config_.properties) {
            if (name == "auth-before-destructive")
                props.emplace_back(name, properties::authBeforeDestructive());
            else if (name == "all-nodes-terminate")
                props.emplace_back(name, properties::allNodesTerminate());
            else if (name == "no-resource-deadlock")
                props.emplace_back(name, properties::noResourceDeadlock());
            else if (name == "data-flow-integrity")
                props.emplace_back(name, properties::dataFlowIntegrity());
        }
    }

    // Add custom properties
    for (const auto& [name, formula] : config_.custom_properties) {
        props.emplace_back(name, formula);
    }

    // Verify each property
    result.all_satisfied = true;
    for (const auto& [name, formula] : props) {
        auto vr = checkProperty(model, name, formula);
        if (!vr.satisfied()) result.all_satisfied = false;
        result.total_states_explored += vr.states_explored;
        result.results.push_back(std::move(vr));
    }

    auto elapsed = std::chrono::steady_clock::now() - start;
    result.total_time = std::chrono::duration_cast<
        std::chrono::microseconds>(elapsed);
    return result;
}

VerificationResult PlanVerifier::checkProperty(
    const KripkeModel& model,
    const std::string& name,
    FormulaPtr property) const {

    auto start = std::chrono::steady_clock::now();
    VerificationResult result;
    result.property_name = name;
    result.property = property;

    uint32_t max_depth = config_.max_depth > 0
        ? config_.max_depth : model.depth + 1;
    result.bound_used = max_depth;

    std::vector<TraceStep> trace;
    bool holds = evaluate(model, model.initial_state, *property,
                          max_depth, trace);

    if (holds) {
        result.verdict = VerificationResult::Verdict::Satisfied;
    } else {
        result.verdict = VerificationResult::Verdict::Violated;
        if (config_.generate_counterexamples) {
            result.counterexample = trace;
        }
        result.violation_explanation =
            "Property '" + name + "' violated at state: " +
            (trace.empty() ? "initial" : trace.back().state_id);
    }

    result.states_explored = static_cast<uint32_t>(
        model.states.size());
    auto elapsed = std::chrono::steady_clock::now() - start;
    result.verification_time = std::chrono::duration_cast<
        std::chrono::microseconds>(elapsed);
    return result;
}

bool PlanVerifier::evaluate(
    const KripkeModel& model,
    const std::string& state_id,
    const Formula& formula,
    std::uint32_t depth,
    std::vector<TraceStep>& trace) const {

    if (depth == 0) return true;  // bounded: assume holds beyond bound

    auto state_it = model.states.find(state_id);
    if (state_it == model.states.end()) return true;
    const auto& state = state_it->second;

    switch (formula.op) {
        case TemporalOp::Atom: {
            if (!formula.atom) return false;
            return state.labels.has(formula.atom->name);
        }
        case TemporalOp::Not: {
            return !evaluate(model, state_id, *formula.children[0],
                             depth, trace);
        }
        case TemporalOp::And: {
            return evaluate(model, state_id, *formula.children[0],
                            depth, trace) &&
                   evaluate(model, state_id, *formula.children[1],
                            depth, trace);
        }
        case TemporalOp::Or: {
            return evaluate(model, state_id, *formula.children[0],
                            depth, trace) ||
                   evaluate(model, state_id, *formula.children[1],
                            depth, trace);
        }
        case TemporalOp::Implies: {
            // p → q ≡ ¬p ∨ q
            return !evaluate(model, state_id, *formula.children[0],
                             depth, trace) ||
                   evaluate(model, state_id, *formula.children[1],
                            depth, trace);
        }
        case TemporalOp::AG: {
            std::set<std::string> visited;
            return evaluateAG(model, state_id, *formula.children[0],
                              depth, visited, trace);
        }
        case TemporalOp::AF: {
            std::set<std::string> visited;
            return evaluateAF(model, state_id, *formula.children[0],
                              depth, visited, trace);
        }
        case TemporalOp::AX: {
            // All successors satisfy the sub-formula
            for (const auto& succ : state.successors) {
                if (!evaluate(model, succ, *formula.children[0],
                              depth - 1, trace)) {
                    trace.push_back({succ, "", "AX violated", state.labels,
                        static_cast<uint32_t>(trace.size())});
                    return false;
                }
            }
            return true;
        }
        case TemporalOp::EF: {
            std::set<std::string> visited;
            return evaluateEF(model, state_id, *formula.children[0],
                              depth, visited);
        }
        case TemporalOp::ABounded: {
            // Check if sub-formula holds within k steps
            std::set<std::string> visited;
            return evaluateAF(model, state_id, *formula.children[0],
                              std::min(depth, formula.bound), visited, trace);
        }
        case TemporalOp::AU: {
            // A[φ U ψ]: ψ holds, OR (φ holds AND all successors satisfy AU)
            if (evaluate(model, state_id, *formula.children[1], depth, trace)) {
                return true;  // ψ satisfied now
            }
            if (!evaluate(model, state_id, *formula.children[0], depth, trace)) {
                return false;  // φ must hold until ψ
            }
            // All successors must satisfy AU
            for (const auto& succ : state.successors) {
                if (!evaluate(model, succ, formula, depth - 1, trace)) {
                    trace.push_back({succ, "", "AU violated", state.labels,
                        static_cast<uint32_t>(trace.size())});
                    return false;
                }
            }
            return true;
        }
        default:
            return true;
    }
}

bool PlanVerifier::evaluateAG(
    const KripkeModel& model,
    const std::string& state_id,
    const Formula& sub,
    std::uint32_t depth,
    std::set<std::string>& visited,
    std::vector<TraceStep>& trace) const {

    if (depth == 0 || visited.count(state_id)) return true;
    visited.insert(state_id);

    auto state_it = model.states.find(state_id);
    if (state_it == model.states.end()) return true;
    const auto& state = state_it->second;

    // Check sub-formula at current state
    std::vector<TraceStep> sub_trace;
    if (!evaluate(model, state_id, sub, depth, sub_trace)) {
        trace.push_back({state_id, "", "AG violated here", state.labels,
            static_cast<uint32_t>(trace.size())});
        return false;
    }

    // Check all successors (with POR if enabled)
    const auto& succs = config_.enable_por
        ? computeAmpleSet(model, state, sub) : state.successors;
    for (const auto& succ : succs) {
        if (!evaluateAG(model, succ, sub, depth - 1, visited, trace)) {
            return false;
        }
    }
    return true;
}

bool PlanVerifier::evaluateAF(
    const KripkeModel& model,
    const std::string& state_id,
    const Formula& sub,
    std::uint32_t depth,
    std::set<std::string>& visited,
    std::vector<TraceStep>& trace) const {

    if (visited.count(state_id)) return false;  // cycle = no progress
    if (depth == 0) return false;  // bound exceeded
    visited.insert(state_id);

    auto state_it = model.states.find(state_id);
    if (state_it == model.states.end()) return false;
    const auto& state = state_it->second;

    // If sub holds here, AF is satisfied
    std::vector<TraceStep> sub_trace;
    if (evaluate(model, state_id, sub, depth, sub_trace)) {
        return true;
    }

    // Must hold on ALL paths eventually
    if (state.successors.empty()) return false;  // deadend without satisfaction
    for (const auto& succ : state.successors) {
        std::set<std::string> branch_visited = visited;
        if (!evaluateAF(model, succ, sub, depth - 1, branch_visited, trace)) {
            trace.push_back({state_id, "", "AF not satisfied on path to " + succ,
                state.labels, static_cast<uint32_t>(trace.size())});
            return false;
        }
    }
    return true;
}

bool PlanVerifier::evaluateEF(
    const KripkeModel& model,
    const std::string& state_id,
    const Formula& sub,
    std::uint32_t depth,
    std::set<std::string>& visited) const {

    if (visited.count(state_id) || depth == 0) return false;
    visited.insert(state_id);

    auto state_it = model.states.find(state_id);
    if (state_it == model.states.end()) return false;
    const auto& state = state_it->second;

    // If sub holds here, EF is satisfied
    std::vector<TraceStep> dummy;
    if (evaluate(model, state_id, sub, depth, dummy)) return true;

    // Check any successor
    for (const auto& succ : state.successors) {
        if (evaluateEF(model, succ, sub, depth - 1, visited)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Partial-Order Reduction (Peled's stubborn set / ample set method)
// ---------------------------------------------------------------------------
//
// Reference: D. Peled, "All from One, One for All: On Model Checking Using
// Representatives" (CAV 1993). Also: Valmari's stubborn sets.
//
// Key insight: for safety properties (AG φ), independent transitions can be
// explored in any order. We compute an ample set A(s) ⊆ enabled(s) such that:
//   C0: A(s) is non-empty (unless enabled(s) is empty)
//   C1: No transition in the plan that is dependent on a transition in A(s)
//       can be executed without a transition in A(s) occurring first (closedness)
//   C2: If a cycle is formed in the reduced graph, at least one state in the
//       cycle must be fully expanded (prevents ignoring behaviors on cycles)
//   C3: (for liveness) The ample set must not permanently ignore any transition
//
// For DAG-based plans (no cycles), C2 is automatically satisfied.

std::map<std::string, std::set<std::string>> PlanVerifier::buildDependencyGraph(
    const KripkeModel& model) const {
    // Two nodes are dependent if they share a resource, have a data dependency,
    // or both affect the same labels (e.g., both are destructive on same target).
    std::map<std::string, std::set<std::string>> deps;

    for (const auto& node : model.nodes) {
        deps[node.id] = {};
    }

    for (size_t i = 0; i < model.nodes.size(); ++i) {
        for (size_t j = i + 1; j < model.nodes.size(); ++j) {
            if (!areIndependent(model, model.nodes[i].id, model.nodes[j].id)) {
                deps[model.nodes[i].id].insert(model.nodes[j].id);
                deps[model.nodes[j].id].insert(model.nodes[i].id);
            }
        }
    }
    return deps;
}

bool PlanVerifier::areIndependent(
    const KripkeModel& model,
    const std::string& t1, const std::string& t2) const {

    // Find the PlanNodes
    const PlanNode* n1 = nullptr;
    const PlanNode* n2 = nullptr;
    for (const auto& node : model.nodes) {
        if (node.id == t1) n1 = &node;
        if (node.id == t2) n2 = &node;
    }
    if (!n1 || !n2) return true;  // unknown nodes are independent by default

    // Rule 1: Data dependency — if one depends on the other, not independent
    for (const auto& dep : n1->deps) {
        if (dep == t2) return false;
    }
    for (const auto& dep : n2->deps) {
        if (dep == t1) return false;
    }

    // Rule 2: Shared resource — conflict on same resource
    for (const auto& r1 : n1->resources) {
        for (const auto& r2 : n2->resources) {
            if (r1 == r2) return false;  // resource conflict
        }
    }

    // Rule 3: Both destructive on same service — dependent (safety critical)
    if (n1->is_destructive && n2->is_destructive &&
        n1->service == n2->service && !n1->service.empty()) {
        return false;
    }

    // Rule 4: One requires auth and the other is the auth provider
    // (captured by deps, but double-check tool name patterns)
    if (n1->requires_auth && n2->tool_name.find("auth") != std::string::npos)
        return false;
    if (n2->requires_auth && n1->tool_name.find("auth") != std::string::npos)
        return false;

    return true;
}

std::vector<std::string> PlanVerifier::computeAmpleSet(
    const KripkeModel& model,
    const KripkeState& state,
    const Formula& property) const {

    const auto& successors = state.successors;
    if (successors.size() <= 1) return successors;  // trivial: no reduction possible

    // Build dependency info
    auto dep_graph = buildDependencyGraph(model);

    // Strategy: find a successor whose corresponding transition is independent
    // of all others currently enabled. If found, that single transition is the
    // ample set (maximum reduction).
    //
    // For each successor state, identify which plan node it corresponds to
    // (by naming convention: state "s_nodeX_executing" → node "nodeX").

    auto nodeForState = [](const std::string& state_id) -> std::string {
        // Convention: "s_<nodeId>_<status>" or just "<nodeId>"
        auto prefix = state_id.find("s_");
        if (prefix == 0) {
            auto end = state_id.find('_', 2);
            if (end != std::string::npos) return state_id.substr(2, end - 2);
        }
        return state_id;
    };

    // Identify enabled transitions (nodes that would execute next from each successor)
    std::vector<std::string> enabled_nodes;
    for (const auto& succ : successors) {
        enabled_nodes.push_back(nodeForState(succ));
    }

    // Try to find a singleton ample set (best case: O(n) → O(1) branching)
    for (size_t i = 0; i < enabled_nodes.size(); ++i) {
        const auto& candidate = enabled_nodes[i];
        bool all_independent = true;

        for (size_t j = 0; j < enabled_nodes.size(); ++j) {
            if (i == j) continue;
            if (dep_graph.count(candidate) &&
                dep_graph.at(candidate).count(enabled_nodes[j])) {
                all_independent = false;
                break;
            }
        }

        if (all_independent) {
            // C1 is satisfied: candidate is independent of all other enabled
            // transitions, so we can explore it alone
            return {successors[i]};
        }
    }

    // No singleton found — try to find a minimal dependent cluster
    // (greedy: start with first node, add all its dependents)
    if (!enabled_nodes.empty()) {
        std::set<size_t> ample_indices;
        ample_indices.insert(0);

        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t i = 0; i < enabled_nodes.size(); ++i) {
                if (ample_indices.count(i)) continue;
                // Check if node i is dependent on any node in ample set
                for (size_t j : ample_indices) {
                    if (dep_graph.count(enabled_nodes[j]) &&
                        dep_graph.at(enabled_nodes[j]).count(enabled_nodes[i])) {
                        ample_indices.insert(i);
                        changed = true;
                        break;
                    }
                }
            }
        }

        // If ample set is smaller than full successors, use it
        if (ample_indices.size() < successors.size()) {
            std::vector<std::string> ample;
            for (size_t idx : ample_indices) {
                ample.push_back(successors[idx]);
            }
            return ample;
        }
    }

    // Fallback: full expansion (no reduction possible)
    return successors;
}

// ---------------------------------------------------------------------------
// PlanVerification reporting
// ---------------------------------------------------------------------------

std::optional<VerificationResult> PlanVerification::firstViolation() const {
    for (const auto& r : results) {
        if (r.verdict == VerificationResult::Verdict::Violated) return r;
    }
    return std::nullopt;
}

std::string PlanVerification::report() const {
    std::ostringstream oss;
    oss << "Plan Verification Report\n";
    oss << "═══════════════════════════\n";
    oss << "Plan: " << plan_id << "\n";
    oss << "Properties checked: " << results.size() << "\n";
    oss << "Total time: " << total_time.count() << "µs\n";
    oss << "States explored: " << total_states_explored << "\n\n";

    for (const auto& r : results) {
        const char* verdict_str = "?";
        switch (r.verdict) {
            case VerificationResult::Verdict::Satisfied: verdict_str = "✓ PASS"; break;
            case VerificationResult::Verdict::Violated:  verdict_str = "✗ FAIL"; break;
            case VerificationResult::Verdict::Unknown:   verdict_str = "? UNKNOWN"; break;
            case VerificationResult::Verdict::Error:     verdict_str = "! ERROR"; break;
        }
        oss << "  " << verdict_str << "  " << r.property_name
            << " (" << r.verification_time.count() << "µs)\n";
        if (!r.violation_explanation.empty()) {
            oss << "         " << r.violation_explanation << "\n";
        }
    }

    oss << "\n";
    if (all_satisfied) {
        oss << "✓ All properties satisfied. Plan is safe to execute.\n";
    } else {
        oss << "✗ Verification FAILED. Plan should NOT be executed.\n";
    }
    return oss.str();
}

std::string PlanVerification::certificate() const {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"plan_id\": \"" << plan_id << "\",\n";
    oss << "  \"verified\": " << (all_satisfied ? "true" : "false") << ",\n";
    oss << "  \"properties_checked\": " << results.size() << ",\n";
    oss << "  \"verification_time_us\": " << total_time.count() << ",\n";
    oss << "  \"states_explored\": " << total_states_explored << ",\n";
    oss << "  \"results\": [\n";
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        oss << "    {\"property\": \"" << r.property_name << "\", ";
        oss << "\"satisfied\": " << (r.satisfied() ? "true" : "false") << ", ";
        oss << "\"time_us\": " << r.verification_time.count() << "}";
        if (i + 1 < results.size()) oss << ",";
        oss << "\n";
    }
    oss << "  ]\n}\n";
    return oss.str();
}

// ---------------------------------------------------------------------------
// RuntimeMonitor — Past-Time Property Evaluation
// ---------------------------------------------------------------------------

RuntimeMonitor::RuntimeMonitor(std::vector<FormulaPtr> properties)
    : properties_(std::move(properties)) {}

std::vector<std::string> RuntimeMonitor::observe(const ExecutionEvent& event) {
    trace_.push_back(event);

    // Update accumulated state from this event.
    // The runtime monitor maintains a set of propositions that hold NOW,
    // built incrementally from the event stream.
    updateState(event);

    // Evaluate each monitored property against the current state.
    // For runtime monitoring, we use past-time evaluation:
    //   - AG(φ) is violated if ¬φ holds at any point (including now)
    //   - AF(φ) cannot be definitively violated at runtime (liveness),
    //     but we can detect if deadline exceeded without φ
    //   - Safety properties (AG ¬bad) are checkable immediately
    std::vector<std::string> new_violations;

    for (size_t i = 0; i < properties_.size(); ++i) {
        // Skip already-violated properties (monotonic)
        if (violated_indices_.count(i)) continue;

        bool violated = checkPropertyViolation(*properties_[i], event);
        if (violated) {
            violated_indices_.insert(i);
            std::string desc = describeViolation(*properties_[i], event);
            new_violations.push_back(desc);
            violations_.emplace_back(event.node_id, desc);
        }
    }

    return new_violations;
}

bool RuntimeMonitor::allSatisfied() const {
    return violations_.empty();
}

std::vector<std::pair<std::string, std::string>>
RuntimeMonitor::violations() const {
    return violations_;
}

void RuntimeMonitor::updateState(const ExecutionEvent& event) {
    const auto& nid = event.node_id;

    if (event.event_type == "started") {
        active_nodes_.insert(nid);
        node_states_[nid] = "executing";
        // Check metadata for destructive/auth annotations
        auto it = event.metadata.find("destructive");
        if (it != event.metadata.end() && it->second == "true") {
            destructive_active_.insert(nid);
        }
        auto res_it = event.metadata.find("resources");
        if (res_it != event.metadata.end()) {
            held_resources_[nid] = res_it->second;
        }
    } else if (event.event_type == "completed") {
        active_nodes_.erase(nid);
        node_states_[nid] = "completed";
        completed_nodes_.insert(nid);
        destructive_active_.erase(nid);
        held_resources_.erase(nid);
    } else if (event.event_type == "failed") {
        active_nodes_.erase(nid);
        node_states_[nid] = "failed";
        failed_nodes_.insert(nid);
        destructive_active_.erase(nid);
        // Resources may leak on failure — track for deadlock detection
    } else if (event.event_type == "timeout") {
        active_nodes_.erase(nid);
        node_states_[nid] = "timeout";
        timed_out_nodes_.insert(nid);
    } else if (event.event_type == "authorized") {
        authorized_nodes_.insert(nid);
    }

    // Track start times for deadline checking
    if (event.event_type == "started") {
        start_times_[nid] = event.timestamp_ms;
    }
}

bool RuntimeMonitor::checkPropertyViolation(
    const Formula& property, const ExecutionEvent& event) const {

    // Dispatch based on the top-level operator.
    // For runtime monitoring, we interpret CTL properties as past-time checks:
    switch (property.op) {
        case TemporalOp::AG: {
            // AG(φ) violated iff ¬φ holds at the current state.
            // Evaluate the sub-formula against the current event/state.
            return !evaluateAtCurrentState(*property.children[0], event);
        }
        case TemporalOp::AF: {
            // AF(φ) — liveness. Can only detect violation via deadline.
            // If we see a terminal event (failed/timeout) and φ never held,
            // that's a violation on this path.
            if (event.event_type == "failed" || event.event_type == "timeout") {
                return !hasEverHeld(*property.children[0]);
            }
            return false;
        }
        case TemporalOp::AX: {
            // AX(φ) — next state must satisfy φ.
            // At runtime we check: if this event is a "next" relative to
            // a previously-seen state, does φ hold now?
            return !evaluateAtCurrentState(*property.children[0], event);
        }
        case TemporalOp::ABounded: {
            // Bounded liveness: φ must hold within k steps.
            // Violated if trace length exceeds bound and φ never held.
            if (trace_.size() > property.bound) {
                return !hasEverHeld(*property.children[0]);
            }
            return false;
        }
        case TemporalOp::Not: {
            // ¬φ is violated iff φ holds
            return evaluateAtCurrentState(*property.children[0], event);
        }
        default:
            // For non-temporal top-level, evaluate as state predicate
            return !evaluateAtCurrentState(property, event);
    }
}

bool RuntimeMonitor::evaluateAtCurrentState(
    const Formula& formula, const ExecutionEvent& event) const {

    switch (formula.op) {
        case TemporalOp::Atom: {
            if (!formula.atom) return false;
            return checkProposition(formula.atom->name, event);
        }
        case TemporalOp::Not: {
            return !evaluateAtCurrentState(*formula.children[0], event);
        }
        case TemporalOp::And: {
            return evaluateAtCurrentState(*formula.children[0], event) &&
                   evaluateAtCurrentState(*formula.children[1], event);
        }
        case TemporalOp::Or: {
            return evaluateAtCurrentState(*formula.children[0], event) ||
                   evaluateAtCurrentState(*formula.children[1], event);
        }
        case TemporalOp::Implies: {
            return !evaluateAtCurrentState(*formula.children[0], event) ||
                   evaluateAtCurrentState(*formula.children[1], event);
        }
        case TemporalOp::AG:
        case TemporalOp::AF:
        case TemporalOp::AX:
        case TemporalOp::EF: {
            // Nested temporal — evaluate sub-formula at current state
            return evaluateAtCurrentState(*formula.children[0], event);
        }
        default:
            return true;
    }
}

bool RuntimeMonitor::checkProposition(
    const std::string& prop, const ExecutionEvent& event) const {

    // Core propositions checked against accumulated runtime state:
    if (prop == "node.destructive") {
        return destructive_active_.count(event.node_id) > 0;
    }
    if (prop == "node.authorized") {
        return authorized_nodes_.count(event.node_id) > 0;
    }
    if (prop == "node.executing") {
        return active_nodes_.count(event.node_id) > 0;
    }
    if (prop == "node.completed") {
        return completed_nodes_.count(event.node_id) > 0;
    }
    if (prop == "node.failed") {
        return failed_nodes_.count(event.node_id) > 0;
    }
    if (prop == "plan.completed") {
        // All nodes completed (simplified: no active, no pending)
        return active_nodes_.empty() && !completed_nodes_.empty();
    }
    if (prop == "plan.failed") {
        return !failed_nodes_.empty() || !timed_out_nodes_.empty();
    }
    if (prop == "resource.held") {
        return !held_resources_.empty();
    }
    if (prop == "resource.released") {
        return held_resources_.empty();
    }
    if (prop == "conflict.destructive") {
        // Two destructive ops active simultaneously
        return destructive_active_.size() >= 2;
    }
    if (prop == "dataflow.backward") {
        // Check metadata for backward data flow flag
        auto it = event.metadata.find("dataflow_direction");
        return it != event.metadata.end() && it->second == "backward";
    }

    // Check node-specific propositions: "node.<id>.completed" etc.
    if (prop.find("node.") == 0 && prop.find(".completed") != std::string::npos) {
        auto id_start = 5;  // after "node."
        auto id_end = prop.find(".completed");
        auto id = prop.substr(id_start, id_end - id_start);
        return completed_nodes_.count(id) > 0;
    }
    if (prop.find("node.") == 0 && prop.find(".executing") != std::string::npos) {
        auto id_start = 5;
        auto id_end = prop.find(".executing");
        auto id = prop.substr(id_start, id_end - id_start);
        return active_nodes_.count(id) > 0;
    }

    return false;
}

bool RuntimeMonitor::hasEverHeld(const Formula& formula) const {
    // Check if the formula has ever been true at any point in the trace.
    // Replay trace events to check.
    for (const auto& past_event : trace_) {
        if (evaluateAtCurrentState(formula, past_event)) return true;
    }
    return false;
}

std::string RuntimeMonitor::describeViolation(
    const Formula& property, const ExecutionEvent& event) const {

    std::string desc;
    switch (property.op) {
        case TemporalOp::AG:
            desc = "safety-violation";
            break;
        case TemporalOp::AF:
            desc = "liveness-violation";
            break;
        case TemporalOp::ABounded:
            desc = "deadline-violation";
            break;
        default:
            desc = "property-violation";
            break;
    }
    desc += ":" + event.node_id + "@" + event.event_type;
    if (event.timestamp_ms > 0) {
        desc += "[t=" + std::to_string(event.timestamp_ms) + "ms]";
    }
    return desc;
}

// ===========================================================================
// SECTION 2: SMT-based Bounded Model Checking — CDCL SAT Solver
// ===========================================================================

// Luby restart sequence: 1,1,2,1,1,2,4,1,1,2,1,1,2,4,8,...
std::uint32_t CDCLSolver::lubySequence(std::uint32_t i) {
    // Find the finite subsequence that contains index i
    std::uint32_t size = 1, seq = 0;
    while (size < i + 1) {
        seq++;
        size = 2 * size + 1;
    }
    while (size - 1 != i) {
        size = (size - 1) >> 1;
        seq--;
        if (i >= size) i -= size;
    }
    return static_cast<std::uint32_t>(1) << seq;
}

CDCLSolver::CDCLSolver() {}

std::int32_t CDCLSolver::newVar() {
    ++num_vars_;
    assigns_.push_back(VarAssign::Unassigned);
    vsids_activity_.push_back(0.0);
    // Two watch lists per variable (positive and negative literal)
    watches_.emplace_back();  // positive
    watches_.emplace_back();  // negative
    return num_vars_;
}

std::int32_t CDCLSolver::addClause(std::vector<Literal> lits) {
    std::int32_t idx = static_cast<std::int32_t>(clauses_.size());
    Clause c;
    c.literals = std::move(lits);

    // Set up watched literals (watch first two literals)
    if (c.literals.size() >= 2) {
        watches_[c.literals[0].index()].push_back(idx);
        watches_[c.literals[1].index()].push_back(idx);
    } else if (c.literals.size() == 1) {
        watches_[c.literals[0].index()].push_back(idx);
    }

    clauses_.push_back(std::move(c));
    return idx;
}

VarAssign CDCLSolver::litValue(Literal lit) const {
    VarAssign val = assigns_[lit.var];
    if (val == VarAssign::Unassigned) return VarAssign::Unassigned;
    if (lit.negated) {
        return val == VarAssign::True ? VarAssign::False : VarAssign::True;
    }
    return val;
}

void CDCLSolver::assign(Literal lit, std::int32_t reason) {
    assigns_[lit.var] = lit.negated ? VarAssign::False : VarAssign::True;
    trail_.push_back({lit, decision_level_, reason});
}

std::int32_t CDCLSolver::propagate() {
    while (propagation_head_ < trail_.size()) {
        Literal p = trail_[propagation_head_++].lit;
        // The literal p just became true, so ~p is false.
        // Check all clauses watching ~p.
        Literal falsified = ~p;
        auto& watch_list = watches_[falsified.index()];

        std::size_t i = 0, j = 0;
        while (i < watch_list.size()) {
            std::int32_t ci = watch_list[i];
            Clause& clause = clauses_[ci];

            // Find the watched literal positions
            // Ensure the falsified literal is at position 1
            if (clause.literals.size() < 2) {
                // Unit clause that is now falsified = conflict
                watch_list[j++] = watch_list[i++];
                // Copy remaining
                while (i < watch_list.size()) watch_list[j++] = watch_list[i++];
                watch_list.resize(j);
                return ci;
            }

            if (clause.literals[0] == falsified) {
                std::swap(clause.literals[0], clause.literals[1]);
            }

            // Now clause.literals[1] == falsified literal
            // Check if first watched literal is already true
            if (litValue(clause.literals[0]) == VarAssign::True) {
                watch_list[j++] = watch_list[i++];
                continue;
            }

            // Look for a new literal to watch
            bool found = false;
            for (std::size_t k = 2; k < clause.literals.size(); ++k) {
                if (litValue(clause.literals[k]) != VarAssign::False) {
                    std::swap(clause.literals[1], clause.literals[k]);
                    watches_[clause.literals[1].index()].push_back(ci);
                    found = true;
                    break;
                }
            }

            if (found) {
                // Removed from this watch list (don't copy to j)
                ++i;
            } else {
                // No replacement found
                watch_list[j++] = watch_list[i++];
                if (litValue(clause.literals[0]) == VarAssign::False) {
                    // Conflict: all literals false
                    while (i < watch_list.size()) watch_list[j++] = watch_list[i++];
                    watch_list.resize(j);
                    return ci;
                } else {
                    // Unit propagation: clause.literals[0] must be true
                    assign(clause.literals[0], ci);
                }
            }
        }
        watch_list.resize(j);
    }
    return -1;  // No conflict
}

std::int32_t CDCLSolver::pickBranchingVar() const {
    // VSIDS: pick the unassigned variable with highest activity
    std::int32_t best = 0;
    double best_act = -1.0;
    for (std::int32_t v = 1; v <= num_vars_; ++v) {
        if (assigns_[v] == VarAssign::Unassigned) {
            if (vsids_activity_[v] > best_act) {
                best_act = vsids_activity_[v];
                best = v;
            }
        }
    }
    return best;
}

void CDCLSolver::bumpVarActivity(std::int32_t var) {
    vsids_activity_[var] += vsids_bump_;
    // Rescale if too large
    if (vsids_activity_[var] > 1e100) {
        for (auto& a : vsids_activity_) a *= 1e-100;
        vsids_bump_ *= 1e-100;
    }
}

void CDCLSolver::decayActivities() {
    vsids_bump_ /= vsids_decay_;
}

Clause CDCLSolver::analyzeConflict(std::int32_t conflict_clause) {
    // 1-UIP: resolve backward from conflict until exactly one literal
    // from the current decision level remains in the learned clause.
    Clause learned;
    std::vector<bool> seen(num_vars_ + 1, false);
    std::int32_t counter = 0;  // number of current-level literals to resolve
    Literal p{0, false};
    std::int32_t reason = conflict_clause;

    std::int32_t trail_idx = static_cast<std::int32_t>(trail_.size()) - 1;

    do {
        // Resolve on all literals in the reason clause
        Clause& reason_clause = clauses_[reason];
        for (const Literal& lit : reason_clause.literals) {
            if (lit == p) continue;  // skip the resolved literal
            if (seen[lit.var]) continue;
            seen[lit.var] = true;
            bumpVarActivity(lit.var);

            // Check the decision level of this variable
            std::int32_t var_level = 0;
            for (const auto& te : trail_) {
                if (te.lit.var == lit.var) { var_level = te.decision_level; break; }
            }

            if (var_level == decision_level_) {
                counter++;
            } else if (var_level > 0) {
                learned.literals.push_back(~lit);
            }
        }

        // Find the next literal on the trail that was seen
        while (trail_idx >= 0 && !seen[trail_[trail_idx].lit.var]) {
            trail_idx--;
        }
        if (trail_idx < 0) break;

        p = trail_[trail_idx].lit;
        reason = trail_[trail_idx].reason_clause;
        seen[p.var] = false;
        counter--;
        trail_idx--;
    } while (counter > 0);

    // The 1-UIP literal goes first
    learned.literals.insert(learned.literals.begin(), ~p);
    learned.is_learned = true;
    return learned;
}

void CDCLSolver::backtrack(std::int32_t level) {
    while (trail_.size() > 0) {
        if (trail_.back().decision_level <= level) break;
        assigns_[trail_.back().lit.var] = VarAssign::Unassigned;
        trail_.pop_back();
    }
    while (trail_lim_.size() > static_cast<std::size_t>(level)) {
        trail_lim_.pop_back();
    }
    decision_level_ = level;
    propagation_head_ = std::min(propagation_head_, trail_.size());
}

SatResult CDCLSolver::solve(std::uint32_t conflict_limit) {
    conflicts_until_restart_ = 100;
    luby_index_ = 0;

    while (true) {
        std::int32_t conflict = propagate();

        if (conflict != -1) {
            // Conflict
            num_conflicts_++;
            if (num_conflicts_ > conflict_limit) return SatResult::UNKNOWN;

            if (decision_level_ == 0) return SatResult::UNSAT;

            Clause learned = analyzeConflict(conflict);
            decayActivities();

            // Determine backtrack level: second highest decision level in learned clause
            std::int32_t bt_level = 0;
            for (std::size_t i = 1; i < learned.literals.size(); ++i) {
                for (const auto& te : trail_) {
                    if (te.lit.var == learned.literals[i].var) {
                        bt_level = std::max(bt_level, te.decision_level);
                        break;
                    }
                }
            }

            backtrack(bt_level);
            std::int32_t clause_idx = addClause(std::move(learned.literals));
            clauses_[clause_idx].is_learned = true;

            // Luby restart check
            if (num_conflicts_ >= conflicts_until_restart_) {
                num_restarts_++;
                luby_index_++;
                conflicts_until_restart_ = num_conflicts_ + 100 * lubySequence(luby_index_);
                backtrack(0);
            }
        } else {
            // No conflict: make a decision
            std::int32_t var = pickBranchingVar();
            if (var == 0) return SatResult::SAT;  // All variables assigned

            decision_level_++;
            trail_lim_.push_back(trail_.size());
            num_decisions_++;
            assign(Literal{var, false}, -1);  // decide positive
        }
    }
}

std::vector<bool> CDCLSolver::model() const {
    std::vector<bool> m(num_vars_ + 1, false);
    for (std::int32_t v = 1; v <= num_vars_; ++v) {
        m[v] = (assigns_[v] == VarAssign::True);
    }
    return m;
}

void CDCLSolver::reset() {
    assigns_.assign(num_vars_ + 1, VarAssign::Unassigned);
    trail_.clear();
    trail_lim_.clear();
    propagation_head_ = 0;
    decision_level_ = 0;
    num_conflicts_ = 0;
    num_decisions_ = 0;
    num_restarts_ = 0;
}

// PLACEHOLDER_LIA_IMPL

// ---------------------------------------------------------------------------
// LIA Theory Solver (Simplex with Bound Propagation)
// ---------------------------------------------------------------------------

LIATheorySolver::LIATheorySolver() {}

std::int32_t LIATheorySolver::newTheoryVar() {
    ++num_theory_vars_;
    lower_bounds_.push_back(std::numeric_limits<std::int64_t>::min() / 2);
    upper_bounds_.push_back(std::numeric_limits<std::int64_t>::max() / 2);
    current_values_.push_back(0);
    return num_theory_vars_;
}

void LIATheorySolver::addConstraint(LinearConstraint constraint) {
    constraints_.push_back(std::move(constraint));
}

bool LIATheorySolver::check(const std::vector<bool>& active_bools) {
    last_conflict_.clear();

    // Collect indices of active constraints
    std::vector<std::size_t> active_indices;
    for (std::size_t i = 0; i < constraints_.size(); ++i) {
        std::int32_t bvar = constraints_[i].boolean_var;
        if (bvar > 0 && static_cast<std::size_t>(bvar) < active_bools.size()
            && active_bools[bvar]) {
            active_indices.push_back(i);
        }
    }

    if (active_indices.empty()) return true;

    // Try bound propagation first (faster than full Simplex)
    if (!propagateBounds(active_indices)) return false;

    // If bound propagation finds no conflict, run Simplex for full check
    return runSimplex(active_indices);
}

bool LIATheorySolver::propagateBounds(const std::vector<std::size_t>& active_indices) {
    // Reset bounds
    std::vector<std::int64_t> lb(num_theory_vars_ + 1, std::numeric_limits<std::int64_t>::min() / 2);
    std::vector<std::int64_t> ub(num_theory_vars_ + 1, std::numeric_limits<std::int64_t>::max() / 2);

    // Propagate each active constraint to tighten bounds
    bool changed = true;
    int iterations = 0;
    while (changed && iterations < 100) {
        changed = false;
        iterations++;

        for (std::size_t ci : active_indices) {
            const auto& c = constraints_[ci];
            if (c.terms.size() == 1) {
                // Single variable: direct bound tightening
                auto [var, coeff] = c.terms[0];
                if (coeff == 0) continue;

                std::int64_t new_bound = c.bound / coeff;
                if (coeff > 0) {
                    switch (c.op) {
                        case LinearConstraint::Op::LEQ:
                            if (new_bound < ub[var]) { ub[var] = new_bound; changed = true; }
                            break;
                        case LinearConstraint::Op::GEQ:
                            if (new_bound > lb[var]) { lb[var] = new_bound; changed = true; }
                            break;
                        case LinearConstraint::Op::EQ:
                            if (new_bound > lb[var]) { lb[var] = new_bound; changed = true; }
                            if (new_bound < ub[var]) { ub[var] = new_bound; changed = true; }
                            break;
                    }
                } else {
                    // Negative coefficient flips the inequality
                    switch (c.op) {
                        case LinearConstraint::Op::LEQ:
                            if (new_bound > lb[var]) { lb[var] = new_bound; changed = true; }
                            break;
                        case LinearConstraint::Op::GEQ:
                            if (new_bound < ub[var]) { ub[var] = new_bound; changed = true; }
                            break;
                        case LinearConstraint::Op::EQ:
                            if (new_bound > lb[var]) { lb[var] = new_bound; changed = true; }
                            if (new_bound < ub[var]) { ub[var] = new_bound; changed = true; }
                            break;
                    }
                }
            }

            // Multi-variable: compute implied bounds from other variables
            if (c.terms.size() > 1) {
                for (std::size_t t = 0; t < c.terms.size(); ++t) {
                    auto [var, coeff] = c.terms[t];
                    if (coeff == 0) continue;

                    // sum of (coeff_j * var_j) for j != t
                    std::int64_t rest_min = 0, rest_max = 0;
                    bool computable = true;
                    for (std::size_t j = 0; j < c.terms.size(); ++j) {
                        if (j == t) continue;
                        auto [vj, cj] = c.terms[j];
                        if (cj > 0) { rest_min += cj * lb[vj]; rest_max += cj * ub[vj]; }
                        else { rest_min += cj * ub[vj]; rest_max += cj * lb[vj]; }
                        // Check for overflow sentinel
                        if (lb[vj] < std::numeric_limits<std::int64_t>::min() / 4 ||
                            ub[vj] > std::numeric_limits<std::int64_t>::max() / 4) {
                            computable = false;
                            break;
                        }
                    }
                    if (!computable) continue;

                    if (c.op == LinearConstraint::Op::LEQ && coeff > 0) {
                        std::int64_t implied_ub = (c.bound - rest_min) / coeff;
                        if (implied_ub < ub[var]) { ub[var] = implied_ub; changed = true; }
                    } else if (c.op == LinearConstraint::Op::GEQ && coeff > 0) {
                        std::int64_t implied_lb = (c.bound - rest_max) / coeff;
                        if (implied_lb > lb[var]) { lb[var] = implied_lb; changed = true; }
                    }
                }
            }
        }

        // Check for bound conflicts
        for (std::int32_t v = 1; v <= num_theory_vars_; ++v) {
            if (lb[v] > ub[v]) {
                // Conflict: build conflict clause from active constraints
                for (std::size_t ci : active_indices) {
                    last_conflict_.push_back(
                        Literal{constraints_[ci].boolean_var, true});  // negate
                }
                return false;
            }
        }
    }

    // Store computed values
    for (std::int32_t v = 1; v <= num_theory_vars_; ++v) {
        current_values_[v - 1] = (lb[v] != std::numeric_limits<std::int64_t>::min() / 2)
                                 ? lb[v] : 0;
    }
    return true;
}

bool LIATheorySolver::runSimplex(const std::vector<std::size_t>& active_indices) {
    // Simplified Simplex: check if all active constraints can be simultaneously satisfied.
    // For each constraint, verify current_values_ satisfy it; if not, try to adjust.
    for (std::size_t ci : active_indices) {
        const auto& c = constraints_[ci];
        std::int64_t lhs = 0;
        for (const auto& [var, coeff] : c.terms) {
            if (var >= 1 && static_cast<std::size_t>(var - 1) < current_values_.size()) {
                lhs += coeff * current_values_[var - 1];
            }
        }

        bool satisfied = false;
        switch (c.op) {
            case LinearConstraint::Op::LEQ: satisfied = (lhs <= c.bound); break;
            case LinearConstraint::Op::GEQ: satisfied = (lhs >= c.bound); break;
            case LinearConstraint::Op::EQ:  satisfied = (lhs == c.bound); break;
        }

        if (!satisfied) {
            // Try pivoting: adjust variables to satisfy
            // Simple heuristic: adjust first variable with non-zero coefficient
            for (const auto& [var, coeff] : c.terms) {
                if (coeff == 0) continue;
                std::int64_t deficit = c.bound - lhs;
                std::int64_t adjustment = deficit / coeff;
                if (var >= 1 && static_cast<std::size_t>(var - 1) < current_values_.size()) {
                    current_values_[var - 1] += adjustment;
                    lhs += coeff * adjustment;
                }
                break;
            }

            // Re-check after adjustment
            switch (c.op) {
                case LinearConstraint::Op::LEQ: satisfied = (lhs <= c.bound); break;
                case LinearConstraint::Op::GEQ: satisfied = (lhs >= c.bound); break;
                case LinearConstraint::Op::EQ:  satisfied = (lhs == c.bound); break;
            }

            if (!satisfied) {
                // Build conflict clause
                for (std::size_t idx : active_indices) {
                    last_conflict_.push_back(
                        Literal{constraints_[idx].boolean_var, true});
                }
                return false;
            }
        }
    }
    return true;
}

std::vector<Literal> LIATheorySolver::conflictClause() const {
    return last_conflict_;
}

std::vector<std::int64_t> LIATheorySolver::model() const {
    return current_values_;
}

// PLACEHOLDER_SMT_SOLVER_IMPL

// ---------------------------------------------------------------------------
// SMT Solver — DPLL(T) combining CDCL + LIA
// ---------------------------------------------------------------------------

SMTSolver::SMTSolver() {}

std::int32_t SMTSolver::newBoolVar() {
    return sat_.newVar();
}

std::int32_t SMTSolver::newIntVar() {
    return lia_.newTheoryVar();
}

void SMTSolver::addClause(std::vector<Literal> clause) {
    sat_.addClause(std::move(clause));
}

void SMTSolver::addTheoryConstraint(LinearConstraint constraint) {
    lia_.addConstraint(constraint);
    theory_constraints_.push_back(std::move(constraint));
}

SatResult SMTSolver::solve(std::uint32_t conflict_limit) {
    return dpllT(conflict_limit);
}

SatResult SMTSolver::dpllT(std::uint32_t conflict_limit) {
    // DPLL(T) loop:
    // 1. SAT solver finds a propositional model
    // 2. Theory solver checks consistency
    // 3. If theory conflict, add blocking clause and restart SAT
    std::uint32_t theory_iterations = 0;
    const std::uint32_t max_theory_iterations = 1000;

    while (theory_iterations < max_theory_iterations) {
        SatResult sat_result = sat_.solve(conflict_limit);

        if (sat_result == SatResult::UNSAT) return SatResult::UNSAT;
        if (sat_result == SatResult::UNKNOWN) return SatResult::UNKNOWN;

        // SAT found a model: check theory consistency
        auto bool_model = sat_.model();
        if (lia_.check(bool_model)) {
            // Theory is consistent: genuine SAT
            return SatResult::SAT;
        }

        // Theory conflict: add blocking clause (negation of conflict)
        auto conflict = lia_.conflictClause();
        if (conflict.empty()) {
            // Cannot refine: give up
            return SatResult::UNKNOWN;
        }
        sat_.addClause(conflict);
        sat_.reset();
        theory_iterations++;
    }

    return SatResult::UNKNOWN;
}

std::vector<bool> SMTSolver::boolModel() const {
    return sat_.model();
}

std::vector<std::int64_t> SMTSolver::intModel() const {
    return lia_.model();
}

// ---------------------------------------------------------------------------
// SMT Model Checker — Bounded Model Checking via SMT encoding
// ---------------------------------------------------------------------------

SMTModelChecker::SMTModelChecker(Config config) : config_(std::move(config)) {}

SMTModelChecker::StepEncoding SMTModelChecker::createStepEncoding(
    SMTSolver& solver, const KripkeModel& model, std::int32_t step) {

    StepEncoding enc;
    enc.step_index = step;
    for (const auto& [state_id, state] : model.states) {
        enc.state_vars[state_id] = solver.newBoolVar();
    }
    return enc;
}

void SMTModelChecker::encodeInitial(SMTSolver& solver, const StepEncoding& step0,
                                     const KripkeModel& model) {
    // The initial state must be active at step 0
    auto it = step0.state_vars.find(model.initial_state);
    if (it != step0.state_vars.end()) {
        solver.addClause({Literal{it->second, false}});
    }
    // All other states must be inactive
    for (const auto& [sid, var] : step0.state_vars) {
        if (sid != model.initial_state) {
            solver.addClause({Literal{var, true}});  // negated = must be false
        }
    }
}

void SMTModelChecker::encodeTransition(SMTSolver& solver,
                                        const StepEncoding& from,
                                        const StepEncoding& to,
                                        const KripkeModel& model) {
    // Transition relation: if state s is active at step i, then exactly one
    // successor of s must be active at step i+1.
    // Encoded as: from_s => (to_s1 OR to_s2 OR ... to_sn) for each state s.
    for (const auto& [sid, state] : model.states) {
        auto from_it = from.state_vars.find(sid);
        if (from_it == from.state_vars.end()) continue;
        std::int32_t from_var = from_it->second;

        if (state.successors.empty()) {
            // Terminal state: self-loop (stays in same state)
            auto to_it = to.state_vars.find(sid);
            if (to_it != to.state_vars.end()) {
                // from_s => to_s  ≡  !from_s OR to_s
                solver.addClause({Literal{from_var, true}, Literal{to_it->second, false}});
            }
        } else {
            // from_s => (to_succ1 OR to_succ2 OR ...)
            std::vector<Literal> clause;
            clause.push_back(Literal{from_var, true});  // !from_s
            for (const auto& succ : state.successors) {
                auto to_it = to.state_vars.find(succ);
                if (to_it != to.state_vars.end()) {
                    clause.push_back(Literal{to_it->second, false});
                }
            }
            if (clause.size() > 1) {
                solver.addClause(std::move(clause));
            }
        }
    }

    // At-least-one and at-most-one for the target step
    std::vector<std::int32_t> to_vars;
    for (const auto& [sid, var] : to.state_vars) {
        to_vars.push_back(var);
    }
    encodeExactlyOne(solver, to_vars);
}

void SMTModelChecker::encodePropertyNegation(SMTSolver& solver,
                                              const StepEncoding& step,
                                              const KripkeModel& model,
                                              const Formula& property) {
    // Encode that the property is violated at this step.
    // For AG(phi): violation means some state where !phi is active.
    // For AF(phi): violation means a path where phi never holds (all steps !phi).

    switch (property.op) {
        case TemporalOp::AG: {
            // AG(phi) violated if any reachable state has !phi.
            // Encode: the active state at this step must NOT satisfy phi.
            const auto& sub = *property.children[0];
            if (sub.op == TemporalOp::Atom && sub.atom) {
                // Find states that DO NOT have this proposition
                for (const auto& [sid, state] : model.states) {
                    if (!state.labels.has(sub.atom->name)) {
                        // This state violates phi; if it's active, property is violated
                        auto it = step.state_vars.find(sid);
                        if (it != step.state_vars.end()) {
                            // We want at least one bad state to be active (disjunction
                            // handled by the overall BMC structure)
                            // This is part of the target: OR of (bad state active)
                        }
                    }
                }
                // Add a clause saying "at least one violating state is active"
                std::vector<Literal> target_clause;
                for (const auto& [sid, state] : model.states) {
                    if (sub.op == TemporalOp::Not) {
                        // AG(!bad) violated by: bad state active
                        if (sub.children[0]->op == TemporalOp::Atom && sub.children[0]->atom) {
                            if (state.labels.has(sub.children[0]->atom->name)) {
                                auto it = step.state_vars.find(sid);
                                if (it != step.state_vars.end()) {
                                    target_clause.push_back(Literal{it->second, false});
                                }
                            }
                        }
                    } else {
                        if (!state.labels.has(sub.atom->name)) {
                            auto it = step.state_vars.find(sid);
                            if (it != step.state_vars.end()) {
                                target_clause.push_back(Literal{it->second, false});
                            }
                        }
                    }
                }
                if (!target_clause.empty()) {
                    solver.addClause(std::move(target_clause));
                }
            } else if (sub.op == TemporalOp::Not && sub.children[0]->op == TemporalOp::Atom) {
                // AG(!atom) violated means atom holds somewhere
                const auto& neg_atom = sub.children[0]->atom;
                if (neg_atom) {
                    std::vector<Literal> target_clause;
                    for (const auto& [sid, state] : model.states) {
                        if (state.labels.has(neg_atom->name)) {
                            auto it = step.state_vars.find(sid);
                            if (it != step.state_vars.end()) {
                                target_clause.push_back(Literal{it->second, false});
                            }
                        }
                    }
                    if (!target_clause.empty()) {
                        solver.addClause(std::move(target_clause));
                    }
                }
            }
            break;
        }
        case TemporalOp::AF: {
            // AF(phi) violated if there exists a path where phi never holds.
            // At every step in the unrolling, phi must NOT hold.
            // For this step: active state must not satisfy phi.
            const auto& sub = *property.children[0];
            if (sub.op == TemporalOp::Atom && sub.atom) {
                // States satisfying phi must not be active at this step
                for (const auto& [sid, state] : model.states) {
                    if (state.labels.has(sub.atom->name)) {
                        auto it = step.state_vars.find(sid);
                        if (it != step.state_vars.end()) {
                            // Must NOT be active: force false
                            solver.addClause({Literal{it->second, true}});
                        }
                    }
                }
            }
            break;
        }
        default:
            break;
    }
}

void SMTModelChecker::encodeExactlyOne(SMTSolver& solver,
                                        const std::vector<std::int32_t>& vars) {
    if (vars.empty()) return;

    // At-least-one: disjunction of all vars
    std::vector<Literal> alo;
    for (auto v : vars) {
        alo.push_back(Literal{v, false});
    }
    solver.addClause(std::move(alo));

    // At-most-one: pairwise mutual exclusion (quadratic but fine for our sizes)
    // For larger sets, use logarithmic encoding; for DAGs up to 500 nodes
    // this gives at most 500*499/2 binary clauses per step which is manageable.
    if (vars.size() <= 500) {
        for (std::size_t i = 0; i < vars.size(); ++i) {
            for (std::size_t j = i + 1; j < vars.size(); ++j) {
                solver.addClause({Literal{vars[i], true}, Literal{vars[j], true}});
            }
        }
    } else {
        // Commander encoding for very large state spaces (>500 states)
        // Group vars into blocks of sqrt(n), each block gets a commander var
        std::size_t block_size = static_cast<std::size_t>(std::sqrt(vars.size())) + 1;
        std::vector<std::int32_t> commanders;
        for (std::size_t base = 0; base < vars.size(); base += block_size) {
            std::int32_t cmd = solver.newBoolVar();
            commanders.push_back(cmd);
            std::size_t end = std::min(base + block_size, vars.size());
            // cmd => at-most-one in block
            for (std::size_t i = base; i < end; ++i) {
                for (std::size_t j = i + 1; j < end; ++j) {
                    solver.addClause({Literal{vars[i], true}, Literal{vars[j], true}});
                }
                // var_i => cmd
                solver.addClause({Literal{vars[i], true}, Literal{cmd, false}});
            }
        }
        // At most one commander
        for (std::size_t i = 0; i < commanders.size(); ++i) {
            for (std::size_t j = i + 1; j < commanders.size(); ++j) {
                solver.addClause({Literal{commanders[i], true}, Literal{commanders[j], true}});
            }
        }
    }
}

SMTModelChecker::Result SMTModelChecker::check(const KripkeModel& model,
                                                FormulaPtr property) {
    auto start = std::chrono::steady_clock::now();
    Result result;

    // Incremental BMC: unroll from depth 1 to max, checking at each depth
    for (std::uint32_t k = 1; k <= config_.max_unroll_depth; ++k) {
        SMTSolver solver;
        std::vector<StepEncoding> steps;

        // Create step encodings
        for (std::uint32_t i = 0; i <= k; ++i) {
            steps.push_back(createStepEncoding(solver, model, static_cast<std::int32_t>(i)));
        }

        // Encode initial state
        encodeInitial(solver, steps[0], model);

        // Encode transition relations
        for (std::uint32_t i = 0; i < k; ++i) {
            encodeTransition(solver, steps[i], steps[i + 1], model);
        }

        // Encode property negation (the BMC target)
        // For AG: encode violation at any step (OR over steps)
        if (property->op == TemporalOp::AG) {
            // AG violated if at any step the sub-property doesn't hold.
            // Create a fresh "target" variable for each step, then assert their disjunction.
            std::vector<std::int32_t> step_targets;
            for (std::uint32_t i = 0; i <= k; ++i) {
                std::int32_t tgt = solver.newBoolVar();
                step_targets.push_back(tgt);
                // Encode: tgt => property violated at step i
                // (simplified: use the encoding at the last step for now)
            }
            encodePropertyNegation(solver, steps[k], model, *property);
        } else if (property->op == TemporalOp::AF) {
            // AF violated if on ALL steps phi doesn't hold
            for (std::uint32_t i = 0; i <= k; ++i) {
                encodePropertyNegation(solver, steps[i], model, *property);
            }
        } else {
            // Generic: check at the last step
            encodePropertyNegation(solver, steps[k], model, *property);
        }

        // Solve
        SatResult sat_result = solver.solve(config_.conflict_limit);
        result.depth_reached = k;

        if (sat_result == SatResult::SAT) {
            // Counterexample found: property violated
            result.status = SatResult::SAT;
            result.counterexample_states = extractCounterexample(solver, steps, model);
            break;
        } else if (sat_result == SatResult::UNSAT) {
            // No counterexample at this depth — continue to next depth
            // If we've reached max depth, property holds within bound
            if (k == config_.max_unroll_depth) {
                result.status = SatResult::UNSAT;
            }
        } else {
            // Unknown (timeout) — stop
            result.status = SatResult::UNKNOWN;
            break;
        }
    }

    auto elapsed = std::chrono::steady_clock::now() - start;
    result.solve_time = std::chrono::duration_cast<std::chrono::microseconds>(elapsed);
    return result;
}

std::vector<std::string> SMTModelChecker::extractCounterexample(
    const SMTSolver& solver,
    const std::vector<StepEncoding>& steps,
    const KripkeModel& model) {

    std::vector<std::string> path;
    auto bool_model = solver.boolModel();

    for (const auto& step : steps) {
        for (const auto& [sid, var] : step.state_vars) {
            if (var > 0 && static_cast<std::size_t>(var) < bool_model.size()
                && bool_model[var]) {
                path.push_back(sid);
                break;
            }
        }
    }
    return path;
}

// PLACEHOLDER_FAIRNESS_IMPL

// ===========================================================================
// SECTION 3: Fairness Constraints for Liveness — Streett Acceptance + Nested DFS
// ===========================================================================

FairLivenessChecker::FairLivenessChecker(FairnessConfig config)
    : config_(std::move(config)) {}

std::set<std::string> FairLivenessChecker::computeAcceptingStates(
    const KripkeModel& model, const Formula& phi) {
    // States where phi holds — these are the accepting states for AF(phi)
    std::set<std::string> accepting;
    for (const auto& [sid, state] : model.states) {
        // Evaluate phi at this state
        if (phi.op == TemporalOp::Atom && phi.atom) {
            if (state.labels.has(phi.atom->name)) {
                accepting.insert(sid);
            }
        } else if (phi.op == TemporalOp::Or) {
            // Recursively check sub-formulas
            bool holds = false;
            for (const auto& child : phi.children) {
                if (child->op == TemporalOp::Atom && child->atom) {
                    if (state.labels.has(child->atom->name)) {
                        holds = true;
                        break;
                    }
                }
            }
            if (holds) accepting.insert(sid);
        } else if (phi.op == TemporalOp::Not && phi.children[0]->op == TemporalOp::Atom) {
            if (phi.children[0]->atom && !state.labels.has(phi.children[0]->atom->name)) {
                accepting.insert(sid);
            }
        }
    }
    return accepting;
}

KripkeModel FairLivenessChecker::removeTrivialSelfLoops(const KripkeModel& model) const {
    KripkeModel filtered = model;
    for (auto& [sid, state] : filtered.states) {
        if (state.successors.size() > 1) {
            // Remove self-loop only if there are other successors
            state.successors.erase(
                std::remove(state.successors.begin(), state.successors.end(), sid),
                state.successors.end());
        }
    }
    return filtered;
}

FairLivenessResult FairLivenessChecker::checkAF(const KripkeModel& model,
                                                  const Formula& phi) {
    FairLivenessResult result;

    // Optionally remove trivial self-loops that cause false liveness violations
    KripkeModel working_model = config_.eliminate_self_loops
        ? removeTrivialSelfLoops(model) : model;

    // Compute accepting states (where phi holds)
    std::set<std::string> accepting = computeAcceptingStates(working_model, phi);

    // If initial state is accepting, trivially satisfied
    if (accepting.count(working_model.initial_state)) {
        result.property_holds = true;
        result.explanation = "Initial state satisfies the property directly";
        return result;
    }

    // If all states are accepting, trivially satisfied
    if (accepting.size() == working_model.states.size()) {
        result.property_holds = true;
        result.explanation = "All states satisfy the property";
        return result;
    }

    // Nested DFS for fair cycle detection:
    // AF(phi) is violated iff there exists a FAIR cycle that never visits
    // an accepting state. We check for such cycles.
    //
    // Algorithm:
    // 1. Outer DFS from initial state
    // 2. When we reach a non-accepting state, start inner DFS
    // 3. Inner DFS looks for a cycle back to the outer DFS stack
    // 4. If cycle found, check if it's fair (Streett acceptance)
    // 5. If fair cycle found that avoids accepting states, AF is violated

    std::set<std::string> visited_outer;
    std::vector<std::string> path;
    bool unfair_cycle_exists = outerDFS(working_model, working_model.initial_state,
                                         accepting, visited_outer, path);

    if (!unfair_cycle_exists) {
        // No fair cycle avoiding accepting states: AF holds under fairness
        result.property_holds = true;
        result.explanation = "No fair cycle avoiding accepting states found";
    } else {
        // Check if the cycle we found is actually fair
        if (!path.empty()) {
            bool is_fair = isFairCycle(path, working_model);
            if (is_fair) {
                result.property_holds = false;
                result.unfair_cycle_found = false;
                result.cycle_states = path;
                result.explanation = "Fair cycle found that never reaches accepting state";
            } else {
                // The cycle is unfair — this is a false positive in naive AF checking
                result.property_holds = true;
                result.unfair_cycle_found = true;
                result.cycle_states = path;
                result.explanation =
                    "Cycle found but it is UNFAIR (violates Streett acceptance). "
                    "AF holds under fairness constraints.";
            }
        } else {
            result.property_holds = true;
            result.explanation = "No violating path found";
        }
    }

    return result;
}

bool FairLivenessChecker::outerDFS(const KripkeModel& model,
                                    const std::string& state_id,
                                    const std::set<std::string>& accepting,
                                    std::set<std::string>& visited_outer,
                                    std::vector<std::string>& path) {
    if (visited_outer.count(state_id)) return false;
    visited_outer.insert(state_id);
    path.push_back(state_id);

    auto state_it = model.states.find(state_id);
    if (state_it == model.states.end()) {
        path.pop_back();
        return false;
    }

    // If this is a non-accepting state, try inner DFS for a cycle
    if (!accepting.count(state_id)) {
        std::set<std::string> visited_inner;
        std::vector<std::string> cycle_path;
        if (innerDFS(model, state_id, state_id, accepting, visited_inner, cycle_path)) {
            // Found a cycle through this non-accepting state
            path.insert(path.end(), cycle_path.begin(), cycle_path.end());
            return true;
        }
    }

    // Continue outer DFS on successors
    for (const auto& succ : state_it->second.successors) {
        if (outerDFS(model, succ, accepting, visited_outer, path)) {
            return true;
        }
    }

    path.pop_back();
    return false;
}

bool FairLivenessChecker::innerDFS(const KripkeModel& model,
                                    const std::string& target,
                                    const std::string& state_id,
                                    const std::set<std::string>& accepting,
                                    std::set<std::string>& visited_inner,
                                    std::vector<std::string>& cycle_path) {
    auto state_it = model.states.find(state_id);
    if (state_it == model.states.end()) return false;

    for (const auto& succ : state_it->second.successors) {
        if (succ == target) {
            // Found cycle back to target
            cycle_path.push_back(succ);
            return true;
        }

        if (visited_inner.count(succ)) continue;
        if (accepting.count(succ)) continue;  // Skip accepting states (we want non-accepting cycles)

        visited_inner.insert(succ);
        cycle_path.push_back(succ);

        if (innerDFS(model, target, succ, accepting, visited_inner, cycle_path)) {
            return true;
        }
        cycle_path.pop_back();
    }
    return false;
}

bool FairLivenessChecker::isFairCycle(const std::vector<std::string>& cycle,
                                       const KripkeModel& model) const {
    return checkStreettAcceptance(cycle, model);
}

bool FairLivenessChecker::checkStreettAcceptance(const std::vector<std::string>& cycle,
                                                  const KripkeModel& model) const {
    // Streett acceptance: for each fairness pair (P_i, Q_i),
    // if the cycle visits a P_i state, it must also visit a Q_i state.
    // A cycle is FAIR iff ALL pairs are satisfied.

    for (const auto& pair : config_.fairness_pairs) {
        bool visits_P = false;
        bool visits_Q = false;

        for (const auto& sid : cycle) {
            if (pair.P_states.count(sid)) visits_P = true;
            if (pair.Q_states.count(sid)) visits_Q = true;
        }

        // Streett condition: inf(P) => inf(Q)
        // For a finite cycle representation: if visits P, must visit Q
        if (visits_P && !visits_Q) {
            return false;  // Unfair: P visited but Q never visited in cycle
        }
    }
    return true;  // All pairs satisfied: fair cycle
}

std::vector<FairnessPair> FairLivenessChecker::autoGeneratePairs(const KripkeModel& model) {
    std::vector<FairnessPair> pairs;

    // Auto-detect request/grant patterns:
    // For each node: executing → completed (weak fairness: if started, must complete)
    for (const auto& node : model.nodes) {
        FairnessPair pair;
        pair.name = "fairness_" + node.id;

        // P states: where the node is executing (request/enabled)
        for (const auto& [sid, state] : model.states) {
            if (state.labels.has("node." + node.id + ".executing")) {
                pair.P_states.insert(sid);
            }
        }

        // Q states: where the node is completed (grant/fired)
        for (const auto& [sid, state] : model.states) {
            if (state.labels.has("node." + node.id + ".completed")) {
                pair.Q_states.insert(sid);
            }
        }

        if (!pair.P_states.empty()) {
            pairs.push_back(std::move(pair));
        }
    }

    // Additional fairness pair: failure self-loops are unfair
    // (a node stuck in failed-retry loop without progress)
    for (const auto& node : model.nodes) {
        if (node.is_idempotent) {
            FairnessPair retry_pair;
            retry_pair.name = "retry_fairness_" + node.id;
            for (const auto& [sid, state] : model.states) {
                if (state.labels.has("node." + node.id + ".failed")) {
                    retry_pair.P_states.insert(sid);
                }
                if (state.labels.has("node." + node.id + ".completed")) {
                    retry_pair.Q_states.insert(sid);
                }
            }
            if (!retry_pair.P_states.empty()) {
                pairs.push_back(std::move(retry_pair));
            }
        }
    }

    return pairs;
}

// PLACEHOLDER_PCTL_IMPL

// ===========================================================================
// SECTION 4: Probabilistic Verification with PCTL — MDP + Value Iteration
// ===========================================================================

// ---------------------------------------------------------------------------
// PCTLFormula factory methods
// ---------------------------------------------------------------------------

PCTLFormulaPtr PCTLFormula::makeAtom(const std::string& name,
                                      const std::string& node_id) {
    auto f = std::make_shared<PCTLFormula>();
    f->op = Op::Atom;
    f->atom = Atom{name, node_id, ""};
    return f;
}

PCTLFormulaPtr PCTLFormula::makeNot(PCTLFormulaPtr sub) {
    auto f = std::make_shared<PCTLFormula>();
    f->op = Op::Not;
    f->children.push_back(std::move(sub));
    return f;
}

PCTLFormulaPtr PCTLFormula::makeAnd(PCTLFormulaPtr a, PCTLFormulaPtr b) {
    auto f = std::make_shared<PCTLFormula>();
    f->op = Op::And;
    f->children.push_back(std::move(a));
    f->children.push_back(std::move(b));
    return f;
}

PCTLFormulaPtr PCTLFormula::makeProb(ProbCompare cmp, double bound,
                                      FormulaPtr path_formula) {
    auto f = std::make_shared<PCTLFormula>();
    f->op = Op::ProbOp;
    f->compare = cmp;
    f->probability_bound = bound;
    f->path_formula = std::move(path_formula);
    return f;
}

// ---------------------------------------------------------------------------
// MDPAction validation
// ---------------------------------------------------------------------------

bool MDPAction::valid(double eps) const {
    double sum = 0.0;
    for (const auto& [_, p] : distribution) {
        sum += p;
    }
    return std::abs(sum - 1.0) < eps;
}

// ---------------------------------------------------------------------------
// MDPModel construction from Kripke model
// ---------------------------------------------------------------------------

MDPModel MDPModel::fromKripke(const KripkeModel& kripke,
                               const std::map<std::string, double>& transition_probs) {
    MDPModel mdp;
    mdp.initial_state = kripke.initial_state;
    mdp.kripke_ref = &kripke;

    for (const auto& [sid, state] : kripke.states) {
        mdp.all_states.insert(sid);

        if (state.successors.empty()) {
            // Terminal state: self-loop with probability 1
            MDPAction action;
            action.name = "self_loop";
            action.distribution.emplace_back(sid, 1.0);
            mdp.states[sid].push_back(std::move(action));
            continue;
        }

        // Build probability distribution over successors.
        // If explicit probabilities are provided, use them.
        // Otherwise, use uniform distribution.
        MDPAction action;
        action.name = "transition_from_" + sid;

        double prob_sum = 0.0;
        std::vector<std::pair<std::string, double>> explicit_probs;
        std::vector<std::string> no_prob_succs;

        for (const auto& succ : state.successors) {
            std::string key = sid + "->" + succ;
            auto it = transition_probs.find(key);
            if (it != transition_probs.end()) {
                explicit_probs.emplace_back(succ, it->second);
                prob_sum += it->second;
            } else {
                no_prob_succs.push_back(succ);
            }
        }

        if (no_prob_succs.empty() && std::abs(prob_sum - 1.0) < 1e-9) {
            // All probabilities specified
            action.distribution = explicit_probs;
        } else if (!no_prob_succs.empty()) {
            // Distribute remaining probability uniformly
            double remaining = 1.0 - prob_sum;
            if (remaining < 0.0) remaining = 0.0;
            double each = no_prob_succs.empty() ? 0.0 : remaining / no_prob_succs.size();
            action.distribution = explicit_probs;
            for (const auto& succ : no_prob_succs) {
                action.distribution.emplace_back(succ, each);
            }
        } else {
            // Normalize explicit probs
            for (auto& [s, p] : explicit_probs) {
                p /= prob_sum;
            }
            action.distribution = explicit_probs;
        }

        mdp.states[sid].push_back(std::move(action));

        // For states with multiple successors of different types (e.g. success/failure),
        // also model nondeterministic choice: the scheduler can pick which action to take.
        // In our model, each successor represents a possible outcome, not a choice,
        // so we have one action per state with a probability distribution.
        // However, for retry-capable nodes, we add a separate "retry" action.
        bool has_retry = false;
        for (const auto& succ : state.successors) {
            auto succ_it = kripke.states.find(succ);
            if (succ_it != kripke.states.end()) {
                if (succ_it->second.labels.has("node.failed")) {
                    // Check if there is a retry path from the failure state
                    for (const auto& retry_succ : succ_it->second.successors) {
                        if (retry_succ.find("executing_") == 0) {
                            has_retry = true;
                            break;
                        }
                    }
                }
            }
        }

        if (has_retry) {
            // Add a nondeterministic "retry" action that goes through failure then retry
            MDPAction retry_action;
            retry_action.name = "retry_from_" + sid;
            for (const auto& succ : state.successors) {
                auto succ_it = kripke.states.find(succ);
                if (succ_it != kripke.states.end() && succ_it->second.labels.has("node.failed")) {
                    retry_action.distribution.emplace_back(succ, 1.0);
                    break;
                }
            }
            if (!retry_action.distribution.empty()) {
                mdp.states[sid].push_back(std::move(retry_action));
            }
        }
    }

    return mdp;
}

StateLabels MDPModel::labelsFor(const std::string& state_id) const {
    if (kripke_ref) {
        auto it = kripke_ref->states.find(state_id);
        if (it != kripke_ref->states.end()) return it->second.labels;
    }
    return {};
}

// ---------------------------------------------------------------------------
// PCTLModelChecker
// ---------------------------------------------------------------------------

PCTLModelChecker::PCTLModelChecker(PCTLConfig config)
    : config_(std::move(config)) {}

double PCTLResult::margin() const {
    return std::abs(computed_probability - lower_bound);
}

std::set<std::string> PCTLModelChecker::satisfyingStates(
    const MDPModel& mdp, const PCTLFormula& formula) {

    std::set<std::string> result;

    switch (formula.op) {
        case PCTLFormula::Op::Atom: {
            if (!formula.atom) return result;
            for (const auto& sid : mdp.all_states) {
                auto labels = mdp.labelsFor(sid);
                if (labels.has(formula.atom->name)) {
                    result.insert(sid);
                }
            }
            break;
        }
        case PCTLFormula::Op::Not: {
            auto sub_sat = satisfyingStates(mdp, *formula.children[0]);
            for (const auto& sid : mdp.all_states) {
                if (!sub_sat.count(sid)) result.insert(sid);
            }
            break;
        }
        case PCTLFormula::Op::And: {
            auto left = satisfyingStates(mdp, *formula.children[0]);
            auto right = satisfyingStates(mdp, *formula.children[1]);
            for (const auto& sid : left) {
                if (right.count(sid)) result.insert(sid);
            }
            break;
        }
        case PCTLFormula::Op::Or: {
            auto left = satisfyingStates(mdp, *formula.children[0]);
            auto right = satisfyingStates(mdp, *formula.children[1]);
            result = left;
            result.insert(right.begin(), right.end());
            break;
        }
        default:
            break;
    }
    return result;
}

std::set<std::string> PCTLModelChecker::computeProb0States(
    const MDPModel& mdp, const std::set<std::string>& target, bool maximize) {
    // Prob0: states from which the target is unreachable (probability 0).
    // Backward BFS from target states.
    std::set<std::string> can_reach;
    std::deque<std::string> queue(target.begin(), target.end());
    can_reach = target;

    // Build reverse graph
    std::map<std::string, std::set<std::string>> reverse_graph;
    for (const auto& [sid, actions] : mdp.states) {
        for (const auto& action : actions) {
            for (const auto& [to, prob] : action.distribution) {
                if (prob > 0.0) {
                    reverse_graph[to].insert(sid);
                }
            }
        }
    }

    while (!queue.empty()) {
        std::string s = queue.front();
        queue.pop_front();
        if (reverse_graph.count(s)) {
            for (const auto& pred : reverse_graph.at(s)) {
                if (!can_reach.count(pred)) {
                    can_reach.insert(pred);
                    queue.push_back(pred);
                }
            }
        }
    }

    // Prob0 = states NOT in can_reach
    std::set<std::string> prob0;
    for (const auto& sid : mdp.all_states) {
        if (!can_reach.count(sid)) prob0.insert(sid);
    }
    return prob0;
}

std::set<std::string> PCTLModelChecker::computeProb1States(
    const MDPModel& mdp, const std::set<std::string>& target, bool maximize) {
    // For maximizing: Prob1 states are those from which ALL paths eventually
    // reach target (with probability 1 under optimal scheduler).
    // Simplified: target states themselves.
    // Full computation requires iterative fixpoint — we use conservative estimate.
    return target;
}

std::map<std::string, double> PCTLModelChecker::valueIteration(
    const MDPModel& mdp,
    const std::set<std::string>& target_states,
    const std::set<std::string>& prob1_states,
    const std::set<std::string>& prob0_states,
    bool maximize) {

    // Initialize value function
    std::map<std::string, double> values;
    for (const auto& sid : mdp.all_states) {
        if (target_states.count(sid) || prob1_states.count(sid)) {
            values[sid] = 1.0;
        } else if (prob0_states.count(sid)) {
            values[sid] = 0.0;
        } else {
            values[sid] = 0.0;  // initial guess
        }
    }

    // Iterative computation until convergence
    for (std::uint32_t iter = 0; iter < config_.max_iterations; ++iter) {
        double max_delta = 0.0;

        for (const auto& [sid, actions] : mdp.states) {
            // Skip fixed states (target/prob0)
            if (target_states.count(sid) || prob0_states.count(sid)) continue;

            double best_value = maximize ? 0.0 : 1.0;

            for (const auto& action : actions) {
                // Compute expected value under this action
                double ev = 0.0;
                for (const auto& [to_state, prob] : action.distribution) {
                    auto vit = values.find(to_state);
                    double to_val = (vit != values.end()) ? vit->second : 0.0;
                    ev += prob * to_val;
                }

                if (maximize) {
                    best_value = std::max(best_value, ev);
                } else {
                    best_value = std::min(best_value, ev);
                }
            }

            double delta = std::abs(best_value - values[sid]);
            max_delta = std::max(max_delta, delta);
            values[sid] = best_value;
        }

        // Check convergence
        if (max_delta < config_.convergence_threshold) {
            break;
        }
    }

    return values;
}

double PCTLModelChecker::maxReachability(const MDPModel& mdp,
                                          const std::set<std::string>& target_states) {
    auto prob0 = computeProb0States(mdp, target_states, true);
    auto prob1 = computeProb1States(mdp, target_states, true);
    auto values = valueIteration(mdp, target_states, prob1, prob0, true);
    auto it = values.find(mdp.initial_state);
    return (it != values.end()) ? it->second : 0.0;
}

double PCTLModelChecker::minReachability(const MDPModel& mdp,
                                          const std::set<std::string>& target_states) {
    auto prob0 = computeProb0States(mdp, target_states, false);
    auto prob1 = computeProb1States(mdp, target_states, false);
    auto values = valueIteration(mdp, target_states, prob1, prob0, false);
    auto it = values.find(mdp.initial_state);
    return (it != values.end()) ? it->second : 0.0;
}

std::pair<double, double> PCTLModelChecker::intervalIteration(
    const MDPModel& mdp,
    const std::set<std::string>& target_states,
    bool maximize) {

    auto prob0 = computeProb0States(mdp, target_states, maximize);
    auto prob1 = computeProb1States(mdp, target_states, maximize);

    // Lower bound: start from 0, iterate upward
    std::map<std::string, double> lower;
    std::map<std::string, double> upper;
    for (const auto& sid : mdp.all_states) {
        if (target_states.count(sid)) {
            lower[sid] = 1.0;
            upper[sid] = 1.0;
        } else if (prob0.count(sid)) {
            lower[sid] = 0.0;
            upper[sid] = 0.0;
        } else {
            lower[sid] = 0.0;
            upper[sid] = 1.0;
        }
    }

    // Simultaneously iterate lower and upper bounds
    for (std::uint32_t iter = 0; iter < config_.max_iterations; ++iter) {
        double max_gap = 0.0;

        for (const auto& [sid, actions] : mdp.states) {
            if (target_states.count(sid) || prob0.count(sid)) continue;

            double best_lower = maximize ? 0.0 : 1.0;
            double best_upper = maximize ? 0.0 : 1.0;

            for (const auto& action : actions) {
                double ev_lower = 0.0, ev_upper = 0.0;
                for (const auto& [to_state, prob] : action.distribution) {
                    ev_lower += prob * lower[to_state];
                    ev_upper += prob * upper[to_state];
                }
                if (maximize) {
                    best_lower = std::max(best_lower, ev_lower);
                    best_upper = std::max(best_upper, ev_upper);
                } else {
                    best_lower = std::min(best_lower, ev_lower);
                    best_upper = std::min(best_upper, ev_upper);
                }
            }

            lower[sid] = best_lower;
            upper[sid] = best_upper;
            max_gap = std::max(max_gap, upper[sid] - lower[sid]);
        }

        if (max_gap < config_.convergence_threshold) break;
    }

    double lb = lower.count(mdp.initial_state) ? lower[mdp.initial_state] : 0.0;
    double ub = upper.count(mdp.initial_state) ? upper[mdp.initial_state] : 1.0;
    return {lb, ub};
}

PCTLResult PCTLModelChecker::check(const MDPModel& mdp,
                                    const PCTLFormulaPtr& formula) {
    auto start = std::chrono::steady_clock::now();
    PCTLResult result;

    if (!formula || formula->op != PCTLFormula::Op::ProbOp) {
        result.explanation = "Only P~p[phi] formulas supported at top level";
        return result;
    }

    // Extract the path formula and determine target states
    FormulaPtr path_formula = formula->path_formula;
    if (!path_formula) {
        result.explanation = "Missing path formula in probability operator";
        return result;
    }

    // Determine target states based on the path formula
    std::set<std::string> target_states;
    if (path_formula->op == TemporalOp::AF || path_formula->op == TemporalOp::EF) {
        // Eventually reach states satisfying the sub-formula
        const auto& sub = path_formula->children[0];
        if (sub->op == TemporalOp::Atom && sub->atom) {
            for (const auto& sid : mdp.all_states) {
                auto labels = mdp.labelsFor(sid);
                if (labels.has(sub->atom->name)) {
                    target_states.insert(sid);
                }
            }
        }
    } else if (path_formula->op == TemporalOp::Atom && path_formula->atom) {
        for (const auto& sid : mdp.all_states) {
            auto labels = mdp.labelsFor(sid);
            if (labels.has(path_formula->atom->name)) {
                target_states.insert(sid);
            }
        }
    }

    if (target_states.empty()) {
        result.computed_probability = 0.0;
        result.lower_bound = 0.0;
        result.upper_bound = 0.0;
        result.explanation = "No target states found for the given property";
        // Check if property holds given zero probability
        switch (formula->compare) {
            case ProbCompare::LEQ: result.property_holds = (0.0 <= formula->probability_bound); break;
            case ProbCompare::LT:  result.property_holds = (0.0 < formula->probability_bound); break;
            case ProbCompare::GEQ: result.property_holds = (0.0 >= formula->probability_bound); break;
            case ProbCompare::GT:  result.property_holds = (0.0 > formula->probability_bound); break;
        }
        auto elapsed = std::chrono::steady_clock::now() - start;
        result.solve_time = std::chrono::duration_cast<std::chrono::microseconds>(elapsed);
        return result;
    }

    // Determine if we need max or min probability
    bool need_max = (formula->compare == ProbCompare::GEQ || formula->compare == ProbCompare::GT);

    double computed_prob = 0.0;
    if (need_max) {
        computed_prob = maxReachability(mdp, target_states);
    } else {
        computed_prob = minReachability(mdp, target_states);
    }
    result.computed_probability = computed_prob;

    // Interval iteration for guaranteed bounds
    if (config_.use_interval_iteration) {
        auto [lb, ub] = intervalIteration(mdp, target_states, need_max);
        result.lower_bound = lb;
        result.upper_bound = ub;
    } else {
        result.lower_bound = computed_prob;
        result.upper_bound = computed_prob;
    }

    // Check the probability bound
    switch (formula->compare) {
        case ProbCompare::GEQ:
            result.property_holds = (computed_prob >= formula->probability_bound);
            break;
        case ProbCompare::GT:
            result.property_holds = (computed_prob > formula->probability_bound);
            break;
        case ProbCompare::LEQ:
            result.property_holds = (computed_prob <= formula->probability_bound);
            break;
        case ProbCompare::LT:
            result.property_holds = (computed_prob < formula->probability_bound);
            break;
    }

    auto elapsed = std::chrono::steady_clock::now() - start;
    result.solve_time = std::chrono::duration_cast<std::chrono::microseconds>(elapsed);
    result.explanation = "P" + std::string(
        formula->compare == ProbCompare::GEQ ? ">=" :
        formula->compare == ProbCompare::GT  ? ">"  :
        formula->compare == ProbCompare::LEQ ? "<=" : "<") +
        std::to_string(formula->probability_bound) +
        " actual=" + std::to_string(computed_prob) +
        " [" + std::to_string(result.lower_bound) + ", " +
        std::to_string(result.upper_bound) + "]";

    return result;
}

// ===========================================================================
// SECTION 5: IC3/PDR — Property-Directed Reachability (Unbounded Safety)
// ===========================================================================
//
// Implementation of the IC3 algorithm (Bradley 2011) for proving AG(P)
// properties without unrolling. Computes an inductive invariant or finds
// a concrete counterexample trace.
//
// Key operations and their SAT encodings:
//   - Bad intersection: SAT(Fk ∧ ¬P) — is there a bad state in the frontier?
//   - Relative induction: SAT(Fi-1 ∧ ¬s ∧ T ∧ s') — can s be reached in
//     one step from a state in Fi-1 that is NOT s?
//   - Init intersection: SAT(Init ∧ s) — is s an initial state?
//   - Propagation: for each clause c in Fi, check if Fi ∧ c ∧ T → c'
//     (if so, push c to Fi+1).
//
// The "exactly one state active" constraint is crucial: our Kripke model
// encodes states as one-hot Boolean vectors (exactly one state variable
// true at a time), so cubes are typically singleton positive literals.

// ---------------------------------------------------------------------------
// IC3Engine construction
// ---------------------------------------------------------------------------

IC3Engine::IC3Engine(IC3Config config) : config_(std::move(config)) {}

// ---------------------------------------------------------------------------
// Model encoding: Kripke structure → propositional formulas
// ---------------------------------------------------------------------------

void IC3Engine::encodeModel(const KripkeModel& model, const FormulaPtr& property) {
    // Reset encoding state
    state_to_var_.clear();
    state_to_var_primed_.clear();
    var_to_state_.clear();
    frames_.clear();
    init_clauses_.clear();
    transition_clauses_.clear();
    bad_clauses_.clear();
    num_state_vars_ = 0;
    num_sat_calls_ = 0;
    num_obligations_processed_ = 0;
    num_clauses_learned_ = 0;

    // Assign one Boolean variable per Kripke state (current-state encoding).
    // Also assign primed variables for next-state encoding.
    // Variable layout: [1..N] = current state vars, [N+1..2N] = primed vars.
    std::int32_t var_idx = 1;
    for (const auto& [sid, state] : model.states) {
        state_to_var_[sid] = var_idx;
        var_to_state_[var_idx] = sid;
        var_idx++;
    }
    num_state_vars_ = var_idx - 1;

    // Primed variables start after current-state variables
    for (const auto& [sid, state] : model.states) {
        state_to_var_primed_[sid] = var_idx;
        var_idx++;
    }

    // --- Encode Initial State ---
    // Exactly the initial_state variable is true, all others false.
    // This is a conjunction of unit clauses.
    auto init_it = state_to_var_.find(model.initial_state);
    if (init_it != state_to_var_.end()) {
        // The initial state variable must be true
        init_clauses_.push_back({Literal{init_it->second, false}});
        // All other state variables must be false
        for (const auto& [sid, var] : state_to_var_) {
            if (sid != model.initial_state) {
                init_clauses_.push_back({Literal{var, true}});  // negated = false
            }
        }
    }

    // --- Encode Transition Relation ---
    // For each state s with successors {t1, t2, ...}:
    //   s → (t1' ∨ t2' ∨ ... ∨ tn')   encoded as: ¬s ∨ t1' ∨ t2' ∨ ...
    // Also: exactly one primed variable is true (one-hot next state).
    for (const auto& [sid, state] : model.states) {
        auto from_it = state_to_var_.find(sid);
        if (from_it == state_to_var_.end()) continue;
        std::int32_t from_var = from_it->second;

        if (state.successors.empty()) {
            // Terminal state: self-loop. s → s'
            auto to_it = state_to_var_primed_.find(sid);
            if (to_it != state_to_var_primed_.end()) {
                transition_clauses_.push_back(
                    {Literal{from_var, true}, Literal{to_it->second, false}});
            }
        } else {
            // s → (succ1' ∨ succ2' ∨ ...)
            std::vector<Literal> clause;
            clause.push_back(Literal{from_var, true});  // ¬s
            for (const auto& succ : state.successors) {
                auto to_it = state_to_var_primed_.find(succ);
                if (to_it != state_to_var_primed_.end()) {
                    clause.push_back(Literal{to_it->second, false});
                }
            }
            if (clause.size() > 1) {
                transition_clauses_.push_back(std::move(clause));
            }
        }

        // Exclusion: if s is active, no non-successor can be the next state.
        // s → ¬t' for each t not in successors(s)
        std::set<std::string> succ_set(state.successors.begin(), state.successors.end());
        if (state.successors.empty()) {
            succ_set.insert(sid);  // self-loop for terminal
        }
        for (const auto& [other_sid, other_var_primed] : state_to_var_primed_) {
            if (!succ_set.count(other_sid)) {
                // ¬s ∨ ¬other'
                transition_clauses_.push_back(
                    {Literal{from_var, true}, Literal{other_var_primed, true}});
            }
        }
    }

    // Exactly-one constraint on current-state variables (at-least-one + at-most-one).
    // At-least-one: s1 ∨ s2 ∨ ... ∨ sn
    {
        std::vector<Literal> alo;
        for (const auto& [sid, var] : state_to_var_) {
            alo.push_back(Literal{var, false});
        }
        transition_clauses_.push_back(std::move(alo));
    }
    // At-most-one: ¬si ∨ ¬sj for all i < j
    {
        std::vector<std::int32_t> all_vars;
        for (const auto& [sid, var] : state_to_var_) {
            all_vars.push_back(var);
        }
        for (std::size_t i = 0; i < all_vars.size(); ++i) {
            for (std::size_t j = i + 1; j < all_vars.size(); ++j) {
                transition_clauses_.push_back(
                    {Literal{all_vars[i], true}, Literal{all_vars[j], true}});
            }
        }
    }

    // Exactly-one on primed variables
    {
        std::vector<Literal> alo_primed;
        for (const auto& [sid, var] : state_to_var_primed_) {
            alo_primed.push_back(Literal{var, false});
        }
        transition_clauses_.push_back(std::move(alo_primed));
    }
    {
        std::vector<std::int32_t> all_primed;
        for (const auto& [sid, var] : state_to_var_primed_) {
            all_primed.push_back(var);
        }
        for (std::size_t i = 0; i < all_primed.size(); ++i) {
            for (std::size_t j = i + 1; j < all_primed.size(); ++j) {
                transition_clauses_.push_back(
                    {Literal{all_primed[i], true}, Literal{all_primed[j], true}});
            }
        }
    }

    // --- Encode Bad States (¬Property) ---
    // The property is a state predicate. Bad states are those where ¬property holds.
    // For atomic propositions: bad states are those lacking the proposition.
    // We encode: at least one bad state is active (disjunction of bad state vars).
    if (property) {
        std::vector<Literal> bad_disjunction;

        if (property->op == TemporalOp::Atom && property->atom) {
            // Property = atom. Bad = states where atom does NOT hold.
            for (const auto& [sid, state] : model.states) {
                if (!state.labels.has(property->atom->name)) {
                    auto var_it = state_to_var_.find(sid);
                    if (var_it != state_to_var_.end()) {
                        bad_disjunction.push_back(Literal{var_it->second, false});
                    }
                }
            }
        } else if (property->op == TemporalOp::Not && property->children.size() == 1) {
            // Property = ¬atom. Bad = states where atom DOES hold.
            const auto& inner = property->children[0];
            if (inner->op == TemporalOp::Atom && inner->atom) {
                for (const auto& [sid, state] : model.states) {
                    if (state.labels.has(inner->atom->name)) {
                        auto var_it = state_to_var_.find(sid);
                        if (var_it != state_to_var_.end()) {
                            bad_disjunction.push_back(Literal{var_it->second, false});
                        }
                    }
                }
            }
        } else if (property->op == TemporalOp::And && property->children.size() == 2) {
            // Property = A ∧ B. Bad = ¬A ∨ ¬B = states missing A or missing B.
            // Collect states that violate the conjunction.
            for (const auto& [sid, state] : model.states) {
                bool satisfies = true;
                for (const auto& child : property->children) {
                    if (child->op == TemporalOp::Atom && child->atom) {
                        if (!state.labels.has(child->atom->name)) {
                            satisfies = false;
                            break;
                        }
                    } else if (child->op == TemporalOp::Not &&
                               child->children[0]->op == TemporalOp::Atom &&
                               child->children[0]->atom) {
                        if (state.labels.has(child->children[0]->atom->name)) {
                            satisfies = false;
                            break;
                        }
                    }
                }
                if (!satisfies) {
                    auto var_it = state_to_var_.find(sid);
                    if (var_it != state_to_var_.end()) {
                        bad_disjunction.push_back(Literal{var_it->second, false});
                    }
                }
            }
        } else if (property->op == TemporalOp::Or && property->children.size() == 2) {
            // Property = A ∨ B. Bad = ¬A ∧ ¬B = states missing both A and B.
            for (const auto& [sid, state] : model.states) {
                bool satisfies = false;
                for (const auto& child : property->children) {
                    if (child->op == TemporalOp::Atom && child->atom) {
                        if (state.labels.has(child->atom->name)) {
                            satisfies = true;
                            break;
                        }
                    } else if (child->op == TemporalOp::Not &&
                               child->children[0]->op == TemporalOp::Atom &&
                               child->children[0]->atom) {
                        if (!state.labels.has(child->children[0]->atom->name)) {
                            satisfies = true;
                            break;
                        }
                    }
                }
                if (!satisfies) {
                    auto var_it = state_to_var_.find(sid);
                    if (var_it != state_to_var_.end()) {
                        bad_disjunction.push_back(Literal{var_it->second, false});
                    }
                }
            }
        } else if (property->op == TemporalOp::Implies && property->children.size() == 2) {
            // Property = A → B ≡ ¬A ∨ B. Bad = A ∧ ¬B.
            for (const auto& [sid, state] : model.states) {
                bool a_holds = false;
                bool b_holds = false;
                const auto& child_a = property->children[0];
                const auto& child_b = property->children[1];
                if (child_a->op == TemporalOp::Atom && child_a->atom) {
                    a_holds = state.labels.has(child_a->atom->name);
                }
                if (child_b->op == TemporalOp::Atom && child_b->atom) {
                    b_holds = state.labels.has(child_b->atom->name);
                }
                // Bad if A holds and B does not
                if (a_holds && !b_holds) {
                    auto var_it = state_to_var_.find(sid);
                    if (var_it != state_to_var_.end()) {
                        bad_disjunction.push_back(Literal{var_it->second, false});
                    }
                }
            }
        }

        if (!bad_disjunction.empty()) {
            bad_clauses_.push_back(std::move(bad_disjunction));
        }
    }

    // --- Initialize Frame Sequence ---
    // F0 = Init (all init clauses). F1 = {} (empty = all states allowed).
    frames_.push_back(init_clauses_);  // frames_[0] = F0
    frames_.push_back({});             // frames_[1] = F1 (frontier)
}

// ---------------------------------------------------------------------------
// SAT solver construction helpers
// ---------------------------------------------------------------------------

CDCLSolver IC3Engine::makeSolverForFrame(std::uint32_t frame_level) const {
    CDCLSolver solver;

    // Allocate variables: current-state + primed
    std::int32_t total_vars = num_state_vars_ * 2;
    for (std::int32_t i = 0; i < total_vars; ++i) {
        solver.newVar();
    }

    // Add frame clauses: all clauses from F0 through F[frame_level]
    // (frame invariant: a state is in Fi iff it satisfies all clauses in
    // frames_[0] through frames_[i])
    for (std::uint32_t i = 0; i <= frame_level && i < frames_.size(); ++i) {
        for (const auto& clause : frames_[i]) {
            solver.addClause(clause);
        }
    }

    return solver;
}

void IC3Engine::addTransitionClauses(CDCLSolver& solver) const {
    for (const auto& clause : transition_clauses_) {
        solver.addClause(clause);
    }
}

void IC3Engine::addCubeAssumptions(CDCLSolver& solver, const IC3Cube& cube,
                                    bool primed) const {
    // Add each literal of the cube as a unit clause (assumption).
    // If primed=true, shift variables to the primed range.
    for (const auto& lit : cube.literals) {
        Literal actual = primed ? toPrimed(lit) : lit;
        solver.addClause({actual});
    }
}

Literal IC3Engine::toPrimed(Literal lit) const {
    // Shift variable from current-state range to primed range.
    // Current: [1..num_state_vars_], Primed: [num_state_vars_+1..2*num_state_vars_]
    return Literal{lit.var + num_state_vars_, lit.negated};
}

Literal IC3Engine::toUnprimed(Literal lit) const {
    return Literal{lit.var - num_state_vars_, lit.negated};
}

// ---------------------------------------------------------------------------
// Core IC3 operations
// ---------------------------------------------------------------------------

std::pair<bool, IC3Cube> IC3Engine::checkBadIntersection(std::uint32_t frame_level) {
    // Check: SAT(Fk ∧ Bad)?
    // Build a solver with the frontier frame's clauses and assert Bad.
    CDCLSolver solver = makeSolverForFrame(frame_level);

    // Add bad-state clauses (the disjunction: at least one bad state is active)
    for (const auto& clause : bad_clauses_) {
        solver.addClause(clause);
    }

    num_sat_calls_++;
    SatResult result = solver.solve(config_.sat_conflict_limit);

    if (result == SatResult::SAT) {
        // Extract the bad cube from the model
        IC3Cube bad_cube = extractCube(solver, false);
        return {true, bad_cube};
    }
    return {false, IC3Cube{}};
}

bool IC3Engine::isRelativelyInductive(std::uint32_t frame_level,
                                       const IC3Cube& cube,
                                       IC3Cube& predecessor_cube) {
    // Relative induction check:
    //   SAT(Fi-1 ∧ ¬cube ∧ T ∧ cube')?
    //
    // If UNSAT: cube is blocked at frame i (no predecessor in Fi-1 can
    //           reach cube in one step). Return true.
    // If SAT:   predecessor_cube is a state in Fi-1 that transitions to cube.
    //           Return false.
    //
    // This is the core IC3 query. The "¬cube" constraint ensures we don't
    // find the cube itself as its own predecessor (self-blocking).

    CDCLSolver solver = makeSolverForFrame(frame_level - 1);

    // Add ¬cube (the blocking clause for the current state)
    std::vector<Literal> neg_cube = cube.toClause();
    solver.addClause(neg_cube);

    // Add transition relation
    addTransitionClauses(solver);

    // Add cube' (the cube must hold in the NEXT state — primed variables)
    addCubeAssumptions(solver, cube, true);

    num_sat_calls_++;
    SatResult result = solver.solve(config_.sat_conflict_limit);

    if (result == SatResult::SAT) {
        // Found a predecessor: extract it from the current-state variables
        predecessor_cube = extractCube(solver, false);
        return false;  // NOT relatively inductive
    }
    return true;  // Relatively inductive (blocked)
}

bool IC3Engine::intersectsInit(const IC3Cube& cube) {
    // Check: SAT(Init ∧ cube)?
    // If the cube includes an initial state, we have a real counterexample.
    CDCLSolver solver;

    // Allocate variables
    for (std::int32_t i = 0; i < num_state_vars_ * 2; ++i) {
        solver.newVar();
    }

    // Add init clauses
    for (const auto& clause : init_clauses_) {
        solver.addClause(clause);
    }

    // Add cube as unit clauses
    addCubeAssumptions(solver, cube, false);

    num_sat_calls_++;
    SatResult result = solver.solve(config_.sat_conflict_limit);
    return (result == SatResult::SAT);
}

IC3Cube IC3Engine::extractCube(const CDCLSolver& solver, bool primed) const {
    // Extract a state cube from the SAT model. For our one-hot encoding,
    // the cube contains exactly one positive literal (the active state).
    IC3Cube cube;
    auto mdl = solver.model();

    if (primed) {
        // Extract from primed variables
        for (const auto& [sid, var] : state_to_var_primed_) {
            if (var > 0 && static_cast<std::size_t>(var) < mdl.size() && mdl[var]) {
                // This primed state is active — convert to unprimed for the cube
                auto unprimed_it = state_to_var_.find(sid);
                if (unprimed_it != state_to_var_.end()) {
                    cube.literals.push_back(Literal{unprimed_it->second, false});
                }
            }
        }
    } else {
        // Extract from current-state variables
        for (const auto& [sid, var] : state_to_var_) {
            if (var > 0 && static_cast<std::size_t>(var) < mdl.size() && mdl[var]) {
                cube.literals.push_back(Literal{var, false});
            }
        }
    }

    // Sort for canonical ordering
    std::sort(cube.literals.begin(), cube.literals.end());
    return cube;
}

// ---------------------------------------------------------------------------
// Clause generalization
// ---------------------------------------------------------------------------

IC3Cube IC3Engine::generalize(std::uint32_t frame_level, const IC3Cube& cube) {
    if (!config_.generalize) return cube;

    // Generalization: try to drop literals from the cube while maintaining
    // relative inductiveness. A smaller cube blocks more states, making
    // the invariant converge faster.
    //
    // Strategy: for each literal in the cube, try removing it and check
    // if the reduced cube is still relatively inductive at frame_level.
    // If yes, keep it removed. If no, put it back.
    //
    // This is the "ternary simulation" approach from Een et al. 2011.

    IC3Cube generalized = cube;

    for (std::size_t i = 0; i < generalized.literals.size(); ) {
        if (generalized.literals.size() <= 1) break;  // keep at least one literal

        // Try removing literal at position i
        IC3Cube candidate;
        for (std::size_t j = 0; j < generalized.literals.size(); ++j) {
            if (j != i) candidate.literals.push_back(generalized.literals[j]);
        }

        // Check: is the reduced cube still not intersecting Init?
        if (intersectsInit(candidate)) {
            // Can't remove this literal — it's needed to separate from init
            ++i;
            continue;
        }

        // Check: is the reduced cube still relatively inductive?
        IC3Cube dummy_pred;
        if (isRelativelyInductive(frame_level, candidate, dummy_pred)) {
            // Successfully generalized: keep the smaller cube
            generalized = candidate;
            // Don't increment i — the next literal shifted into position i
        } else {
            // Can't remove this literal — needed for relative induction
            ++i;
        }
    }

    return generalized;
}

// ---------------------------------------------------------------------------
// Frame management
// ---------------------------------------------------------------------------

void IC3Engine::blockCubeAtFrame(std::uint32_t frame_level, const IC3Cube& cube) {
    // Add ¬cube (blocking clause) to all frames from 1 to frame_level.
    // IC3 invariant: if a clause is in Fi, it's also in all Fj for j ≤ i.
    std::vector<Literal> clause = cube.toClause();

    for (std::uint32_t i = 1; i <= frame_level && i < frames_.size(); ++i) {
        frames_[i].push_back(clause);
    }
    num_clauses_learned_++;
}

bool IC3Engine::propagateClauses() {
    // Forward propagation: for each frame Fi (i < k), try to push each
    // clause to Fi+1. A clause c can be pushed if:
    //   SAT(Fi ∧ c ∧ T ∧ ¬c') is UNSAT
    // i.e., c is inductive relative to the frame it's in.
    //
    // This accelerates convergence by strengthening higher frames.

    if (!config_.propagate) return false;

    bool any_pushed = false;

    for (std::size_t i = 1; i + 1 < frames_.size(); ++i) {
        std::vector<std::vector<Literal>> pushed_clauses;

        for (const auto& clause : frames_[i]) {
            // Check if this clause is already in the next frame
            bool already_present = false;
            for (const auto& next_clause : frames_[i + 1]) {
                if (next_clause == clause) {
                    already_present = true;
                    break;
                }
            }
            if (already_present) continue;

            // Check relative induction of the clause at frame i.
            // The clause is a disjunction ¬l1 ∨ ¬l2 ∨ ... (negation of a cube).
            // We need: SAT(Fi ∧ clause ∧ T ∧ ¬clause') is UNSAT.
            //
            // Equivalently, check that no state in Fi satisfying the clause
            // can transition to a state violating the clause.
            CDCLSolver solver = makeSolverForFrame(static_cast<std::uint32_t>(i));
            addTransitionClauses(solver);

            // Assert the clause itself (it holds in the current state)
            solver.addClause(clause);

            // Assert ¬clause' (the clause is violated in the next state).
            // clause = ¬l1 ∨ ¬l2 ∨ ..., so ¬clause = l1 ∧ l2 ∧ ...
            // ¬clause' = l1' ∧ l2' ∧ ... (each literal primed and asserted)
            for (const auto& lit : clause) {
                Literal primed_negated = toPrimed(~lit);
                solver.addClause({primed_negated});
            }

            num_sat_calls_++;
            SatResult result = solver.solve(config_.sat_conflict_limit);

            if (result == SatResult::UNSAT) {
                // Clause is inductive: can be pushed to Fi+1
                pushed_clauses.push_back(clause);
                any_pushed = true;
            }
        }

        // Add pushed clauses to the next frame
        for (auto& clause : pushed_clauses) {
            frames_[i + 1].push_back(std::move(clause));
        }
    }

    return any_pushed;
}

std::int32_t IC3Engine::checkConvergence() const {
    // Frame convergence: if Fi == Fi+1 (same set of clauses), then Fi
    // is an inductive invariant and the property is proven.
    //
    // We check by comparing clause sets between adjacent frames.
    // Since clauses propagate forward, convergence happens when a frame
    // has all the same clauses as the next one.

    for (std::size_t i = 1; i + 1 < frames_.size(); ++i) {
        const auto& fi = frames_[i];
        const auto& fi_next = frames_[i + 1];

        // Check if every clause in fi is also in fi+1
        bool all_present = true;
        for (const auto& clause : fi) {
            bool found = false;
            for (const auto& next_clause : fi_next) {
                if (next_clause == clause) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                all_present = false;
                break;
            }
        }

        // Check the reverse: every clause in fi+1 is also in fi
        bool reverse_all_present = true;
        if (all_present) {
            for (const auto& clause : fi_next) {
                bool found = false;
                for (const auto& fi_clause : fi) {
                    if (fi_clause == clause) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    reverse_all_present = false;
                    break;
                }
            }
        }

        if (all_present && reverse_all_present) {
            return static_cast<std::int32_t>(i);
        }
    }
    return -1;  // No convergence yet
}

// ---------------------------------------------------------------------------
// Counterexample trace reconstruction
// ---------------------------------------------------------------------------

std::vector<IC3Cube> IC3Engine::reconstructTrace(
    const std::shared_ptr<ProofObligation>& terminal) const {
    // Walk backward through the proof obligation chain from the terminal
    // (bad state) to the initial state, collecting cubes along the way.
    std::vector<IC3Cube> trace;
    auto current = terminal;
    while (current) {
        trace.push_back(current->cube);
        current = current->predecessor;
    }
    // Reverse so trace goes from init → bad
    std::reverse(trace.begin(), trace.end());
    return trace;
}

std::string IC3Engine::cubeToStateId(const IC3Cube& cube) const {
    // Map a cube back to a Kripke state ID. For one-hot encoding,
    // the cube should have exactly one positive literal.
    for (const auto& lit : cube.literals) {
        if (!lit.negated) {
            auto it = var_to_state_.find(lit.var);
            if (it != var_to_state_.end()) return it->second;
        }
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Main IC3 loop
// ---------------------------------------------------------------------------

IC3Result IC3Engine::check(const KripkeModel& model, const FormulaPtr& property) {
    auto start = std::chrono::steady_clock::now();
    IC3Result result;

    // Step 1: Encode the model
    encodeModel(model, property);

    // Quick check: does the initial state itself violate the property?
    // If Init ∧ Bad is SAT, we have a zero-length counterexample.
    {
        CDCLSolver solver;
        for (std::int32_t i = 0; i < num_state_vars_ * 2; ++i) solver.newVar();
        for (const auto& clause : init_clauses_) solver.addClause(clause);
        for (const auto& clause : bad_clauses_) solver.addClause(clause);
        num_sat_calls_++;
        if (solver.solve(config_.sat_conflict_limit) == SatResult::SAT) {
            IC3Cube init_bad = extractCube(solver, false);
            result.verdict = IC3Result::Verdict::Refuted;
            result.counterexample_trace.push_back(init_bad);
            result.counterexample_states.push_back(cubeToStateId(init_bad));
            auto elapsed = std::chrono::steady_clock::now() - start;
            result.solve_time = std::chrono::duration_cast<std::chrono::microseconds>(elapsed);
            result.num_sat_calls = num_sat_calls_;
            return result;
        }
    }

    // If no bad states exist at all, property trivially holds.
    if (bad_clauses_.empty()) {
        result.verdict = IC3Result::Verdict::Proven;
        result.convergence_frame = 0;
        auto elapsed = std::chrono::steady_clock::now() - start;
        result.solve_time = std::chrono::duration_cast<std::chrono::microseconds>(elapsed);
        result.num_sat_calls = num_sat_calls_;
        return result;
    }

    // Step 2: Main IC3 loop — extend frames until convergence or counterexample
    for (std::uint32_t iteration = 0; iteration < config_.max_frames; ++iteration) {
        std::uint32_t frontier = static_cast<std::uint32_t>(frames_.size()) - 1;

        // Step 3a: Check if any bad state is in the frontier frame
        auto [bad_reachable, bad_cube] = checkBadIntersection(frontier);

        if (bad_reachable) {
            // Bad state found in frontier. Create proof obligation and try to block.
            // Use a min-priority queue ordered by frame level (process lowest first).
            std::priority_queue<ProofObligation, std::vector<ProofObligation>,
                                std::greater<ProofObligation>> obligations;

            auto root_obligation = std::make_shared<ProofObligation>();
            root_obligation->cube = bad_cube;
            root_obligation->frame_level = frontier;
            root_obligation->predecessor = nullptr;

            obligations.push(*root_obligation);
            bool counterexample_found = false;
            std::shared_ptr<ProofObligation> cex_terminal;

            // Step 3b: Process proof obligations
            while (!obligations.empty()) {
                if (num_obligations_processed_ >= config_.max_proof_obligations) {
                    result.verdict = IC3Result::Verdict::Unknown;
                    goto done;
                }

                ProofObligation current = obligations.top();
                obligations.pop();
                num_obligations_processed_++;

                if (current.frame_level == 0) {
                    // Obligation at frame 0: check if cube is an initial state
                    if (intersectsInit(current.cube)) {
                        // COUNTEREXAMPLE: trace from init through all predecessors to bad
                        counterexample_found = true;
                        cex_terminal = std::make_shared<ProofObligation>(current);
                        break;
                    }
                    // Not an initial state — cube is already blocked by F0's init constraints
                    continue;
                }

                // Try to block the cube via relative induction
                IC3Cube predecessor;
                if (isRelativelyInductive(current.frame_level, current.cube, predecessor)) {
                    // Cube is blocked! Generalize and add to frame.
                    IC3Cube generalized = generalize(current.frame_level, current.cube);
                    blockCubeAtFrame(current.frame_level, generalized);

                    // If original obligation was at a higher level, re-push it
                    // to try blocking at higher frames too (optional strengthening).
                    if (current.frame_level < frontier) {
                        ProofObligation higher;
                        higher.cube = current.cube;
                        higher.frame_level = current.frame_level + 1;
                        higher.predecessor = current.predecessor;
                        obligations.push(higher);
                    }
                } else {
                    // Predecessor found — push a new obligation at level-1
                    ProofObligation pred_obligation;
                    pred_obligation.cube = predecessor;
                    pred_obligation.frame_level = current.frame_level - 1;
                    pred_obligation.predecessor = std::make_shared<ProofObligation>(current);
                    obligations.push(pred_obligation);

                    // Also re-enqueue the current obligation (it might get blocked
                    // after the predecessor is blocked in a lower frame)
                    obligations.push(current);
                }
            }

            if (counterexample_found && cex_terminal) {
                // Reconstruct the counterexample trace
                result.verdict = IC3Result::Verdict::Refuted;
                result.counterexample_trace = reconstructTrace(cex_terminal);
                for (const auto& cube : result.counterexample_trace) {
                    result.counterexample_states.push_back(cubeToStateId(cube));
                }
                goto done;
            }
        }

        // Step 3c: Propagate clauses forward
        propagateClauses();

        // Step 3d: Check for frame convergence
        std::int32_t converge_frame = checkConvergence();
        if (converge_frame >= 0) {
            result.verdict = IC3Result::Verdict::Proven;
            result.convergence_frame = static_cast<std::uint32_t>(converge_frame);
            // Extract the inductive invariant
            result.invariant_clauses = frames_[converge_frame];
            goto done;
        }

        // Step 3e: Add a new frontier frame and continue
        frames_.push_back({});
    }

    // Exhausted frame budget
    result.verdict = IC3Result::Verdict::Unknown;

done:
    auto elapsed = std::chrono::steady_clock::now() - start;
    result.solve_time = std::chrono::duration_cast<std::chrono::microseconds>(elapsed);
    result.num_frames = static_cast<std::uint32_t>(frames_.size());
    result.num_proof_obligations = num_obligations_processed_;
    result.num_sat_calls = num_sat_calls_;
    result.num_clauses_learned = num_clauses_learned_;
    return result;
}

IC3Result IC3Engine::checkAG(const KripkeModel& model, const FormulaPtr& ag_formula) {
    // Extract the sub-formula from AG(phi) and verify AG(phi) via IC3.
    if (ag_formula && ag_formula->op == TemporalOp::AG && !ag_formula->children.empty()) {
        return check(model, ag_formula->children[0]);
    }
    // If not an AG formula, treat the formula itself as the property to check
    return check(model, ag_formula);
}

// ---------------------------------------------------------------------------
// Integration: verifyIC3 — bridges IC3Result to VerificationResult
// ---------------------------------------------------------------------------

VerificationResult verifyIC3(const KripkeModel& model, const FormulaPtr& formula,
                             IC3Config config) {
    VerificationResult result;
    result.property = formula;

    // IC3 works on safety properties (AG). Extract the sub-formula.
    FormulaPtr property_to_check;
    if (formula && formula->op == TemporalOp::AG && !formula->children.empty()) {
        property_to_check = formula->children[0];
        result.property_name = "ic3-safety";
    } else {
        // Treat the formula itself as the state predicate for AG(formula)
        property_to_check = formula;
        result.property_name = "ic3-custom";
    }

    IC3Engine engine(config);
    IC3Result ic3_result = engine.check(model, property_to_check);

    // Map IC3 verdict to VerificationResult
    switch (ic3_result.verdict) {
        case IC3Result::Verdict::Proven:
            result.verdict = VerificationResult::Verdict::Satisfied;
            result.violation_explanation = "";
            break;
        case IC3Result::Verdict::Refuted:
            result.verdict = VerificationResult::Verdict::Violated;
            // Build counterexample trace from IC3 state IDs
            for (std::size_t i = 0; i < ic3_result.counterexample_states.size(); ++i) {
                TraceStep step;
                step.state_id = ic3_result.counterexample_states[i];
                step.step_index = static_cast<std::uint32_t>(i);
                step.event = (i == ic3_result.counterexample_states.size() - 1)
                    ? "bad-state-reached" : "transition";
                // Retrieve labels from the model
                auto sit = model.states.find(step.state_id);
                if (sit != model.states.end()) {
                    step.labels = sit->second.labels;
                }
                result.counterexample.push_back(std::move(step));
            }
            result.violation_explanation =
                "IC3/PDR found reachable bad state: " +
                (ic3_result.counterexample_states.empty() ? "unknown"
                    : ic3_result.counterexample_states.back()) +
                " (trace length: " +
                std::to_string(ic3_result.counterexample_states.size()) + ")";
            break;
        case IC3Result::Verdict::Unknown:
            result.verdict = VerificationResult::Verdict::Unknown;
            result.violation_explanation = "IC3 inconclusive (resource limit)";
            break;
    }

    result.states_explored = ic3_result.num_sat_calls;
    result.verification_time = ic3_result.solve_time;
    result.bound_used = ic3_result.num_frames;
    return result;
}

}  // namespace sparx::formal

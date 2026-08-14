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

}  // namespace sparx::formal

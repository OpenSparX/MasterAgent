/**
 * @file sparx_formal_verify.h
 * @brief Formal Plan Verification — LTL/CTL Model Checking on IntentDAG.
 *
 * Research basis:
 *   - AgentVerify: Compositional Formal Verification via LTL (Preprints 202604.1029)
 *   - SENTINEL: Multi-Level Safety Evaluation Framework (arXiv:2510.12985)
 *   - Agent-C: Enforcing Temporal Constraints for LLM Agents (arXiv:2512.23738)
 *   - Lean4Agent: Formal Modeling and Verification (arXiv:2606.06523)
 *   - Causal Past Logic for Runtime Verification (arXiv:2605.20923)
 *
 * This module provides:
 *   1. Static verification: check execution plans BEFORE running them
 *   2. Runtime monitoring: verify temporal properties during execution
 *   3. Safety certificates: generate proof artifacts for auditing
 *
 * Temporal logic properties expressed in a subset of CTL*:
 *   - Safety:   AG(¬bad_state)         "bad states are never reached"
 *   - Liveness: AG(request → AF done)  "every request eventually completes"
 *   - Ordering: AG(auth → AX allowed)  "auth always precedes access"
 *   - Fairness: AG(AF scheduled)       "no node is starved indefinitely"
 *   - Bounded:  A[done ≤ deadline]     "completion within deadline"
 *
 * The model checker uses bounded model checking (BMC) for efficiency:
 *   - DAGs are unrolled to depth k (= longest path + 1)
 *   - Properties are checked via SAT-like constraint propagation
 *   - Counterexamples produce concrete execution traces
 *   - Typical verification: <10ms for plans with ≤50 nodes
 *
 * Integration with existing Sparx components:
 *   - sparx_dag_builder.h provides the IntentDAG representation
 *   - cmd_plan.cpp uses this for `sparx plan verify`
 *   - The orchestrator calls verify() before executing any plan
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace sparx::formal {

// ---------------------------------------------------------------------------
// Temporal Logic AST
// ---------------------------------------------------------------------------

/// Atomic proposition — a predicate over plan/node state.
struct Atom {
    std::string name;           // e.g., "node.completed", "resource.acquired"
    std::string node_id;        // which node (empty = global)
    std::string parameter;      // optional parameter
};

/// Temporal logic formula (CTL* subset).
/// Supports: And, Or, Not, Implies, AG, AF, AX, AU, EG, EF, EX, EU
enum class TemporalOp : std::uint8_t {
    // Propositional
    Atom,       // base proposition
    Not,        // ¬φ
    And,        // φ ∧ ψ
    Or,         // φ ∨ ψ
    Implies,    // φ → ψ
    // Universal path quantifiers (All paths)
    AG,         // A□φ  — on all paths, globally φ
    AF,         // A◇φ  — on all paths, eventually φ
    AX,         // A○φ  — on all paths, next state φ
    AU,         // A[φ U ψ] — on all paths, φ until ψ
    // Existential path quantifiers (some path)
    EG,         // E□φ  — there exists a path where globally φ
    EF,         // E◇φ  — there exists a path where eventually φ
    EX,         // E○φ  — there exists a path where next state φ
    EU,         // E[φ U ψ] — there exists a path where φ until ψ
    // Bounded operators
    ABounded,   // A[φ within k steps]
};

struct Formula;
using FormulaPtr = std::shared_ptr<Formula>;

struct Formula {
    TemporalOp op;
    std::optional<Atom> atom;           // for Atom
    std::vector<FormulaPtr> children;   // sub-formulas
    std::uint32_t bound = 0;            // for ABounded

    // Factory methods for readable construction
    static FormulaPtr makeAtom(const std::string& name,
                               const std::string& node_id = "",
                               const std::string& param = "");
    static FormulaPtr makeNot(FormulaPtr f);
    static FormulaPtr makeAnd(FormulaPtr a, FormulaPtr b);
    static FormulaPtr makeOr(FormulaPtr a, FormulaPtr b);
    static FormulaPtr makeImplies(FormulaPtr a, FormulaPtr b);
    static FormulaPtr makeAG(FormulaPtr f);
    static FormulaPtr makeAF(FormulaPtr f);
    static FormulaPtr makeAX(FormulaPtr f);
    static FormulaPtr makeAU(FormulaPtr f, FormulaPtr g);
    static FormulaPtr makeEF(FormulaPtr f);
    static FormulaPtr makeBounded(FormulaPtr f, std::uint32_t k);
};

// ---------------------------------------------------------------------------
// Plan Model (Kripke Structure from IntentDAG)
// ---------------------------------------------------------------------------

/// A node in the execution plan (maps to IntentDAG node).
struct PlanNode {
    std::string id;
    std::string tool_name;          // MCP tool being called
    std::string service;            // MCP service
    std::vector<std::string> deps;  // dependency edges (must complete before)
    bool is_destructive = false;    // annotated as destructive?
    bool is_idempotent = false;     // annotated as idempotent?
    bool requires_auth = false;     // needs authorization?
    std::uint32_t timeout_ms = 0;   // per-node deadline
    std::vector<std::string> resources;  // resources acquired
};

/// State labels for model checking.
struct StateLabels {
    std::set<std::string> propositions;  // active atomic propositions

    bool has(const std::string& prop) const {
        return propositions.count(prop) > 0;
    }
};

/// A state in the Kripke structure (one execution snapshot).
struct KripkeState {
    std::string id;
    StateLabels labels;
    std::vector<std::string> successors;  // transitions to next states
};

/// Kripke structure extracted from an IntentDAG.
struct KripkeModel {
    std::string initial_state;
    std::map<std::string, KripkeState> states;
    std::vector<PlanNode> nodes;
    std::uint32_t depth = 0;  // longest path length

    /// Builds a Kripke model from plan nodes.
    static KripkeModel fromPlan(const std::vector<PlanNode>& nodes);
};

// ---------------------------------------------------------------------------
// Safety Properties (Built-in library)
// ---------------------------------------------------------------------------

/// Pre-defined safety properties for common patterns.
namespace properties {

/// No destructive operation runs without preceding authorization.
FormulaPtr authBeforeDestructive();

/// No two destructive operations on the same resource run concurrently.
FormulaPtr noConflictingDestructive();

/// Every node eventually reaches completion or failure (no hang).
FormulaPtr allNodesTerminate();

/// Deadline property: all nodes complete within their timeout.
FormulaPtr deadlineSafety(std::uint32_t global_deadline_ms);

/// No resource deadlock: acquired resources are eventually released.
FormulaPtr noResourceDeadlock();

/// Idempotent operations can be safely retried (no state corruption).
FormulaPtr retryIsSafe();

/// Data flows only forward in the DAG (no backward information flow).
FormulaPtr dataFlowIntegrity();

}  // namespace properties

// ---------------------------------------------------------------------------
// Verification Result
// ---------------------------------------------------------------------------

/// A concrete execution step in a counterexample trace.
struct TraceStep {
    std::string state_id;
    std::string node_id;        // which node is executing
    std::string event;          // what happened
    StateLabels labels;         // active propositions at this point
    std::uint32_t step_index;   // position in trace
};

/// Result of verifying one property against a plan.
struct VerificationResult {
    enum class Verdict : std::uint8_t {
        Satisfied,      // property holds on all paths
        Violated,       // counterexample found
        Unknown,        // bound exceeded (inconclusive)
        Error,          // verification failed (malformed input)
    };

    Verdict verdict = Verdict::Unknown;
    std::string property_name;
    std::string property_description;
    FormulaPtr property;

    /// Counterexample trace (only when Violated).
    std::vector<TraceStep> counterexample;

    /// Human-readable explanation of the violation.
    std::string violation_explanation;

    /// Verification performance.
    std::uint32_t states_explored = 0;
    std::chrono::microseconds verification_time{0};
    std::uint32_t bound_used = 0;

    bool satisfied() const { return verdict == Verdict::Satisfied; }
};

/// Aggregate result of verifying all properties.
struct PlanVerification {
    std::string plan_id;
    std::vector<VerificationResult> results;
    bool all_satisfied = false;
    std::chrono::microseconds total_time{0};
    std::uint32_t total_states_explored = 0;

    /// Returns the first violated property, if any.
    std::optional<VerificationResult> firstViolation() const;

    /// Generates a human-readable report.
    std::string report() const;

    /// Generates a machine-readable safety certificate (JSON).
    std::string certificate() const;
};

// ---------------------------------------------------------------------------
// Model Checker
// ---------------------------------------------------------------------------

/// Configuration for the bounded model checker.
struct VerifierConfig {
    /// Maximum unrolling depth (0 = auto from DAG depth + 1).
    std::uint32_t max_depth = 0;
    /// Maximum states to explore before declaring Unknown.
    std::uint32_t max_states = 100000;
    /// Timeout for total verification.
    std::chrono::milliseconds timeout{100};
    /// Which built-in properties to check (empty = all).
    std::vector<std::string> properties;
    /// Additional custom properties.
    std::vector<std::pair<std::string, FormulaPtr>> custom_properties;
    /// Generate counterexample traces on violation.
    bool generate_counterexamples = true;
    /// Enable Partial-Order Reduction (reduces state space for concurrent plans).
    bool enable_por = true;
    /// Verbosity level (0=silent, 1=summary, 2=detailed).
    std::uint8_t verbosity = 1;
};

/**
 * @brief Bounded model checker for IntentDAG execution plans.
 *
 * Algorithm (Bounded Model Checking):
 *   1. Build Kripke structure from plan nodes
 *   2. For each property φ:
 *      a. Unroll the model to depth k
 *      b. Check ¬φ at each reachable state
 *      c. If ¬φ is satisfiable → counterexample found (Violated)
 *      d. If not → property holds up to bound k (Satisfied at k)
 *   3. For liveness: check fair paths using ranking functions
 *
 * Complexity: O(|S| × |φ| × k) where S = states, k = bound
 * Typical performance: <10ms for plans with ≤50 nodes
 */
class PlanVerifier {
public:
    explicit PlanVerifier(VerifierConfig config = {});

    /// Verify a plan against all configured properties.
    PlanVerification verify(const std::vector<PlanNode>& plan) const;

    /// Verify a single formula against a plan.
    VerificationResult checkProperty(
        const KripkeModel& model,
        const std::string& name,
        FormulaPtr property) const;

    /// Access configuration.
    const VerifierConfig& config() const { return config_; }

private:
    /// Evaluates a CTL formula on a Kripke state (recursive descent).
    bool evaluate(const KripkeModel& model,
                  const std::string& state_id,
                  const Formula& formula,
                  std::uint32_t depth,
                  std::vector<TraceStep>& trace) const;

    /// Evaluates AG (always globally) — checks all reachable states.
    bool evaluateAG(const KripkeModel& model,
                    const std::string& state_id,
                    const Formula& sub,
                    std::uint32_t depth,
                    std::set<std::string>& visited,
                    std::vector<TraceStep>& trace) const;

    /// Evaluates AF (always eventually) — checks fair termination.
    bool evaluateAF(const KripkeModel& model,
                    const std::string& state_id,
                    const Formula& sub,
                    std::uint32_t depth,
                    std::set<std::string>& visited,
                    std::vector<TraceStep>& trace) const;

    /// Evaluates EF (exists eventually) — finds a satisfying path.
    bool evaluateEF(const KripkeModel& model,
                    const std::string& state_id,
                    const Formula& sub,
                    std::uint32_t depth,
                    std::set<std::string>& visited) const;

    /// Partial-Order Reduction: compute ample set for a state.
    /// Returns a subset of successor transitions that is sufficient
    /// to explore (independent transitions can be collapsed).
    /// Based on Peled's stubborn-set method (1993).
    std::vector<std::string> computeAmpleSet(
        const KripkeModel& model,
        const KripkeState& state,
        const Formula& property) const;

    /// Check if two transitions are independent (no shared resources,
    /// no data dependency, no conflicting labels).
    bool areIndependent(const KripkeModel& model,
                        const std::string& t1,
                        const std::string& t2) const;

    /// Build dependency graph for POR analysis.
    std::map<std::string, std::set<std::string>> buildDependencyGraph(
        const KripkeModel& model) const;

    VerifierConfig config_;
};

// ---------------------------------------------------------------------------
// Runtime Monitor (online verification during execution)
// ---------------------------------------------------------------------------

/// Events observed during plan execution.
struct ExecutionEvent {
    std::string node_id;
    std::string event_type;     // "started", "completed", "failed", "timeout"
    std::int64_t timestamp_ms;
    std::map<std::string, std::string> metadata;
};

/// Monitors temporal properties during live execution.
/// Based on: "Causal Past Logic for Runtime Verification" (arXiv:2605.20923)
class RuntimeMonitor {
public:
    explicit RuntimeMonitor(std::vector<FormulaPtr> properties);

    /// Feed an execution event to the monitor.
    /// Returns violated properties (empty if all still hold).
    std::vector<std::string> observe(const ExecutionEvent& event);

    /// Check if all monitored properties still hold.
    bool allSatisfied() const;

    /// Get violation details.
    std::vector<std::pair<std::string, std::string>> violations() const;

private:
    std::vector<FormulaPtr> properties_;
    std::vector<ExecutionEvent> trace_;
    std::vector<std::pair<std::string, std::string>> violations_;

    // Runtime state accumulated from events
    std::set<std::string> active_nodes_;       // currently executing
    std::set<std::string> completed_nodes_;    // finished successfully
    std::set<std::string> failed_nodes_;       // failed
    std::set<std::string> timed_out_nodes_;    // timed out
    std::set<std::string> authorized_nodes_;   // have been authorized
    std::set<std::string> destructive_active_; // destructive ops in flight
    std::map<std::string, std::string> node_states_;    // node → state string
    std::map<std::string, std::string> held_resources_; // node → resource
    std::map<std::string, std::int64_t> start_times_;   // node → start ts
    std::set<size_t> violated_indices_;         // already-violated property indices

    /// Update accumulated state from an event.
    void updateState(const ExecutionEvent& event);

    /// Check if a property is violated at the current event.
    bool checkPropertyViolation(const Formula& property,
                                const ExecutionEvent& event) const;

    /// Evaluate a formula as a state predicate against the current state.
    bool evaluateAtCurrentState(const Formula& formula,
                                const ExecutionEvent& event) const;

    /// Check a single atomic proposition against accumulated state.
    bool checkProposition(const std::string& prop,
                          const ExecutionEvent& event) const;

    /// Check if a formula has ever been true at any point in the trace.
    bool hasEverHeld(const Formula& formula) const;

    /// Generate human-readable violation description.
    std::string describeViolation(const Formula& property,
                                  const ExecutionEvent& event) const;
};

}  // namespace sparx::formal

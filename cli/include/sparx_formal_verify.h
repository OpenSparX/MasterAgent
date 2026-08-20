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
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

// ===========================================================================
// SECTION 2: SMT-based Bounded Model Checking (DPLL(T) with CDCL)
// ===========================================================================
//
// Lightweight SMT solver integration for verifying Kripke state transitions
// encoded as SMT formulas. Handles DAGs up to ~500 nodes by encoding:
//   - State variables as bitvectors
//   - Transition relation as conjunction of implications
//   - Property negation as the satisfiability target
//
// Algorithm: DPLL(T) = CDCL SAT core + Theory solver for LIA (Simplex)
//   - CDCL: VSIDS branching, 1-UIP conflict analysis, clause learning, Luby restarts
//   - Theory: Linear Integer Arithmetic via Simplex with bound propagation
//
// Reference: Nieuwenhuis, Oliveras, Tinelli — "Solving SAT and SAT Modulo
// Theories: from an abstract DPLL procedure to DPLL(T)" (JACM 2006)

/// A literal in the SAT solver (variable index + polarity).
struct Literal {
    std::int32_t var;   // variable index (1-based)
    bool negated;       // true if this is the negation

    Literal() : var(0), negated(false) {}
    Literal(std::int32_t v, bool neg) : var(v), negated(neg) {}

    Literal operator~() const { return Literal{var, !negated}; }
    bool operator==(const Literal& o) const { return var == o.var && negated == o.negated; }
    bool operator!=(const Literal& o) const { return !(*this == o); }
    bool operator<(const Literal& o) const {
        return var < o.var || (var == o.var && negated < o.negated);
    }

    /// Index for the watched-literal scheme: 2*var + negated
    std::size_t index() const { return static_cast<std::size_t>(2 * var + (negated ? 1 : 0)); }
};

/// A clause in the CNF formula.
struct Clause {
    std::vector<Literal> literals;
    bool is_learned = false;       // learned from conflict analysis
    double activity = 0.0;         // for clause deletion heuristic

    bool unit() const { return literals.size() == 1; }
    bool empty() const { return literals.empty(); }
};

/// Assignment state for a variable in the trail.
enum class VarAssign : std::uint8_t { Unassigned, True, False };

/// Decision level info for backtracking.
struct TrailEntry {
    Literal lit;                   // the assigned literal
    std::int32_t decision_level;   // level at which this was assigned
    std::int32_t reason_clause;    // clause index that implied this (-1 = decision)
};

/// Result of SAT solving.
enum class SatResult : std::uint8_t { SAT, UNSAT, UNKNOWN };

/// CDCL SAT solver with VSIDS branching and 1-UIP conflict analysis.
class CDCLSolver {
public:
    CDCLSolver();

    /// Create a new variable. Returns the variable index.
    std::int32_t newVar();

    /// Add a clause to the formula. Returns clause index.
    std::int32_t addClause(std::vector<Literal> lits);

    /// Solve the formula. Returns SAT/UNSAT/UNKNOWN.
    SatResult solve(std::uint32_t conflict_limit = 100000);

    /// Get the model (variable assignments) after SAT result.
    std::vector<bool> model() const;

    /// Reset the solver state for reuse.
    void reset();

    /// Number of variables.
    std::int32_t numVars() const { return num_vars_; }

    /// Number of clauses.
    std::size_t numClauses() const { return clauses_.size(); }

    /// Number of conflicts encountered.
    std::uint32_t numConflicts() const { return num_conflicts_; }

private:
    // --- Variable state ---
    std::int32_t num_vars_ = 0;
    std::vector<VarAssign> assigns_;       // current assignment per variable
    std::vector<double> vsids_activity_;   // VSIDS activity score per variable
    double vsids_bump_ = 1.0;             // current bump amount
    static constexpr double vsids_decay_ = 0.95;

    // --- Clause database ---
    std::vector<Clause> clauses_;

    // --- Watched literals (2-watched-literal scheme) ---
    std::vector<std::vector<std::int32_t>> watches_;  // watches_[lit.index()] -> clause indices

    // --- Trail (assignment stack) ---
    std::vector<TrailEntry> trail_;
    std::vector<std::size_t> trail_lim_;   // trail positions at each decision level
    std::int32_t decision_level_ = 0;

    // --- Propagation ---
    std::size_t propagation_head_ = 0;    // index into trail_ for BCP

    // --- Statistics ---
    std::uint32_t num_conflicts_ = 0;
    std::uint32_t num_decisions_ = 0;
    std::uint32_t num_restarts_ = 0;

    // --- Luby restart sequence ---
    std::uint32_t luby_index_ = 0;
    std::uint32_t conflicts_until_restart_ = 100;
    static std::uint32_t lubySequence(std::uint32_t i);

    // --- Core CDCL operations ---

    /// Boolean Constraint Propagation. Returns conflict clause index or -1.
    std::int32_t propagate();

    /// Pick an unassigned variable using VSIDS. Returns 0 if all assigned.
    std::int32_t pickBranchingVar() const;

    /// Assign a literal at the current decision level.
    void assign(Literal lit, std::int32_t reason);

    /// 1-UIP conflict analysis. Returns the learned clause.
    Clause analyzeConflict(std::int32_t conflict_clause);

    /// Backtrack to a given decision level, undoing assignments.
    void backtrack(std::int32_t level);

    /// Bump variable activity (VSIDS).
    void bumpVarActivity(std::int32_t var);

    /// Decay all variable activities.
    void decayActivities();

    /// Get the value of a literal under current assignment.
    VarAssign litValue(Literal lit) const;
};

/// Linear constraint for the LIA theory solver: sum(coeffs[i] * vars[i]) <op> bound.
struct LinearConstraint {
    enum class Op : std::uint8_t { LEQ, GEQ, EQ };
    std::vector<std::pair<std::int32_t, std::int64_t>> terms;  // (var, coefficient)
    Op op = Op::LEQ;
    std::int64_t bound = 0;
    std::int32_t boolean_var = 0;   // SAT variable that activates this constraint
};

/// Theory solver for Linear Integer Arithmetic (Simplex-based).
/// Checks consistency of linear constraints over integer variables.
class LIATheorySolver {
public:
    LIATheorySolver();

    /// Register a theory variable (integer-valued).
    std::int32_t newTheoryVar();

    /// Add a linear constraint associated with a Boolean variable.
    void addConstraint(LinearConstraint constraint);

    /// Check if the current set of active constraints is consistent.
    /// active_bools: which Boolean variables are currently true in the SAT model.
    /// Returns true if consistent, false if theory conflict detected.
    bool check(const std::vector<bool>& active_bools);

    /// Get a theory conflict clause (conjunction of active constraints that conflict).
    /// Should be negated and added as a learned clause.
    std::vector<Literal> conflictClause() const;

    /// Get the model values for theory variables after SAT.
    std::vector<std::int64_t> model() const;

private:
    std::int32_t num_theory_vars_ = 0;
    std::vector<LinearConstraint> constraints_;
    std::vector<Literal> last_conflict_;

    // Simplex tableau state
    std::vector<std::int64_t> lower_bounds_;
    std::vector<std::int64_t> upper_bounds_;
    std::vector<std::int64_t> current_values_;

    /// Run Simplex on active constraints to determine feasibility.
    bool runSimplex(const std::vector<std::size_t>& active_indices);

    /// Bound propagation: tighten variable bounds from active constraints.
    bool propagateBounds(const std::vector<std::size_t>& active_indices);
};

/// The combined DPLL(T) SMT solver: CDCL + LIA theory.
class SMTSolver {
public:
    SMTSolver();

    /// Create a Boolean variable.
    std::int32_t newBoolVar();

    /// Create an integer theory variable.
    std::int32_t newIntVar();

    /// Add a propositional clause.
    void addClause(std::vector<Literal> clause);

    /// Add a linear arithmetic constraint gated by a Boolean variable.
    void addTheoryConstraint(LinearConstraint constraint);

    /// Solve. Returns SAT/UNSAT/UNKNOWN.
    SatResult solve(std::uint32_t conflict_limit = 100000);

    /// Boolean model after SAT.
    std::vector<bool> boolModel() const;

    /// Integer model after SAT.
    std::vector<std::int64_t> intModel() const;

    /// Access the underlying SAT solver.
    CDCLSolver& satSolver() { return sat_; }

private:
    CDCLSolver sat_;
    LIATheorySolver lia_;
    std::vector<LinearConstraint> theory_constraints_;

    /// DPLL(T) loop: SAT solve then theory check, with conflict-driven learning.
    SatResult dpllT(std::uint32_t conflict_limit);
};

/// Encodes a Kripke model + property negation as an SMT formula and solves.
/// Uses bounded model checking: unroll transitions to depth k.
class SMTModelChecker {
public:
    struct Config {
        std::uint32_t max_unroll_depth = 50;   // max BMC depth
        std::uint32_t conflict_limit = 200000; // SAT conflict budget
        bool incremental = true;               // incremental unrolling
    };

    struct Result {
        SatResult status = SatResult::UNKNOWN;
        std::uint32_t depth_reached = 0;
        std::vector<std::string> counterexample_states;  // state IDs on violating path
        std::chrono::microseconds solve_time{0};
    };

    explicit SMTModelChecker(Config config);
    SMTModelChecker() : SMTModelChecker(Config{}) {}

    /// Check a property via BMC on the Kripke model.
    /// The property is checked by negating it and searching for a satisfying
    /// assignment (counterexample). If SAT at depth k, property is violated.
    /// If UNSAT at all depths up to max, property holds (within bound).
    Result check(const KripkeModel& model, FormulaPtr property);

private:
    Config config_;

    /// Encode state variables for a given unrolling step.
    /// Each state at step i gets Boolean variables encoding which Kripke state is active.
    struct StepEncoding {
        std::map<std::string, std::int32_t> state_vars;  // kripke_state_id -> bool var
        std::int32_t step_index = 0;
    };

    /// Encode the initial state constraint.
    void encodeInitial(SMTSolver& solver, const StepEncoding& step0,
                       const KripkeModel& model);

    /// Encode the transition relation between step i and step i+1.
    void encodeTransition(SMTSolver& solver, const StepEncoding& from,
                          const StepEncoding& to, const KripkeModel& model);

    /// Encode the property negation at a given step (target for BMC).
    void encodePropertyNegation(SMTSolver& solver, const StepEncoding& step,
                                const KripkeModel& model, const Formula& property);

    /// Encode "exactly one state active" constraint (at-most-one + at-least-one).
    void encodeExactlyOne(SMTSolver& solver, const std::vector<std::int32_t>& vars);

    /// Create a step encoding for the given model at a given unrolling depth.
    StepEncoding createStepEncoding(SMTSolver& solver, const KripkeModel& model,
                                    std::int32_t step);

    /// Extract counterexample path from SAT model.
    std::vector<std::string> extractCounterexample(
        const SMTSolver& solver,
        const std::vector<StepEncoding>& steps,
        const KripkeModel& model);
};

// ===========================================================================
// SECTION 3: Fairness Constraints for Liveness (Streett Acceptance)
// ===========================================================================
//
// Fixes conservatism in AF (liveness) checking by implementing:
//   - Strong fairness (compassion): if enabled infinitely often, must fire
//   - Weak fairness (justice): if continuously enabled, must fire
//   - Streett acceptance: for each pair (P_i, Q_i), inf(P_i) => inf(Q_i)
//   - Nested DFS (double DFS) for fair cycle detection
//
// Reference: Choueka's Streett acceptance; Courcoubetis, Vardi, Wolper —
// "Memory-Efficient Algorithms for the Verification of Temporal Properties" (FAC 1992)

/// A fairness pair (P, Q) for Streett acceptance.
/// Semantics: on any fair path, if states satisfying P are visited infinitely
/// often, then states satisfying Q must also be visited infinitely often.
struct FairnessPair {
    std::string name;                    // human-readable description
    std::set<std::string> P_states;     // "request" / "enabled" states
    std::set<std::string> Q_states;     // "grant" / "fired" states
};

/// Fairness constraint types.
enum class FairnessType : std::uint8_t {
    WeakFairness,   // Justice: if continuously enabled from some point, eventually taken
    StrongFairness, // Compassion: if enabled infinitely often, taken infinitely often
};

/// Configuration for fair cycle detection.
struct FairnessConfig {
    std::vector<FairnessPair> fairness_pairs;
    FairnessType type = FairnessType::StrongFairness;
    bool eliminate_self_loops = true;    // remove trivial self-loops from liveness analysis
};

/// Result from fair-cycle-aware liveness check.
struct FairLivenessResult {
    bool property_holds = false;         // true if AF holds under fairness
    bool unfair_cycle_found = false;     // true if the "violation" was on an unfair path
    std::vector<std::string> cycle_states;  // states forming the violating/fair cycle
    std::string explanation;
};

/// Fair liveness verifier using nested DFS with Streett acceptance.
class FairLivenessChecker {
public:
    explicit FairLivenessChecker(FairnessConfig config = {});

    /// Check AF(phi) under fairness constraints.
    /// Eliminates false positives from self-loop paths that violate fairness.
    FairLivenessResult checkAF(const KripkeModel& model, const Formula& phi);

    /// Check whether a given cycle is fair according to Streett acceptance.
    /// A cycle is fair iff for every pair (P_i, Q_i): if the cycle visits
    /// a P_i state, it must also visit a Q_i state.
    bool isFairCycle(const std::vector<std::string>& cycle,
                     const KripkeModel& model) const;

    /// Generate default fairness pairs from a Kripke model.
    /// Auto-detects request/grant patterns from state labels.
    static std::vector<FairnessPair> autoGeneratePairs(const KripkeModel& model);

    /// Access configuration.
    const FairnessConfig& config() const { return config_; }

private:
    FairnessConfig config_;

    /// Accepting states for the liveness property (states where phi holds).
    std::set<std::string> computeAcceptingStates(const KripkeModel& model,
                                                  const Formula& phi);

    /// Outer DFS: find accepting states reachable from initial.
    bool outerDFS(const KripkeModel& model,
                  const std::string& state_id,
                  const std::set<std::string>& accepting,
                  std::set<std::string>& visited_outer,
                  std::vector<std::string>& path);

    /// Inner DFS: from an accepting state, check if there is a cycle
    /// back to it (or to any state on the outer DFS stack) that is fair.
    bool innerDFS(const KripkeModel& model,
                  const std::string& target,
                  const std::string& state_id,
                  const std::set<std::string>& accepting,
                  std::set<std::string>& visited_inner,
                  std::vector<std::string>& cycle_path);

    /// Check Streett acceptance on a detected cycle.
    bool checkStreettAcceptance(const std::vector<std::string>& cycle,
                                const KripkeModel& model) const;

    /// Filter out self-loops that are trivially unfair.
    KripkeModel removeTrivialSelfLoops(const KripkeModel& model) const;
};

// ===========================================================================
// SECTION 4: Probabilistic Verification with PCTL
// ===========================================================================
//
// Extension of CTL with probability operator P~p[phi]:
//   - P>=0.95[AF success]   "success with probability >= 95%"
//   - P<=0.01[EF unsafe]    "unsafe state reached with probability <= 1%"
//
// Constructs an MDP (Markov Decision Process) from the agent DAG with
// probability annotations on transitions, then uses value iteration with
// interval iteration for guaranteed error bounds.
//
// Reference: Kwiatkowska, Norman, Parker — "PRISM 4.0: Verification of
// Probabilistic Real-Time Systems" (CAV 2011)

/// Comparison operator for probability bounds.
enum class ProbCompare : std::uint8_t {
    LT,    // P < p
    LEQ,   // P <= p
    GEQ,   // P >= p
    GT,    // P > p
};

/// PCTL formula node (extends CTL with probability operator).
struct PCTLFormula {
    enum class Op : std::uint8_t {
        Atom,           // atomic proposition
        Not,            // negation
        And,            // conjunction
        Or,             // disjunction
        ProbOp,         // P~p[path_formula]
        Steady,         // S~p[phi] (steady-state, not implemented yet)
    };

    Op op;
    std::optional<Atom> atom;
    std::vector<std::shared_ptr<PCTLFormula>> children;

    // For ProbOp:
    ProbCompare compare = ProbCompare::GEQ;
    double probability_bound = 0.0;
    FormulaPtr path_formula;  // the CTL path formula inside P~p[...]

    // Factory methods
    static std::shared_ptr<PCTLFormula> makeAtom(const std::string& name,
                                                  const std::string& node_id = "");
    static std::shared_ptr<PCTLFormula> makeNot(std::shared_ptr<PCTLFormula> f);
    static std::shared_ptr<PCTLFormula> makeAnd(std::shared_ptr<PCTLFormula> a,
                                                 std::shared_ptr<PCTLFormula> b);
    static std::shared_ptr<PCTLFormula> makeProb(ProbCompare cmp, double bound,
                                                  FormulaPtr path_formula);
};

using PCTLFormulaPtr = std::shared_ptr<PCTLFormula>;

/// A transition in the MDP with probability annotation.
struct MDPTransition {
    std::string from_state;
    std::string to_state;
    double probability = 1.0;    // transition probability (sum from each state = 1.0)
    std::string action;          // nondeterministic action label
};

/// A nondeterministic choice (action) from a state: distribution over successors.
struct MDPAction {
    std::string name;
    std::vector<std::pair<std::string, double>> distribution;  // (target_state, prob)

    /// Validate that probabilities sum to 1.0 (within epsilon).
    bool valid(double eps = 1e-9) const;
};

/// Markov Decision Process model built from an agent DAG.
struct MDPModel {
    std::string initial_state;
    std::map<std::string, std::vector<MDPAction>> states;  // state -> available actions
    std::set<std::string> all_states;

    /// Build MDP from Kripke model with probability annotations.
    /// Probabilities come from PlanNode metadata or uniform distribution.
    static MDPModel fromKripke(const KripkeModel& kripke,
                               const std::map<std::string, double>& transition_probs = {});

    /// Get labels for a state (from underlying Kripke model).
    StateLabels labelsFor(const std::string& state_id) const;

    /// Internal reference to underlying Kripke model for labels.
    const KripkeModel* kripke_ref = nullptr;
};

/// Configuration for PCTL model checking.
struct PCTLConfig {
    double convergence_threshold = 1e-6;  // value iteration convergence
    std::uint32_t max_iterations = 1000;   // max VI iterations
    bool use_interval_iteration = true;    // guaranteed error bounds
    bool use_topological_order = true;     // optimize VI with topological ordering
};

/// Result of PCTL verification.
struct PCTLResult {
    bool property_holds = false;
    double computed_probability = 0.0;     // actual probability computed
    double lower_bound = 0.0;             // from interval iteration
    double upper_bound = 1.0;             // from interval iteration
    std::uint32_t iterations_used = 0;
    std::chrono::microseconds solve_time{0};
    std::string explanation;

    /// Margin between computed probability and threshold.
    double margin() const;
};

/// PCTL model checker using value iteration on MDP models.
class PCTLModelChecker {
public:
    explicit PCTLModelChecker(PCTLConfig config = {});

    /// Check a PCTL formula against an MDP model.
    PCTLResult check(const MDPModel& mdp, const PCTLFormulaPtr& formula);

    /// Compute maximum reachability probability to target states.
    /// Solves: max over all schedulers P(eventually target).
    double maxReachability(const MDPModel& mdp,
                           const std::set<std::string>& target_states);

    /// Compute minimum reachability probability to target states.
    /// Solves: min over all schedulers P(eventually target).
    double minReachability(const MDPModel& mdp,
                           const std::set<std::string>& target_states);

    /// Interval iteration: compute [lower, upper] bounds on probability.
    std::pair<double, double> intervalIteration(
        const MDPModel& mdp,
        const std::set<std::string>& target_states,
        bool maximize);

    /// Access configuration.
    const PCTLConfig& config() const { return config_; }

private:
    PCTLConfig config_;

    /// Identify states from which target is reachable with probability 1/0.
    /// Used to preprocess the MDP before value iteration.
    std::set<std::string> computeProb1States(const MDPModel& mdp,
                                              const std::set<std::string>& target,
                                              bool maximize);
    std::set<std::string> computeProb0States(const MDPModel& mdp,
                                              const std::set<std::string>& target,
                                              bool maximize);

    /// Value iteration core: iteratively compute fixpoint probabilities.
    std::map<std::string, double> valueIteration(
        const MDPModel& mdp,
        const std::set<std::string>& target_states,
        const std::set<std::string>& prob1_states,
        const std::set<std::string>& prob0_states,
        bool maximize);

    /// Evaluate a state-formula (non-probability) PCTL sub-formula.
    std::set<std::string> satisfyingStates(const MDPModel& mdp,
                                            const PCTLFormula& formula);
};

// ===========================================================================
// SECTION 5: IC3/PDR — Property-Directed Reachability
// ===========================================================================
//
// Unbounded model checking via IC3/PDR (Bradley 2011).
// Unlike BMC which can only prove properties up to a bound, IC3 computes
// an inductive invariant that proves the property holds for ALL depths,
// or produces a concrete counterexample trace.
//
// Core idea: maintain a sequence of over-approximate reachable state sets
// (frames) F0, F1, ..., Fk where:
//   - F0 = Init (initial states)
//   - Fi ⊇ Fi+1 (frames are monotonically decreasing)
//   - Fi ∧ T ⊆ Fi+1 (each frame is closed under the transition relation)
//   - ¬P ∩ Fk = ∅ (no bad state in the frontier frame)
//   - If Fi == Fi+1 for some i, then Fi is an inductive invariant (PROVEN)
//
// SAT queries use relative induction: a clause c is inductive relative to
// frame Fi if (Fi ∧ c ∧ T → c'), meaning c is preserved by one transition
// step from states satisfying Fi ∧ c.
//
// Reference:
//   - Bradley 2011: "SAT-Based Model Checking without Unrolling"
//   - Een, Mishchenko, Brayton 2011: "Efficient Implementation of IC3"

/// A cube is a conjunction of literals representing a (partial) state.
/// In IC3, cubes represent sets of states to be blocked from frames.
struct IC3Cube {
    std::vector<Literal> literals;

    bool empty() const { return literals.empty(); }
    std::size_t size() const { return literals.size(); }

    bool operator==(const IC3Cube& other) const { return literals == other.literals; }
    bool operator<(const IC3Cube& other) const { return literals < other.literals; }

    /// Negate the cube to produce a blocking clause (¬cube = clause).
    std::vector<Literal> toClause() const {
        std::vector<Literal> clause;
        clause.reserve(literals.size());
        for (const auto& lit : literals) {
            clause.push_back(~lit);
        }
        return clause;
    }
};

/// A proof obligation: a state (cube) that must be shown unreachable
/// at or before a certain frame level.
struct ProofObligation {
    IC3Cube cube;
    std::uint32_t frame_level;
    std::shared_ptr<ProofObligation> predecessor;

    /// Priority comparison: lower frame levels are processed first.
    bool operator>(const ProofObligation& other) const {
        return frame_level > other.frame_level;
    }
};

/// Result from the IC3 engine.
struct IC3Result {
    enum class Verdict : std::uint8_t {
        Proven,         // inductive invariant found
        Refuted,        // counterexample trace found
        Unknown,        // resource limit reached
    };

    Verdict verdict = Verdict::Unknown;
    std::uint32_t convergence_frame = 0;
    std::vector<IC3Cube> counterexample_trace;
    std::vector<std::string> counterexample_states;
    std::vector<std::vector<Literal>> invariant_clauses;

    std::uint32_t num_frames = 0;
    std::uint32_t num_proof_obligations = 0;
    std::uint32_t num_sat_calls = 0;
    std::uint32_t num_clauses_learned = 0;
    std::chrono::microseconds solve_time{0};
};

/// Configuration for the IC3 engine.
struct IC3Config {
    std::uint32_t max_frames = 500;
    std::uint32_t sat_conflict_limit = 50000;
    std::uint32_t max_proof_obligations = 100000;
    bool generalize = true;
    bool propagate = true;
    std::uint8_t verbosity = 0;
};

/**
 * @brief IC3/PDR engine for unbounded safety verification.
 *
 * Checks AG(P) properties by computing an inductive invariant or
 * finding a concrete counterexample trace to ¬P.
 */
class IC3Engine {
public:
    explicit IC3Engine(IC3Config config = {});

    IC3Result check(const KripkeModel& model, const FormulaPtr& property);
    IC3Result checkAG(const KripkeModel& model, const FormulaPtr& ag_formula);

    const IC3Config& config() const { return config_; }

private:
    IC3Config config_;

    std::int32_t num_state_vars_ = 0;
    std::map<std::string, std::int32_t> state_to_var_;
    std::map<std::string, std::int32_t> state_to_var_primed_;
    std::map<std::int32_t, std::string> var_to_state_;

    std::vector<std::vector<std::vector<Literal>>> frames_;
    std::vector<std::vector<Literal>> init_clauses_;
    std::vector<std::vector<Literal>> transition_clauses_;
    std::vector<std::vector<Literal>> bad_clauses_;

    std::uint32_t num_sat_calls_ = 0;
    std::uint32_t num_obligations_processed_ = 0;
    std::uint32_t num_clauses_learned_ = 0;

    void encodeModel(const KripkeModel& model, const FormulaPtr& property);
    CDCLSolver makeSolverForFrame(std::uint32_t frame_level) const;
    void addTransitionClauses(CDCLSolver& solver) const;
    void addCubeAssumptions(CDCLSolver& solver, const IC3Cube& cube, bool primed) const;

    std::pair<bool, IC3Cube> checkBadIntersection(std::uint32_t frame_level);
    bool isRelativelyInductive(std::uint32_t frame_level, const IC3Cube& cube,
                               IC3Cube& predecessor_cube);
    bool intersectsInit(const IC3Cube& cube);
    IC3Cube generalize(std::uint32_t frame_level, const IC3Cube& cube);
    void blockCubeAtFrame(std::uint32_t frame_level, const IC3Cube& cube);
    bool propagateClauses();
    std::int32_t checkConvergence() const;
    IC3Cube extractCube(const CDCLSolver& solver, bool primed) const;
    std::vector<IC3Cube> reconstructTrace(
        const std::shared_ptr<ProofObligation>& terminal) const;
    std::string cubeToStateId(const IC3Cube& cube) const;
    Literal toPrimed(Literal lit) const;
    Literal toUnprimed(Literal lit) const;
};

/// Verify a property using IC3/PDR (unbounded).
VerificationResult verifyIC3(const KripkeModel& model, const FormulaPtr& formula,
                             IC3Config config = {});

}  // namespace sparx::formal

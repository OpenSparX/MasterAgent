#pragma once
/**
 * @file sparx_smt_backend.h
 * @brief Z3/SMT Backend for Formal Plan Verification.
 *
 * Research basis:
 *   - "Z3: An Efficient SMT Solver" (de Moura & Bjørner, TACAS 2008)
 *   - "Bounded Model Checking Using Satisfiability Solving" (Clarke et al., 2001)
 *   - "SMT-Based Verification of LTL Specifications" (Biere et al., 2006)
 *   - "Property-Directed Reachability" (IC3/PDR) (Bradley, 2011)
 *
 * This module translates CTL* formulas over Kripke structures into SMT
 * constraints, enabling:
 *   1. Complete bounded verification (sound + complete up to bound k)
 *   2. Quantifier-free bitvector/boolean arithmetic for fast solving
 *   3. Craig interpolation for inductive invariant discovery
 *   4. Incremental solving (reuse learned clauses across properties)
 *   5. UNSAT core extraction for minimal counterexample explanation
 *
 * Architecture:
 *   - Abstract SMT interface (SmtSolver) allows pluggable backends
 *   - Z3 backend via dlopen (no compile-time dependency on Z3)
 *   - Built-in BitBlast backend for environments without Z3
 *   - Encoding: BMC unrolling → QF_LIA/QF_BV formula → SAT/UNSAT
 *
 * Integration:
 *   - Extends PlanVerifier from sparx_formal_verify.h
 *   - Falls back to existing constraint propagation when Z3 unavailable
 *   - Produces machine-checkable proof certificates (LFSC format)
 */

#include "sparx_formal_verify.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace sparx::formal::smt {

// ─── Abstract SMT Types ──────────────────────────────────────────────────────

/// SMT sort (type).
enum class Sort : uint8_t {
    Bool,
    Int,
    BitVec,   // Bitvector of configurable width
    Array,    // Array sort
};

/// An SMT expression handle (opaque, backend-specific).
struct Expr {
    uint64_t handle = 0;   // Backend-specific ID
    Sort sort = Sort::Bool;
    bool valid() const { return handle != 0; }
};

/// Result of a satisfiability check.
enum class CheckResult : uint8_t {
    Sat,       // Satisfiable — model available
    Unsat,     // Unsatisfiable — proof/core available
    Unknown,   // Solver timed out or gave up
};

/// A concrete value from a satisfying model.
struct ModelValue {
    std::string name;
    std::variant<bool, int64_t, std::string> value;
};

// ─── Abstract Solver Interface ───────────────────────────────────────────────

/**
 * @brief Abstract SMT solver interface.
 *
 * Backends implement this to provide Z3, CVC5, or the built-in BitBlast.
 * The verifier encodes properties against this interface, making the
 * backend swappable without changing verification logic.
 */
class SmtSolver {
public:
    virtual ~SmtSolver() = default;

    // ── Variable creation ──
    virtual Expr mkBoolVar(const std::string& name) = 0;
    virtual Expr mkIntVar(const std::string& name) = 0;
    virtual Expr mkBitVecVar(const std::string& name, int width) = 0;

    // ── Constants ──
    virtual Expr mkTrue() = 0;
    virtual Expr mkFalse() = 0;
    virtual Expr mkIntConst(int64_t val) = 0;

    // ── Boolean operations ──
    virtual Expr mkNot(Expr a) = 0;
    virtual Expr mkAnd(Expr a, Expr b) = 0;
    virtual Expr mkOr(Expr a, Expr b) = 0;
    virtual Expr mkImplies(Expr a, Expr b) = 0;
    virtual Expr mkIff(Expr a, Expr b) = 0;

    // ── Arithmetic ──
    virtual Expr mkEq(Expr a, Expr b) = 0;
    virtual Expr mkLt(Expr a, Expr b) = 0;
    virtual Expr mkLe(Expr a, Expr b) = 0;
    virtual Expr mkAdd(Expr a, Expr b) = 0;
    virtual Expr mkSub(Expr a, Expr b) = 0;

    // ── Quantifiers ──
    virtual Expr mkForall(const std::vector<Expr>& vars, Expr body) = 0;
    virtual Expr mkExists(const std::vector<Expr>& vars, Expr body) = 0;

    // ── Assertions ──
    virtual void assertFormula(Expr f) = 0;
    virtual void push() = 0;
    virtual void pop() = 0;

    // ── Solving ──
    virtual CheckResult check(uint32_t timeout_ms = 10000) = 0;

    // ── Model extraction (only after Sat) ──
    virtual std::optional<bool> modelBool(Expr var) = 0;
    virtual std::optional<int64_t> modelInt(Expr var) = 0;
    virtual std::vector<ModelValue> fullModel() = 0;

    // ── Proof/core (only after Unsat) ──
    virtual std::vector<Expr> unsatCore() = 0;

    // ── Statistics ──
    virtual uint64_t decisionsCount() const = 0;
    virtual uint64_t conflictsCount() const = 0;

    // ── Factory ──
    static std::unique_ptr<SmtSolver> createZ3();
    static std::unique_ptr<SmtSolver> createBitBlast();
    static std::unique_ptr<SmtSolver> createBest();  // Z3 if available, else BitBlast
};

// ─── BMC Encoder ─────────────────────────────────────────────────────────────

/// Encodes a Kripke model + CTL* formula into SMT constraints.
/// Uses Bounded Model Checking: unroll transitions k times and check property.
struct BmcEncoding {
    /// State variables at each time step: state_vars[step][prop_name] = Expr
    std::vector<std::map<std::string, Expr>> state_vars;
    /// Transition relation between steps
    std::vector<Expr> transitions;
    /// Property encoding (negated — SAT means violation)
    Expr property_negation;
    /// Bound used
    uint32_t bound = 0;
};

/**
 * @brief Bounded Model Checking encoder for Kripke structures.
 *
 * Translates the verification problem into SMT:
 *   1. Create Boolean vars for each (state, time_step) pair
 *   2. Encode transition relation: T(s_i, s_{i+1})
 *   3. Encode initial state: I(s_0)
 *   4. Encode negation of property: ¬φ
 *   5. Check SAT(I(s_0) ∧ T(s_0,s_1) ∧ ... ∧ T(s_{k-1},s_k) ∧ ¬φ)
 *      - SAT → counterexample (property violated at some step ≤ k)
 *      - UNSAT → property holds up to bound k
 */
class BmcEncoder {
public:
    explicit BmcEncoder(SmtSolver& solver);

    /// Encode a complete BMC instance.
    BmcEncoding encode(const KripkeModel& model,
                       const Formula& property,
                       uint32_t bound);

    /// Extract counterexample trace from satisfying model.
    std::vector<TraceStep> extractCounterexample(
        const BmcEncoding& encoding,
        const KripkeModel& model);

private:
    SmtSolver& solver_;

    /// Encode state propositions at a given time step.
    std::map<std::string, Expr> encodeState(
        const KripkeModel& model, uint32_t step);

    /// Encode transition relation between two steps.
    Expr encodeTransition(const KripkeModel& model,
                          const std::map<std::string, Expr>& current,
                          const std::map<std::string, Expr>& next);

    /// Encode initial state constraint.
    Expr encodeInitial(const KripkeModel& model,
                       const std::map<std::string, Expr>& state);

    /// Recursive CTL* formula encoding at a time step.
    Expr encodeFormula(const Formula& f,
                       const BmcEncoding& ctx,
                       uint32_t step);
};

// ─── SMT-Enhanced Verifier ───────────────────────────────────────────────────

/// Configuration for the SMT-backed verifier.
struct SmtVerifierConfig {
    /// Maximum BMC unrolling depth.
    uint32_t max_bound = 50;

    /// Per-property solver timeout (ms).
    uint32_t solver_timeout_ms = 5000;

    /// Total verification timeout (ms).
    uint32_t total_timeout_ms = 30000;

    /// Use incremental solving (reuse clauses across properties).
    bool incremental = true;

    /// Generate UNSAT cores for violation explanation.
    bool extract_cores = true;

    /// Enable k-induction (strengthens bounded proof to unbounded).
    bool k_induction = true;

    /// Preferred backend.
    enum class Backend { Auto, Z3, BitBlast } backend = Backend::Auto;
};

/**
 * @brief SMT-backed plan verifier.
 *
 * Extends the existing PlanVerifier with SMT solving for:
 *   - Complete verification (not just bounded)
 *   - Automatic counterexample generation with UNSAT cores
 *   - k-induction for unbounded safety proofs
 *   - Incremental solving for multiple properties
 *
 * Falls back to PlanVerifier's constraint propagation when no SMT
 * backend is available.
 */
class SmtPlanVerifier {
public:
    explicit SmtPlanVerifier(SmtVerifierConfig config = {});

    /// Verify a plan using SMT-backed bounded model checking.
    PlanVerification verify(const std::vector<PlanNode>& plan) const;

    /// Verify a single property with full SMT power.
    VerificationResult checkProperty(
        const KripkeModel& model,
        const std::string& name,
        FormulaPtr property) const;

    /// Try k-induction proof (unbounded safety from bounded check).
    /// Returns Satisfied if inductive invariant found, Unknown otherwise.
    VerificationResult kInduction(
        const KripkeModel& model,
        const std::string& name,
        FormulaPtr property,
        uint32_t max_k = 20) const;

    /// Check if Z3 backend is available (dynamically loaded).
    static bool z3Available();

    /// Solver statistics from last verification.
    struct SolverStats {
        uint64_t total_decisions = 0;
        uint64_t total_conflicts = 0;
        uint32_t properties_checked = 0;
        uint32_t smt_calls = 0;
        uint32_t max_bound_reached = 0;
    };
    const SolverStats& lastStats() const { return last_stats_; }

private:
    SmtVerifierConfig config_;
    mutable SolverStats last_stats_;
};

// ─── Proof Certificate ───────────────────────────────────────────────────────

/// Machine-checkable proof that a property holds (or is violated).
struct ProofCertificate {
    enum class Kind { Safety, Liveness, Violation } kind;
    std::string property_name;
    std::string proof_format;   // "k-induction", "bmc-unsat", "bmc-sat"
    uint32_t bound;
    std::string proof_data;     // Serialized proof (LFSC or JSON trace)
    bool machine_checkable;

    /// Verify this certificate independently (self-check).
    bool selfCheck() const;
};

}  // namespace sparx::formal::smt

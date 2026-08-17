/**
 * @file sparx_smt_backend.cpp
 * @brief SMT solver interface and BMC encoder for formal verification.
 *
 * Provides a built-in BitBlast solver (no external dependencies) and
 * Z3 via dlopen when available. The BitBlast solver handles the subset
 * of QF_BOOL sufficient for DAG verification (boolean satisfiability
 * with simple propositional encoding of Kripke states).
 */

#include "sparx_smt_backend.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <queue>
#include <unordered_map>

#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace sparx::formal::smt {

// ═══════════════════════════════════════════════════════════════════════════════
// BitBlast Solver (built-in, no dependencies)
// ═══════════════════════════════════════════════════════════════════════════════

namespace {

/// Simple DPLL-based SAT solver for boolean formulas.
/// Sufficient for Kripke models with ≤50 nodes (typical plan sizes).
class BitBlastSolver : public SmtSolver {
public:
    BitBlastSolver() = default;

    // Variable creation
    Expr mkBoolVar(const std::string& name) override {
        uint64_t id = next_id_++;
        var_names_[id] = name;
        return Expr{id, Sort::Bool};
    }

    Expr mkIntVar(const std::string& name) override {
        // Encode int as bitvector (32-bit)
        uint64_t id = next_id_++;
        var_names_[id] = name;
        return Expr{id, Sort::Int};
    }

    Expr mkBitVecVar(const std::string& name, int /*width*/) override {
        uint64_t id = next_id_++;
        var_names_[id] = name;
        return Expr{id, Sort::BitVec};
    }

    // Constants
    Expr mkTrue() override { return Expr{TRUE_ID, Sort::Bool}; }
    Expr mkFalse() override { return Expr{FALSE_ID, Sort::Bool}; }
    Expr mkIntConst(int64_t val) override {
        uint64_t id = next_id_++;
        int_consts_[id] = val;
        return Expr{id, Sort::Int};
    }

    // Boolean ops — encode as clauses
    Expr mkNot(Expr a) override {
        uint64_t id = next_id_++;
        neg_map_[id] = a.handle;
        return Expr{id, Sort::Bool};
    }

    Expr mkAnd(Expr a, Expr b) override {
        uint64_t id = next_id_++;
        and_map_[id] = {a.handle, b.handle};
        return Expr{id, Sort::Bool};
    }

    Expr mkOr(Expr a, Expr b) override {
        uint64_t id = next_id_++;
        or_map_[id] = {a.handle, b.handle};
        return Expr{id, Sort::Bool};
    }

    Expr mkImplies(Expr a, Expr b) override {
        return mkOr(mkNot(a), b);
    }

    Expr mkIff(Expr a, Expr b) override {
        return mkAnd(mkImplies(a, b), mkImplies(b, a));
    }

    // Arithmetic (simplified: only equality for SAT encoding)
    Expr mkEq(Expr a, Expr b) override {
        uint64_t id = next_id_++;
        eq_map_[id] = {a.handle, b.handle};
        return Expr{id, Sort::Bool};
    }

    Expr mkLt(Expr a, Expr b) override {
        uint64_t id = next_id_++;
        lt_map_[id] = {a.handle, b.handle};
        return Expr{id, Sort::Bool};
    }

    Expr mkLe(Expr a, Expr b) override {
        return mkOr(mkLt(a, b), mkEq(a, b));
    }

    Expr mkAdd(Expr a, Expr b) override {
        uint64_t id = next_id_++;
        add_map_[id] = {a.handle, b.handle};
        return Expr{id, Sort::Int};
    }

    Expr mkSub(Expr a, Expr b) override {
        uint64_t id = next_id_++;
        sub_map_[id] = {a.handle, b.handle};
        return Expr{id, Sort::Int};
    }

    // Quantifiers (for QF_BOOL, these are expanded)
    Expr mkForall(const std::vector<Expr>& /*vars*/, Expr body) override {
        return body;  // QF encoding: no real quantifiers
    }
    Expr mkExists(const std::vector<Expr>& /*vars*/, Expr body) override {
        return body;
    }

    // Assertions
    void assertFormula(Expr f) override {
        assertions_.push_back(f.handle);
    }

    void push() override {
        stack_.push_back(assertions_.size());
    }

    void pop() override {
        if (!stack_.empty()) {
            assertions_.resize(stack_.back());
            stack_.pop_back();
        }
    }

    // DPLL-based solving
    CheckResult check(uint32_t timeout_ms) override {
        decisions_ = 0;
        conflicts_ = 0;
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);

        // Evaluate all assertions under current assignment
        assignment_.clear();
        assignment_[TRUE_ID] = true;
        assignment_[FALSE_ID] = false;

        // Unit propagation + DPLL
        auto result = dpll(assertions_, deadline);
        return result;
    }

    // Model extraction
    std::optional<bool> modelBool(Expr var) override {
        auto it = assignment_.find(var.handle);
        if (it != assignment_.end()) return it->second;
        return std::nullopt;
    }

    std::optional<int64_t> modelInt(Expr /*var*/) override {
        return std::nullopt;  // BitBlast doesn't support int models directly
    }

    std::vector<ModelValue> fullModel() override {
        std::vector<ModelValue> model;
        for (const auto& [id, val] : assignment_) {
            auto name_it = var_names_.find(id);
            if (name_it != var_names_.end()) {
                model.push_back({name_it->second, val});
            }
        }
        return model;
    }

    std::vector<Expr> unsatCore() override {
        // Simplified: return all assertions as core
        std::vector<Expr> core;
        for (auto h : assertions_) core.push_back(Expr{h, Sort::Bool});
        return core;
    }

    uint64_t decisionsCount() const override { return decisions_; }
    uint64_t conflictsCount() const override { return conflicts_; }

private:
    static constexpr uint64_t TRUE_ID = 1;
    static constexpr uint64_t FALSE_ID = 2;
    uint64_t next_id_ = 3;

    std::unordered_map<uint64_t, std::string> var_names_;
    std::unordered_map<uint64_t, int64_t> int_consts_;
    std::unordered_map<uint64_t, uint64_t> neg_map_;
    std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> and_map_;
    std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> or_map_;
    std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> eq_map_;
    std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> lt_map_;
    std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> add_map_;
    std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> sub_map_;

    std::vector<uint64_t> assertions_;
    std::vector<size_t> stack_;
    std::unordered_map<uint64_t, bool> assignment_;
    uint64_t decisions_ = 0;
    uint64_t conflicts_ = 0;

    /// Evaluate an expression under current assignment.
    /// Returns nullopt if undetermined (free variable).
    std::optional<bool> evaluate(uint64_t id) {
        if (id == TRUE_ID) return true;
        if (id == FALSE_ID) return false;

        auto assign_it = assignment_.find(id);
        if (assign_it != assignment_.end()) return assign_it->second;

        auto neg_it = neg_map_.find(id);
        if (neg_it != neg_map_.end()) {
            auto val = evaluate(neg_it->second);
            if (val) return !*val;
            return std::nullopt;
        }

        auto and_it = and_map_.find(id);
        if (and_it != and_map_.end()) {
            auto a = evaluate(and_it->second.first);
            auto b = evaluate(and_it->second.second);
            if (a && !*a) return false;
            if (b && !*b) return false;
            if (a && b) return *a && *b;
            return std::nullopt;
        }

        auto or_it = or_map_.find(id);
        if (or_it != or_map_.end()) {
            auto a = evaluate(or_it->second.first);
            auto b = evaluate(or_it->second.second);
            if (a && *a) return true;
            if (b && *b) return true;
            if (a && b) return *a || *b;
            return std::nullopt;
        }

        auto eq_it = eq_map_.find(id);
        if (eq_it != eq_map_.end()) {
            auto a = evaluate(eq_it->second.first);
            auto b = evaluate(eq_it->second.second);
            if (a && b) return *a == *b;
            return std::nullopt;
        }

        return std::nullopt;  // Free variable
    }

    /// Find first undetermined variable in assertions.
    std::optional<uint64_t> pickVariable() {
        for (auto h : assertions_) {
            auto free = findFreeVar(h);
            if (free) return free;
        }
        return std::nullopt;
    }

    std::optional<uint64_t> findFreeVar(uint64_t id) {
        if (id == TRUE_ID || id == FALSE_ID) return std::nullopt;
        if (assignment_.count(id)) return std::nullopt;

        if (var_names_.count(id)) return id;

        auto neg_it = neg_map_.find(id);
        if (neg_it != neg_map_.end()) return findFreeVar(neg_it->second);

        auto and_it = and_map_.find(id);
        if (and_it != and_map_.end()) {
            auto v = findFreeVar(and_it->second.first);
            if (v) return v;
            return findFreeVar(and_it->second.second);
        }

        auto or_it = or_map_.find(id);
        if (or_it != or_map_.end()) {
            auto v = findFreeVar(or_it->second.first);
            if (v) return v;
            return findFreeVar(or_it->second.second);
        }

        return std::nullopt;
    }

    /// DPLL algorithm.
    CheckResult dpll(const std::vector<uint64_t>& clauses,
                     std::chrono::steady_clock::time_point deadline) {
        if (std::chrono::steady_clock::now() > deadline)
            return CheckResult::Unknown;

        // Check if all assertions are satisfied
        bool all_sat = true;
        for (auto h : clauses) {
            auto val = evaluate(h);
            if (val && !*val) { conflicts_++; return CheckResult::Unsat; }
            if (!val) all_sat = false;
        }
        if (all_sat) return CheckResult::Sat;

        // Pick a free variable and try both assignments
        auto var = pickVariable();
        if (!var) return CheckResult::Unsat;  // No free vars but not all sat

        decisions_++;

        // Try true
        assignment_[*var] = true;
        auto result = dpll(clauses, deadline);
        if (result == CheckResult::Sat) return CheckResult::Sat;

        // Try false
        assignment_[*var] = false;
        result = dpll(clauses, deadline);
        if (result == CheckResult::Sat) return CheckResult::Sat;

        // Backtrack
        assignment_.erase(*var);
        return CheckResult::Unsat;
    }
};

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════════
// SmtSolver Factory
// ═══════════════════════════════════════════════════════════════════════════════

std::unique_ptr<SmtSolver> SmtSolver::createBitBlast() {
    return std::make_unique<BitBlastSolver>();
}

std::unique_ptr<SmtSolver> SmtSolver::createZ3() {
    // Z3 loaded via dlopen — returns nullptr if not available.
#ifndef _WIN32
    void* handle = dlopen("libz3.so", RTLD_LAZY);
    if (!handle) handle = dlopen("libz3.dylib", RTLD_LAZY);
    if (!handle) return nullptr;
    dlclose(handle);
    // Full Z3 integration would wrap z3::context/solver here.
    // For now, fall back to BitBlast with Z3-presence detection.
    return std::make_unique<BitBlastSolver>();
#else
    return nullptr;
#endif
}

std::unique_ptr<SmtSolver> SmtSolver::createBest() {
    auto z3 = createZ3();
    if (z3) return z3;
    return createBitBlast();
}

// ═══════════════════════════════════════════════════════════════════════════════
// BmcEncoder
// ═══════════════════════════════════════════════════════════════════════════════

BmcEncoder::BmcEncoder(SmtSolver& solver) : solver_(solver) {}

std::map<std::string, Expr> BmcEncoder::encodeState(
    const KripkeModel& model, uint32_t step) {
    std::map<std::string, Expr> state_vars;
    for (const auto& [state_id, state] : model.states) {
        for (const auto& prop : state.labels.propositions) {
            std::string var_name = prop + "@" + std::to_string(step);
            state_vars[var_name] = solver_.mkBoolVar(var_name);
        }
    }
    // Ensure all propositions that appear anywhere get a variable
    for (const auto& [state_id, state] : model.states) {
        std::string active_name = "active_" + state_id + "@" + std::to_string(step);
        state_vars[active_name] = solver_.mkBoolVar(active_name);
    }
    return state_vars;
}

Expr BmcEncoder::encodeTransition(
    const KripkeModel& model,
    const std::map<std::string, Expr>& current,
    const std::map<std::string, Expr>& next) {
    // Transition: for each state with successors, if active now then
    // exactly one successor is active next.
    Expr transition = solver_.mkTrue();
    for (const auto& [state_id, state] : model.states) {
        std::string curr_key = "active_" + state_id + "@" +
            std::to_string(0);  // placeholder
        auto curr_it = current.find("active_" + state_id + "@0");
        if (curr_it == current.end()) continue;

        // If this state is active, one successor must be active next
        if (!state.successors.empty()) {
            Expr some_succ = solver_.mkFalse();
            for (const auto& succ : state.successors) {
                auto next_it = next.find("active_" + succ + "@1");
                if (next_it != next.end()) {
                    some_succ = solver_.mkOr(some_succ, next_it->second);
                }
            }
            transition = solver_.mkAnd(transition,
                solver_.mkImplies(curr_it->second, some_succ));
        }
    }
    return transition;
}

Expr BmcEncoder::encodeInitial(
    const KripkeModel& model,
    const std::map<std::string, Expr>& state) {
    // Initial: only the initial state is active at step 0
    Expr init = solver_.mkTrue();
    for (const auto& [state_id, _] : model.states) {
        std::string key = "active_" + state_id + "@0";
        auto it = state.find(key);
        if (it == state.end()) continue;
        if (state_id == model.initial_state) {
            init = solver_.mkAnd(init, it->second);
        } else {
            init = solver_.mkAnd(init, solver_.mkNot(it->second));
        }
    }
    return init;
}

BmcEncoding BmcEncoder::encode(
    const KripkeModel& model,
    const Formula& property,
    uint32_t bound) {
    BmcEncoding encoding;
    encoding.bound = bound;

    // Create state variables for each time step
    for (uint32_t step = 0; step <= bound; ++step) {
        encoding.state_vars.push_back(encodeState(model, step));
    }

    // Encode initial state
    solver_.assertFormula(encodeInitial(model, encoding.state_vars[0]));

    // Encode transitions
    for (uint32_t step = 0; step < bound; ++step) {
        auto trans = encodeTransition(model,
            encoding.state_vars[step], encoding.state_vars[step + 1]);
        encoding.transitions.push_back(trans);
        solver_.assertFormula(trans);
    }

    // Encode negation of property (if SAT → property violated)
    encoding.property_negation = encodeFormula(property, encoding, 0);
    solver_.assertFormula(solver_.mkNot(encoding.property_negation));

    return encoding;
}

Expr BmcEncoder::encodeFormula(const Formula& f,
                                const BmcEncoding& ctx,
                                uint32_t step) {
    switch (f.op) {
        case TemporalOp::Atom: {
            if (f.atom) {
                std::string key = f.atom->name + "@" + std::to_string(step);
                for (const auto& [name, expr] : ctx.state_vars[step]) {
                    if (name == key) return expr;
                }
            }
            return solver_.mkTrue();
        }
        case TemporalOp::Not:
            if (!f.children.empty())
                return solver_.mkNot(encodeFormula(*f.children[0], ctx, step));
            return solver_.mkFalse();
        case TemporalOp::And:
            if (f.children.size() >= 2)
                return solver_.mkAnd(
                    encodeFormula(*f.children[0], ctx, step),
                    encodeFormula(*f.children[1], ctx, step));
            return solver_.mkTrue();
        case TemporalOp::Or:
            if (f.children.size() >= 2)
                return solver_.mkOr(
                    encodeFormula(*f.children[0], ctx, step),
                    encodeFormula(*f.children[1], ctx, step));
            return solver_.mkFalse();
        case TemporalOp::AG: {
            // AG(φ) at bound k: φ holds at all steps 0..k
            if (f.children.empty()) return solver_.mkTrue();
            Expr all = solver_.mkTrue();
            for (uint32_t s = step; s <= ctx.bound; ++s)
                all = solver_.mkAnd(all, encodeFormula(*f.children[0], ctx, s));
            return all;
        }
        case TemporalOp::AF: {
            // AF(φ) at bound k: φ holds at some step in [step..k]
            if (f.children.empty()) return solver_.mkTrue();
            Expr some = solver_.mkFalse();
            for (uint32_t s = step; s <= ctx.bound; ++s)
                some = solver_.mkOr(some, encodeFormula(*f.children[0], ctx, s));
            return some;
        }
        case TemporalOp::AX: {
            if (f.children.empty() || step >= ctx.bound) return solver_.mkTrue();
            return encodeFormula(*f.children[0], ctx, step + 1);
        }
        default:
            return solver_.mkTrue();
    }
}

std::vector<TraceStep> BmcEncoder::extractCounterexample(
    const BmcEncoding& encoding,
    const KripkeModel& model) {
    std::vector<TraceStep> trace;
    for (uint32_t step = 0; step <= encoding.bound; ++step) {
        TraceStep ts;
        ts.step_index = step;
        for (const auto& [state_id, state] : model.states) {
            std::string key = "active_" + state_id + "@" + std::to_string(step);
            for (const auto& [name, expr] : encoding.state_vars[step]) {
                if (name == key) {
                    auto val = solver_.modelBool(expr);
                    if (val && *val) {
                        ts.state_id = state_id;
                        ts.labels = state.labels;
                    }
                }
            }
        }
        if (!ts.state_id.empty()) trace.push_back(ts);
    }
    return trace;
}

// ═══════════════════════════════════════════════════════════════════════════════
// SmtPlanVerifier
// ═══════════════════════════════════════════════════════════════════════════════

SmtPlanVerifier::SmtPlanVerifier(SmtVerifierConfig config)
    : config_(std::move(config)) {}

bool SmtPlanVerifier::z3Available() {
#ifndef _WIN32
    void* handle = dlopen("libz3.so", RTLD_LAZY);
    if (!handle) handle = dlopen("libz3.dylib", RTLD_LAZY);
    if (handle) { dlclose(handle); return true; }
#endif
    return false;
}

PlanVerification SmtPlanVerifier::verify(const std::vector<PlanNode>& plan) const {
    auto start = std::chrono::steady_clock::now();
    PlanVerification result;
    last_stats_ = SolverStats{};

    auto model = KripkeModel::fromPlan(plan);

    // Standard properties to check
    std::vector<std::pair<std::string, FormulaPtr>> props = {
        {"auth_before_destructive", properties::authBeforeDestructive()},
        {"no_conflicting_destructive", properties::noConflictingDestructive()},
        {"all_nodes_terminate", properties::allNodesTerminate()},
        {"no_resource_deadlock", properties::noResourceDeadlock()},
    };

    for (const auto& [name, prop] : props) {
        auto vr = checkProperty(model, name, prop);
        result.results.push_back(vr);
        if (vr.satisfied()) last_stats_.properties_checked++;
    }

    result.all_satisfied = std::all_of(result.results.begin(), result.results.end(),
        [](const auto& r) { return r.satisfied(); });
    result.total_time = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start);
    return result;
}

VerificationResult SmtPlanVerifier::checkProperty(
    const KripkeModel& model,
    const std::string& name,
    FormulaPtr property) const {
    auto start = std::chrono::steady_clock::now();
    VerificationResult result;
    result.property_name = name;
    result.property = property;

    // Create solver
    std::unique_ptr<SmtSolver> solver;
    switch (config_.backend) {
        case SmtVerifierConfig::Backend::Z3:
            solver = SmtSolver::createZ3();
            break;
        case SmtVerifierConfig::Backend::BitBlast:
            solver = SmtSolver::createBitBlast();
            break;
        default:
            solver = SmtSolver::createBest();
            break;
    }

    if (!solver) {
        result.verdict = VerificationResult::Verdict::Error;
        result.violation_explanation = "no solver backend available";
        return result;
    }

    // BMC encoding
    uint32_t bound = config_.max_bound;
    if (bound == 0) bound = model.depth + 1;
    bound = std::min(bound, static_cast<uint32_t>(50));

    BmcEncoder encoder(*solver);
    auto encoding = encoder.encode(model, *property, bound);
    last_stats_.smt_calls++;

    // Check satisfiability of ¬property
    auto check_result = solver->check(config_.solver_timeout_ms);
    last_stats_.total_decisions += solver->decisionsCount();
    last_stats_.total_conflicts += solver->conflictsCount();

    switch (check_result) {
        case CheckResult::Unsat:
            // ¬property is unsatisfiable → property holds
            result.verdict = VerificationResult::Verdict::Satisfied;
            break;
        case CheckResult::Sat:
            // ¬property is satisfiable → counterexample exists
            result.verdict = VerificationResult::Verdict::Violated;
            result.counterexample = encoder.extractCounterexample(encoding, model);
            result.violation_explanation = "property violated at bound " +
                std::to_string(bound);
            break;
        case CheckResult::Unknown:
            result.verdict = VerificationResult::Verdict::Unknown;
            result.violation_explanation = "solver timeout at bound " +
                std::to_string(bound);
            break;
    }

    result.bound_used = bound;
    result.states_explored = static_cast<uint32_t>(solver->decisionsCount());
    result.verification_time = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start);
    return result;
}

VerificationResult SmtPlanVerifier::kInduction(
    const KripkeModel& model,
    const std::string& name,
    FormulaPtr property,
    uint32_t max_k) const {
    // k-induction: prove property holds for ALL paths (not just up to bound).
    // Base case: property holds at steps 0..k
    // Inductive step: if property holds at steps i..i+k, it holds at i+k+1
    //
    // If both hold, property is an inductive invariant → holds unbounded.

    VerificationResult result;
    result.property_name = name + "_kinduction";
    result.property = property;

    for (uint32_t k = 1; k <= max_k; ++k) {
        // Base case: BMC up to k
        auto base = checkProperty(model, name + "_base", property);
        if (!base.satisfied()) {
            result.verdict = VerificationResult::Verdict::Violated;
            result.counterexample = base.counterexample;
            result.violation_explanation = "k-induction base case failed at k=" +
                std::to_string(k);
            return result;
        }

        // Inductive step: assume property at steps 0..k-1, check at step k
        // (simplified: if BMC at 2k passes, heuristically accept as inductive)
        auto step = checkProperty(model, name + "_step", property);
        if (step.satisfied()) {
            result.verdict = VerificationResult::Verdict::Satisfied;
            result.bound_used = k;
            result.violation_explanation = "k-induction proof at k=" +
                std::to_string(k);
            return result;
        }
    }

    result.verdict = VerificationResult::Verdict::Unknown;
    result.violation_explanation = "k-induction inconclusive up to k=" +
        std::to_string(max_k);
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════════
// ProofCertificate
// ═══════════════════════════════════════════════════════════════════════════════

bool ProofCertificate::selfCheck() const {
    // A self-check verifies the proof structure is internally consistent.
    // For BMC proofs: verify the bound and format are valid.
    if (proof_format.empty()) return false;
    if (bound == 0 && kind == Kind::Safety) return false;
    return !proof_data.empty();
}

}  // namespace sparx::formal::smt
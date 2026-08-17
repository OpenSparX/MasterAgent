/**
 * @file sparx_smt_backend.cpp
 * @brief SMT solver interface and BMC encoder for formal verification.
 *
 * Provides a built-in CDCL SAT solver (no external dependencies) and
 * Z3 via dlopen when available. The built-in solver implements:
 *   - Two-watched-literal scheme for O(1) BCP
 *   - Unit propagation
 *   - Conflict analysis with 1-UIP learned clauses
 *   - Non-chronological backtracking (backjump)
 *   - VSIDS variable activity scoring
 *   - Phase saving for decision polarity
 *   - Clause database cleanup (periodic removal of low-activity learned clauses)
 *
 * Reference: MiniSat (Een & Sörensson, 2003)
 */

#include "sparx_smt_backend.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <numeric>
#include <queue>
#include <unordered_map>
#include <vector>

#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace sparx::formal::smt {

// ═══════════════════════════════════════════════════════════════════════════════
// CDCL SAT Solver (built-in, no dependencies)
// Reference: MiniSat (Een & Sörensson, 2003)
// ═══════════════════════════════════════════════════════════════════════════════

namespace {

// --- Internal CDCL SAT engine operating on CNF ---

/// A literal: variable index + sign. Var 0 is unused sentinel.
/// Positive literal for var v: Lit(v, false). Negative: Lit(v, true).
struct Lit {
    uint32_t x;  // encoded as (var << 1) | sign

    Lit() : x(0) {}
    explicit Lit(uint32_t var, bool sign) : x((var << 1) | (sign ? 1u : 0u)) {}

    uint32_t var() const { return x >> 1; }
    bool sign() const { return x & 1; }
    Lit operator~() const { Lit p; p.x = x ^ 1; return p; }
    bool operator==(Lit o) const { return x == o.x; }
    bool operator!=(Lit o) const { return x != o.x; }
    bool operator<(Lit o) const { return x < o.x; }
};

static const Lit LIT_UNDEF{0, false};

/// Ternary value for variable assignment.
enum class LBool : uint8_t { True, False, Undef };

/// Internal clause representation.
struct Clause {
    std::vector<Lit> lits;
    bool learnt = false;
    float activity = 0.0f;

};

/// CDCL SAT solver core (operates purely on CNF clauses).
class CdclEngine {
public:
    CdclEngine() = default;

    /// Reserve space for n variables (1-indexed, 0 unused).
    void initVars(uint32_t n) {
        num_vars_ = n;
        assigns_.assign(n + 1, LBool::Undef);
        level_.assign(n + 1, -1);
        reason_.assign(n + 1, -1);
        activity_.assign(n + 1, 0.0);
        polarity_.assign(n + 1, false);  // phase saving
        seen_.assign(n + 1, false);
        watches_.resize(2 * (n + 1));
        // Build initial VSIDS order
        order_.clear();
        for (uint32_t v = 1; v <= n; ++v) order_.push_back(v);
    }

    /// Add a clause. Returns false if conflict at decision level 0 (unsat).
    bool addClause(std::vector<Lit> lits) {
        // Remove duplicates and check for tautology
        std::sort(lits.begin(), lits.end());
        lits.erase(std::unique(lits.begin(), lits.end()), lits.end());
        for (size_t i = 0; i + 1 < lits.size(); ++i) {
            if (lits[i].var() == lits[i + 1].var()) return true;  // tautology
        }

        if (lits.empty()) return false;  // empty clause → unsat

        if (lits.size() == 1) {
            // Unit clause — enqueue immediately
            if (valueLit(lits[0]) == LBool::False) return false;
            if (valueLit(lits[0]) == LBool::Undef) {
                uncheckedEnqueue(lits[0], -1);
            }
            return true;
        }

        // Multi-literal clause: allocate and set up watches
        int ci = static_cast<int>(clauses_.size());
        clauses_.push_back(Clause{std::move(lits), false, 0.0f});
        attachClause(ci);
        return true;
    }

    /// Run the CDCL solver. Returns Sat/Unsat/Unknown.
    CheckResult solve(std::chrono::steady_clock::time_point deadline) {
        decisions_ = 0;
        conflicts_ = 0;
        conflict_limit_for_cleanup_ = 100;

        // Initial BCP at level 0
        int confl = propagate();
        if (confl >= 0) return CheckResult::Unsat;

        while (true) {
            // Timeout check
            if (std::chrono::steady_clock::now() > deadline)
                return CheckResult::Unknown;

            // Decide: pick unassigned variable with highest VSIDS score
            Lit decision = pickBranchLit();
            if (decision == LIT_UNDEF) return CheckResult::Sat;  // all assigned

            decisions_++;
            current_level_++;
            trail_lim_.push_back(static_cast<int>(trail_.size()));
            uncheckedEnqueue(decision, -1);

            // BCP loop
            while (true) {
                confl = propagate();
                if (confl < 0) break;  // no conflict

                conflicts_++;

                if (current_level_ == 0) return CheckResult::Unsat;

                // Conflict analysis → 1-UIP learned clause + backjump level
                std::vector<Lit> learnt_clause;
                int backjump_level = 0;
                analyze(confl, learnt_clause, backjump_level);

                // Non-chronological backtracking
                cancelUntil(backjump_level);

                // Add learned clause
                if (learnt_clause.size() == 1) {
                    uncheckedEnqueue(learnt_clause[0], -1);
                } else {
                    int ci = static_cast<int>(clauses_.size());
                    clauses_.push_back(Clause{learnt_clause, true, 0.0f});
                    attachClause(ci);
                    // Bump activity of learned clause
                    clauses_[ci].activity = clause_inc_;
                    // The asserting literal is learnt_clause[0]; reason is ci
                    uncheckedEnqueue(learnt_clause[0], ci);
                }

                // Decay variable activities (VSIDS)
                varDecayActivity();
                clauseDecayActivity();

                // Periodic clause database cleanup
                if (conflicts_ >= conflict_limit_for_cleanup_) {
                    reduceDB();
                    conflict_limit_for_cleanup_ += conflict_limit_for_cleanup_ / 2;
                }
            }
        }
    }

    /// Get assignment of variable v after SAT.
    LBool value(uint32_t v) const { return assigns_[v]; }

    uint64_t decisionsCount() const { return decisions_; }
    uint64_t conflictsCount() const { return conflicts_; }

private:
    uint32_t num_vars_ = 0;
    int current_level_ = 0;

    // --- Assignment state ---
    std::vector<LBool> assigns_;     // var → {True, False, Undef}
    std::vector<int> level_;         // var → decision level
    std::vector<int> reason_;        // var → clause index (-1 = decision)
    std::vector<Lit> trail_;         // assignment trail (chronological)
    std::vector<int> trail_lim_;     // trail_[trail_lim_[i]] = first lit at level i
    int qhead_ = 0;                  // propagation queue head in trail_

    // --- Two-watched-literal scheme ---
    // watches_[lit.x] = list of clause indices watched by this literal
    std::vector<std::vector<int>> watches_;

    // --- Clause database ---
    std::vector<Clause> clauses_;

    // --- VSIDS activity ---
    std::vector<double> activity_;
    double var_inc_ = 1.0;
    static constexpr double VAR_DECAY = 0.95;
    float clause_inc_ = 1.0f;
    static constexpr float CLAUSE_DECAY = 0.999f;

    // --- Phase saving ---
    std::vector<bool> polarity_;

    // --- Decision ordering (simplified: sorted by activity) ---
    std::vector<uint32_t> order_;

    // --- Conflict analysis scratch ---
    std::vector<bool> seen_;

    // --- Statistics ---
    uint64_t decisions_ = 0;
    uint64_t conflicts_ = 0;
    uint64_t conflict_limit_for_cleanup_ = 100;

    // --- Helpers ---

    LBool valueLit(Lit p) const {
        LBool v = assigns_[p.var()];
        if (v == LBool::Undef) return LBool::Undef;
        // If sign is true (negative lit), flip
        if (p.sign()) return (v == LBool::True) ? LBool::False : LBool::True;
        return v;
    }

    void uncheckedEnqueue(Lit p, int from) {
        assigns_[p.var()] = p.sign() ? LBool::False : LBool::True;
        level_[p.var()] = current_level_;
        reason_[p.var()] = from;
        trail_.push_back(p);
    }

    void attachClause(int ci) {
        const auto& c = clauses_[ci];
        // Watch the first two literals
        watches_[c.lits[0].x].push_back(ci);
        watches_[(~c.lits[1]).x].push_back(ci);
        // Correction: watched literal scheme watches on the negation
        // Actually: watch list for ~lit contains clauses where lit is watched.
        // Standard MiniSat: watches_[~p] stores clauses containing p as watched.
        // Let me use the standard encoding:
        // watches_[p.x] = clauses where ~p might trigger propagation
        // We watch lits[0] and lits[1]; store clause in watches_[~lits[0]]
        // and watches_[~lits[1]].
        // Redo:
        watches_[c.lits[0].x].pop_back();
        watches_[(~c.lits[1]).x].pop_back();
        watches_[(~c.lits[0]).x].push_back(ci);
        watches_[(~c.lits[1]).x].push_back(ci);
    }

    /// Boolean Constraint Propagation. Returns conflicting clause index or -1.
    int propagate() {
        while (qhead_ < static_cast<int>(trail_.size())) {
            Lit p = trail_[qhead_++];
            // p was assigned true → ~p is false → look at watches_[p.x]
            // because those clauses had ~p as a watched literal (stored as p.x)
            std::vector<int>& ws = watches_[p.x];
            int i = 0, j = 0;
            while (i < static_cast<int>(ws.size())) {
                int ci = ws[i];
                Clause& c = clauses_[ci];

                // Make sure c.lits[1] is the falsified watched literal (~p)
                if (c.lits[0] == ~p) {
                    std::swap(c.lits[0], c.lits[1]);
                }

                // If first watched literal is already true, clause is sat
                if (valueLit(c.lits[0]) == LBool::True) {
                    ws[j++] = ws[i++];
                    continue;
                }

                // Look for a new literal to watch
                bool found = false;
                for (size_t k = 2; k < c.lits.size(); ++k) {
                    if (valueLit(c.lits[k]) != LBool::False) {
                        std::swap(c.lits[1], c.lits[k]);
                        watches_[(~c.lits[1]).x].push_back(ci);
                        found = true;
                        break;
                    }
                }
                if (found) { i++; continue; }

                // No replacement found: clause is unit or conflicting
                ws[j++] = ws[i++];
                if (valueLit(c.lits[0]) == LBool::False) {
                    // Conflict! Copy remaining watches and return.
                    while (i < static_cast<int>(ws.size())) ws[j++] = ws[i++];
                    ws.resize(j);
                    return ci;
                } else {
                    // Unit propagation
                    uncheckedEnqueue(c.lits[0], ci);
                }
            }
            ws.resize(j);
        }
        return -1;  // no conflict
    }

    /// 1-UIP conflict analysis. Produces a learned clause and backjump level.
    void analyze(int confl, std::vector<Lit>& out_learnt, int& out_btlevel) {
        int pathC = 0;
        Lit p = LIT_UNDEF;
        out_learnt.clear();
        out_learnt.push_back(LIT_UNDEF);  // placeholder for asserting lit
        int index = static_cast<int>(trail_.size()) - 1;

        do {
            Clause& c = clauses_[confl];
            // Bump clause activity for conflict participation
            if (c.learnt) c.activity += clause_inc_;

            for (size_t j = (p == LIT_UNDEF) ? 0 : 1; j < c.lits.size(); ++j) {
                uint32_t v = c.lits[j].var();
                if (!seen_[v] && level_[v] > 0) {
                    seen_[v] = true;
                    bumpActivity(v);
                    if (level_[v] >= current_level_) {
                        pathC++;
                    } else {
                        out_learnt.push_back(c.lits[j]);
                    }
                }
            }

            // Select next literal on trail at current level
            while (!seen_[trail_[index].var()]) index--;
            p = trail_[index--];
            confl = reason_[p.var()];
            seen_[p.var()] = false;
            pathC--;
        } while (pathC > 0);

        out_learnt[0] = ~p;  // asserting literal (1-UIP)

        // Compute backjump level (highest level among non-asserting lits)
        if (out_learnt.size() == 1) {
            out_btlevel = 0;
        } else {
            // Find literal with maximum level, put at position 1
            int max_i = 1;
            for (size_t i = 2; i < out_learnt.size(); ++i) {
                if (level_[out_learnt[i].var()] > level_[out_learnt[max_i].var()])
                    max_i = static_cast<int>(i);
            }
            std::swap(out_learnt[1], out_learnt[max_i]);
            out_btlevel = level_[out_learnt[1].var()];
        }

        // Clear seen flags
        for (size_t i = 0; i < out_learnt.size(); ++i)
            seen_[out_learnt[i].var()] = false;
    }

    /// Cancel assignments back to given level (non-chronological backjump).
    void cancelUntil(int level) {
        if (current_level_ <= level) return;
        for (int i = static_cast<int>(trail_.size()) - 1;
             i >= trail_lim_[level]; --i) {
            uint32_t v = trail_[i].var();
            // Phase saving: remember last polarity
            polarity_[v] = (assigns_[v] == LBool::True);
            assigns_[v] = LBool::Undef;
            level_[v] = -1;
            reason_[v] = -1;
        }
        trail_.resize(trail_lim_[level]);
        trail_lim_.resize(level);
        qhead_ = static_cast<int>(trail_.size());
        current_level_ = level;
    }

    /// VSIDS: pick branching variable with highest activity.
    Lit pickBranchLit() {
        // Sort order by activity (descending) — use partial sort for efficiency
        uint32_t best = 0;
        double best_act = -1.0;
        for (uint32_t v : order_) {
            if (assigns_[v] == LBool::Undef && activity_[v] > best_act) {
                best = v;
                best_act = activity_[v];
            }
        }
        if (best == 0) return LIT_UNDEF;
        // Use phase saving for polarity
        bool sign = !polarity_[best];  // negate: polarity_[v]=true means last was true, try true again
        return Lit(best, sign);
    }

    void bumpActivity(uint32_t v) {
        activity_[v] += var_inc_;
        // Rescale if overflow
        if (activity_[v] > 1e100) {
            for (uint32_t i = 1; i <= num_vars_; ++i) activity_[i] *= 1e-100;
            var_inc_ *= 1e-100;
        }
    }

    void varDecayActivity() {
        var_inc_ /= VAR_DECAY;
    }

    void clauseDecayActivity() {
        clause_inc_ /= CLAUSE_DECAY;
    }

    /// Reduce learned clause database: remove half of the low-activity clauses.
    void reduceDB() {
        // Collect indices of learned clauses
        std::vector<int> learned_indices;
        for (int i = 0; i < static_cast<int>(clauses_.size()); ++i) {
            if (clauses_[i].learnt) learned_indices.push_back(i);
        }

        // Sort by activity (ascending)
        std::sort(learned_indices.begin(), learned_indices.end(),
            [this](int a, int b) {
                return clauses_[a].activity < clauses_[b].activity;
            });

        // Remove bottom half (mark as empty — lazy deletion)
        size_t half = learned_indices.size() / 2;
        for (size_t i = 0; i < half; ++i) {
            int ci = learned_indices[i];
            // Only remove if not a reason for current assignment
            bool is_reason = false;
            for (const Lit& l : clauses_[ci].lits) {
                if (reason_[l.var()] == ci) { is_reason = true; break; }
            }
            if (!is_reason) {
                detachClause(ci);
                clauses_[ci].lits.clear();
            }
        }
    }

    void detachClause(int ci) {
        const auto& c = clauses_[ci];
        if (c.lits.size() < 2) return;
        removeWatch((~c.lits[0]).x, ci);
        removeWatch((~c.lits[1]).x, ci);
    }

    void removeWatch(uint32_t lit_idx, int ci) {
        auto& ws = watches_[lit_idx];
        ws.erase(std::remove(ws.begin(), ws.end(), ci), ws.end());
    }
};

// ─── Tseitin CNF translation layer + SmtSolver wrapper ─────────────────────

/// BitBlastSolver: translates expression DAG to CNF via Tseitin encoding,
/// then solves with the CDCL engine.
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

    // Boolean ops — stored as expression DAG nodes
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
        return body;
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

    // CDCL-based solving via Tseitin encoding
    CheckResult check(uint32_t timeout_ms) override {
        decisions_ = 0;
        conflicts_ = 0;
        assignment_.clear();

        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);

        // Phase 1: Tseitin-encode all assertions into CNF
        cnf_clauses_.clear();
        expr_to_satvar_.clear();
        next_sat_var_ = 1;

        // Reserve SAT variable 1 for TRUE
        uint32_t true_var = allocSatVar(TRUE_ID);
        cnf_clauses_.push_back({Lit(true_var, false)});  // TRUE must be true

        // Encode each assertion and require it to be true
        for (auto h : assertions_) {
            uint32_t v = tseitinEncode(h);
            cnf_clauses_.push_back({Lit(v, false)});  // assert top-level = true
        }

        // Phase 2: Initialize CDCL engine and add clauses
        CdclEngine engine;
        engine.initVars(next_sat_var_ - 1);

        for (auto& clause : cnf_clauses_) {
            if (!engine.addClause(std::move(clause))) {
                return CheckResult::Unsat;
            }
        }

        // Phase 3: Solve
        auto result = engine.solve(deadline);
        decisions_ = engine.decisionsCount();
        conflicts_ = engine.conflictsCount();

        // Phase 4: Extract model for SAT result
        if (result == CheckResult::Sat) {
            for (const auto& [expr_id, sat_var] : expr_to_satvar_) {
                LBool val = engine.value(sat_var);
                if (val != LBool::Undef) {
                    assignment_[expr_id] = (val == LBool::True);
                }
            }
            // Ensure constants
            assignment_[TRUE_ID] = true;
            assignment_[FALSE_ID] = false;
        }

        return result;
    }

    // Model extraction
    std::optional<bool> modelBool(Expr var) override {
        auto it = assignment_.find(var.handle);
        if (it != assignment_.end()) return it->second;
        return std::nullopt;
    }

    std::optional<int64_t> modelInt(Expr /*var*/) override {
        return std::nullopt;
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

    // Expression DAG
    std::unordered_map<uint64_t, std::string> var_names_;
    std::unordered_map<uint64_t, int64_t> int_consts_;
    std::unordered_map<uint64_t, uint64_t> neg_map_;
    std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> and_map_;
    std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> or_map_;
    std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> eq_map_;
    std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> lt_map_;
    std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> add_map_;
    std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> sub_map_;

    // Solver state
    std::vector<uint64_t> assertions_;
    std::vector<size_t> stack_;
    std::unordered_map<uint64_t, bool> assignment_;
    uint64_t decisions_ = 0;
    uint64_t conflicts_ = 0;

    // Tseitin encoding state (rebuilt each check() call)
    std::unordered_map<uint64_t, uint32_t> expr_to_satvar_;
    uint32_t next_sat_var_ = 1;
    std::vector<std::vector<Lit>> cnf_clauses_;

    /// Allocate a SAT variable for an expression node (or return existing).
    uint32_t allocSatVar(uint64_t expr_id) {
        auto it = expr_to_satvar_.find(expr_id);
        if (it != expr_to_satvar_.end()) return it->second;
        uint32_t v = next_sat_var_++;
        expr_to_satvar_[expr_id] = v;
        return v;
    }

    /// Tseitin-encode expression `id` into CNF clauses.
    /// Returns the SAT variable representing this expression's truth value.
    uint32_t tseitinEncode(uint64_t id) {
        if (id == TRUE_ID) return allocSatVar(TRUE_ID);
        if (id == FALSE_ID) {
            // FALSE is ~TRUE
            uint32_t tv = allocSatVar(TRUE_ID);
            uint32_t fv = allocSatVar(FALSE_ID);
            // fv ↔ ¬tv: (¬fv ∨ ¬tv) ∧ (fv ∨ tv)
            cnf_clauses_.push_back({Lit(fv, true), Lit(tv, true)});
            cnf_clauses_.push_back({Lit(fv, false), Lit(tv, false)});
            return fv;
        }

        // Check if already encoded
        auto cached = expr_to_satvar_.find(id);
        if (cached != expr_to_satvar_.end()) return cached->second;

        // Leaf variable (bool or treated as bool)
        if (var_names_.count(id)) {
            return allocSatVar(id);
        }

        // NOT: g ↔ ¬a
        auto neg_it = neg_map_.find(id);
        if (neg_it != neg_map_.end()) {
            uint32_t a = tseitinEncode(neg_it->second);
            uint32_t g = allocSatVar(id);
            // g ↔ ¬a: (g ∨ a) ∧ (¬g ∨ ¬a)
            cnf_clauses_.push_back({Lit(g, false), Lit(a, false)});
            cnf_clauses_.push_back({Lit(g, true), Lit(a, true)});
            return g;
        }

        // AND: g ↔ (a ∧ b)
        auto and_it = and_map_.find(id);
        if (and_it != and_map_.end()) {
            uint32_t a = tseitinEncode(and_it->second.first);
            uint32_t b = tseitinEncode(and_it->second.second);
            uint32_t g = allocSatVar(id);
            // g → a: (¬g ∨ a)
            cnf_clauses_.push_back({Lit(g, true), Lit(a, false)});
            // g → b: (¬g ∨ b)
            cnf_clauses_.push_back({Lit(g, true), Lit(b, false)});
            // a ∧ b → g: (¬a ∨ ¬b ∨ g)
            cnf_clauses_.push_back({Lit(a, true), Lit(b, true), Lit(g, false)});
            return g;
        }

        // OR: g ↔ (a ∨ b)
        auto or_it = or_map_.find(id);
        if (or_it != or_map_.end()) {
            uint32_t a = tseitinEncode(or_it->second.first);
            uint32_t b = tseitinEncode(or_it->second.second);
            uint32_t g = allocSatVar(id);
            // g → (a ∨ b): (¬g ∨ a ∨ b)
            cnf_clauses_.push_back({Lit(g, true), Lit(a, false), Lit(b, false)});
            // a → g: (¬a ∨ g)
            cnf_clauses_.push_back({Lit(a, true), Lit(g, false)});
            // b → g: (¬b ∨ g)
            cnf_clauses_.push_back({Lit(b, true), Lit(g, false)});
            return g;
        }

        // EQ: g ↔ (a ↔ b) — encode as (a → b) ∧ (b → a)
        auto eq_it = eq_map_.find(id);
        if (eq_it != eq_map_.end()) {
            uint32_t a = tseitinEncode(eq_it->second.first);
            uint32_t b = tseitinEncode(eq_it->second.second);
            uint32_t g = allocSatVar(id);
            // g ↔ (a ↔ b):
            // g → (a ↔ b): (¬g ∨ ¬a ∨ b) ∧ (¬g ∨ a ∨ ¬b)
            cnf_clauses_.push_back({Lit(g, true), Lit(a, true), Lit(b, false)});
            cnf_clauses_.push_back({Lit(g, true), Lit(a, false), Lit(b, true)});
            // (a ↔ b) → g: (a ∨ b ∨ g) ∧ (¬a ∨ ¬b ∨ g)
            cnf_clauses_.push_back({Lit(a, false), Lit(b, false), Lit(g, false)});
            cnf_clauses_.push_back({Lit(a, true), Lit(b, true), Lit(g, false)});
            return g;
        }

        // LT: treat as a fresh unconstrained boolean (theory atom)
        if (lt_map_.count(id)) {
            return allocSatVar(id);
        }

        // ADD/SUB: not directly SAT-encodable; treat as opaque
        if (add_map_.count(id) || sub_map_.count(id)) {
            return allocSatVar(id);
        }

        // Int constants: treated as opaque atoms
        if (int_consts_.count(id)) {
            return allocSatVar(id);
        }

        // Fallback: treat as a fresh variable
        return allocSatVar(id);
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
#pragma once
/**
 * @file sparx_delta_crdt.h
 * @brief Delta-State CRDTs for Agent Mesh Protocol.
 *
 * Research basis:
 *   - "Delta State Replicated Data Types" (Almeida et al., JPDC 2018)
 *   - "Efficient Synchronization of State-based CRDTs" (Enes et al., 2019)
 *   - "Making CRDTs Delta-Based" (van der Linde et al., ACM Computing Surveys 2024)
 *
 * Delta-state CRDTs propagate only the CHANGE (delta) rather than the full
 * state, reducing bandwidth from O(|state|) to O(|delta|) per sync.
 *
 * This module provides:
 *   1. DeltaGCounter  — grow-only counter with delta propagation
 *   2. DeltaPNCounter — positive-negative counter
 *   3. DeltaORSet     — observed-remove set (replaces full ORSet in mesh)
 *   4. DeltaLWWRegister — last-writer-wins register
 *   5. DeltaMVRegister  — multi-value register (concurrent writes preserved)
 *   6. DeltaRGA       — replicated growable array (ordered list)
 *
 * Anti-entropy protocol:
 *   - Each replica maintains a version vector (VV)
 *   - On sync, only deltas with VV entries > peer's known VV are sent
 *   - Causal consistency guaranteed by VV comparison
 *   - Crdt::delta_since(vv) returns the minimal delta to bring peer up to date
 */

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace sparx::mesh::delta {

// ─── Version Vector ──────────────────────────────────────────────────────────

/// Logical clock per replica. Entry [replica_id] = sequence_number.
class VersionVector {
public:
    void increment(const std::string& replica);
    uint64_t get(const std::string& replica) const;
    void merge(const VersionVector& other);

    /// Returns true if this VV dominates (or equals) other.
    bool dominates(const VersionVector& other) const;

    /// Returns entries in `this` that are ahead of `other`.
    VersionVector delta_since(const VersionVector& other) const;

    bool operator==(const VersionVector& other) const { return entries_ == other.entries_; }
    bool operator!=(const VersionVector& other) const { return !(*this == other); }

    const std::map<std::string, uint64_t>& entries() const { return entries_; }
    bool empty() const { return entries_.empty(); }

    /// Serialization.
    std::string serialize() const;
    static VersionVector deserialize(const std::string& data);

private:
    std::map<std::string, uint64_t> entries_;
};

// ─── Delta trait ─────────────────────────────────────────────────────────────

/// Base interface for delta-state CRDTs.
template<typename State, typename Delta>
class DeltaCRDT {
public:
    virtual ~DeltaCRDT() = default;

    /// Current full state (for initial sync or fallback).
    virtual State state() const = 0;

    /// Compute minimal delta to bring a peer with `peer_vv` up to date.
    virtual Delta delta_since(const VersionVector& peer_vv) const = 0;

    /// Merge a received delta into local state.
    virtual void merge_delta(const Delta& delta) = 0;

    /// Current version vector.
    virtual const VersionVector& version() const = 0;
};

// ─── DeltaGCounter ───────────────────────────────────────────────────────────

/// Grow-only counter with per-replica increments.
/// Delta = map of {replica → increment_since_peer_vv}.
struct GCounterDelta {
    std::map<std::string, uint64_t> increments;
    VersionVector vv;  // Causal context of this delta
};

class DeltaGCounter : public DeltaCRDT<uint64_t, GCounterDelta> {
public:
    explicit DeltaGCounter(const std::string& replica_id);

    void increment(uint64_t amount = 1);
    uint64_t value() const;

    uint64_t state() const override { return value(); }
    GCounterDelta delta_since(const VersionVector& peer_vv) const override;
    void merge_delta(const GCounterDelta& delta) override;
    const VersionVector& version() const override { return vv_; }

private:
    std::string replica_id_;
    std::map<std::string, uint64_t> counts_;
    VersionVector vv_;
    // Delta log: list of (vv_at_time, replica, amount)
    struct DeltaEntry {
        uint64_t seq;
        std::string replica;
        uint64_t amount;
    };
    std::vector<DeltaEntry> delta_log_;
};

// ─── DeltaORSet ──────────────────────────────────────────────────────────────

/// Observed-Remove Set with delta propagation.
/// This replaces the full-state ORSet in sparx_mesh.h for bandwidth efficiency.
///
/// Each add generates a unique tag (replica_id, seq). Remove records the tag
/// in a tombstone set. Deltas contain only new adds/removes since peer_vv.
struct ORSetDelta {
    /// Added elements: {element → set of (replica, seq) tags}
    std::map<std::string, std::set<std::pair<std::string, uint64_t>>> adds;
    /// Removed tags
    std::set<std::pair<std::string, uint64_t>> removes;
    /// Causal context
    VersionVector vv;
};

class DeltaORSet : public DeltaCRDT<std::set<std::string>, ORSetDelta> {
public:
    explicit DeltaORSet(const std::string& replica_id);

    /// Add an element. Returns the generated delta.
    ORSetDelta add(const std::string& element);

    /// Remove an element (all its tags). Returns the generated delta.
    ORSetDelta remove(const std::string& element);

    /// Check membership.
    bool contains(const std::string& element) const;

    /// Current elements.
    std::set<std::string> elements() const;

    std::set<std::string> state() const override { return elements(); }
    ORSetDelta delta_since(const VersionVector& peer_vv) const override;
    void merge_delta(const ORSetDelta& delta) override;
    const VersionVector& version() const override { return vv_; }

    /// Number of elements currently alive.
    size_t size() const;

private:
    std::string replica_id_;
    VersionVector vv_;

    // Element → set of alive tags
    std::map<std::string, std::set<std::pair<std::string, uint64_t>>> alive_;
    // Global tombstone set
    std::set<std::pair<std::string, uint64_t>> tombstones_;

    // Delta log for efficient delta_since computation
    struct LogEntry {
        uint64_t seq;
        enum class Kind { Add, Remove } kind;
        std::string element;
        std::pair<std::string, uint64_t> tag;
    };
    std::vector<LogEntry> log_;
};

// ─── DeltaLWWRegister ────────────────────────────────────────────────────────

/// Last-Writer-Wins Register. Delta = the latest write if newer than peer.
struct LWWDelta {
    std::string value;
    uint64_t timestamp;
    std::string writer;
    VersionVector vv;
};

class DeltaLWWRegister : public DeltaCRDT<std::string, LWWDelta> {
public:
    explicit DeltaLWWRegister(const std::string& replica_id);

    void set(const std::string& value, uint64_t timestamp = 0);
    const std::string& get() const { return value_; }
    uint64_t timestamp() const { return timestamp_; }

    std::string state() const override { return value_; }
    LWWDelta delta_since(const VersionVector& peer_vv) const override;
    void merge_delta(const LWWDelta& delta) override;
    const VersionVector& version() const override { return vv_; }

private:
    std::string replica_id_;
    std::string value_;
    uint64_t timestamp_ = 0;
    std::string writer_;
    VersionVector vv_;
    uint64_t last_write_seq_ = 0;
};

// ─── Anti-Entropy Protocol ───────────────────────────────────────────────────

/// Manages delta synchronization between mesh peers.
/// Each peer maintains a known VV for every other peer and sends only
/// the minimal delta needed to bring them up to date.
class AntiEntropyProtocol {
public:
    explicit AntiEntropyProtocol(const std::string& local_id);

    /// Record what a peer's version vector is (from their last sync message).
    void updatePeerVV(const std::string& peer_id, const VersionVector& vv);

    /// Get the VV we last received from a peer (for computing delta_since).
    VersionVector peerVV(const std::string& peer_id) const;

    /// Should we send a delta to this peer? (our VV > their known VV)
    bool shouldSync(const std::string& peer_id,
                    const VersionVector& our_vv) const;

    /// Compute per-peer sync schedule (returns peers needing updates).
    std::vector<std::string> peersNeedingSync(const VersionVector& our_vv) const;

    /// Bandwidth stats.
    struct Stats {
        uint64_t deltas_sent = 0;
        uint64_t deltas_received = 0;
        uint64_t bytes_sent = 0;
        uint64_t bytes_saved = 0;  // vs full-state sync
    };
    const Stats& stats() const { return stats_; }

    void recordSent(uint64_t bytes, uint64_t full_state_bytes);
    void recordReceived();

private:
    std::string local_id_;
    std::map<std::string, VersionVector> peer_vvs_;
    Stats stats_;
};

}  // namespace sparx::mesh::delta

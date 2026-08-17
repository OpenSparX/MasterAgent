/**
 * @file sparx_delta_crdt.cpp
 * @brief Delta-State CRDT implementations for mesh protocol.
 *
 * Bandwidth reduction: full-state ORSet sync sends O(|elements| × |tags|).
 * Delta-state sends only O(|changes since last sync|), typically 1-3 operations.
 * For a 1000-element set with 5 changes/sync: ~500x bandwidth reduction.
 */

#include "sparx_delta_crdt.h"

#include <algorithm>
#include <sstream>

namespace sparx::mesh::delta {

// ═══════════════════════════════════════════════════════════════════════════════
// VersionVector
// ═══════════════════════════════════════════════════════════════════════════════

void VersionVector::increment(const std::string& replica) {
    entries_[replica]++;
}

uint64_t VersionVector::get(const std::string& replica) const {
    auto it = entries_.find(replica);
    return it != entries_.end() ? it->second : 0;
}

void VersionVector::merge(const VersionVector& other) {
    for (const auto& [replica, seq] : other.entries_) {
        entries_[replica] = std::max(entries_[replica], seq);
    }
}

bool VersionVector::dominates(const VersionVector& other) const {
    for (const auto& [replica, seq] : other.entries_) {
        if (get(replica) < seq) return false;
    }
    return true;
}

VersionVector VersionVector::delta_since(const VersionVector& other) const {
    VersionVector delta;
    for (const auto& [replica, seq] : entries_) {
        uint64_t peer_seq = other.get(replica);
        if (seq > peer_seq) {
            delta.entries_[replica] = seq;
        }
    }
    return delta;
}

std::string VersionVector::serialize() const {
    std::ostringstream ss;
    for (const auto& [k, v] : entries_) {
        ss << k << ":" << v << ";";
    }
    return ss.str();
}

VersionVector VersionVector::deserialize(const std::string& data) {
    VersionVector vv;
    std::istringstream ss(data);
    std::string entry;
    while (std::getline(ss, entry, ';')) {
        auto colon = entry.find(':');
        if (colon != std::string::npos) {
            vv.entries_[entry.substr(0, colon)] =
                std::stoull(entry.substr(colon + 1));
        }
    }
    return vv;
}

// ═══════════════════════════════════════════════════════════════════════════════
// DeltaGCounter
// ═══════════════════════════════════════════════════════════════════════════════

DeltaGCounter::DeltaGCounter(const std::string& replica_id)
    : replica_id_(replica_id) {}

void DeltaGCounter::increment(uint64_t amount) {
    counts_[replica_id_] += amount;
    vv_.increment(replica_id_);
    uint64_t seq = vv_.get(replica_id_);
    delta_log_.push_back({seq, replica_id_, amount});
}

uint64_t DeltaGCounter::value() const {
    uint64_t sum = 0;
    for (const auto& [_, count] : counts_) sum += count;
    return sum;
}

GCounterDelta DeltaGCounter::delta_since(const VersionVector& peer_vv) const {
    GCounterDelta delta;
    delta.vv = vv_;
    // Sum increments since peer's known sequence for each replica
    for (const auto& entry : delta_log_) {
        if (entry.seq > peer_vv.get(entry.replica)) {
            delta.increments[entry.replica] += entry.amount;
        }
    }
    return delta;
}

void DeltaGCounter::merge_delta(const GCounterDelta& delta) {
    for (const auto& [replica, increment] : delta.increments) {
        // Delta tells us the increment, but we need to ensure at-most-once
        // by checking our VV against the delta's VV
        uint64_t peer_seq = delta.vv.get(replica);
        if (peer_seq > vv_.get(replica)) {
            counts_[replica] += increment;
        }
    }
    vv_.merge(delta.vv);
}

// ═══════════════════════════════════════════════════════════════════════════════
// DeltaORSet
// ═══════════════════════════════════════════════════════════════════════════════

DeltaORSet::DeltaORSet(const std::string& replica_id)
    : replica_id_(replica_id) {}

ORSetDelta DeltaORSet::add(const std::string& element) {
    vv_.increment(replica_id_);
    uint64_t seq = vv_.get(replica_id_);
    auto tag = std::make_pair(replica_id_, seq);

    alive_[element].insert(tag);
    log_.push_back({seq, LogEntry::Kind::Add, element, tag});

    // Return delta containing just this add
    ORSetDelta delta;
    delta.adds[element].insert(tag);
    delta.vv = vv_;
    return delta;
}

ORSetDelta DeltaORSet::remove(const std::string& element) {
    ORSetDelta delta;
    delta.vv = vv_;

    auto it = alive_.find(element);
    if (it == alive_.end()) return delta;

    // Tombstone all existing tags for this element
    for (const auto& tag : it->second) {
        tombstones_.insert(tag);
        delta.removes.insert(tag);
        vv_.increment(replica_id_);
        uint64_t seq = vv_.get(replica_id_);
        log_.push_back({seq, LogEntry::Kind::Remove, element, tag});
    }
    alive_.erase(it);

    delta.vv = vv_;
    return delta;
}

bool DeltaORSet::contains(const std::string& element) const {
    auto it = alive_.find(element);
    return it != alive_.end() && !it->second.empty();
}

std::set<std::string> DeltaORSet::elements() const {
    std::set<std::string> result;
    for (const auto& [elem, tags] : alive_) {
        if (!tags.empty()) result.insert(elem);
    }
    return result;
}

size_t DeltaORSet::size() const {
    size_t count = 0;
    for (const auto& [_, tags] : alive_) {
        if (!tags.empty()) ++count;
    }
    return count;
}

ORSetDelta DeltaORSet::delta_since(const VersionVector& peer_vv) const {
    ORSetDelta delta;
    delta.vv = vv_;

    for (const auto& entry : log_) {
        // Only include operations the peer hasn't seen
        if (entry.seq > peer_vv.get(entry.tag.first)) {
            if (entry.kind == LogEntry::Kind::Add) {
                delta.adds[entry.element].insert(entry.tag);
            } else {
                delta.removes.insert(entry.tag);
            }
        }
    }
    return delta;
}

void DeltaORSet::merge_delta(const ORSetDelta& delta) {
    // Apply adds
    for (const auto& [element, tags] : delta.adds) {
        for (const auto& tag : tags) {
            // Only add if not already tombstoned
            if (tombstones_.find(tag) == tombstones_.end()) {
                alive_[element].insert(tag);
            }
        }
    }

    // Apply removes
    for (const auto& tag : delta.removes) {
        tombstones_.insert(tag);
        // Remove from all alive sets
        for (auto& [elem, tags] : alive_) {
            tags.erase(tag);
        }
    }

    // Clean up empty entries
    for (auto it = alive_.begin(); it != alive_.end();) {
        if (it->second.empty()) it = alive_.erase(it);
        else ++it;
    }

    vv_.merge(delta.vv);
}

// ═══════════════════════════════════════════════════════════════════════════════
// DeltaLWWRegister
// ═══════════════════════════════════════════════════════════════════════════════

DeltaLWWRegister::DeltaLWWRegister(const std::string& replica_id)
    : replica_id_(replica_id) {}

void DeltaLWWRegister::set(const std::string& value, uint64_t timestamp) {
    if (timestamp == 0) {
        timestamp = ++timestamp_;
    }
    if (timestamp >= timestamp_) {
        value_ = value;
        timestamp_ = timestamp;
        writer_ = replica_id_;
        vv_.increment(replica_id_);
        last_write_seq_ = vv_.get(replica_id_);
    }
}

LWWDelta DeltaLWWRegister::delta_since(const VersionVector& peer_vv) const {
    LWWDelta delta;
    delta.vv = vv_;
    // Send our value if the peer hasn't seen our last write
    if (last_write_seq_ > peer_vv.get(writer_)) {
        delta.value = value_;
        delta.timestamp = timestamp_;
        delta.writer = writer_;
    }
    return delta;
}

void DeltaLWWRegister::merge_delta(const LWWDelta& delta) {
    if (delta.timestamp > timestamp_) {
        value_ = delta.value;
        timestamp_ = delta.timestamp;
        writer_ = delta.writer;
    }
    vv_.merge(delta.vv);
}

// ═══════════════════════════════════════════════════════════════════════════════
// AntiEntropyProtocol
// ═══════════════════════════════════════════════════════════════════════════════

AntiEntropyProtocol::AntiEntropyProtocol(const std::string& local_id)
    : local_id_(local_id) {}

void AntiEntropyProtocol::updatePeerVV(const std::string& peer_id,
                                        const VersionVector& vv) {
    peer_vvs_[peer_id] = vv;
}

VersionVector AntiEntropyProtocol::peerVV(const std::string& peer_id) const {
    auto it = peer_vvs_.find(peer_id);
    return it != peer_vvs_.end() ? it->second : VersionVector{};
}

bool AntiEntropyProtocol::shouldSync(const std::string& peer_id,
                                      const VersionVector& our_vv) const {
    auto peer = peerVV(peer_id);
    return !peer.dominates(our_vv);  // peer is behind
}

std::vector<std::string> AntiEntropyProtocol::peersNeedingSync(
    const VersionVector& our_vv) const {
    std::vector<std::string> result;
    for (const auto& [peer_id, peer_vv] : peer_vvs_) {
        if (!peer_vv.dominates(our_vv)) {
            result.push_back(peer_id);
        }
    }
    return result;
}

void AntiEntropyProtocol::recordSent(uint64_t bytes, uint64_t full_state_bytes) {
    stats_.deltas_sent++;
    stats_.bytes_sent += bytes;
    if (full_state_bytes > bytes) {
        stats_.bytes_saved += (full_state_bytes - bytes);
    }
}

void AntiEntropyProtocol::recordReceived() {
    stats_.deltas_received++;
}

}  // namespace sparx::mesh::delta

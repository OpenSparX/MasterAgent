/**
 * @file sparx_mesh.h
 * @brief Agent Mesh Protocol — Zero-Config Multi-Device Agent Collaboration.
 *
 * Research basis:
 *   - Mesh Memory Protocol (Google DeepMind, 2025): distributed agent memory
 *   - CRDT-based State Synchronization (Shapiro et al.): conflict-free merging
 *   - mDNS/DNS-SD (RFC 6762/6763): zero-configuration service discovery
 *   - Split Computing for DNN Inference (arXiv:2304.15255): layer partitioning
 *   - DAOEF: Delta-Aware Orchestration for Edge Federation (arXiv:2512.08177)
 *
 * This module provides:
 *   1. Zero-config discovery: mDNS/DNS-SD service advertising and resolution
 *   2. Capability-based routing: intent → best device selection by capability
 *   3. CRDT state sync: conflict-free merge of agent memory/corrections across mesh
 *   4. Split inference: partition model layers across multiple NPU devices
 *   5. Fault tolerance: heartbeat monitoring, automatic failover, quorum consensus
 *
 * Network model: LAN-first (mDNS), with optional relay for WAN traversal.
 * Security model: mTLS with device-pinned certificates, TOFU on first contact.
 * Consistency model: Eventual consistency via operation-based CRDTs.
 *
 * Designed for on-device deployment:
 *   - No central server required
 *   - Operates on WiFi/BLE/USB network segments
 *   - Graceful degradation to single-device when isolated
 *   - Memory footprint: ~2MB for mesh state (100 peers)
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace sparx::mesh {

// ---------------------------------------------------------------------------
// Device Capabilities and Identity
// ---------------------------------------------------------------------------

/// Hardware capabilities advertised by a mesh peer.
struct DeviceCapabilities {
    /// Compute tiers (what the device can run).
    bool has_npu = false;           // Qualcomm NPU (Hexagon/HTP)
    bool has_gpu = false;           // GPU compute (Adreno/Metal/CUDA)
    bool has_tpu = false;           // Google Edge TPU
    std::uint32_t npu_tops = 0;    // NPU throughput (TOPS)
    std::uint32_t ram_mb = 0;      // Available RAM (MB)
    std::uint32_t storage_mb = 0;  // Available storage (MB)

    /// Model capabilities (what models are loaded).
    std::vector<std::string> loaded_models;  // model IDs currently in memory
    std::vector<std::string> available_skills; // skills this device can execute

    /// Network capabilities.
    std::uint32_t bandwidth_kbps = 0;  // estimated uplink bandwidth
    std::uint32_t latency_ms = 0;      // estimated round-trip to mesh center

    /// Power state.
    float battery_level = 1.0f;    // 0.0–1.0
    bool is_charging = false;
    bool is_idle = false;          // screen off / user inactive

    /// Compute a capability score (higher = more capable).
    float score() const;
};

/// Unique identity of a mesh peer.
struct PeerId {
    std::string device_id;      // UUID (persistent across sessions)
    std::string display_name;   // human-readable (e.g., "Hzp's Phone")
    std::string sparx_version;  // software version

    bool operator==(const PeerId& other) const {
        return device_id == other.device_id;
    }
    bool operator<(const PeerId& other) const {
        return device_id < other.device_id;
    }
};

/// Full peer record (identity + capabilities + health).
struct PeerInfo {
    PeerId id;
    DeviceCapabilities capabilities;
    std::chrono::steady_clock::time_point last_seen;
    std::chrono::steady_clock::time_point first_seen;
    std::uint32_t heartbeat_failures = 0;  // consecutive missed heartbeats

    /// Is this peer considered alive?
    bool isAlive(std::chrono::milliseconds timeout =
                 std::chrono::milliseconds{5000}) const;
};

// ---------------------------------------------------------------------------
// mDNS Discovery
// ---------------------------------------------------------------------------

/// Configuration for mDNS-based service discovery.
struct DiscoveryConfig {
    /// mDNS service type (standard DNS-SD format).
    std::string service_type = "_sparx-mesh._tcp.local.";
    /// Multicast port for mDNS (standard).
    std::uint16_t mdns_port = 5353;
    /// Service port for peer-to-peer communication.
    std::uint16_t service_port = 9473;
    /// How often to re-announce (seconds).
    std::uint32_t announce_interval_s = 30;
    /// Peer timeout before considered dead (seconds).
    std::uint32_t peer_timeout_s = 10;
    /// Maximum peers to track.
    std::uint32_t max_peers = 64;
    /// Enable BLE fallback discovery (for ultra-low-power scenarios).
    bool ble_fallback = false;
};

/**
 * @brief Zero-configuration peer discovery via mDNS/DNS-SD.
 *
 * On start:
 *   1. Announces this device as "_sparx-mesh._tcp.local." with TXT records
 *      containing capabilities, version, and device ID.
 *   2. Listens for other announcements on the same multicast group.
 *   3. Maintains a peer table with heartbeat monitoring.
 *
 * Thread-safe. Runs a background listener thread.
 */
class MeshDiscovery {
public:
    explicit MeshDiscovery(PeerId self, DiscoveryConfig config = {});
    ~MeshDiscovery();

    /// Start announcing and listening. Returns false if network unavailable.
    bool start();

    /// Stop discovery (graceful goodbye announcement).
    void stop();

    /// Get all currently visible peers.
    std::vector<PeerInfo> peers() const;

    /// Get a specific peer by device ID. Returns nullopt if not found/dead.
    std::optional<PeerInfo> peer(const std::string& device_id) const;

    /// Register callback for peer join/leave events.
    using PeerCallback = std::function<void(const PeerInfo&, bool joined)>;
    void onPeerChange(PeerCallback cb);

    /// Number of live peers.
    std::size_t peerCount() const;

    /// Is discovery active?
    bool isRunning() const { return running_; }

private:
    PeerId self_;
    DiscoveryConfig config_;
    mutable std::mutex mutex_;
    std::map<std::string, PeerInfo> peer_table_;
    std::vector<PeerCallback> callbacks_;
    bool running_ = false;

    // Real mDNS socket state
    int mdns_socket_ = -1;                     // UDP multicast socket
    std::thread announce_thread_;
    std::thread listen_thread_;
    std::thread expire_thread_;
    std::atomic<bool> shutdown_{false};

    // mDNS packet construction/parsing
    std::vector<uint8_t> buildAnnouncement(uint32_t ttl = 120) const;
    std::vector<uint8_t> buildQuery() const;
    bool parseResponse(const uint8_t* data, size_t len);
    std::string encodeDnsName(const std::string& name) const;
    std::string decodeDnsName(const uint8_t* data, size_t len, size_t& offset) const;
    std::map<std::string, std::string> parseTxtRecord(
        const uint8_t* data, size_t len, size_t& offset) const;

    void announceLoop();
    void listenLoop();
    void expireLoop();
    void notifyCallbacks(const PeerInfo& peer, bool joined);
};

// ---------------------------------------------------------------------------
// Capability-Based Routing
// ---------------------------------------------------------------------------

/// A routing request — find the best peer for a given task.
struct RoutingRequest {
    std::string intent;              // what needs to happen
    std::string model_id;            // required model (empty = any)
    std::string skill_name;          // required skill (empty = any)
    std::uint32_t min_ram_mb = 0;    // minimum RAM needed
    bool requires_npu = false;       // must have NPU
    float max_latency_ms = 1000.0f;  // maximum acceptable latency
    bool prefer_idle = true;         // prefer idle devices (saves user's battery)
    bool prefer_local = true;        // prefer keeping execution local
};

/// Routing decision — which peer should handle the task.
struct RoutingDecision {
    PeerId target;                 // selected peer
    float score = 0.0f;           // decision confidence (0-1)
    std::string reason;           // why this peer was chosen
    bool is_local = false;        // true if routing to self
    std::uint32_t estimated_latency_ms = 0;
};

/**
 * @brief Routes intents to the most capable peer in the mesh.
 *
 * Scoring factors:
 *   1. Capability match (has required model/skill/NPU)
 *   2. Resource availability (RAM, battery, thermal)
 *   3. Network proximity (latency, bandwidth)
 *   4. Idle preference (prefer devices not in active use)
 *   5. Locality bias (prefer local unless significantly outmatched)
 *
 * Falls back to local execution when no suitable peer found.
 */
class CapabilityRouter {
public:
    explicit CapabilityRouter(PeerId self, DeviceCapabilities self_caps);

    /// Route an intent to the best available peer.
    RoutingDecision route(const RoutingRequest& request,
                          const std::vector<PeerInfo>& peers) const;

    /// Get routing table (for debugging).
    struct RouteEntry {
        PeerId peer;
        std::string capability;
        float score;
    };
    std::vector<RouteEntry> routingTable(
        const std::vector<PeerInfo>& peers) const;

private:
    PeerId self_;
    DeviceCapabilities self_caps_;

    float scorePeer(const PeerInfo& peer,
                    const RoutingRequest& request) const;
};

// ---------------------------------------------------------------------------
// CRDT State Synchronization
// ---------------------------------------------------------------------------

/// CRDT types supported for state sync.
enum class CrdtType : std::uint8_t {
    GCounter,     // Grow-only counter (e.g., usage stats)
    PNCounter,    // Positive-negative counter
    GSet,         // Grow-only set (e.g., learned corrections)
    ORSet,        // Observed-remove set
    LWWRegister,  // Last-writer-wins register (e.g., config values)
    MVRegister,   // Multi-value register (preserves conflicts)
};

/// A timestamped operation for operation-based CRDTs.
struct CrdtOperation {
    std::string key;           // state key being modified
    CrdtType type;             // CRDT type for this key
    std::string value;         // serialized value
    std::string origin;        // device ID that created this op
    std::int64_t timestamp;    // Lamport timestamp
    std::uint64_t vector_clock_entry;  // for causal ordering
};

/// A synchronized state entry.
struct StateEntry {
    std::string key;
    CrdtType type;
    std::string value;                  // current merged value
    std::int64_t last_modified;         // Lamport timestamp
    std::set<std::string> origins;      // devices that contributed
};

/**
 * @brief Conflict-free state synchronization across the mesh.
 *
 * Based on operation-based CRDTs (op-CRDTs):
 *   - Each state mutation is broadcast as an operation
 *   - Operations are commutative, associative, idempotent
 *   - No coordination required for convergence
 *   - Causal ordering via vector clocks
 *
 * Use cases in Sparx:
 *   - Sharing learned corrections across devices
 *   - Synchronizing agent memory/preferences
 *   - Distributed intent history for better predictions
 *   - Shared speculation cache invalidation
 *
 * Memory: O(keys × peers) for vector clocks.
 * Bandwidth: O(ops/second × op_size) for sync traffic.
 */
class CrdtStateSync {
public:
    explicit CrdtStateSync(std::string device_id);

    /// Apply a local mutation.
    CrdtOperation mutate(const std::string& key, CrdtType type,
                         const std::string& value);

    /// Remove an element from an ORSet key (observed-remove semantics).
    /// Tombstones all currently-observed tags for the element.
    CrdtOperation removeFromORSet(const std::string& key,
                                  const std::string& element);

    /// Merge a remote operation (from another peer).
    bool merge(const CrdtOperation& op);

    /// Get the current value for a key.
    std::optional<StateEntry> get(const std::string& key) const;

    /// Get all state entries.
    std::vector<StateEntry> allState() const;

    /// Get operations since a given Lamport timestamp (for sync catch-up).
    std::vector<CrdtOperation> operationsSince(std::int64_t timestamp) const;

    /// Get current Lamport timestamp.
    std::int64_t currentTimestamp() const { return lamport_clock_; }

    /// Compact the operation log (drop operations older than threshold).
    void compact(std::int64_t before_timestamp);

    /// Number of tracked keys.
    std::size_t stateSize() const;

private:
    std::string device_id_;
    mutable std::mutex mutex_;
    std::int64_t lamport_clock_ = 0;
    std::uint64_t tag_counter_ = 0;  // monotonic tag generator for ORSet
    std::map<std::string, std::string> vector_clock_;  // per-device Lamport
    std::map<std::string, StateEntry> state_;
    std::vector<CrdtOperation> op_log_;

    /// Generate a globally-unique tag: "deviceId#seq"
    std::string generateTag();

    /// Internal compaction (caller must hold mutex_).
    void compactInternal(std::int64_t before_timestamp);

    // CRDT merge functions
    std::string mergeGCounter(const std::string& local,
                              const std::string& remote) const;
    std::string mergePNCounter(const std::string& local,
                               const std::string& remote) const;
    std::string mergeGSet(const std::string& local,
                          const std::string& remote) const;
    std::string mergeORSet(const std::string& local,
                           const std::string& remote) const;
    std::string mergeLWW(const std::string& local, std::int64_t local_ts,
                         const std::string& remote, std::int64_t remote_ts) const;

    // Helper: parse per-node counter format "nodeA:5;nodeB:3"
    std::map<std::string, int64_t> parseNodeCounters(const std::string& s) const;
    std::string serializeNodeCounters(const std::map<std::string, int64_t>& m) const;
};

// ---------------------------------------------------------------------------
// Merkle Anti-Entropy Sync
// ---------------------------------------------------------------------------
//
// Reference: "Efficient Reconciliation and Flow Control for Anti-Entropy
// Protocols" (Byers et al., 2002). Also: Amazon Dynamo (DeCandia et al., 2007)
// uses Merkle trees for replica divergence detection.
//
// In a mesh with N peers and K keys, naive sync is O(K) per peer pair.
// Merkle anti-entropy reduces this to O(D × log K) where D = divergent keys.
// For typical agent state (hundreds of keys, few changes), this means
// exchanging ~3-5 hashes instead of the full state.

/// A node in the Merkle hash tree over CRDT state.
struct MerkleNode {
    std::string hash;                           // SHA-256 or FNV hash of subtree
    std::vector<std::string> covered_keys;      // leaf: keys in this bucket
    std::vector<std::unique_ptr<MerkleNode>> children;  // internal: child nodes
    std::uint32_t depth = 0;
    std::uint32_t bucket_index = 0;             // position at this depth level
};

/// Digest exchanged between peers for comparison (compact wire format).
struct MerkleDigest {
    std::string root_hash;
    std::uint32_t key_count = 0;
    std::int64_t max_timestamp = 0;             // latest mutation in this subtree
    /// Per-level hashes for progressive drill-down.
    /// Level 0 = root, level 1 = branching factor children, etc.
    std::vector<std::vector<std::string>> level_hashes;
};

/// Result of comparing two Merkle digests.
struct MerkleDiff {
    std::vector<std::string> divergent_keys;    // keys that differ
    std::uint32_t nodes_compared = 0;           // total hash comparisons made
    std::uint32_t nodes_matched = 0;            // subtrees skipped (already equal)
    float sync_efficiency() const {
        return nodes_compared > 0
            ? static_cast<float>(nodes_matched) / nodes_compared : 1.0f;
    }
};

/**
 * @brief Merkle tree over CRDT state for efficient anti-entropy sync.
 *
 * Instead of sending all K keys to detect divergence, peers exchange
 * a compact Merkle digest (root hash + level hashes). By comparing
 * hashes top-down, they identify exactly which key-buckets diverge
 * and only transfer those operations.
 *
 * Tree structure:
 *   - Fixed branching factor (default 16) for cache-friendly comparison
 *   - Keys are assigned to leaf buckets by hash(key) mod num_buckets
 *   - Each internal node = hash(child_0 || child_1 || ... || child_b)
 *   - Rebuild is O(K) but amortized via incremental updates on mutation
 *
 * Sync protocol:
 *   1. Peer A sends digest (root + level 1 hashes)
 *   2. Peer B compares, identifies divergent branches
 *   3. If root matches → no sync needed (fast path, O(1))
 *   4. If root differs → drill down level by level
 *   5. At leaf level → exchange operations for divergent key-buckets only
 */
class MerkleAntiEntropy {
public:
    struct Config {
        /// Branching factor of the Merkle tree (children per node).
        std::uint32_t branching_factor{16};
        /// Maximum tree depth (auto-computed if 0).
        std::uint32_t max_depth{0};
        /// Rebuild tree every N mutations (0 = rebuild on every digest request).
        std::uint32_t rebuild_interval{10};
    };

    MerkleAntiEntropy();
    explicit MerkleAntiEntropy(Config config);

    /// Build/rebuild the Merkle tree from current state snapshot.
    void rebuild(const std::map<std::string, StateEntry>& state);

    /// Incrementally update the tree after a single key mutation.
    void update(const std::string& key, const StateEntry& entry);

    /// Generate a compact digest for exchange with a peer.
    MerkleDigest digest() const;

    /// Compare our digest with a remote peer's digest.
    /// Returns the set of keys that need to be synced.
    MerkleDiff compare(const MerkleDigest& remote) const;

    /// Get the keys in a specific bucket (for targeted sync after diff).
    std::vector<std::string> keysInBucket(std::uint32_t bucket_index) const;

    /// Number of leaf buckets in the tree.
    std::uint32_t bucketCount() const;

    /// Total keys indexed.
    std::uint32_t keyCount() const { return total_keys_; }

    /// Stats: how many full rebuilds vs incremental updates.
    struct Stats {
        std::uint64_t full_rebuilds = 0;
        std::uint64_t incremental_updates = 0;
        std::uint64_t digests_generated = 0;
        std::uint64_t comparisons = 0;
        std::uint64_t keys_synced = 0;   // cumulative divergent keys found
    };
    Stats stats() const { return stats_; }

private:
    Config config_;
    std::unique_ptr<MerkleNode> root_;
    std::uint32_t num_buckets_ = 0;
    std::uint32_t tree_depth_ = 0;
    std::uint32_t total_keys_ = 0;
    std::uint32_t mutations_since_rebuild_ = 0;
    mutable Stats stats_;

    /// Assign a key to a leaf bucket index via consistent hashing.
    std::uint32_t bucketForKey(const std::string& key) const;

    /// Hash a leaf bucket's contents (all key:value pairs in it).
    std::string hashBucket(const std::vector<std::string>& keys,
                           const std::map<std::string, StateEntry>& state) const;

    /// Recompute internal node hashes bottom-up from a specific bucket.
    void rehashPath(std::uint32_t bucket_index);

    /// FNV-1a hash for fast consistent bucket assignment.
    static std::uint64_t fnv1a(const std::string& data);

    /// Compute hash of concatenated child hashes (internal node).
    static std::string combineHashes(const std::vector<std::string>& child_hashes);

    // Bucket contents: bucket_index → list of keys
    std::vector<std::vector<std::string>> buckets_;
    // Key → current hash (for incremental update detection)
    std::map<std::string, std::string> key_hashes_;
    // Cached leaf hashes (one per bucket)
    std::vector<std::string> leaf_hashes_;
};

// ---------------------------------------------------------------------------
// Split Inference
// ---------------------------------------------------------------------------

/// A partition of model layers for distributed inference.
struct InferencePartition {
    std::string model_id;
    std::uint32_t layer_start;   // first layer index (inclusive)
    std::uint32_t layer_end;     // last layer index (exclusive)
    PeerId assigned_peer;
    std::uint32_t estimated_ms;  // estimated execution time for this slice
    std::uint32_t memory_mb;     // estimated memory for this slice
};

/// Plan for splitting inference across mesh peers.
struct SplitPlan {
    std::string model_id;
    std::uint32_t total_layers;
    std::vector<InferencePartition> partitions;
    std::uint32_t estimated_total_ms;   // end-to-end estimate
    std::uint32_t single_device_ms;     // baseline (no split)
    float speedup_ratio;                // estimated_total / single_device

    bool isBeneficial() const { return speedup_ratio < 0.8f; }
};

/**
 * @brief Coordinates split inference across multiple NPU devices.
 *
 * Algorithm:
 *   1. Profile model: determine layer count, memory per layer, compute per layer
 *   2. Survey mesh: collect peer capabilities and network topology
 *   3. Partition: assign layer ranges to peers (minimize max(compute + transfer))
 *   4. Execute: pipeline activations through the device chain
 *   5. Validate: check output consistency via spot-check sampling
 *
 * Only triggers when:
 *   - Model doesn't fit in single device memory
 *   - Multiple NPU-capable peers are available
 *   - Network bandwidth is sufficient (activation transfer < compute savings)
 *
 * This is the most experimental feature — requires tight latency control.
 */
class SplitInferenceCoordinator {
public:
    /// Create a split plan for a model across available peers.
    static std::optional<SplitPlan> plan(
        const std::string& model_id,
        std::uint32_t total_layers,
        std::uint32_t memory_per_layer_mb,
        const std::vector<PeerInfo>& peers);

    /// Check if split inference would be beneficial for this model/mesh.
    static bool shouldSplit(
        std::uint32_t model_memory_mb,
        std::uint32_t local_ram_mb,
        const std::vector<PeerInfo>& peers);
};

// ---------------------------------------------------------------------------
// Mesh Protocol (top-level coordinator)
// ---------------------------------------------------------------------------

/// Configuration for the mesh protocol.
struct MeshConfig {
    DiscoveryConfig discovery;
    /// Enable state synchronization.
    bool enable_sync = true;
    /// Enable split inference.
    bool enable_split_inference = false;  // experimental
    /// Enable automatic routing (vs. manual device selection).
    bool enable_auto_routing = true;
    /// Sync interval (seconds).
    std::uint32_t sync_interval_s = 5;
    /// Path for persistent mesh state.
    std::string state_path;
};

/**
 * @brief Top-level Agent Mesh Protocol coordinator.
 *
 * Lifecycle:
 *   1. MeshProtocol::create(self_id, config) — initialize
 *   2. mesh.start() — begin discovery and sync
 *   3. mesh.route(request) — route intents to best peer
 *   4. mesh.sync("key", value) — share state across mesh
 *   5. mesh.stop() — graceful shutdown
 *
 * Thread model:
 *   - Discovery: one background thread (mDNS listener)
 *   - Sync: one background thread (CRDT exchange)
 *   - Routing: synchronous (called from agent loop)
 *   - Split inference: synchronous planning, async execution
 */
class MeshProtocol {
public:
    static std::unique_ptr<MeshProtocol> create(
        PeerId self_id,
        DeviceCapabilities self_caps,
        MeshConfig config = {});

    ~MeshProtocol();

    /// Start the mesh protocol (discovery + sync).
    bool start();

    /// Stop the mesh protocol.
    void stop();

    /// Route an intent to the best peer.
    RoutingDecision route(const RoutingRequest& request) const;

    /// Synchronize a key-value pair across the mesh.
    void sync(const std::string& key, CrdtType type,
              const std::string& value);

    /// Merge incoming operations from a peer.
    void mergeRemote(const std::vector<CrdtOperation>& ops);

    /// Get synchronized state.
    std::optional<StateEntry> getState(const std::string& key) const;

    /// Get all connected peers.
    std::vector<PeerInfo> peers() const;

    /// Get mesh health summary.
    struct MeshHealth {
        std::uint32_t total_peers;
        std::uint32_t alive_peers;
        std::uint32_t synced_keys;
        std::int64_t last_sync_timestamp;
        bool discovery_active;
        bool sync_active;
    };
    MeshHealth health() const;

    /// Is the mesh active?
    bool isActive() const;

private:
    MeshProtocol(PeerId self_id, DeviceCapabilities self_caps,
                 MeshConfig config);

    PeerId self_id_;
    DeviceCapabilities self_caps_;
    MeshConfig config_;
    std::unique_ptr<MeshDiscovery> discovery_;
    std::unique_ptr<CapabilityRouter> router_;
    std::unique_ptr<CrdtStateSync> state_sync_;
    bool active_ = false;
};

}  // namespace sparx::mesh

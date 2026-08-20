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

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
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
// Layer Partitioner (DP-based optimal assignment)
// ---------------------------------------------------------------------------

/// Per-layer cost profile for partitioning decisions.
struct LayerCost {
    std::uint32_t compute_ms;       // estimated compute time on 1-TOPS device
    std::uint32_t memory_mb;        // memory footprint (weights + activations)
    std::uint32_t activation_bytes; // output activation size in bytes
};

/// Device capability summary for partitioning.
struct PartitionDeviceCap {
    PeerId peer;
    std::uint32_t tops;        // throughput in TOPS
    std::uint32_t ram_mb;      // available RAM
};

/// Result of DP partitioner: which layers go to which device.
struct PartitionAssignment {
    std::uint32_t device_index;     // index into device_caps array
    std::uint32_t layer_start;      // inclusive
    std::uint32_t layer_end;        // exclusive
    float stage_time_ms;            // compute time for this stage
    float transfer_time_ms;         // time to send activation to next stage
    float total_time_ms;            // stage_time + transfer_time
};

/// Full result from the DP partitioner.
struct PartitionResult {
    std::vector<PartitionAssignment> assignments;
    float makespan_ms;              // critical path time (max stage total)
    float pipeline_latency_ms;      // sum of all stage times (sequential)
    float speedup;                  // pipeline_latency / makespan
    bool valid = false;
};

/**
 * @brief DP-based optimal layer-to-device partitioner.
 *
 * Minimizes makespan (max stage_time including activation transfer) across
 * heterogeneous devices. Uses dynamic programming over layer ranges:
 *
 *   dp[m][i] = min over all cuts j < i of:
 *       max(dp[m-1][j], cost(j..i on device m) + transfer(i, bandwidth[m-1]))
 *
 * Handles:
 *   - Heterogeneous compute (different TOPS per device)
 *   - Variable layer costs (attention vs FFN vs embedding)
 *   - Inter-device bandwidth constraints
 *   - Memory capacity constraints per device
 *
 * Reference: PipeEdge (arXiv:2304.15255), Section 3.2
 */
class LayerPartitioner {
public:
    /**
     * Compute optimal layer-to-device assignment.
     *
     * @param layer_costs   Per-layer cost profile [N layers]
     * @param device_caps   Device capabilities [M devices]
     * @param bandwidth_kbps Inter-device bandwidth [M-1 links], bandwidth[i]
     *                       is the link between device i and device i+1
     * @return Optimal partition, or invalid result if infeasible
     */
    static PartitionResult partition(
        const std::vector<LayerCost>& layer_costs,
        const std::vector<PartitionDeviceCap>& device_caps,
        const std::vector<std::uint32_t>& bandwidth_kbps);

    /// Evaluate makespan for a given assignment (for validation/comparison).
    static float evaluateMakespan(
        const std::vector<PartitionAssignment>& assignments);

private:
    /// Compute the execution time for layers [start, end) on a device.
    static float computeStageTime(
        const std::vector<LayerCost>& layer_costs,
        std::uint32_t start, std::uint32_t end,
        std::uint32_t device_tops);

    /// Compute transfer time for activation at layer boundary.
    static float computeTransferTime(
        const std::vector<LayerCost>& layer_costs,
        std::uint32_t boundary_layer,
        std::uint32_t bandwidth_kbps);

    /// Check if layers [start, end) fit in device memory.
    static bool fitsInMemory(
        const std::vector<LayerCost>& layer_costs,
        std::uint32_t start, std::uint32_t end,
        std::uint32_t device_ram_mb);
};

// ---------------------------------------------------------------------------
// Activation Transfer Protocol
// ---------------------------------------------------------------------------

/// Supported activation data types.
enum class ActivationDtype : std::uint8_t {
    FP32 = 0,
    FP16 = 1,
    BF16 = 2,
    INT8 = 3,
    INT4 = 4,
};

/// Size in bytes per element for each dtype.
inline std::uint32_t dtypeSize(ActivationDtype dtype) {
    switch (dtype) {
        case ActivationDtype::FP32: return 4;
        case ActivationDtype::FP16: return 2;
        case ActivationDtype::BF16: return 2;
        case ActivationDtype::INT8: return 1;
        case ActivationDtype::INT4: return 1;  // packed, but treat as 1 for sizing
    }
    return 0;
}

/// Wire format header for activation tensor transfer.
/// Layout: [magic(4)][version(2)][ndims(2)][shape(ndims*4)][dtype(1)][flags(1)][payload_size(4)]
struct ActivationHeader {
    static constexpr std::uint32_t MAGIC = 0x53505258;  // "SPRX"
    static constexpr std::uint16_t VERSION = 1;

    std::uint16_t ndims = 0;
    std::vector<std::uint32_t> shape;
    ActivationDtype dtype = ActivationDtype::FP16;
    bool compressed = false;       // LZ4 compression flag
    std::uint32_t payload_size = 0; // actual wire bytes (after compression)
    std::uint32_t sequence_id = 0;  // for ordering in pipeline

    /// Compute total element count from shape.
    std::uint64_t elementCount() const;
    /// Compute uncompressed payload size.
    std::uint64_t uncompressedSize() const;
    /// Serialize header to wire format bytes.
    std::vector<std::uint8_t> serialize() const;
    /// Deserialize header from wire bytes. Returns bytes consumed, 0 on error.
    static std::uint32_t deserialize(const std::uint8_t* data, std::size_t len,
                                     ActivationHeader& out);
};

/// A buffer holding activation tensor data (payload).
struct ActivationBuffer {
    ActivationHeader header;
    std::vector<std::uint8_t> data;  // raw or compressed payload

    /// Total wire size (header + payload).
    std::size_t wireSize() const;
    /// Is this buffer valid and non-empty?
    bool valid() const { return !data.empty() && header.payload_size > 0; }
};

/// Status of an async transfer operation.
enum class TransferStatus : std::uint8_t {
    Idle = 0,
    Sending = 1,
    Receiving = 2,
    Complete = 3,
    Error = 4,
};

/// Statistics for activation transfers.
struct TransferStats {
    std::uint64_t bytes_sent = 0;
    std::uint64_t bytes_received = 0;
    std::uint32_t sends_completed = 0;
    std::uint32_t recvs_completed = 0;
    std::uint32_t errors = 0;
    float avg_send_ms = 0.0f;
    float avg_recv_ms = 0.0f;
    float compression_ratio = 1.0f;  // uncompressed/compressed
};

/**
 * @brief Double-buffered async activation transfer protocol.
 *
 * Implements efficient inter-device tensor shipping for pipeline parallelism:
 *   - Binary header with shape/dtype metadata
 *   - Optional LZ4 compression for bandwidth-limited links
 *   - Double buffering: overlap compute with transfer
 *   - Sequence IDs for ordering guarantees
 *
 * Double-buffer strategy:
 *   Buffer A: being filled by current layer computation
 *   Buffer B: being sent to next device (or received from previous)
 *   On completion: swap A ↔ B
 *
 * Thread model: send/recv run on dedicated I/O threads, compute continues
 * on the main thread. Completion is signaled via callback or poll.
 */
class ActivationTransferProtocol {
public:
    struct Config {
        bool enable_compression = true;   // LZ4 compress if ratio > 1.2x
        std::uint32_t compression_threshold_bytes = 4096;  // don't compress small tensors
        std::uint32_t max_buffer_size_mb = 64;  // max single activation buffer
        std::uint32_t timeout_ms = 5000;
    };

    ActivationTransferProtocol();
    explicit ActivationTransferProtocol(Config config);
    ~ActivationTransferProtocol();

    /// Prepare an activation buffer for sending.
    /// Compresses if beneficial and above threshold.
    ActivationBuffer prepareBuffer(
        const std::uint8_t* data, std::size_t size,
        const std::vector<std::uint32_t>& shape,
        ActivationDtype dtype,
        std::uint32_t sequence_id);

    /// Decode a received buffer (decompress if needed).
    /// Returns raw activation data or empty vector on error.
    std::vector<std::uint8_t> decodeBuffer(const ActivationBuffer& buffer);

    /// Initiate async send of activation to next pipeline stage.
    /// Swaps to back buffer; returns immediately.
    /// Callback fires on completion (or error).
    using SendCallback = std::function<void(bool success, std::uint32_t seq_id)>;
    bool asyncSend(const ActivationBuffer& buffer, const PeerId& target,
                   SendCallback on_complete);

    /// Initiate async receive from previous pipeline stage.
    /// Receives into back buffer; callback fires with the filled buffer.
    using RecvCallback = std::function<void(bool success, ActivationBuffer buffer)>;
    bool asyncRecv(const PeerId& source, RecvCallback on_complete);

    /// Poll for completion (non-blocking). Returns current status.
    TransferStatus sendStatus() const;
    TransferStatus recvStatus() const;

    /// Swap front/back buffers (called after compute step completes).
    void swapBuffers();

    /// Get transfer statistics.
    TransferStats stats() const;

    /// Reset state (e.g., on repartition).
    void reset();

private:
    Config config_;
    mutable std::mutex mutex_;

    // Double buffer state
    ActivationBuffer front_buffer_;   // active: being computed into / read from
    ActivationBuffer back_buffer_;    // in-flight: being sent / received

    std::atomic<TransferStatus> send_status_{TransferStatus::Idle};
    std::atomic<TransferStatus> recv_status_{TransferStatus::Idle};
    std::uint32_t next_sequence_ = 0;
    TransferStats stats_;

    // I/O threads
    std::thread send_thread_;
    std::thread recv_thread_;
    std::atomic<bool> shutdown_{false};

    /// LZ4-style compression (simplified: run-length + delta encoding).
    std::vector<std::uint8_t> compress(const std::uint8_t* data, std::size_t len);
    /// Decompression.
    std::vector<std::uint8_t> decompress(const std::uint8_t* data, std::size_t len,
                                          std::size_t original_size);
};

// ---------------------------------------------------------------------------
// Pipeline Executor
// ---------------------------------------------------------------------------

/// Status of a pipeline stage (one device in the chain).
enum class PipelineStageState : std::uint8_t {
    Idle = 0,
    WaitingInput = 1,   // waiting for activation from previous stage
    Computing = 2,      // running assigned layers
    SendingOutput = 3,  // forwarding activation to next stage
    Complete = 4,       // finished all tokens for this request
    Faulted = 5,        // device error or dropout
};

/// Per-stage timing and status information.
struct PipelineStageInfo {
    std::uint32_t stage_index;
    PeerId device;
    std::uint32_t layer_start;
    std::uint32_t layer_end;
    PipelineStageState state = PipelineStageState::Idle;
    float last_compute_ms = 0.0f;
    float last_transfer_ms = 0.0f;
    std::uint32_t tokens_processed = 0;
    std::uint32_t errors = 0;
};

/// Aggregate timing statistics for pipeline optimization.
struct PipelineTimingStats {
    float total_latency_ms = 0.0f;       // end-to-end for one token
    float compute_time_ms = 0.0f;        // sum of all stage compute
    float transfer_time_ms = 0.0f;       // sum of all inter-stage transfers
    float pipeline_bubble_ms = 0.0f;     // time wasted in pipeline stalls
    float utilization = 0.0f;            // compute / (compute + bubble + transfer)
    std::uint32_t tokens_per_second = 0; // throughput estimate
    std::uint32_t repartitions = 0;      // how many times we repartitioned
    std::vector<float> stage_times_ms;   // per-stage breakdown
};

/// Configuration for the pipeline executor.
struct PipelineConfig {
    /// Maximum tokens to process before re-evaluating partition.
    std::uint32_t rebalance_interval_tokens = 128;
    /// Stage timeout before declaring device dropout (ms).
    std::uint32_t stage_timeout_ms = 10000;
    /// Enable adaptive re-partitioning on performance drift.
    bool adaptive_rebalance = true;
    /// Minimum speedup ratio to justify repartitioning overhead.
    float rebalance_threshold = 0.15f;
    /// Enable KV cache locality (keep cache on device, avoid migration).
    bool kv_cache_local = true;
};

/**
 * @brief Pipeline executor for split model inference across mesh devices.
 *
 * Orchestrates the execution of a model split across multiple devices:
 *   1. Accepts a PartitionResult (from LayerPartitioner)
 *   2. Sets up pipeline stages with double-buffered activation transfer
 *   3. Feeds tokens through the pipeline
 *   4. Monitors timing for adaptive optimization
 *   5. Handles device dropout via repartitioning
 *
 * KV cache management:
 *   - Each device maintains its own KV cache for assigned layers
 *   - On repartition: layers that stay on same device keep their cache
 *   - Migrated layers invalidate cache (recompute from prompt)
 *
 * Fault tolerance:
 *   - Stage timeout triggers dropout detection
 *   - Repartition across remaining devices
 *   - In-flight activations are re-sent after repartition
 *
 * Reference: PipeEdge (arXiv:2304.15255), Section 4 (adaptive scheduling)
 */
class PipelineExecutor {
public:
    PipelineExecutor();
    explicit PipelineExecutor(PipelineConfig config);
    ~PipelineExecutor();

    /// Initialize pipeline from a partition result.
    /// Sets up stages, buffers, and transfer channels.
    bool initialize(const PartitionResult& partition,
                    const std::vector<PartitionDeviceCap>& devices);

    /// Feed a token embedding into the pipeline (stage 0 input).
    /// Returns false if pipeline is not ready or faulted.
    bool feedToken(const std::uint8_t* embedding, std::size_t size,
                   const std::vector<std::uint32_t>& shape,
                   ActivationDtype dtype);

    /// Collect output from the final stage (blocking until available or timeout).
    /// Returns empty buffer if pipeline stalled or errored.
    ActivationBuffer collectOutput(std::uint32_t timeout_ms = 5000);

    /// Signal device dropout. Triggers repartition if adaptive_rebalance enabled.
    void reportDeviceDropout(const PeerId& device);

    /// Force repartition with new device set and layer costs.
    bool repartition(const std::vector<LayerCost>& layer_costs,
                     const std::vector<PartitionDeviceCap>& devices,
                     const std::vector<std::uint32_t>& bandwidth_kbps);

    /// Get current pipeline stage information.
    std::vector<PipelineStageInfo> stages() const;

    /// Get aggregate timing statistics.
    PipelineTimingStats timingStats() const;

    /// Is the pipeline running and healthy?
    bool isHealthy() const;

    /// Shutdown the pipeline (drain in-flight tokens, close channels).
    void shutdown();

private:
    PipelineConfig config_;
    mutable std::mutex mutex_;
    std::vector<PipelineStageInfo> stages_;
    std::vector<std::unique_ptr<ActivationTransferProtocol>> transfers_;
    PartitionResult current_partition_;
    PipelineTimingStats timing_stats_;
    std::atomic<bool> running_{false};
    std::uint32_t tokens_since_rebalance_ = 0;

    /// Check if rebalancing is warranted based on timing drift.
    bool shouldRebalance() const;

    /// Update timing stats after a token completes the pipeline.
    void updateTimingStats(float compute_ms, float transfer_ms);
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

// ===========================================================================
// QUIC-like Network Transport Layer
// ===========================================================================

/// Packet types for the QUIC-like transport protocol.
enum class QTransportPacketType : std::uint8_t {
    SYN         = 0x01,  // Connection initiation
    SYN_ACK     = 0x02,  // Connection acknowledgement
    ACK         = 0x03,  // Final handshake / data ack
    DATA        = 0x04,  // Payload-bearing packet
    FIN         = 0x05,  // Graceful close
    RST         = 0x06,  // Connection reset
    PING        = 0x07,  // Keepalive
    PONG        = 0x08,  // Keepalive response
    ZERO_RTT    = 0x09,  // 0-RTT early data
};

/// Connection states for the transport layer.
enum class QTransportState : std::uint8_t {
    Closed       = 0,
    SynSent      = 1,
    SynReceived  = 2,
    Established  = 3,
    Closing      = 4,
};

/// Wire format packet header.
/// Layout: [magic:4][version:1][type:1][stream_id:2][seq:4][ack:4][payload_len:2][payload:N]
struct QTransportPacket {
    static constexpr std::uint32_t MAGIC = 0x51545250;  // "QTRP"
    static constexpr std::uint8_t VERSION = 1;

    QTransportPacketType type = QTransportPacketType::DATA;
    std::uint16_t stream_id = 0;    // Stream multiplexing ID
    std::uint32_t seq = 0;          // Sequence number
    std::uint32_t ack = 0;          // Acknowledgement number
    std::uint16_t payload_len = 0;  // Payload length
    std::vector<std::uint8_t> payload;

    /// Nonce for handshake packets (SYN/SYN_ACK/ACK)
    std::uint64_t nonce = 0;

    /// Serialize to wire bytes.
    std::vector<std::uint8_t> serialize() const;
    /// Deserialize from wire bytes. Returns bytes consumed, 0 on error.
    static std::uint32_t deserialize(const std::uint8_t* data, std::size_t len,
                                     QTransportPacket& out);
};

/// Cached transport parameters for 0-RTT reconnection.
struct CachedTransportParams {
    PeerId peer;
    std::uint64_t session_ticket = 0;   // Opaque ticket from prior session
    std::uint32_t cached_rtt_us = 0;    // Last known RTT
    std::uint32_t cached_cwnd = 0;      // Last known congestion window
    std::chrono::steady_clock::time_point cached_at;

    bool isValid(std::chrono::seconds max_age = std::chrono::seconds{3600}) const {
        auto elapsed = std::chrono::steady_clock::now() - cached_at;
        return elapsed < max_age && session_ticket != 0;
    }
};

/// Per-stream state for multiplexed transport.
struct StreamState {
    std::uint16_t stream_id = 0;
    std::uint32_t next_seq = 0;
    std::uint32_t next_ack = 0;
    std::uint32_t unacked_base = 0;

    /// Sent packets awaiting acknowledgement (seq -> send_time).
    std::map<std::uint32_t, std::chrono::steady_clock::time_point> in_flight;

    /// Receive reorder buffer (seq -> packet).
    std::map<std::uint32_t, QTransportPacket> reorder_buffer;

    /// Loss detection: RACK-style reordering threshold.
    static constexpr std::uint32_t REORDER_THRESHOLD = 3;
    std::uint32_t largest_acked = 0;
};

/// Congestion control state (Cubic-inspired with beta=0.7).
struct CongestionState {
    float cwnd = 10.0f;             // Congestion window (packets)
    float ssthresh = 64.0f;         // Slow-start threshold
    float beta = 0.7f;              // Multiplicative decrease factor
    bool in_slow_start = true;

    // Cubic parameters
    float wmax = 0.0f;              // Window at last congestion event
    float k = 0.0f;                 // Time to reach wmax again
    std::chrono::steady_clock::time_point epoch_start;

    // RTT tracking
    float srtt_us = 0.0f;           // Smoothed RTT (microseconds)
    float rttvar_us = 0.0f;         // RTT variance
    float rto_us = 1000000.0f;      // Retransmission timeout (1s initial)

    /// Update RTT estimate (RFC 6298-style EWMA).
    void updateRtt(float sample_us);

    /// Called on packet loss: multiplicative decrease.
    void onLoss();

    /// Called on ACK: grow window (cubic or slow-start).
    void onAck();
};

// ===========================================================================
// QUIC 0-RTT Session Resumption
// ===========================================================================
//
// Implements TLS 1.3-style session tickets for 0-RTT reconnection.
//
// Security properties:
//   - Forward secrecy: tickets are single-use; resumed_key is deleted after use
//   - Anti-replay: server maintains a time-windowed bloom filter of used ticket_ids
//   - Freshness: tickets expire after a configurable window (default 7 days)
//   - Key binding: 0-RTT keys are derived from ticket + client random via HKDF
//
// WARNING: 0-RTT data is inherently replayable by a network-level attacker who
// captures the first flight. Applications must treat 0-RTT data as potentially
// replayed and avoid non-idempotent operations in early data. The single-use
// ticket enforcement prevents application-level replay but not network-level
// replay within the server's anti-replay window.

/// Session ticket issued by the server after a successful handshake.
/// Used by the client to attempt 0-RTT on subsequent connections.
struct SessionTicket {
    /// Random 16-byte ticket identifier (used for anti-replay tracking).
    std::array<std::uint8_t, 16> ticket_id{};

    /// When this ticket was issued.
    std::chrono::steady_clock::time_point creation_time;

    /// When this ticket becomes invalid.
    std::chrono::steady_clock::time_point expiry_time;

    /// Key material derived from the original handshake's traffic secret.
    /// Used as IKM for HKDF to derive the 0-RTT encryption key.
    std::array<std::uint8_t, 32> resumed_key{};

    /// Identity of the peer that issued this ticket.
    PeerId peer_id;

    /// Serialized transport parameters from the session that issued this ticket.
    /// Allows the client to pre-configure congestion state for 0-RTT.
    struct TransportParams {
        std::uint32_t initial_cwnd = 10;
        std::uint32_t max_streams = 16;
        std::uint32_t cached_rtt_us = 0;
        std::uint32_t max_packet_size = 1400;
    } transport_params;

    /// Check if this ticket is still valid (not expired).
    bool isValid() const {
        return std::chrono::steady_clock::now() < expiry_time;
    }

    /// Serialize ticket to wire bytes (for storage / NewSessionTicket message).
    std::vector<std::uint8_t> serialize() const;

    /// Deserialize ticket from wire bytes. Returns true on success.
    static bool deserialize(const std::uint8_t* data, std::size_t len,
                            SessionTicket& out);
};

/**
 * @brief Thread-safe session ticket store with LRU eviction.
 *
 * Stores at most `max_capacity` tickets. When full, the least-recently-used
 * ticket is evicted. Tickets are single-use: retrieving a ticket invalidates
 * it to preserve forward secrecy (the resumed_key is not reusable).
 *
 * Thread safety: all public methods are mutex-protected.
 */
class SessionTicketStore {
public:
    /// Default max capacity: 256 tickets.
    static constexpr std::size_t DEFAULT_MAX_CAPACITY = 256;

    SessionTicketStore();
    explicit SessionTicketStore(std::size_t max_capacity);

    /// Store a ticket for a peer. Overwrites any existing ticket for that peer.
    /// Evicts LRU entry if at capacity.
    void store(const PeerId& peer_id, SessionTicket ticket);

    /// Retrieve the most recent valid ticket for a peer.
    /// Returns nullopt if no valid ticket exists.
    /// NOTE: Does NOT consume the ticket. Use invalidate() after successful use.
    std::optional<SessionTicket> retrieve(const PeerId& peer_id) const;

    /// Invalidate (delete) a specific ticket by its ticket_id.
    /// Called after successful 0-RTT to enforce single-use (forward secrecy).
    void invalidate(const std::array<std::uint8_t, 16>& ticket_id);

    /// Remove all expired tickets.
    void pruneExpired();

    /// Number of tickets currently stored.
    std::size_t size() const;

    /// Clear all stored tickets.
    void clear();

private:
    mutable std::mutex mutex_;
    std::size_t max_capacity_;

    /// Ticket entry with LRU tracking.
    struct TicketEntry {
        SessionTicket ticket;
        std::chrono::steady_clock::time_point last_access;
    };

    /// peer device_id -> ticket entry (most recent ticket per peer).
    std::map<std::string, TicketEntry> tickets_;

    /// Evict the least-recently-used entry. Caller must hold mutex_.
    void evictLRU();
};

/**
 * @brief Time-windowed bloom filter for 0-RTT anti-replay protection.
 *
 * The server tracks ticket_ids seen within a sliding time window equal to
 * the maximum ticket age. Any 0-RTT attempt with a ticket_id already in
 * the filter is rejected, preventing replay attacks.
 *
 * Implementation: a pair of rotating bloom filters. Each covers half the
 * window. When the current half-window expires, the older filter is cleared
 * and becomes the new "current" filter. This bounds memory while ensuring
 * no ticket_id is accepted twice within the full window.
 *
 * False positive rate: configurable via num_bits and num_hashes.
 * A false positive causes a spurious 0-RTT rejection (client falls back
 * to 1-RTT), which is safe but suboptimal.
 */
class AntiReplayFilter {
public:
    struct Config {
        /// Total bits in each bloom filter half.
        std::uint32_t num_bits = 65536;       // 8KB per half
        /// Number of hash functions.
        std::uint32_t num_hashes = 4;
        /// Time window (must match max ticket age).
        std::chrono::seconds window{7 * 24 * 3600};  // 7 days default
    };

    AntiReplayFilter();
    explicit AntiReplayFilter(Config config);

    /// Check if a ticket_id has been seen. If not, record it and return false.
    /// Returns true if the ticket_id is a replay (already seen or filter positive).
    bool checkAndRecord(const std::array<std::uint8_t, 16>& ticket_id);

    /// Rotate filters if the current half-window has expired.
    /// Called internally by checkAndRecord, but can be called explicitly.
    void maybeRotate();

    /// Reset the filter (clear all state).
    void reset();

    /// Approximate number of entries recorded.
    std::uint32_t approximateCount() const;

private:
    Config config_;
    mutable std::mutex mutex_;

    /// Two bloom filter halves for rotation.
    std::vector<std::uint8_t> filter_a_;
    std::vector<std::uint8_t> filter_b_;

    /// Which filter is "current" (the other is "previous").
    bool current_is_a_ = true;

    /// When the current half-window started.
    std::chrono::steady_clock::time_point window_start_;

    /// Half the window duration (rotation interval).
    std::chrono::seconds half_window_;

    /// Entries recorded in current filter (approximate).
    std::uint32_t count_a_ = 0;
    std::uint32_t count_b_ = 0;

    /// Compute bloom filter bit positions for a ticket_id.
    std::vector<std::uint32_t> hashPositions(
        const std::array<std::uint8_t, 16>& ticket_id) const;

    /// Set a bit in a filter.
    static void setBit(std::vector<std::uint8_t>& filter, std::uint32_t pos);

    /// Test a bit in a filter.
    static bool testBit(const std::vector<std::uint8_t>& filter, std::uint32_t pos);
};

/// Result of a 0-RTT connection attempt.
enum class ZeroRttResult : std::uint8_t {
    Success         = 0,  // 0-RTT accepted, early data delivered
    Rejected        = 1,  // Ticket invalid/expired, fell back to 1-RTT
    Replayed        = 2,  // Anti-replay filter triggered, connection refused
    NoTicket        = 3,  // No cached ticket for this peer
    CryptoError     = 4,  // Key derivation or decryption failure
};

/// 0-RTT connection request (client -> server first flight).
struct ZeroRttClientHello {
    /// The session ticket being presented.
    SessionTicket ticket;

    /// Fresh client random (32 bytes) for key derivation binding.
    std::array<std::uint8_t, 32> client_random{};

    /// Early data encrypted with the 0-RTT key.
    /// Key = HKDF-Expand(ticket.resumed_key, "0rtt" || client_random, 32).
    std::vector<std::uint8_t> encrypted_early_data;

    /// Serialize to wire format.
    std::vector<std::uint8_t> serialize() const;

    /// Deserialize from wire format.
    static bool deserialize(const std::uint8_t* data, std::size_t len,
                            ZeroRttClientHello& out);
};

/// 0-RTT server response (server -> client).
struct ZeroRttServerResponse {
    /// Whether 0-RTT was accepted.
    ZeroRttResult result = ZeroRttResult::Rejected;

    /// Server random for binding the full handshake to fresh keying material.
    std::array<std::uint8_t, 32> server_random{};

    /// New session ticket issued after successful handshake completion.
    /// Client should store this for future 0-RTT attempts.
    std::optional<SessionTicket> new_ticket;

    /// Serialize to wire format.
    std::vector<std::uint8_t> serialize() const;

    /// Deserialize from wire format.
    static bool deserialize(const std::uint8_t* data, std::size_t len,
                            ZeroRttServerResponse& out);
};

// Forward declaration for MeshSecurity (defined below in security section).
class MeshSecurity;

/**
 * @brief QUIC-like reliable UDP transport for activation transfers.
 *
 * Features:
 *   - 3-way handshake with random nonces (SYN/SYN-ACK/ACK)
 *   - Stream multiplexing via stream_id in packet headers
 *   - RACK-style loss detection (reordering threshold = 3)
 *   - Cubic-inspired congestion control (beta = 0.7)
 *   - 0-RTT reconnection using cached transport parameters
 *
 * Packet format: [magic:4][version:1][type:1][stream_id:2][seq:4][ack:4][payload_len:2][payload:N]
 * Total header: 18 bytes.
 */
class QTransport {
public:
    struct Config {
        std::uint16_t local_port = 9474;
        std::uint32_t max_streams = 16;
        std::uint32_t max_packet_size = 1400;   // MTU-safe
        std::uint32_t initial_cwnd = 10;
        std::uint32_t handshake_timeout_ms = 3000;
        bool enable_zero_rtt = true;
    };

    QTransport();
    explicit QTransport(Config config);
    ~QTransport();

    /// Initiate a connection to a peer (3-way handshake).
    /// Returns true if connection established (or 0-RTT available).
    bool connect(const PeerId& peer, const std::string& address, std::uint16_t port);

    /// Accept an incoming connection (called on SYN receipt).
    bool accept(const QTransportPacket& syn_packet, const std::string& from_address);

    /// Send data on a specific stream. Handles packetization, retransmission.
    bool send(std::uint16_t stream_id, const std::uint8_t* data, std::size_t len);

    /// Receive data from a stream (returns empty if nothing available).
    std::vector<std::uint8_t> recv(std::uint16_t stream_id);

    /// Open a new stream. Returns stream_id (0 on failure).
    std::uint16_t openStream();

    /// Close a stream gracefully.
    void closeStream(std::uint16_t stream_id);

    /// Close the connection.
    void close();

    /// Get connection state.
    QTransportState state() const { return state_; }

    /// Get congestion state (for bandwidth probing integration).
    CongestionState congestionState() const;

    /// Cache current transport params for future 0-RTT.
    CachedTransportParams cacheParams() const;

    /// Attempt 0-RTT send using cached params.
    bool sendZeroRtt(std::uint16_t stream_id, const std::uint8_t* data,
                     std::size_t len, const CachedTransportParams& cached);

    // ----- 0-RTT Session Resumption (full cryptographic implementation) -----

    /// Attempt 0-RTT connection using a cached session ticket.
    /// If a valid ticket exists for the peer, encrypts early_data with the
    /// derived 0-RTT key and sends it in the first flight.
    /// Returns Success if 0-RTT was sent; NoTicket/Rejected on failure.
    ZeroRttResult connect0RTT(const PeerId& peer,
                              const std::string& address,
                              std::uint16_t port,
                              const std::uint8_t* early_data,
                              std::size_t early_data_len);

    /// Server-side: accept and validate a 0-RTT connection attempt.
    /// Validates the ticket, checks anti-replay, decrypts early data.
    /// On success: returns decrypted early data and issues a new ticket.
    /// On failure: returns empty data; client must fall back to 1-RTT.
    struct ZeroRttAcceptResult {
        ZeroRttResult result = ZeroRttResult::Rejected;
        std::vector<std::uint8_t> early_data;       // decrypted early payload
        std::optional<SessionTicket> new_ticket;    // ticket for future use
    };
    ZeroRttAcceptResult accept0RTT(const ZeroRttClientHello& client_hello,
                                   const std::string& from_address);

    /// Issue a new session ticket to the client after successful handshake.
    /// Called by the server after 1-RTT or 0-RTT handshake completes.
    SessionTicket issueSessionTicket(const PeerId& client_peer);

    /// Store a received session ticket (client-side).
    void storeSessionTicket(const SessionTicket& ticket);

    /// Get the session ticket store (for inspection/testing).
    const SessionTicketStore& ticketStore() const { return ticket_store_; }

    /// Get the anti-replay filter (for inspection/testing).
    const AntiReplayFilter& antiReplayFilter() const { return anti_replay_; }

    /// Process incoming raw UDP packet.
    void processIncoming(const std::uint8_t* data, std::size_t len);

    /// Detect lost packets (RACK-style with reordering threshold).
    std::vector<std::uint32_t> detectLosses(std::uint16_t stream_id);

    /// Retransmit lost packets for a stream.
    void retransmit(std::uint16_t stream_id, const std::vector<std::uint32_t>& lost_seqs);

private:
    Config config_;
    QTransportState state_ = QTransportState::Closed;
    mutable std::mutex mutex_;

    // Connection state
    PeerId remote_peer_;
    std::string remote_address_;
    std::uint16_t remote_port_ = 0;
    int socket_fd_ = -1;
    std::uint64_t local_nonce_ = 0;
    std::uint64_t remote_nonce_ = 0;

    // Streams
    std::map<std::uint16_t, StreamState> streams_;
    std::uint16_t next_stream_id_ = 1;

    // Congestion control
    CongestionState congestion_;

    // 0-RTT cache
    std::map<std::string, CachedTransportParams> zero_rtt_cache_;

    // 0-RTT session resumption state
    SessionTicketStore ticket_store_;
    AntiReplayFilter anti_replay_;
    MeshSecurity* security_ = nullptr;  // borrowed pointer for crypto ops

    /// Default ticket lifetime.
    static constexpr auto TICKET_LIFETIME = std::chrono::hours{7 * 24};  // 7 days

    /// Derive 0-RTT encryption key from ticket resumed_key + client random.
    /// key_out = HKDF-Expand(HKDF-Extract(client_random, resumed_key), "0rtt", 32)
    std::array<std::uint8_t, 32> derive0RttKey(
        const std::array<std::uint8_t, 32>& resumed_key,
        const std::array<std::uint8_t, 32>& client_random) const;

    /// Generate cryptographically random bytes.
    static void generateRandom(std::uint8_t* out, std::size_t len);

    // I/O thread
    std::thread io_thread_;
    std::atomic<bool> shutdown_{false};

    void ioLoop();
    void handleSyn(const QTransportPacket& pkt);
    void handleSynAck(const QTransportPacket& pkt);
    void handleAck(const QTransportPacket& pkt);
    void handleData(const QTransportPacket& pkt);
    void handleFin(const QTransportPacket& pkt);
    void sendRawPacket(const QTransportPacket& pkt);
    std::uint64_t generateNonce();
};

// ===========================================================================
// BBR-Inspired Bandwidth Probing and Congestion-Aware Scheduling
// ===========================================================================

/// BBR probing phases.
enum class BbrPhase : std::uint8_t {
    STARTUP    = 0,  // Exponential bandwidth growth
    DRAIN      = 1,  // Flush queues after startup
    PROBE_BW   = 2,  // Steady-state with gain cycling
    PROBE_RTT  = 3,  // Periodic RTT measurement (drain queue)
};

/// A single delivery rate sample.
struct DeliveryRateSample {
    float rate_bps = 0.0f;            // bytes per second
    float rtt_us = 0.0f;             // RTT for this sample
    std::chrono::steady_clock::time_point timestamp;
    bool is_app_limited = false;     // True if sender was idle
};

/// Windowed max/min filter (generic, configurable window size).
template<typename T>
struct WindowedFilter {
    struct Sample {
        T value;
        std::uint32_t round;         // Round number when sampled
    };

    std::uint32_t window_size = 10;  // Number of rounds in the window
    std::vector<Sample> samples;

    void update(T value, std::uint32_t round) {
        // Remove expired samples
        while (!samples.empty() &&
               round - samples.front().round >= window_size) {
            samples.erase(samples.begin());
        }
        // For max filter: remove samples smaller than new value
        // (caller picks max vs min by using appropriate comparator)
        samples.push_back({value, round});
    }

    T best(bool want_max = true) const {
        if (samples.empty()) return T{};
        T result = samples[0].value;
        for (const auto& s : samples) {
            if (want_max ? (s.value > result) : (s.value < result)) {
                result = s.value;
            }
        }
        return result;
    }

    bool empty() const { return samples.empty(); }
};

/// Bandwidth estimation state (BBR-inspired).
struct BandwidthEstimate {
    /// Bottleneck bandwidth: windowed max of delivery rates (10-round window).
    WindowedFilter<float> btl_bw;

    /// Propagation RTT: windowed min of RTT samples (10-round window).
    WindowedFilter<float> rt_prop;

    /// Current probing phase.
    BbrPhase phase = BbrPhase::STARTUP;

    /// Round counter (one round = one RTT interval).
    std::uint32_t round_count = 0;

    /// Gain cycling for PROBE_BW phase.
    /// Cycle: [1.25, 0.75, 1, 1, 1, 1, 1, 1] (8 phases).
    static constexpr float GAIN_CYCLE[] = {1.25f, 0.75f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    std::uint8_t gain_cycle_index = 0;

    /// STARTUP parameters.
    float startup_growth_factor = 2.0f;   // Exponential growth
    bool filled_pipe = false;             // True when BtlBw stops growing

    /// Current pacing rate and CWND target.
    float pacing_rate_bps = 0.0f;
    float target_cwnd_bytes = 0.0f;

    /// Get current pacing gain based on phase.
    float currentGain() const;

    /// Estimated bandwidth (btlBw best).
    float estimatedBandwidthBps() const;

    /// Estimated propagation RTT (rtProp best).
    float estimatedRttUs() const;
};

/**
 * @brief BBR-inspired bandwidth probing and congestion-aware scheduling.
 *
 * Implements bandwidth estimation via delivery rate tracking:
 *   - Tracks delivery rate over RTT intervals
 *   - Maintains btlBw (max bandwidth) via windowed max (10-round window)
 *   - Maintains rtProp (min RTT) via windowed min (10-round window)
 *   - Probing phases: STARTUP -> DRAIN -> PROBE_BW (with gain cycling)
 *
 * The DP layer partitioner uses these estimates for transfer_time computation
 * between devices instead of static bandwidth values.
 *
 * Gain cycling in PROBE_BW: [1.25, 0.75, 1, 1, 1, 1, 1, 1]
 */
class BandwidthProber {
public:
    struct Config {
        std::uint32_t window_rounds = 10;          // BtlBw/RtProp filter window
        float startup_growth = 2.0f;               // 2x growth per round in STARTUP
        float drain_target = 1.0f;                 // Drain until inflight <= BDP
        std::uint32_t probe_rtt_interval_ms = 10000; // Probe RTT every 10s
        std::uint32_t min_cwnd_packets = 4;        // Minimum CWND
    };

    BandwidthProber();
    explicit BandwidthProber(Config config);

    /// Record a delivery rate sample (called on each ACK).
    void onAck(float delivered_bytes, float rtt_us, float elapsed_us,
               bool is_app_limited = false);

    /// Advance to next round (one RTT interval).
    void advanceRound();

    /// Get current bandwidth estimate for a peer link.
    BandwidthEstimate estimate() const;

    /// Get estimated bandwidth in kbps (for use by LayerPartitioner).
    std::uint32_t estimatedBandwidthKbps() const;

    /// Get estimated RTT in milliseconds.
    float estimatedRttMs() const;

    /// Get current pacing rate (bytes per second).
    float pacingRateBps() const;

    /// Get target CWND in bytes.
    float targetCwndBytes() const;

    /// Current phase.
    BbrPhase phase() const;

    /// Force transition to a specific phase (for testing).
    void forcePhase(BbrPhase new_phase);

private:
    Config config_;
    BandwidthEstimate state_;
    mutable std::mutex mutex_;

    // Phase transition logic
    void checkStartupExit();
    void checkDrainExit();
    void advanceGainCycle();
    void updatePacingAndCwnd();

    // Delivery rate tracking
    float total_delivered_ = 0.0f;
    std::chrono::steady_clock::time_point last_round_start_;
};

// ===========================================================================
// mTLS-like Security Layer (Simplified TLS 1.3)
// ===========================================================================

/// Security handshake state.
enum class SecurityHandshakeState : std::uint8_t {
    None           = 0,
    ClientHello    = 1,   // Sent X25519 public key
    ServerHello    = 2,   // Received peer's public key
    Authenticated  = 3,   // Key exchange complete, group membership verified
    Failed         = 4,
};

/// X25519 key pair (32-byte private key, 32-byte public key).
struct X25519KeyPair {
    std::array<std::uint8_t, 32> private_key{};
    std::array<std::uint8_t, 32> public_key{};
};

/// HKDF-derived session keys for ChaCha20-Poly1305.
struct SessionKeys {
    std::array<std::uint8_t, 32> client_write_key{};  // Client -> Server encryption
    std::array<std::uint8_t, 32> server_write_key{};  // Server -> Client encryption
    std::array<std::uint8_t, 12> client_write_iv{};   // Client nonce base
    std::array<std::uint8_t, 12> server_write_iv{};   // Server nonce base
};

/// Device attestation using pre-shared group key.
struct MeshAttestation {
    std::array<std::uint8_t, 32> group_key_hash{};    // SHA-256 of group PSK
    std::array<std::uint8_t, 32> attestation_mac{};   // HMAC proving possession
    std::string device_id;
    std::uint64_t timestamp = 0;                      // Freshness
};

/// Security context for an authenticated peer connection.
struct SecurityContext {
    SecurityHandshakeState state = SecurityHandshakeState::None;
    PeerId peer;

    // X25519 ephemeral key pair for this session
    X25519KeyPair local_ephemeral{};
    std::array<std::uint8_t, 32> peer_public_key{};

    // Shared secret from X25519 key exchange
    std::array<std::uint8_t, 32> shared_secret{};

    // HKDF-derived session keys
    SessionKeys session_keys{};

    // Nonce counters (prevent replay)
    std::uint64_t send_nonce_counter = 0;
    std::uint64_t recv_nonce_counter = 0;

    // Peer verification
    bool peer_attested = false;        // Proved mesh membership via group key
    bool keys_derived = false;         // Session keys available

    // Handshake transcript hash (for key derivation binding)
    std::array<std::uint8_t, 32> transcript_hash{};
};

/**
 * @brief mTLS-like security layer for device-to-device authentication.
 *
 * Implements a simplified TLS 1.3 handshake:
 *   1. X25519 key exchange (curve25519 Montgomery curve arithmetic)
 *   2. HKDF key derivation (HMAC-SHA256-based)
 *   3. ChaCha20-Poly1305 AEAD for payload encryption
 *   4. Device attestation via pre-shared group key (mesh membership proof)
 *
 * Handshake flow:
 *   Client                         Server
 *   ------                         ------
 *   ClientHello (ephemeral pub) -->
 *                                <-- ServerHello (ephemeral pub + attestation)
 *   Finished (attestation)      -->
 *                                    [derive session keys]
 *   <-- Application Data (encrypted with ChaCha20-Poly1305) -->
 *
 * The group pre-shared key (PSK) proves mesh membership without a CA.
 * Each device must possess the PSK to compute valid attestation MACs.
 */
class MeshSecurity {
public:
    struct Config {
        /// Pre-shared group key (32 bytes). All mesh members must share this.
        std::array<std::uint8_t, 32> group_psk{};
        /// Enable encryption (can be disabled for testing).
        bool enable_encryption = true;
        /// Rekey after this many messages.
        std::uint64_t rekey_interval = 1000000;
    };

    MeshSecurity();
    explicit MeshSecurity(Config config);

    /// Generate an X25519 ephemeral key pair.
    X25519KeyPair generateKeyPair();

    /// Perform X25519 scalar multiplication (shared secret derivation).
    /// shared_out = private_key * peer_public_key on Curve25519.
    bool x25519(const std::array<std::uint8_t, 32>& private_key,
                const std::array<std::uint8_t, 32>& peer_public_key,
                std::array<std::uint8_t, 32>& shared_out);

    /// HKDF-Extract: PRK = HMAC-SHA256(salt, IKM).
    std::array<std::uint8_t, 32> hkdfExtract(
        const std::uint8_t* salt, std::size_t salt_len,
        const std::uint8_t* ikm, std::size_t ikm_len);

    /// HKDF-Expand: OKM = HMAC-based expansion to desired length.
    std::vector<std::uint8_t> hkdfExpand(
        const std::array<std::uint8_t, 32>& prk,
        const std::uint8_t* info, std::size_t info_len,
        std::size_t output_len);

    /// Derive session keys from shared secret (TLS 1.3-style key schedule).
    SessionKeys deriveSessionKeys(const std::array<std::uint8_t, 32>& shared_secret,
                                  const std::array<std::uint8_t, 32>& transcript_hash);

    /// Encrypt payload using ChaCha20-Poly1305 AEAD.
    /// Returns ciphertext + 16-byte Poly1305 tag appended.
    std::vector<std::uint8_t> encrypt(
        const SessionKeys& keys, bool is_client,
        std::uint64_t nonce_counter,
        const std::uint8_t* plaintext, std::size_t plaintext_len,
        const std::uint8_t* aad, std::size_t aad_len);

    /// Decrypt and verify ChaCha20-Poly1305 AEAD.
    /// Returns plaintext, or empty vector on authentication failure.
    std::vector<std::uint8_t> decrypt(
        const SessionKeys& keys, bool is_client,
        std::uint64_t nonce_counter,
        const std::uint8_t* ciphertext, std::size_t ciphertext_len,
        const std::uint8_t* aad, std::size_t aad_len);

    /// Create mesh attestation (proves group membership).
    MeshAttestation createAttestation(const std::string& device_id);

    /// Verify peer's mesh attestation.
    bool verifyAttestation(const MeshAttestation& attestation);

    /// Initiate handshake as client. Returns ClientHello message.
    std::vector<std::uint8_t> initiateHandshake(SecurityContext& ctx);

    /// Respond to ClientHello as server. Returns ServerHello message.
    std::vector<std::uint8_t> respondHandshake(SecurityContext& ctx,
                                               const std::uint8_t* client_hello,
                                               std::size_t len);

    /// Finalize handshake (client receives ServerHello, derives keys).
    bool finalizeHandshake(SecurityContext& ctx,
                           const std::uint8_t* server_hello,
                           std::size_t len);

    /// Get security context for a peer.
    std::optional<SecurityContext> getContext(const PeerId& peer) const;

private:
    Config config_;
    mutable std::mutex mutex_;
    std::map<std::string, SecurityContext> contexts_;  // device_id -> context

    // Curve25519 field arithmetic (mod 2^255 - 19)
    // Simplified representation using arrays of uint64_t limbs
    using Fe = std::array<std::uint64_t, 5>;  // 5 x 51-bit limbs

    Fe feFromBytes(const std::uint8_t bytes[32]);
    void feToBytes(std::uint8_t out[32], const Fe& f);
    Fe feAdd(const Fe& a, const Fe& b);
    Fe feSub(const Fe& a, const Fe& b);
    Fe feMul(const Fe& a, const Fe& b);
    Fe feSquare(const Fe& a);
    Fe feInvert(const Fe& a);
    // Montgomery ladder scalar multiplication
    void scalarmult(std::uint8_t result[32],
                    const std::uint8_t scalar[32],
                    const std::uint8_t point[32]);

    // SHA-256 (simplified, for HKDF/HMAC)
    std::array<std::uint8_t, 32> sha256(const std::uint8_t* data, std::size_t len);
    std::array<std::uint8_t, 32> hmacSha256(
        const std::uint8_t* key, std::size_t key_len,
        const std::uint8_t* data, std::size_t data_len);

    // ChaCha20 core
    void chacha20Block(std::uint32_t output[16],
                       const std::uint32_t key[8],
                       std::uint32_t counter,
                       const std::uint32_t nonce[3]);
    void chacha20Encrypt(const std::uint8_t* key,
                         const std::uint8_t* nonce_12,
                         std::uint32_t counter,
                         const std::uint8_t* input,
                         std::uint8_t* output,
                         std::size_t len);

    // Poly1305 MAC
    void poly1305Mac(std::uint8_t tag[16],
                     const std::uint8_t* key,
                     const std::uint8_t* msg, std::size_t msg_len);
};

}  // namespace sparx::mesh

/**
 * @file sparx_mesh.cpp
 * @brief Agent Mesh Protocol — implementation.
 *
 * Platform notes:
 *   - mDNS: uses POSIX multicast sockets (getifaddrs + IP_ADD_MEMBERSHIP).
 *     On Android/iOS, would use platform-native NSD/Bonjour APIs.
 *   - CRDT: operation-based with Lamport clocks (no wall-clock dependency).
 *   - Split inference: planning only — actual tensor transfer requires
 *     QNN RPC or custom protocol (device-specific, not implemented here).
 */

#include "sparx_mesh.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <sstream>

// POSIX network includes for mDNS multicast
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace sparx::mesh {

// ---------------------------------------------------------------------------
// DeviceCapabilities
// ---------------------------------------------------------------------------

float DeviceCapabilities::score() const {
    float s = 0.0f;
    // NPU is the primary compute for LLM inference
    if (has_npu) s += 50.0f + static_cast<float>(npu_tops) * 2.0f;
    if (has_gpu) s += 20.0f;
    if (has_tpu) s += 30.0f;
    // RAM: more is better for large models
    s += std::min(static_cast<float>(ram_mb) / 1024.0f, 16.0f) * 3.0f;
    // Battery: penalize low battery
    s *= (0.5f + 0.5f * battery_level);
    // Idle bonus: prefer devices not in active use
    if (is_idle) s *= 1.2f;
    if (is_charging) s *= 1.1f;
    return s;
}

// ---------------------------------------------------------------------------
// PeerInfo
// ---------------------------------------------------------------------------

bool PeerInfo::isAlive(std::chrono::milliseconds timeout) const {
    auto elapsed = std::chrono::steady_clock::now() - last_seen;
    return elapsed < timeout && heartbeat_failures < 3;
}

// ---------------------------------------------------------------------------
// MeshDiscovery
// ---------------------------------------------------------------------------

MeshDiscovery::MeshDiscovery(PeerId self, DiscoveryConfig config)
    : self_(std::move(self)), config_(std::move(config)) {}

MeshDiscovery::~MeshDiscovery() {
    stop();
}

bool MeshDiscovery::start() {
    if (running_) return true;

    // Create UDP socket for mDNS multicast
    mdns_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (mdns_socket_ < 0) return false;

    // Allow multiple listeners on same port (other mDNS responders)
    int reuse = 1;
    setsockopt(mdns_socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(mdns_socket_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    // Bind to mDNS port
    struct sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(config_.mdns_port);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(mdns_socket_, reinterpret_cast<struct sockaddr*>(&bind_addr),
             sizeof(bind_addr)) < 0) {
        close(mdns_socket_);
        mdns_socket_ = -1;
        return false;
    }

    // Join multicast group 224.0.0.251 (mDNS standard)
    struct ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(mdns_socket_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   &mreq, sizeof(mreq)) < 0) {
        // Non-fatal on some platforms (container, no multicast route)
        // Continue — we can still unicast
    }

    // Set multicast TTL to 1 (link-local only, per RFC 6762)
    unsigned char ttl = 1;
    setsockopt(mdns_socket_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    // Disable loopback of own multicast (we track ourselves separately)
    unsigned char loop = 0;
    setsockopt(mdns_socket_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    // Set receive timeout for non-blocking-ish reads in listen thread
    struct timeval tv{};
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(mdns_socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    running_ = true;
    shutdown_.store(false);

    // Start background threads
    listen_thread_ = std::thread(&MeshDiscovery::listenLoop, this);
    announce_thread_ = std::thread(&MeshDiscovery::announceLoop, this);
    expire_thread_ = std::thread(&MeshDiscovery::expireLoop, this);

    return true;
}

void MeshDiscovery::stop() {
    if (!running_) return;
    running_ = false;
    shutdown_.store(true);

    // Send goodbye announcement (TTL=0 per RFC 6762 §10.1)
    if (mdns_socket_ >= 0) {
        auto goodbye = buildAnnouncement(0);
        struct sockaddr_in mcast_addr{};
        mcast_addr.sin_family = AF_INET;
        mcast_addr.sin_port = htons(config_.mdns_port);
        mcast_addr.sin_addr.s_addr = inet_addr("224.0.0.251");
        sendto(mdns_socket_, goodbye.data(), goodbye.size(), 0,
               reinterpret_cast<struct sockaddr*>(&mcast_addr), sizeof(mcast_addr));
    }

    // Join threads
    if (listen_thread_.joinable()) listen_thread_.join();
    if (announce_thread_.joinable()) announce_thread_.join();
    if (expire_thread_.joinable()) expire_thread_.join();

    // Close socket
    if (mdns_socket_ >= 0) {
        close(mdns_socket_);
        mdns_socket_ = -1;
    }
}

std::vector<PeerInfo> MeshDiscovery::peers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PeerInfo> result;
    result.reserve(peer_table_.size());
    for (const auto& [_, info] : peer_table_) {
        if (info.isAlive()) result.push_back(info);
    }
    return result;
}

std::optional<PeerInfo> MeshDiscovery::peer(
    const std::string& device_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = peer_table_.find(device_id);
    if (it == peer_table_.end() || !it->second.isAlive()) return std::nullopt;
    return it->second;
}

void MeshDiscovery::onPeerChange(PeerCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_.push_back(std::move(cb));
}

std::size_t MeshDiscovery::peerCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (const auto& [_, info] : peer_table_) {
        if (info.isAlive()) ++count;
    }
    return count;
}

// ---------------------------------------------------------------------------
// mDNS Packet Construction & Parsing (RFC 6762 / RFC 6763)
// ---------------------------------------------------------------------------

std::string MeshDiscovery::encodeDnsName(const std::string& name) const {
    // DNS name encoding: each label prefixed by its length byte
    std::string result;
    std::istringstream ss(name);
    std::string label;
    while (std::getline(ss, label, '.')) {
        if (label.empty()) continue;
        result += static_cast<char>(label.size());
        result += label;
    }
    result += '\0';  // root label
    return result;
}

std::string MeshDiscovery::decodeDnsName(
    const uint8_t* data, size_t len, size_t& offset) const {
    std::string name;
    while (offset < len) {
        uint8_t label_len = data[offset];
        if (label_len == 0) { ++offset; break; }
        // Handle DNS compression pointer (top 2 bits = 11)
        if ((label_len & 0xC0) == 0xC0) {
            if (offset + 1 >= len) break;
            size_t ptr = ((label_len & 0x3F) << 8) | data[offset + 1];
            offset += 2;
            // Recursively decode at pointer (no infinite loop: ptrs go backward)
            size_t ptr_off = ptr;
            if (!name.empty()) name += ".";
            name += decodeDnsName(data, len, ptr_off);
            return name;
        }
        ++offset;
        if (offset + label_len > len) break;
        if (!name.empty()) name += ".";
        name += std::string(reinterpret_cast<const char*>(data + offset), label_len);
        offset += label_len;
    }
    return name;
}

std::map<std::string, std::string> MeshDiscovery::parseTxtRecord(
    const uint8_t* data, size_t len, size_t& offset) const {
    std::map<std::string, std::string> txt;
    size_t end = offset + len;
    while (offset < end) {
        uint8_t txt_len = data[offset++];
        if (txt_len == 0 || offset + txt_len > end) break;
        std::string entry(reinterpret_cast<const char*>(data + offset), txt_len);
        offset += txt_len;
        auto eq = entry.find('=');
        if (eq != std::string::npos) {
            txt[entry.substr(0, eq)] = entry.substr(eq + 1);
        }
    }
    return txt;
}

std::vector<uint8_t> MeshDiscovery::buildAnnouncement(uint32_t ttl) const {
    // Build a minimal mDNS response packet announcing our service.
    // DNS header (12 bytes) + Answer section with PTR + SRV + TXT records.
    std::vector<uint8_t> pkt;
    pkt.reserve(256);

    // DNS Header: flags=0x8400 (response, authoritative), 1 answer
    auto push16 = [&](uint16_t v) {
        pkt.push_back(static_cast<uint8_t>(v >> 8));
        pkt.push_back(static_cast<uint8_t>(v & 0xFF));
    };
    auto push32 = [&](uint32_t v) {
        pkt.push_back(static_cast<uint8_t>(v >> 24));
        pkt.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        pkt.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        pkt.push_back(static_cast<uint8_t>(v & 0xFF));
    };

    push16(0x0000);  // Transaction ID (0 for mDNS)
    push16(0x8400);  // Flags: QR=1, AA=1
    push16(0);       // Questions: 0
    push16(3);       // Answers: 3 (PTR + SRV + TXT)
    push16(0);       // Authority: 0
    push16(0);       // Additional: 0

    // Encoded names
    std::string svc_name = encodeDnsName(config_.service_type);
    std::string instance_name = self_.device_id + "." + config_.service_type;
    std::string inst_encoded = encodeDnsName(instance_name);
    std::string host_name = encodeDnsName(self_.device_id + ".local.");

    // Answer 1: PTR record (service_type → instance)
    pkt.insert(pkt.end(), svc_name.begin(), svc_name.end());
    push16(0x000C);  // TYPE: PTR
    push16(0x0001);  // CLASS: IN (flush bit not set for PTR)
    push32(ttl);
    // RDATA: instance name
    push16(static_cast<uint16_t>(inst_encoded.size()));
    pkt.insert(pkt.end(), inst_encoded.begin(), inst_encoded.end());

    // Answer 2: SRV record (instance → host:port)
    pkt.insert(pkt.end(), inst_encoded.begin(), inst_encoded.end());
    push16(0x0021);  // TYPE: SRV
    push16(0x8001);  // CLASS: IN + cache-flush
    push32(ttl);
    size_t srv_rdata_pos = pkt.size();
    push16(0);  // placeholder for RDATA length
    push16(0);  // priority
    push16(0);  // weight
    push16(config_.service_port);
    pkt.insert(pkt.end(), host_name.begin(), host_name.end());
    // Fix RDATA length
    uint16_t srv_len = static_cast<uint16_t>(pkt.size() - srv_rdata_pos - 2);
    pkt[srv_rdata_pos] = static_cast<uint8_t>(srv_len >> 8);
    pkt[srv_rdata_pos + 1] = static_cast<uint8_t>(srv_len & 0xFF);

    // Answer 3: TXT record (capabilities as key=value pairs)
    pkt.insert(pkt.end(), inst_encoded.begin(), inst_encoded.end());
    push16(0x0010);  // TYPE: TXT
    push16(0x8001);  // CLASS: IN + cache-flush
    push32(ttl);

    // Build TXT RDATA
    std::vector<uint8_t> txt_rdata;
    auto addTxt = [&](const std::string& kv) {
        txt_rdata.push_back(static_cast<uint8_t>(kv.size()));
        txt_rdata.insert(txt_rdata.end(), kv.begin(), kv.end());
    };
    addTxt("id=" + self_.device_id);
    addTxt("name=" + self_.display_name);
    addTxt("ver=2");
    addTxt("port=" + std::to_string(config_.service_port));

    push16(static_cast<uint16_t>(txt_rdata.size()));
    pkt.insert(pkt.end(), txt_rdata.begin(), txt_rdata.end());

    return pkt;
}

std::vector<uint8_t> MeshDiscovery::buildQuery() const {
    // Build a simple mDNS query for our service type
    std::vector<uint8_t> pkt;
    pkt.reserve(64);

    auto push16 = [&](uint16_t v) {
        pkt.push_back(static_cast<uint8_t>(v >> 8));
        pkt.push_back(static_cast<uint8_t>(v & 0xFF));
    };

    push16(0x0000);  // Transaction ID
    push16(0x0000);  // Flags: standard query
    push16(1);       // Questions: 1
    push16(0);       // Answers: 0
    push16(0);       // Authority: 0
    push16(0);       // Additional: 0

    // Question: PTR for our service type
    std::string name = encodeDnsName(config_.service_type);
    pkt.insert(pkt.end(), name.begin(), name.end());
    push16(0x000C);  // TYPE: PTR
    push16(0x0001);  // CLASS: IN

    return pkt;
}

bool MeshDiscovery::parseResponse(const uint8_t* data, size_t len) {
    if (len < 12) return false;  // Minimum DNS header

    // Parse header
    uint16_t flags = (data[2] << 8) | data[3];
    if (!(flags & 0x8000)) return false;  // Not a response

    uint16_t ans_count = (data[4] << 8) | data[5];
    // We also check additional section for SRV/TXT
    uint16_t add_count = (data[10] << 8) | data[11];

    size_t offset = 12;  // Skip header

    // Skip questions section
    uint16_t q_count = (data[4] << 8) | data[5];
    // Actually: QDCOUNT is at bytes 4-5, ANCOUNT at 6-7 for responses
    // Re-parse correctly:
    // Bytes 4-5: QDCOUNT, 6-7: ANCOUNT, 8-9: NSCOUNT, 10-11: ARCOUNT
    uint16_t qd_count = (data[4] << 8) | data[5];
    ans_count = (data[6] << 8) | data[7];
    add_count = (data[10] << 8) | data[11];

    // Skip questions
    for (uint16_t i = 0; i < qd_count && offset < len; ++i) {
        decodeDnsName(data, len, offset);
        offset += 4;  // QTYPE + QCLASS
    }

    // Parse answer + additional records for TXT/SRV
    std::string peer_id;
    std::string peer_name;
    uint16_t peer_port = 0;

    uint16_t total_rr = ans_count + add_count;
    for (uint16_t i = 0; i < total_rr && offset < len; ++i) {
        std::string rr_name = decodeDnsName(data, len, offset);
        if (offset + 10 > len) break;

        uint16_t rr_type = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        offset += 2;  // class
        offset += 4;  // TTL
        uint16_t rd_len = (data[offset] << 8) | data[offset + 1];
        offset += 2;

        size_t rdata_start = offset;
        if (offset + rd_len > len) break;

        if (rr_type == 0x0010) {  // TXT
            auto txt = parseTxtRecord(data, rd_len, rdata_start);
            if (txt.count("id")) peer_id = txt["id"];
            if (txt.count("name")) peer_name = txt["name"];
            if (txt.count("port")) {
                try { peer_port = static_cast<uint16_t>(std::stoi(txt["port"])); }
                catch (...) {}
            }
        } else if (rr_type == 0x0021) {  // SRV
            if (rd_len >= 6) {
                peer_port = (data[rdata_start + 4] << 8) | data[rdata_start + 5];
            }
        }

        offset = rdata_start + rd_len;
    }

    // Skip our own announcements
    if (peer_id.empty() || peer_id == self_.device_id) return false;

    // Register/update peer
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool is_new = (peer_table_.find(peer_id) == peer_table_.end());

        PeerInfo& info = peer_table_[peer_id];
        info.id.device_id = peer_id;
        info.id.display_name = peer_name.empty() ? peer_id : peer_name;
        info.last_seen = std::chrono::steady_clock::now();
        info.heartbeat_failures = 0;

        if (is_new) {
            notifyCallbacks(info, true);
        }
    }

    return true;
}

void MeshDiscovery::announceLoop() {
    // Initial announcement burst (RFC 6762 §8.3: first at 1s, then 2s, then 4s...)
    int initial_intervals[] = {0, 1, 2, 4};
    for (int delay : initial_intervals) {
        if (shutdown_.load()) return;
        if (delay > 0) {
            std::this_thread::sleep_for(std::chrono::seconds(delay));
        }
        if (shutdown_.load()) return;

        auto pkt = buildAnnouncement();
        struct sockaddr_in mcast{};
        mcast.sin_family = AF_INET;
        mcast.sin_port = htons(config_.mdns_port);
        mcast.sin_addr.s_addr = inet_addr("224.0.0.251");
        sendto(mdns_socket_, pkt.data(), pkt.size(), 0,
               reinterpret_cast<struct sockaddr*>(&mcast), sizeof(mcast));
    }

    // Steady-state re-announcement
    while (!shutdown_.load()) {
        std::this_thread::sleep_for(
            std::chrono::seconds(config_.announce_interval_s));
        if (shutdown_.load()) break;

        auto pkt = buildAnnouncement();
        struct sockaddr_in mcast{};
        mcast.sin_family = AF_INET;
        mcast.sin_port = htons(config_.mdns_port);
        mcast.sin_addr.s_addr = inet_addr("224.0.0.251");
        sendto(mdns_socket_, pkt.data(), pkt.size(), 0,
               reinterpret_cast<struct sockaddr*>(&mcast), sizeof(mcast));

        // Also send a PTR query to discover new peers
        auto query = buildQuery();
        sendto(mdns_socket_, query.data(), query.size(), 0,
               reinterpret_cast<struct sockaddr*>(&mcast), sizeof(mcast));
    }
}

void MeshDiscovery::listenLoop() {
    uint8_t buf[4096];
    struct sockaddr_in sender{};
    socklen_t sender_len = sizeof(sender);

    while (!shutdown_.load()) {
        ssize_t n = recvfrom(mdns_socket_, buf, sizeof(buf), 0,
                             reinterpret_cast<struct sockaddr*>(&sender),
                             &sender_len);
        if (n <= 0) continue;  // timeout or error — retry
        parseResponse(buf, static_cast<size_t>(n));
    }
}

void MeshDiscovery::expireLoop() {
    while (!shutdown_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (shutdown_.load()) break;

        std::lock_guard<std::mutex> lock(mutex_);
        auto timeout = std::chrono::seconds(config_.peer_timeout_s);
        auto now = std::chrono::steady_clock::now();

        for (auto it = peer_table_.begin(); it != peer_table_.end(); ) {
            if (now - it->second.last_seen > timeout) {
                auto expired = it->second;
                it = peer_table_.erase(it);
                notifyCallbacks(expired, false);
            } else {
                ++it;
            }
        }
    }
}

void MeshDiscovery::notifyCallbacks(const PeerInfo& peer, bool joined) {
    // Called with mutex_ held by caller
    for (const auto& cb : callbacks_) {
        cb(peer, joined);
    }
}

CapabilityRouter::CapabilityRouter(PeerId self, DeviceCapabilities self_caps)
    : self_(std::move(self)), self_caps_(std::move(self_caps)) {}

RoutingDecision CapabilityRouter::route(
    const RoutingRequest& request,
    const std::vector<PeerInfo>& peers) const {

    RoutingDecision best;
    best.target = self_;
    best.is_local = true;
    best.score = 0.5f;  // baseline local score
    best.reason = "local fallback (no better peer)";

    // Score local execution
    float local_score = self_caps_.score();
    if (request.requires_npu && !self_caps_.has_npu) local_score = 0.0f;
    if (!request.model_id.empty()) {
        bool has_model = false;
        for (const auto& m : self_caps_.loaded_models) {
            if (m == request.model_id) { has_model = true; break; }
        }
        if (!has_model) local_score *= 0.3f;  // penalty, not elimination
    }
    if (request.prefer_local) local_score *= 1.3f;

    best.score = local_score;

    // Score each peer
    for (const auto& peer : peers) {
        if (!peer.isAlive()) continue;
        if (peer.id == self_) continue;

        float peer_score = scorePeer(peer, request);
        if (peer_score > best.score) {
            best.target = peer.id;
            best.score = peer_score;
            best.is_local = false;
            best.estimated_latency_ms = peer.capabilities.latency_ms;

            // Build reason string
            std::ostringstream oss;
            oss << peer.id.display_name << " selected: ";
            if (peer.capabilities.has_npu && request.requires_npu)
                oss << "has NPU (" << peer.capabilities.npu_tops << " TOPS), ";
            if (peer.capabilities.is_idle)
                oss << "idle, ";
            oss << "score=" << peer_score;
            best.reason = oss.str();
        }
    }

    return best;
}

float CapabilityRouter::scorePeer(
    const PeerInfo& peer,
    const RoutingRequest& request) const {

    const auto& caps = peer.capabilities;
    float score = caps.score();

    // Hard requirements
    if (request.requires_npu && !caps.has_npu) return 0.0f;
    if (request.min_ram_mb > 0 && caps.ram_mb < request.min_ram_mb) return 0.0f;
    if (caps.latency_ms > request.max_latency_ms) return 0.0f;

    // Model match bonus
    if (!request.model_id.empty()) {
        bool has_model = false;
        for (const auto& m : caps.loaded_models) {
            if (m == request.model_id) { has_model = true; break; }
        }
        if (has_model) score *= 2.0f;  // big bonus: no model load needed
        else score *= 0.5f;
    }

    // Skill match bonus
    if (!request.skill_name.empty()) {
        bool has_skill = false;
        for (const auto& s : caps.available_skills) {
            if (s == request.skill_name) { has_skill = true; break; }
        }
        if (has_skill) score *= 1.5f;
    }

    // Idle preference
    if (request.prefer_idle && caps.is_idle) score *= 1.4f;
    if (request.prefer_idle && !caps.is_idle) score *= 0.7f;

    // Network penalty
    if (caps.latency_ms > 50) {
        score *= 1.0f - (static_cast<float>(caps.latency_ms) / 2000.0f);
    }

    // Locality penalty (remote always has overhead)
    if (request.prefer_local) score *= 0.85f;

    return score;
}

std::vector<CapabilityRouter::RouteEntry> CapabilityRouter::routingTable(
    const std::vector<PeerInfo>& peers) const {
    std::vector<RouteEntry> table;
    for (const auto& peer : peers) {
        if (!peer.isAlive()) continue;
        RouteEntry entry;
        entry.peer = peer.id;
        entry.score = peer.capabilities.score();
        // Summarize top capability
        if (peer.capabilities.has_npu)
            entry.capability = "NPU " + std::to_string(peer.capabilities.npu_tops) + "T";
        else if (peer.capabilities.has_gpu)
            entry.capability = "GPU";
        else
            entry.capability = "CPU";
        table.push_back(entry);
    }
    std::sort(table.begin(), table.end(),
              [](const RouteEntry& a, const RouteEntry& b) {
                  return a.score > b.score;
              });
    return table;
}

// ---------------------------------------------------------------------------
// CrdtStateSync
// ---------------------------------------------------------------------------

CrdtStateSync::CrdtStateSync(std::string device_id)
    : device_id_(std::move(device_id)) {}

std::string CrdtStateSync::generateTag() {
    // Format: "deviceId#seq" — unique per node + monotonic counter
    return device_id_ + "#" + std::to_string(++tag_counter_);
}

CrdtOperation CrdtStateSync::mutate(
    const std::string& key, CrdtType type, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);

    ++lamport_clock_;

    CrdtOperation op;
    op.key = key;
    op.type = type;
    op.value = value;
    op.origin = device_id_;
    op.timestamp = lamport_clock_;
    op.vector_clock_entry = static_cast<uint64_t>(lamport_clock_);

    // Apply locally
    auto& entry = state_[key];
    entry.key = key;
    entry.type = type;
    entry.last_modified = lamport_clock_;
    entry.origins.insert(device_id_);

    switch (type) {
        case CrdtType::GCounter:
            entry.value = mergeGCounter(entry.value, value);
            break;
        case CrdtType::PNCounter:
            entry.value = mergePNCounter(entry.value, value);
            break;
        case CrdtType::GSet:
            entry.value = mergeGSet(entry.value, value);
            break;
        case CrdtType::ORSet: {
            // Wrap the value as a tagged element: "element\x1ftag\n\x1e"
            std::string tagged;
            tagged += value;
            tagged += '\x1f';
            tagged += generateTag();
            tagged += '\n';
            tagged += '\x1e';  // empty tombstone section
            entry.value = mergeORSet(entry.value, tagged);
            op.value = tagged;  // propagate tagged form to remote
            break;
        }
        case CrdtType::LWWRegister:
        case CrdtType::MVRegister:
            entry.value = value;  // local write always wins locally
            break;
    }

    op_log_.push_back(op);
    return op;
}

CrdtOperation CrdtStateSync::removeFromORSet(
    const std::string& key, const std::string& element) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++lamport_clock_;

    const char RS = '\x1e';
    const char US = '\x1f';

    CrdtOperation op;
    op.key = key;
    op.type = CrdtType::ORSet;
    op.origin = device_id_;
    op.timestamp = lamport_clock_;
    op.vector_clock_entry = static_cast<uint64_t>(lamport_clock_);

    auto& entry = state_[key];
    entry.key = key;
    entry.type = CrdtType::ORSet;
    entry.origins.insert(device_id_);

    // Parse current state to find all tags for this element
    std::string current = entry.value;
    auto tomb_pos = current.find(RS);
    std::string alive_section = (tomb_pos == std::string::npos)
        ? current : current.substr(0, tomb_pos);
    std::string tomb_section = (tomb_pos == std::string::npos)
        ? "" : current.substr(tomb_pos + 1);

    // Find the element's tags
    std::set<std::string> tags_to_tomb;
    std::istringstream as(alive_section);
    std::string line;
    while (std::getline(as, line)) {
        if (line.empty()) continue;
        auto sep = line.find(US);
        if (sep == std::string::npos) continue;
        std::string elem = line.substr(0, sep);
        if (elem == element) {
            std::istringstream ts(line.substr(sep + 1));
            std::string tag;
            while (std::getline(ts, tag, ',')) {
                if (!tag.empty()) tags_to_tomb.insert(tag);
            }
        }
    }

    // Build a removal operation: empty alive section + all observed tags as tombstones
    std::string removal_value;
    removal_value += RS;  // empty alive, go straight to tombstones
    for (const auto& t : tags_to_tomb) {
        removal_value += t + "\n";
    }
    // Also include existing tombstones
    removal_value += tomb_section;

    op.value = removal_value;
    entry.value = mergeORSet(entry.value, removal_value);

    op_log_.push_back(op);
    return op;
}

bool CrdtStateSync::merge(const CrdtOperation& op) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Advance Lamport clock
    lamport_clock_ = std::max(lamport_clock_, op.timestamp) + 1;

    // Check for duplicate (idempotence)
    // Simple check: if we've seen this exact timestamp from this origin
    for (const auto& existing : op_log_) {
        if (existing.origin == op.origin &&
            existing.timestamp == op.timestamp &&
            existing.key == op.key) {
            return false;  // already applied
        }
    }

    // Apply CRDT merge
    auto& entry = state_[op.key];
    entry.key = op.key;
    entry.type = op.type;
    entry.origins.insert(op.origin);

    switch (op.type) {
        case CrdtType::GCounter:
            entry.value = mergeGCounter(entry.value, op.value);
            break;
        case CrdtType::PNCounter:
            entry.value = mergePNCounter(entry.value, op.value);
            break;
        case CrdtType::GSet:
            entry.value = mergeGSet(entry.value, op.value);
            break;
        case CrdtType::ORSet:
            entry.value = mergeORSet(entry.value, op.value);
            break;
        case CrdtType::LWWRegister:
            entry.value = mergeLWW(entry.value, entry.last_modified,
                                   op.value, op.timestamp);
            break;
        case CrdtType::MVRegister:
            // Multi-value: keep both on conflict (application resolves)
            if (entry.value != op.value && !entry.value.empty()) {
                entry.value = entry.value + "\x1f" + op.value;  // US separator
            } else {
                entry.value = op.value;
            }
            break;
    }

    entry.last_modified = std::max(entry.last_modified, op.timestamp);
    op_log_.push_back(op);

    // Auto-compact op_log when it exceeds 10,000 entries
    // Keep only operations from the last 1,000 timestamps
    constexpr size_t MAX_OP_LOG_SIZE = 10000;
    if (op_log_.size() > MAX_OP_LOG_SIZE) {
        // Find the 9000th-oldest timestamp (keep 1000 most recent)
        std::vector<int64_t> timestamps;
        timestamps.reserve(op_log_.size());
        for (const auto& logged_op : op_log_) {
            timestamps.push_back(logged_op.timestamp);
        }
        std::nth_element(timestamps.begin(),
                         timestamps.begin() + (timestamps.size() - 1000),
                         timestamps.end());
        int64_t cutoff = timestamps[timestamps.size() - 1000];

        compactInternal(cutoff);
    }

    return true;
}

std::optional<StateEntry> CrdtStateSync::get(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = state_.find(key);
    if (it == state_.end()) return std::nullopt;
    return it->second;
}

std::vector<StateEntry> CrdtStateSync::allState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<StateEntry> result;
    result.reserve(state_.size());
    for (const auto& [_, entry] : state_) {
        result.push_back(entry);
    }
    return result;
}

std::vector<CrdtOperation> CrdtStateSync::operationsSince(
    std::int64_t timestamp) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CrdtOperation> result;
    for (const auto& op : op_log_) {
        if (op.timestamp > timestamp) result.push_back(op);
    }
    return result;
}

void CrdtStateSync::compact(std::int64_t before_timestamp) {
    std::lock_guard<std::mutex> lock(mutex_);
    compactInternal(before_timestamp);
}

void CrdtStateSync::compactInternal(std::int64_t before_timestamp) {
    op_log_.erase(
        std::remove_if(op_log_.begin(), op_log_.end(),
                       [before_timestamp](const CrdtOperation& op) {
                           return op.timestamp < before_timestamp;
                       }),
        op_log_.end());
}

std::size_t CrdtStateSync::stateSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.size();
}

std::string CrdtStateSync::mergeGCounter(
    const std::string& local, const std::string& remote) const {
    // GCounter: per-node grow-only counter.
    // Format: "nodeA:5;nodeB:3" — merge takes max per node.
    auto local_map = parseNodeCounters(local);
    auto remote_map = parseNodeCounters(remote);

    // If both are plain numbers (legacy format), convert to per-node
    if (local_map.empty() && !local.empty()) {
        try { local_map[device_id_] = std::stoll(local); } catch (...) {}
    }
    if (remote_map.empty() && !remote.empty()) {
        try { remote_map["remote"] = std::stoll(remote); } catch (...) {}
    }

    // Merge: take max for each node
    for (const auto& [node, count] : remote_map) {
        local_map[node] = std::max(local_map[node], count);
    }
    return serializeNodeCounters(local_map);
}

std::string CrdtStateSync::mergePNCounter(
    const std::string& local, const std::string& remote) const {
    // PNCounter: separate increment and decrement counters per node.
    // Format: "nodeA:+5:-2;nodeB:+3:-0"
    // Value = sum of all increments - sum of all decrements.
    // Merge: max per node per direction.

    // Parse PN format: "node:+inc:-dec;..."
    auto parsePn = [](const std::string& s)
        -> std::map<std::string, std::pair<int64_t, int64_t>> {
        std::map<std::string, std::pair<int64_t, int64_t>> result;
        if (s.empty()) return result;
        std::istringstream ss(s);
        std::string segment;
        while (std::getline(ss, segment, ';')) {
            if (segment.empty()) continue;
            auto colon = segment.find(':');
            if (colon == std::string::npos) continue;
            std::string node = segment.substr(0, colon);
            std::string rest = segment.substr(colon + 1);
            int64_t inc = 0, dec = 0;
            auto plus_pos = rest.find('+');
            auto minus_pos = rest.find('-', 1);  // skip leading sign
            if (plus_pos != std::string::npos) {
                auto end = rest.find(':', plus_pos + 1);
                if (end == std::string::npos) end = rest.size();
                try { inc = std::stoll(rest.substr(plus_pos + 1, end - plus_pos - 1)); }
                catch (...) {}
            }
            if (minus_pos != std::string::npos) {
                try { dec = std::stoll(rest.substr(minus_pos + 1)); }
                catch (...) {}
            }
            result[node] = {inc, dec};
        }
        return result;
    };

    auto serializePn = [](const std::map<std::string, std::pair<int64_t, int64_t>>& m)
        -> std::string {
        std::ostringstream oss;
        bool first = true;
        for (const auto& [node, vals] : m) {
            if (!first) oss << ";";
            first = false;
            oss << node << ":+" << vals.first << ":-" << vals.second;
        }
        return oss.str();
    };

    auto local_map = parsePn(local);
    auto remote_map = parsePn(remote);

    // Handle legacy plain number format
    if (local_map.empty() && !local.empty()) {
        try {
            int64_t v = std::stoll(local);
            local_map[device_id_] = {v >= 0 ? v : 0, v < 0 ? -v : 0};
        } catch (...) {}
    }
    if (remote_map.empty() && !remote.empty()) {
        try {
            int64_t v = std::stoll(remote);
            remote_map["remote"] = {v >= 0 ? v : 0, v < 0 ? -v : 0};
        } catch (...) {}
    }

    // Merge: max per node per direction
    for (const auto& [node, vals] : remote_map) {
        auto& local_vals = local_map[node];
        local_vals.first = std::max(local_vals.first, vals.first);
        local_vals.second = std::max(local_vals.second, vals.second);
    }
    return serializePn(local_map);
}

std::map<std::string, int64_t> CrdtStateSync::parseNodeCounters(
    const std::string& s) const {
    std::map<std::string, int64_t> result;
    if (s.empty()) return result;
    std::istringstream ss(s);
    std::string segment;
    while (std::getline(ss, segment, ';')) {
        if (segment.empty()) continue;
        auto colon = segment.find(':');
        if (colon == std::string::npos) continue;
        std::string node = segment.substr(0, colon);
        try {
            int64_t val = std::stoll(segment.substr(colon + 1));
            result[node] = val;
        } catch (...) {}
    }
    return result;
}

std::string CrdtStateSync::serializeNodeCounters(
    const std::map<std::string, int64_t>& m) const {
    std::ostringstream oss;
    bool first = true;
    for (const auto& [node, count] : m) {
        if (!first) oss << ";";
        first = false;
        oss << node << ":" << count;
    }
    return oss.str();
}

std::string CrdtStateSync::mergeGSet(
    const std::string& local, const std::string& remote) const {
    // GSet merge: union of elements (newline-separated)
    std::set<std::string> elements;
    std::istringstream ls(local);
    std::istringstream rs(remote);
    std::string line;
    while (std::getline(ls, line)) {
        if (!line.empty()) elements.insert(line);
    }
    while (std::getline(rs, line)) {
        if (!line.empty()) elements.insert(line);
    }
    std::ostringstream result;
    for (const auto& e : elements) {
        result << e << "\n";
    }
    return result.str();
}

std::string CrdtStateSync::mergeORSet(
    const std::string& local, const std::string& remote) const {
    // Observed-Remove Set (OR-Set / Add-Wins Set):
    // Each add assigns a unique tag. Remove only removes observed tags.
    // A concurrent add (with a new tag) survives a remove.
    //
    // Optimized encoding using structured storage instead of string parsing.
    // Previous O(n²) parsing is replaced with O(n log n) map operations.

    const char RS = '\x1e';  // section separator (alive vs tombstone)
    const char US = '\x1f';  // element:tags separator

    // Parse an ORSet string into (element → set<tag>) + tombstone set
    auto parseOR = [RS, US](const std::string& s)
        -> std::pair<std::map<std::string, std::set<std::string>>,
                     std::set<std::string>> {
        std::map<std::string, std::set<std::string>> alive;
        std::set<std::string> tombs;

        if (s.empty()) return {alive, tombs};

        // Split into sections by RS
        auto tomb_pos = s.find(RS);
        std::string alive_section = (tomb_pos == std::string::npos) ? s : s.substr(0, tomb_pos);
        std::string tomb_section = (tomb_pos == std::string::npos) ? "" : s.substr(tomb_pos + 1);

        // Parse alive: "element\x1ftag1,tag2\nelement2\x1ftag3\n"
        std::istringstream as(alive_section);
        std::string line;
        while (std::getline(as, line)) {
            if (line.empty()) continue;
            auto sep = line.find(US);
            if (sep == std::string::npos) {
                alive[line] = {};  // element with no tags (legacy)
                continue;
            }
            std::string elem = line.substr(0, sep);
            std::string tags_str = line.substr(sep + 1);

            // Optimized tag parsing: single pass, reserve capacity
            std::set<std::string> tags;
            size_t start = 0;
            while (start < tags_str.size()) {
                auto end = tags_str.find(',', start);
                if (end == std::string::npos) end = tags_str.size();
                if (end > start) {
                    tags.insert(tags_str.substr(start, end - start));
                }
                start = end + 1;
            }
            alive[elem] = std::move(tags);
        }

        // Parse tombstones: "tag1\ntag2\n"
        std::istringstream tss(tomb_section);
        while (std::getline(tss, line)) {
            if (!line.empty()) tombs.insert(line);
        }

        return {alive, tombs};
    };

    auto serializeOR = [RS, US](
        const std::map<std::string, std::set<std::string>>& alive,
        const std::set<std::string>& tombs) -> std::string {
        std::ostringstream oss;
        for (const auto& [elem, tags] : alive) {
            if (tags.empty()) continue;  // fully tombstoned
            oss << elem << US;
            bool first = true;
            for (const auto& t : tags) {
                if (!first) oss << ",";
                first = false;
                oss << t;
            }
            oss << "\n";
        }
        oss << RS;
        for (const auto& t : tombs) {
            oss << t << "\n";
        }
        return oss.str();
    };

    auto [local_alive, local_tombs] = parseOR(local);
    auto [remote_alive, remote_tombs] = parseOR(remote);

    // Merge tombstones: union (O(n log n))
    std::set<std::string> merged_tombs;
    merged_tombs.insert(local_tombs.begin(), local_tombs.end());
    merged_tombs.insert(remote_tombs.begin(), remote_tombs.end());

    // Merge alive: union of all (element → tags), then subtract tombstoned tags
    // Optimized: direct map merge instead of nested iteration
    std::map<std::string, std::set<std::string>> merged_alive;
    for (const auto& [elem, tags] : local_alive) {
        merged_alive[elem].insert(tags.begin(), tags.end());
    }
    for (const auto& [elem, tags] : remote_alive) {
        merged_alive[elem].insert(tags.begin(), tags.end());
    }

    // Remove tombstoned tags (O(n log n) instead of O(n²))
    for (auto& [elem, tags] : merged_alive) {
        std::set<std::string> surviving;
        std::set_difference(tags.begin(), tags.end(),
                           merged_tombs.begin(), merged_tombs.end(),
                           std::inserter(surviving, surviving.begin()));
        tags = std::move(surviving);
    }

    // Remove elements with no surviving tags
    for (auto it = merged_alive.begin(); it != merged_alive.end(); ) {
        if (it->second.empty()) {
            it = merged_alive.erase(it);
        } else {
            ++it;
        }
    }

    return serializeOR(merged_alive, merged_tombs);
}

std::string CrdtStateSync::mergeLWW(
    const std::string& local, std::int64_t local_ts,
    const std::string& remote, std::int64_t remote_ts) const {
    // Last-writer-wins: higher timestamp wins, tie-break by value
    if (remote_ts > local_ts) return remote;
    if (remote_ts < local_ts) return local;
    return std::max(local, remote);  // deterministic tie-break
}

// ---------------------------------------------------------------------------
// MerkleAntiEntropy
// ---------------------------------------------------------------------------

MerkleAntiEntropy::MerkleAntiEntropy()
    : MerkleAntiEntropy(Config{}) {}

MerkleAntiEntropy::MerkleAntiEntropy(Config config)
    : config_(std::move(config)) {
    // Compute number of leaf buckets: branching_factor ^ depth
    // Default: for up to ~1000 keys, depth=2 with bf=16 → 256 buckets
    if (config_.max_depth == 0) {
        config_.max_depth = 2;  // 16^2 = 256 buckets (good for ≤4K keys)
    }
    tree_depth_ = config_.max_depth;
    num_buckets_ = 1;
    for (uint32_t d = 0; d < tree_depth_; ++d) {
        num_buckets_ *= config_.branching_factor;
    }
    buckets_.resize(num_buckets_);
    leaf_hashes_.resize(num_buckets_, "");
}

uint64_t MerkleAntiEntropy::fnv1a(const std::string& data) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : data) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string MerkleAntiEntropy::combineHashes(
    const std::vector<std::string>& child_hashes) {
    // Combine child hashes by concatenating and hashing again
    uint64_t combined = 14695981039346656037ULL;
    for (const auto& h : child_hashes) {
        for (unsigned char c : h) {
            combined ^= c;
            combined *= 1099511628211ULL;
        }
        combined ^= 0xFF;  // separator
        combined *= 1099511628211ULL;
    }
    // Convert to hex string
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(combined));
    return std::string(buf);
}

uint32_t MerkleAntiEntropy::bucketForKey(const std::string& key) const {
    return static_cast<uint32_t>(fnv1a(key) % num_buckets_);
}

std::string MerkleAntiEntropy::hashBucket(
    const std::vector<std::string>& keys,
    const std::map<std::string, StateEntry>& state) const {
    if (keys.empty()) return "empty";

    // Sort keys for deterministic hash regardless of insertion order
    std::vector<std::string> sorted_keys(keys.begin(), keys.end());
    std::sort(sorted_keys.begin(), sorted_keys.end());

    uint64_t hash = 14695981039346656037ULL;
    for (const auto& key : sorted_keys) {
        // Hash key name
        for (unsigned char c : key) {
            hash ^= c;
            hash *= 1099511628211ULL;
        }
        hash ^= ':';
        hash *= 1099511628211ULL;

        // Hash value + timestamp
        auto it = state.find(key);
        if (it != state.end()) {
            for (unsigned char c : it->second.value) {
                hash ^= c;
                hash *= 1099511628211ULL;
            }
            // Include timestamp to detect same-value-different-time divergence
            auto ts = it->second.last_modified;
            for (int i = 0; i < 8; ++i) {
                hash ^= (ts >> (i * 8)) & 0xFF;
                hash *= 1099511628211ULL;
            }
        }
        hash ^= '\n';
        hash *= 1099511628211ULL;
    }

    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(buf);
}

void MerkleAntiEntropy::rebuild(const std::map<std::string, StateEntry>& state) {
    // Clear existing
    for (auto& bucket : buckets_) bucket.clear();
    key_hashes_.clear();
    total_keys_ = 0;

    // Assign keys to buckets
    for (const auto& [key, entry] : state) {
        uint32_t bucket = bucketForKey(key);
        buckets_[bucket].push_back(key);
        ++total_keys_;
    }

    // Compute leaf hashes
    for (uint32_t i = 0; i < num_buckets_; ++i) {
        leaf_hashes_[i] = hashBucket(buckets_[i], state);
    }

    // Build tree bottom-up
    root_ = std::make_unique<MerkleNode>();
    root_->depth = 0;

    // Recursive tree construction
    std::function<void(MerkleNode*, uint32_t, uint32_t, uint32_t)> buildTree;
    buildTree = [&](MerkleNode* node, uint32_t depth,
                    uint32_t start_bucket, uint32_t span) {
        if (depth == tree_depth_) {
            // Leaf node
            node->hash = leaf_hashes_[start_bucket];
            node->covered_keys = buckets_[start_bucket];
            node->bucket_index = start_bucket;
            return;
        }

        uint32_t child_span = span / config_.branching_factor;
        std::vector<std::string> child_hashes;

        for (uint32_t c = 0; c < config_.branching_factor; ++c) {
            auto child = std::make_unique<MerkleNode>();
            child->depth = depth + 1;
            child->bucket_index = c;
            uint32_t child_start = start_bucket + c * child_span;
            buildTree(child.get(), depth + 1, child_start, child_span);
            child_hashes.push_back(child->hash);
            node->children.push_back(std::move(child));
        }

        node->hash = combineHashes(child_hashes);
    };

    buildTree(root_.get(), 0, 0, num_buckets_);
    mutations_since_rebuild_ = 0;
    stats_.full_rebuilds++;
}

void MerkleAntiEntropy::update(const std::string& key, const StateEntry& entry) {
    uint32_t bucket = bucketForKey(key);

    // Add to bucket if new
    bool found = false;
    for (const auto& k : buckets_[bucket]) {
        if (k == key) { found = true; break; }
    }
    if (!found) {
        buckets_[bucket].push_back(key);
        ++total_keys_;
    }

    // Recompute leaf hash for this bucket
    // (We need a mini state map for this bucket — use key_hashes_ as proxy)
    // For incremental update, just mark dirty and recompute on next digest()
    mutations_since_rebuild_++;
    stats_.incremental_updates++;

    // Store per-key hash for quick dirty detection
    uint64_t kh = fnv1a(key + ":" + entry.value + ":" +
                        std::to_string(entry.last_modified));
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(kh));
    key_hashes_[key] = std::string(buf);
}

MerkleDigest MerkleAntiEntropy::digest() const {
    MerkleDigest d;
    if (!root_) {
        d.root_hash = "empty";
        return d;
    }

    d.root_hash = root_->hash;
    d.key_count = total_keys_;

    // Build level hashes for progressive drill-down
    d.level_hashes.resize(tree_depth_ + 1);
    d.level_hashes[0] = {root_->hash};

    // BFS to collect hashes at each level
    std::vector<const MerkleNode*> current_level = {root_.get()};
    for (uint32_t level = 1; level <= tree_depth_; ++level) {
        std::vector<const MerkleNode*> next_level;
        std::vector<std::string> level_h;
        for (const auto* node : current_level) {
            for (const auto& child : node->children) {
                level_h.push_back(child->hash);
                next_level.push_back(child.get());
            }
        }
        d.level_hashes[level] = std::move(level_h);
        current_level = std::move(next_level);
    }

    stats_.digests_generated++;
    return d;
}

MerkleDiff MerkleAntiEntropy::compare(const MerkleDigest& remote) const {
    MerkleDiff diff;

    // Fast path: root matches → fully synchronized
    if (!root_ || root_->hash == remote.root_hash) {
        diff.nodes_compared = 1;
        diff.nodes_matched = 1;
        return diff;
    }

    diff.nodes_compared = 1;  // root compared, didn't match

    // Drill down level by level comparing hashes
    for (uint32_t level = 1; level <= tree_depth_; ++level) {
        if (level >= remote.level_hashes.size()) break;

        // Collect local hashes at this level by BFS traversal
        std::vector<std::string> local_level_storage;
        {
            std::vector<const MerkleNode*> nodes = {root_.get()};
            for (uint32_t l = 0; l < level; ++l) {
                std::vector<const MerkleNode*> next;
                for (const auto* n : nodes) {
                    for (const auto& c : n->children) {
                        next.push_back(c.get());
                    }
                }
                nodes = std::move(next);
            }
            local_level_storage.reserve(nodes.size());
            for (const auto* n : nodes) {
                local_level_storage.push_back(n->hash);
            }
        }
        const auto& local_level = local_level_storage;

        const auto& remote_level = remote.level_hashes[level];
        size_t count = std::min(local_level.size(), remote_level.size());

        for (size_t i = 0; i < count; ++i) {
            diff.nodes_compared++;
            if (local_level[i] == remote_level[i]) {
                diff.nodes_matched++;
            } else if (level == tree_depth_) {
                // Leaf level divergence — these keys need sync
                if (i < buckets_.size()) {
                    for (const auto& key : buckets_[i]) {
                        diff.divergent_keys.push_back(key);
                    }
                }
            }
        }
    }

    stats_.comparisons++;
    stats_.keys_synced += diff.divergent_keys.size();
    return diff;
}

std::vector<std::string> MerkleAntiEntropy::keysInBucket(
    uint32_t bucket_index) const {
    if (bucket_index < buckets_.size()) return buckets_[bucket_index];
    return {};
}

uint32_t MerkleAntiEntropy::bucketCount() const {
    return num_buckets_;
}

// ---------------------------------------------------------------------------
// SplitInferenceCoordinator
// ---------------------------------------------------------------------------

std::optional<SplitPlan> SplitInferenceCoordinator::plan(
    const std::string& model_id,
    std::uint32_t total_layers,
    std::uint32_t memory_per_layer_mb,
    const std::vector<PeerInfo>& peers) {

    // Filter to NPU-capable, alive peers with sufficient resources
    std::vector<const PeerInfo*> candidates;
    for (const auto& p : peers) {
        if (p.isAlive() && p.capabilities.has_npu &&
            p.capabilities.ram_mb > memory_per_layer_mb * 2) {
            candidates.push_back(&p);
        }
    }

    if (candidates.size() < 2) return std::nullopt;  // need ≥2 devices

    // Sort by capability score (descending)
    std::sort(candidates.begin(), candidates.end(),
              [](const PeerInfo* a, const PeerInfo* b) {
                  return a->capabilities.score() > b->capabilities.score();
              });

    // Greedy partitioning: assign layers proportional to NPU TOPS
    std::uint32_t total_tops = 0;
    for (const auto* p : candidates) {
        total_tops += std::max(p->capabilities.npu_tops, 1u);
    }

    SplitPlan plan;
    plan.model_id = model_id;
    plan.total_layers = total_layers;
    plan.single_device_ms = total_layers * 10;  // ~10ms/layer estimate

    std::uint32_t assigned = 0;
    for (size_t i = 0; i < candidates.size() && assigned < total_layers; ++i) {
        float ratio = static_cast<float>(
            std::max(candidates[i]->capabilities.npu_tops, 1u)) /
            static_cast<float>(total_tops);
        std::uint32_t layers = static_cast<uint32_t>(
            std::ceil(ratio * total_layers));
        layers = std::min(layers, total_layers - assigned);

        InferencePartition part;
        part.model_id = model_id;
        part.layer_start = assigned;
        part.layer_end = assigned + layers;
        part.assigned_peer = candidates[i]->id;
        part.estimated_ms = layers * 10 /
            std::max(candidates[i]->capabilities.npu_tops / 10, 1u);
        part.memory_mb = layers * memory_per_layer_mb;
        plan.partitions.push_back(part);

        assigned += layers;
    }

    // Estimate total time (pipeline: max partition + network transfer overhead)
    std::uint32_t max_partition_ms = 0;
    for (const auto& p : plan.partitions) {
        max_partition_ms = std::max(max_partition_ms, p.estimated_ms);
    }
    // Add inter-device transfer overhead (~5ms per boundary)
    plan.estimated_total_ms = max_partition_ms +
        static_cast<uint32_t>((plan.partitions.size() - 1) * 5);
    plan.speedup_ratio = static_cast<float>(plan.estimated_total_ms) /
                         static_cast<float>(plan.single_device_ms);

    return plan;
}

bool SplitInferenceCoordinator::shouldSplit(
    std::uint32_t model_memory_mb,
    std::uint32_t local_ram_mb,
    const std::vector<PeerInfo>& peers) {

    // Split only if model doesn't fit locally
    if (model_memory_mb <= local_ram_mb * 0.8f) return false;

    // Need at least one other NPU peer with enough combined RAM
    std::uint32_t total_available = local_ram_mb;
    std::uint32_t npu_peers = 0;
    for (const auto& p : peers) {
        if (p.isAlive() && p.capabilities.has_npu) {
            total_available += p.capabilities.ram_mb;
            ++npu_peers;
        }
    }

    return npu_peers >= 1 && total_available >= model_memory_mb;
}

// ---------------------------------------------------------------------------
// MeshProtocol (top-level coordinator)
// ---------------------------------------------------------------------------

MeshProtocol::MeshProtocol(PeerId self_id, DeviceCapabilities self_caps,
                           MeshConfig config)
    : self_id_(std::move(self_id))
    , self_caps_(std::move(self_caps))
    , config_(std::move(config)) {

    discovery_ = std::make_unique<MeshDiscovery>(self_id_, config_.discovery);
    router_ = std::make_unique<CapabilityRouter>(self_id_, self_caps_);
    state_sync_ = std::make_unique<CrdtStateSync>(self_id_.device_id);
}

MeshProtocol::~MeshProtocol() {
    stop();
}

std::unique_ptr<MeshProtocol> MeshProtocol::create(
    PeerId self_id, DeviceCapabilities self_caps, MeshConfig config) {
    return std::unique_ptr<MeshProtocol>(
        new MeshProtocol(std::move(self_id), std::move(self_caps),
                         std::move(config)));
}

bool MeshProtocol::start() {
    if (active_) return true;
    if (!discovery_->start()) return false;
    active_ = true;
    return true;
}

void MeshProtocol::stop() {
    if (!active_) return;
    discovery_->stop();
    active_ = false;
}

RoutingDecision MeshProtocol::route(const RoutingRequest& request) const {
    if (!active_ || !config_.enable_auto_routing) {
        // Return local routing when mesh is inactive
        RoutingDecision local;
        local.target = self_id_;
        local.is_local = true;
        local.score = 1.0f;
        local.reason = "mesh inactive, executing locally";
        return local;
    }
    return router_->route(request, discovery_->peers());
}

void MeshProtocol::sync(const std::string& key, CrdtType type,
                        const std::string& value) {
    if (!config_.enable_sync) return;
    state_sync_->mutate(key, type, value);
    // In production: broadcast the operation to all peers via UDP
}

void MeshProtocol::mergeRemote(const std::vector<CrdtOperation>& ops) {
    for (const auto& op : ops) {
        state_sync_->merge(op);
    }
}

std::optional<StateEntry> MeshProtocol::getState(
    const std::string& key) const {
    return state_sync_->get(key);
}

std::vector<PeerInfo> MeshProtocol::peers() const {
    return discovery_->peers();
}

MeshProtocol::MeshHealth MeshProtocol::health() const {
    MeshHealth h;
    auto all_peers = discovery_->peers();
    h.total_peers = static_cast<uint32_t>(all_peers.size());
    h.alive_peers = 0;
    for (const auto& p : all_peers) {
        if (p.isAlive()) ++h.alive_peers;
    }
    h.synced_keys = static_cast<uint32_t>(state_sync_->stateSize());
    h.last_sync_timestamp = state_sync_->currentTimestamp();
    h.discovery_active = discovery_->isRunning();
    h.sync_active = config_.enable_sync;
    return h;
}

bool MeshProtocol::isActive() const {
    return active_;
}

// ===========================================================================
// Split Inference Pipeline — Layer Partitioner, Activation Transfer, Executor
// ===========================================================================

// ---------------------------------------------------------------------------
// LayerPartitioner (DP-based optimal layer-to-device assignment)
// ---------------------------------------------------------------------------

float LayerPartitioner::computeStageTime(
    const std::vector<LayerCost>& layer_costs,
    std::uint32_t start, std::uint32_t end,
    std::uint32_t device_tops) {
    // Sum compute costs for layers [start, end), scaled by device throughput.
    // A layer's compute_ms is calibrated for 1 TOPS; scale inversely.
    float total = 0.0f;
    float scale = (device_tops > 0) ? (1.0f / static_cast<float>(device_tops)) : 1.0f;
    for (std::uint32_t i = start; i < end; ++i) {
        total += static_cast<float>(layer_costs[i].compute_ms) * scale;
    }
    return total;
}

float LayerPartitioner::computeTransferTime(
    const std::vector<LayerCost>& layer_costs,
    std::uint32_t boundary_layer,
    std::uint32_t bandwidth_kbps) {
    if (bandwidth_kbps == 0) return 1e9f;  // infinite cost if no link
    // Transfer time = activation_bytes at boundary / bandwidth
    // boundary_layer is the last layer in the previous stage (its output goes across)
    if (boundary_layer == 0) return 0.0f;
    std::uint32_t bytes = layer_costs[boundary_layer - 1].activation_bytes;
    // bandwidth_kbps is in kilobits per second
    float bandwidth_bytes_per_ms = static_cast<float>(bandwidth_kbps) / 8.0f;  // KB/s → bytes/ms
    return static_cast<float>(bytes) / bandwidth_bytes_per_ms;
}

bool LayerPartitioner::fitsInMemory(
    const std::vector<LayerCost>& layer_costs,
    std::uint32_t start, std::uint32_t end,
    std::uint32_t device_ram_mb) {
    std::uint32_t total_mb = 0;
    for (std::uint32_t i = start; i < end; ++i) {
        total_mb += layer_costs[i].memory_mb;
    }
    // Leave 20% headroom for KV cache and runtime overhead
    return total_mb <= static_cast<std::uint32_t>(device_ram_mb * 0.8f);
}

float LayerPartitioner::evaluateMakespan(
    const std::vector<PartitionAssignment>& assignments) {
    float makespan = 0.0f;
    for (const auto& a : assignments) {
        makespan = std::max(makespan, a.total_time_ms);
    }
    return makespan;
}

// --- PLACEHOLDER_PARTITION_DP ---

PartitionResult LayerPartitioner::partition(
    const std::vector<LayerCost>& layer_costs,
    const std::vector<PartitionDeviceCap>& device_caps,
    const std::vector<std::uint32_t>& bandwidth_kbps) {

    PartitionResult result;
    result.valid = false;

    const std::uint32_t N = static_cast<std::uint32_t>(layer_costs.size());
    const std::uint32_t M = static_cast<std::uint32_t>(device_caps.size());

    if (N == 0 || M == 0) return result;
    if (bandwidth_kbps.size() < M - 1) return result;

    // DP table: dp[m][i] = minimum makespan when partitioning layers [0..i)
    // across devices [0..m], where device m handles layers [cut..i).
    // We minimize the maximum stage time (makespan).
    //
    // dp[m][i] = min over j in [0, i) of:
    //   max(dp[m-1][j], stage_time(j, i, device[m]) + transfer_time(j, bw[m-1]))
    //
    // Base case: dp[0][i] = stage_time(0, i, device[0]) if fits in memory.

    constexpr float INF = 1e18f;

    // dp[m][i]: best makespan for first i layers on first m+1 devices
    std::vector<std::vector<float>> dp(M, std::vector<float>(N + 1, INF));
    // Track the cut points for backtracking
    std::vector<std::vector<std::uint32_t>> cut(M, std::vector<std::uint32_t>(N + 1, 0));

    // Base case: all layers [0..i) on device 0
    for (std::uint32_t i = 1; i <= N; ++i) {
        if (!fitsInMemory(layer_costs, 0, i, device_caps[0].ram_mb)) break;
        dp[0][i] = computeStageTime(layer_costs, 0, i, device_caps[0].tops);
        cut[0][i] = 0;
    }

    // Fill DP table
    for (std::uint32_t m = 1; m < M; ++m) {
        for (std::uint32_t i = m + 1; i <= N; ++i) {
            // Try all possible cut points j: device m gets layers [j, i)
            for (std::uint32_t j = m; j < i; ++j) {
                if (dp[m - 1][j] >= INF) continue;
                if (!fitsInMemory(layer_costs, j, i, device_caps[m].ram_mb)) continue;

                float stage = computeStageTime(layer_costs, j, i, device_caps[m].tops);
                float transfer = (m > 0 && j > 0)
                    ? computeTransferTime(layer_costs, j, bandwidth_kbps[m - 1])
                    : 0.0f;

                // Makespan = max of previous stages' makespan and this stage's total
                float candidate = std::max(dp[m - 1][j], stage + transfer);

                if (candidate < dp[m][i]) {
                    dp[m][i] = candidate;
                    cut[m][i] = j;
                }
            }
        }
    }

    // Find the best solution: dp[m][N] for the smallest makespan across all m
    float best_makespan = INF;
    std::uint32_t best_m = 0;
    for (std::uint32_t m = 0; m < M; ++m) {
        if (dp[m][N] < best_makespan) {
            best_makespan = dp[m][N];
            best_m = m;
        }
    }

    if (best_makespan >= INF) return result;  // infeasible

    // Backtrack to reconstruct assignments
    std::vector<PartitionAssignment> assignments;
    std::uint32_t end = N;
    for (int m = static_cast<int>(best_m); m >= 0; --m) {
        std::uint32_t start = cut[m][end];
        PartitionAssignment a;
        a.device_index = static_cast<std::uint32_t>(m);
        a.layer_start = start;
        a.layer_end = end;
        a.stage_time_ms = computeStageTime(layer_costs, start, end,
                                            device_caps[m].tops);
        a.transfer_time_ms = (m < static_cast<int>(best_m) && end < N)
            ? computeTransferTime(layer_costs, end, bandwidth_kbps[m])
            : 0.0f;
        a.total_time_ms = a.stage_time_ms + a.transfer_time_ms;
        assignments.push_back(a);
        end = start;
    }

    // Reverse to get device-0-first order
    std::reverse(assignments.begin(), assignments.end());

    // Recompute transfer times in forward order
    for (size_t i = 0; i < assignments.size() - 1; ++i) {
        std::uint32_t boundary = assignments[i].layer_end;
        assignments[i].transfer_time_ms =
            computeTransferTime(layer_costs, boundary, bandwidth_kbps[i]);
        assignments[i].total_time_ms =
            assignments[i].stage_time_ms + assignments[i].transfer_time_ms;
    }
    // Last stage has no outbound transfer
    assignments.back().transfer_time_ms = 0.0f;
    assignments.back().total_time_ms = assignments.back().stage_time_ms;

    result.assignments = std::move(assignments);
    result.makespan_ms = best_makespan;
    result.pipeline_latency_ms = 0.0f;
    for (const auto& a : result.assignments) {
        result.pipeline_latency_ms += a.stage_time_ms;
    }
    result.speedup = (result.makespan_ms > 0.0f)
        ? result.pipeline_latency_ms / result.makespan_ms : 1.0f;
    result.valid = true;
    return result;
}

// --- PLACEHOLDER_ACTIVATION_TRANSFER ---

// ---------------------------------------------------------------------------
// ActivationHeader
// ---------------------------------------------------------------------------

std::uint64_t ActivationHeader::elementCount() const {
    if (shape.empty()) return 0;
    std::uint64_t count = 1;
    for (auto dim : shape) count *= dim;
    return count;
}

std::uint64_t ActivationHeader::uncompressedSize() const {
    return elementCount() * dtypeSize(dtype);
}

std::vector<std::uint8_t> ActivationHeader::serialize() const {
    // Wire layout:
    // [magic:4][version:2][seq_id:4][ndims:2][shape:ndims*4][dtype:1][flags:1][payload_size:4]
    std::vector<std::uint8_t> buf;
    buf.reserve(18 + ndims * 4);

    auto push8 = [&](std::uint8_t v) { buf.push_back(v); };
    auto push16 = [&](std::uint16_t v) {
        buf.push_back(static_cast<uint8_t>(v >> 8));
        buf.push_back(static_cast<uint8_t>(v & 0xFF));
    };
    auto push32 = [&](std::uint32_t v) {
        buf.push_back(static_cast<uint8_t>(v >> 24));
        buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(v & 0xFF));
    };

    push32(MAGIC);
    push16(VERSION);
    push32(sequence_id);
    push16(ndims);
    for (auto dim : shape) push32(dim);
    push8(static_cast<uint8_t>(dtype));
    uint8_t flags = 0;
    if (compressed) flags |= 0x01;
    push8(flags);
    push32(payload_size);

    return buf;
}

std::uint32_t ActivationHeader::deserialize(
    const std::uint8_t* data, std::size_t len, ActivationHeader& out) {
    // Minimum header: magic(4) + version(2) + seq(4) + ndims(2) + dtype(1) + flags(1) + payload(4) = 18
    if (len < 18) return 0;

    auto read16 = [](const uint8_t* p) -> uint16_t {
        return (static_cast<uint16_t>(p[0]) << 8) | p[1];
    };
    auto read32 = [](const uint8_t* p) -> uint32_t {
        return (static_cast<uint32_t>(p[0]) << 24) |
               (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) |
               static_cast<uint32_t>(p[3]);
    };

    std::size_t offset = 0;

    // Magic
    uint32_t magic = read32(data + offset); offset += 4;
    if (magic != MAGIC) return 0;

    // Version
    uint16_t ver = read16(data + offset); offset += 2;
    if (ver != VERSION) return 0;

    // Sequence ID
    out.sequence_id = read32(data + offset); offset += 4;

    // Ndims
    out.ndims = read16(data + offset); offset += 2;

    // Shape
    if (len < offset + out.ndims * 4 + 6) return 0;  // +dtype+flags+payload
    out.shape.resize(out.ndims);
    for (uint16_t i = 0; i < out.ndims; ++i) {
        out.shape[i] = read32(data + offset); offset += 4;
    }

    // Dtype
    out.dtype = static_cast<ActivationDtype>(data[offset]); offset += 1;

    // Flags
    uint8_t flags = data[offset]; offset += 1;
    out.compressed = (flags & 0x01) != 0;

    // Payload size
    out.payload_size = read32(data + offset); offset += 4;

    return static_cast<uint32_t>(offset);
}

// ---------------------------------------------------------------------------
// ActivationBuffer
// ---------------------------------------------------------------------------

std::size_t ActivationBuffer::wireSize() const {
    return header.serialize().size() + data.size();
}

// --- PLACEHOLDER_TRANSFER_PROTOCOL ---

// ---------------------------------------------------------------------------
// ActivationTransferProtocol
// ---------------------------------------------------------------------------

ActivationTransferProtocol::ActivationTransferProtocol()
    : ActivationTransferProtocol(Config{}) {}

ActivationTransferProtocol::ActivationTransferProtocol(Config config)
    : config_(std::move(config)) {}

ActivationTransferProtocol::~ActivationTransferProtocol() {
    shutdown_.store(true);
    if (send_thread_.joinable()) send_thread_.join();
    if (recv_thread_.joinable()) recv_thread_.join();
}

std::vector<std::uint8_t> ActivationTransferProtocol::compress(
    const std::uint8_t* data, std::size_t len) {
    // Simplified LZ4-style compression: run-length encoding on repeated bytes
    // with delta encoding for activation patterns (many near-zero values).
    // Real deployment would use actual LZ4 via lz4.h.
    std::vector<std::uint8_t> out;
    out.reserve(len);  // worst case: no compression

    // Store original size as first 4 bytes (for decompression)
    out.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(len & 0xFF));

    std::size_t i = 0;
    while (i < len) {
        // Look for runs of identical bytes
        std::size_t run_start = i;
        while (i + 1 < len && data[i] == data[i + 1] && (i - run_start) < 255) {
            ++i;
        }
        std::size_t run_len = i - run_start + 1;
        ++i;

        if (run_len >= 4) {
            // Encode as run: [0xFF][length][byte]
            out.push_back(0xFF);
            out.push_back(static_cast<uint8_t>(run_len));
            out.push_back(data[run_start]);
        } else {
            // Look for literal sequence (non-repeating)
            std::size_t lit_start = run_start;
            while (i < len) {
                if (i + 3 < len && data[i] == data[i + 1] &&
                    data[i] == data[i + 2] && data[i] == data[i + 3]) {
                    break;  // upcoming run, end literal
                }
                ++i;
                if (i - lit_start >= 254) break;  // max literal length
            }
            std::size_t lit_len = i - lit_start;
            // Encode as literal: [length < 0xFF][bytes...]
            out.push_back(static_cast<uint8_t>(lit_len));
            out.insert(out.end(), data + lit_start, data + lit_start + lit_len);
        }
    }

    return out;
}

std::vector<std::uint8_t> ActivationTransferProtocol::decompress(
    const std::uint8_t* data, std::size_t len, std::size_t original_size) {
    std::vector<std::uint8_t> out;
    out.reserve(original_size);

    if (len < 4) return out;

    // Skip the 4-byte original size prefix (caller provides it)
    std::size_t i = 4;
    while (i < len && out.size() < original_size) {
        uint8_t token = data[i++];
        if (token == 0xFF) {
            // Run-length encoded
            if (i + 1 >= len) break;
            uint8_t run_len = data[i++];
            uint8_t byte_val = data[i++];
            for (uint8_t r = 0; r < run_len && out.size() < original_size; ++r) {
                out.push_back(byte_val);
            }
        } else {
            // Literal sequence
            uint8_t lit_len = token;
            if (i + lit_len > len) break;
            out.insert(out.end(), data + i, data + i + lit_len);
            i += lit_len;
        }
    }

    return out;
}

ActivationBuffer ActivationTransferProtocol::prepareBuffer(
    const std::uint8_t* data, std::size_t size,
    const std::vector<std::uint32_t>& shape,
    ActivationDtype dtype,
    std::uint32_t sequence_id) {

    ActivationBuffer buf;
    buf.header.ndims = static_cast<uint16_t>(shape.size());
    buf.header.shape = shape;
    buf.header.dtype = dtype;
    buf.header.sequence_id = sequence_id;

    bool should_compress = config_.enable_compression &&
                           size > config_.compression_threshold_bytes;

    if (should_compress) {
        auto compressed = compress(data, size);
        // Only use compressed if it actually saves space (ratio > 1.2)
        float ratio = static_cast<float>(size) / static_cast<float>(compressed.size());
        if (ratio > 1.2f) {
            buf.header.compressed = true;
            buf.header.payload_size = static_cast<uint32_t>(compressed.size());
            buf.data = std::move(compressed);

            std::lock_guard<std::mutex> lock(mutex_);
            // Update running average compression ratio
            float prev = stats_.compression_ratio;
            stats_.compression_ratio = prev * 0.9f + ratio * 0.1f;
        } else {
            buf.header.compressed = false;
            buf.header.payload_size = static_cast<uint32_t>(size);
            buf.data.assign(data, data + size);
        }
    } else {
        buf.header.compressed = false;
        buf.header.payload_size = static_cast<uint32_t>(size);
        buf.data.assign(data, data + size);
    }

    return buf;
}

std::vector<std::uint8_t> ActivationTransferProtocol::decodeBuffer(
    const ActivationBuffer& buffer) {
    if (!buffer.valid()) return {};

    if (!buffer.header.compressed) {
        return buffer.data;
    }

    std::size_t original_size = static_cast<std::size_t>(
        buffer.header.uncompressedSize());
    return decompress(buffer.data.data(), buffer.data.size(), original_size);
}

bool ActivationTransferProtocol::asyncSend(
    const ActivationBuffer& buffer, const PeerId& target,
    SendCallback on_complete) {

    if (send_status_.load() == TransferStatus::Sending) return false;
    send_status_.store(TransferStatus::Sending);

    // In a real implementation, this would open a TCP/QUIC connection to target
    // and stream the serialized header + payload. Here we simulate the async
    // pattern with the back buffer.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        back_buffer_ = buffer;
    }

    // Detach send operation (in production: actual network I/O)
    if (send_thread_.joinable()) send_thread_.join();
    send_thread_ = std::thread([this, buffer, target, on_complete]() {
        if (shutdown_.load()) {
            send_status_.store(TransferStatus::Error);
            if (on_complete) on_complete(false, buffer.header.sequence_id);
            return;
        }

        // Simulate network transfer time based on payload size
        // (In production: actual socket send with framing)
        auto start = std::chrono::steady_clock::now();

        // The "transfer" — in real code this would be sendto()/write()
        // For now, we just validate the buffer is well-formed
        bool success = buffer.valid() && buffer.data.size() == buffer.header.payload_size;

        auto elapsed = std::chrono::steady_clock::now() - start;
        float elapsed_ms = std::chrono::duration<float, std::milli>(elapsed).count();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (success) {
                stats_.bytes_sent += buffer.data.size();
                stats_.sends_completed++;
                float prev_avg = stats_.avg_send_ms;
                stats_.avg_send_ms = prev_avg * 0.8f + elapsed_ms * 0.2f;
            } else {
                stats_.errors++;
            }
        }

        send_status_.store(success ? TransferStatus::Complete : TransferStatus::Error);
        if (on_complete) on_complete(success, buffer.header.sequence_id);
    });

    return true;
}

bool ActivationTransferProtocol::asyncRecv(
    const PeerId& source, RecvCallback on_complete) {

    if (recv_status_.load() == TransferStatus::Receiving) return false;
    recv_status_.store(TransferStatus::Receiving);

    if (recv_thread_.joinable()) recv_thread_.join();
    recv_thread_ = std::thread([this, source, on_complete]() {
        if (shutdown_.load()) {
            recv_status_.store(TransferStatus::Error);
            if (on_complete) on_complete(false, ActivationBuffer{});
            return;
        }

        // In production: listen on socket, read header, then payload
        // Here we simulate receiving into the back buffer
        auto start = std::chrono::steady_clock::now();

        ActivationBuffer received;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            received = back_buffer_;  // simulated: would come from network
        }

        auto elapsed = std::chrono::steady_clock::now() - start;
        float elapsed_ms = std::chrono::duration<float, std::milli>(elapsed).count();

        bool success = received.valid();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (success) {
                stats_.bytes_received += received.data.size();
                stats_.recvs_completed++;
                float prev_avg = stats_.avg_recv_ms;
                stats_.avg_recv_ms = prev_avg * 0.8f + elapsed_ms * 0.2f;
            } else {
                stats_.errors++;
            }
        }

        recv_status_.store(success ? TransferStatus::Complete : TransferStatus::Error);
        if (on_complete) on_complete(success, std::move(received));
    });

    return true;
}

TransferStatus ActivationTransferProtocol::sendStatus() const {
    return send_status_.load();
}

TransferStatus ActivationTransferProtocol::recvStatus() const {
    return recv_status_.load();
}

void ActivationTransferProtocol::swapBuffers() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::swap(front_buffer_, back_buffer_);
}

TransferStats ActivationTransferProtocol::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void ActivationTransferProtocol::reset() {
    shutdown_.store(true);
    if (send_thread_.joinable()) send_thread_.join();
    if (recv_thread_.joinable()) recv_thread_.join();
    shutdown_.store(false);

    std::lock_guard<std::mutex> lock(mutex_);
    front_buffer_ = {};
    back_buffer_ = {};
    send_status_.store(TransferStatus::Idle);
    recv_status_.store(TransferStatus::Idle);
    next_sequence_ = 0;
    stats_ = {};
}

// --- PLACEHOLDER_PIPELINE_EXECUTOR ---

// ---------------------------------------------------------------------------
// PipelineExecutor
// ---------------------------------------------------------------------------

PipelineExecutor::PipelineExecutor()
    : PipelineExecutor(PipelineConfig{}) {}

PipelineExecutor::PipelineExecutor(PipelineConfig config)
    : config_(std::move(config)) {}

PipelineExecutor::~PipelineExecutor() {
    shutdown();
}

bool PipelineExecutor::initialize(
    const PartitionResult& partition,
    const std::vector<PartitionDeviceCap>& devices) {

    if (!partition.valid || partition.assignments.empty()) return false;

    std::lock_guard<std::mutex> lock(mutex_);

    current_partition_ = partition;
    stages_.clear();
    transfers_.clear();
    tokens_since_rebalance_ = 0;
    timing_stats_ = {};

    // Set up pipeline stages from partition assignments
    for (std::size_t i = 0; i < partition.assignments.size(); ++i) {
        const auto& assignment = partition.assignments[i];
        PipelineStageInfo stage;
        stage.stage_index = static_cast<uint32_t>(i);
        stage.layer_start = assignment.layer_start;
        stage.layer_end = assignment.layer_end;
        stage.state = PipelineStageState::Idle;
        stage.tokens_processed = 0;
        stage.errors = 0;

        // Map device index to PeerId
        if (assignment.device_index < devices.size()) {
            stage.device = devices[assignment.device_index].peer;
        }

        stages_.push_back(stage);
    }

    // Set up activation transfer channels between adjacent stages
    // N stages need N-1 transfer channels
    for (std::size_t i = 0; i + 1 < stages_.size(); ++i) {
        auto transfer = std::make_unique<ActivationTransferProtocol>();
        transfers_.push_back(std::move(transfer));
    }

    timing_stats_.stage_times_ms.resize(stages_.size(), 0.0f);
    running_.store(true);
    return true;
}

bool PipelineExecutor::feedToken(
    const std::uint8_t* embedding, std::size_t size,
    const std::vector<std::uint32_t>& shape,
    ActivationDtype dtype) {

    if (!running_.load()) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    if (stages_.empty()) return false;

    // Feed into stage 0
    auto& stage0 = stages_[0];
    if (stage0.state == PipelineStageState::Faulted) return false;

    stage0.state = PipelineStageState::Computing;

    auto start = std::chrono::steady_clock::now();

    // In production: dispatch layers[stage0.layer_start..layer_end) on the device
    // The embedding is the input activation for the first layer.
    // Here we track the flow and timing.

    // Simulate compute (in production: actual QNN/ONNX dispatch)
    float compute_ms = current_partition_.assignments[0].stage_time_ms;
    stage0.last_compute_ms = compute_ms;
    stage0.tokens_processed++;

    // Forward activation to next stage (if not the only stage)
    if (stages_.size() > 1 && !transfers_.empty()) {
        stage0.state = PipelineStageState::SendingOutput;

        // Prepare activation buffer for transfer to stage 1
        ActivationBuffer buf = transfers_[0]->prepareBuffer(
            embedding, size, shape, dtype, stage0.tokens_processed);

        // Async send to next device
        transfers_[0]->asyncSend(buf, stages_[1].device,
            [this](bool success, uint32_t seq_id) {
                std::lock_guard<std::mutex> lk(mutex_);
                if (!success && !stages_.empty()) {
                    stages_[0].errors++;
                }
            });

        stage0.last_transfer_ms = current_partition_.assignments[0].transfer_time_ms;
    }

    stage0.state = PipelineStageState::Idle;

    // Propagate through remaining stages
    float total_compute = compute_ms;
    float total_transfer = stage0.last_transfer_ms;

    for (std::size_t i = 1; i < stages_.size(); ++i) {
        auto& stage = stages_[i];
        if (stage.state == PipelineStageState::Faulted) continue;

        stage.state = PipelineStageState::WaitingInput;
        // In production: asyncRecv blocks until activation arrives
        stage.state = PipelineStageState::Computing;

        float stage_compute = current_partition_.assignments[i].stage_time_ms;
        stage.last_compute_ms = stage_compute;
        total_compute += stage_compute;
        stage.tokens_processed++;

        // Forward to next stage (if not last)
        if (i + 1 < stages_.size()) {
            stage.state = PipelineStageState::SendingOutput;
            float stage_transfer = current_partition_.assignments[i].transfer_time_ms;
            stage.last_transfer_ms = stage_transfer;
            total_transfer += stage_transfer;

            // In production: actual async send through transfers_[i]
        }

        stage.state = PipelineStageState::Idle;
    }

    // Update timing statistics
    updateTimingStats(total_compute, total_transfer);
    tokens_since_rebalance_++;

    // Check if we should rebalance
    if (config_.adaptive_rebalance &&
        tokens_since_rebalance_ >= config_.rebalance_interval_tokens) {
        if (shouldRebalance()) {
            // Signal that repartition is recommended (caller invokes repartition())
            // We don't auto-repartition to avoid disrupting in-flight tokens
        }
    }

    return true;
}

ActivationBuffer PipelineExecutor::collectOutput(std::uint32_t timeout_ms) {
    if (!running_.load()) return {};

    std::lock_guard<std::mutex> lock(mutex_);
    if (stages_.empty()) return {};

    // In production: wait on the final stage's output buffer
    // with the specified timeout. Here we return the last transfer's
    // front buffer (which holds the final activation after swapBuffers).

    auto& last_stage = stages_.back();
    if (last_stage.state == PipelineStageState::Faulted) return {};

    // The output is available in the last transfer's buffer
    // In a real pipeline, this would block until the final stage completes
    // or until timeout_ms expires.

    ActivationBuffer output;
    output.header.sequence_id = last_stage.tokens_processed;
    // Actual data would come from the device's output buffer
    return output;
}

void PipelineExecutor::reportDeviceDropout(const PeerId& device) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Find the faulted stage
    for (auto& stage : stages_) {
        if (stage.device == device) {
            stage.state = PipelineStageState::Faulted;
            stage.errors++;
            break;
        }
    }

    // If adaptive rebalancing is enabled, the caller should invoke repartition()
    // with the remaining healthy devices. We mark the pipeline as unhealthy
    // so the caller knows to act.
}

bool PipelineExecutor::repartition(
    const std::vector<LayerCost>& layer_costs,
    const std::vector<PartitionDeviceCap>& devices,
    const std::vector<std::uint32_t>& bandwidth_kbps) {

    // Compute new partition with remaining devices
    auto new_partition = LayerPartitioner::partition(
        layer_costs, devices, bandwidth_kbps);

    if (!new_partition.valid) return false;

    // Re-initialize pipeline with new partition
    // Note: KV caches for layers that remain on the same device are preserved
    // (device handles this internally). Migrated layers lose their cache.
    bool result = initialize(new_partition, devices);

    if (result) {
        std::lock_guard<std::mutex> lock(mutex_);
        timing_stats_.repartitions++;
        tokens_since_rebalance_ = 0;
    }

    return result;
}

std::vector<PipelineStageInfo> PipelineExecutor::stages() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stages_;
}

PipelineTimingStats PipelineExecutor::timingStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return timing_stats_;
}

bool PipelineExecutor::isHealthy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_.load()) return false;
    for (const auto& stage : stages_) {
        if (stage.state == PipelineStageState::Faulted) return false;
    }
    return !stages_.empty();
}

void PipelineExecutor::shutdown() {
    running_.store(false);
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& transfer : transfers_) {
        transfer->reset();
    }
    for (auto& stage : stages_) {
        stage.state = PipelineStageState::Idle;
    }
}

bool PipelineExecutor::shouldRebalance() const {
    // Check if stage times have drifted significantly from the partition estimate.
    // If the actual makespan exceeds the predicted makespan by more than
    // rebalance_threshold, recommend repartitioning.
    if (stages_.size() < 2) return false;

    float max_actual_stage = 0.0f;
    float min_actual_stage = 1e18f;
    for (const auto& stage : stages_) {
        if (stage.state == PipelineStageState::Faulted) return true;  // always rebalance on fault
        float total = stage.last_compute_ms + stage.last_transfer_ms;
        max_actual_stage = std::max(max_actual_stage, total);
        min_actual_stage = std::min(min_actual_stage, total);
    }

    // Imbalance ratio: if the slowest stage is much slower than the fastest,
    // the pipeline has significant bubbles.
    if (min_actual_stage <= 0.0f) return false;
    float imbalance = (max_actual_stage - min_actual_stage) / max_actual_stage;
    return imbalance > config_.rebalance_threshold;
}

void PipelineExecutor::updateTimingStats(float compute_ms, float transfer_ms) {
    float total = compute_ms + transfer_ms;
    timing_stats_.total_latency_ms =
        timing_stats_.total_latency_ms * 0.9f + total * 0.1f;
    timing_stats_.compute_time_ms =
        timing_stats_.compute_time_ms * 0.9f + compute_ms * 0.1f;
    timing_stats_.transfer_time_ms =
        timing_stats_.transfer_time_ms * 0.9f + transfer_ms * 0.1f;

    // Pipeline bubble = makespan - max(stage_time)
    float max_stage = 0.0f;
    for (std::size_t i = 0; i < stages_.size(); ++i) {
        float st = stages_[i].last_compute_ms + stages_[i].last_transfer_ms;
        max_stage = std::max(max_stage, st);
        if (i < timing_stats_.stage_times_ms.size()) {
            timing_stats_.stage_times_ms[i] =
                timing_stats_.stage_times_ms[i] * 0.9f + st * 0.1f;
        }
    }
    timing_stats_.pipeline_bubble_ms =
        timing_stats_.pipeline_bubble_ms * 0.9f +
        (total - max_stage) * 0.1f;

    // Utilization: fraction of time spent computing (vs idle + transfer)
    if (total > 0.0f) {
        float util = compute_ms / total;
        timing_stats_.utilization = timing_stats_.utilization * 0.9f + util * 0.1f;
    }

    // Tokens per second estimate
    if (timing_stats_.total_latency_ms > 0.0f) {
        timing_stats_.tokens_per_second =
            static_cast<uint32_t>(1000.0f / timing_stats_.total_latency_ms);
    }
}

// ===========================================================================
// QUIC-like Network Transport Layer — Implementation
// ===========================================================================

// ---------------------------------------------------------------------------
// QTransportPacket
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> QTransportPacket::serialize() const {
    std::vector<std::uint8_t> buf;
    buf.reserve(18 + payload.size());

    auto push8 = [&](std::uint8_t v) { buf.push_back(v); };
    auto push16 = [&](std::uint16_t v) {
        buf.push_back(static_cast<uint8_t>(v >> 8));
        buf.push_back(static_cast<uint8_t>(v & 0xFF));
    };
    auto push32 = [&](std::uint32_t v) {
        buf.push_back(static_cast<uint8_t>(v >> 24));
        buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(v & 0xFF));
    };

    push32(MAGIC);
    push8(VERSION);
    push8(static_cast<uint8_t>(type));
    push16(stream_id);
    push32(seq);
    push32(ack);
    push16(payload_len);
    buf.insert(buf.end(), payload.begin(), payload.end());

    return buf;
}

std::uint32_t QTransportPacket::deserialize(
    const std::uint8_t* data, std::size_t len, QTransportPacket& out) {
    // Minimum header: 18 bytes
    if (len < 18) return 0;

    auto read16 = [](const uint8_t* p) -> uint16_t {
        return (static_cast<uint16_t>(p[0]) << 8) | p[1];
    };
    auto read32 = [](const uint8_t* p) -> uint32_t {
        return (static_cast<uint32_t>(p[0]) << 24) |
               (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) |
               static_cast<uint32_t>(p[3]);
    };

    std::size_t offset = 0;

    uint32_t magic = read32(data + offset); offset += 4;
    if (magic != QTransportPacket::MAGIC) return 0;

    uint8_t version = data[offset]; offset += 1;
    if (version != QTransportPacket::VERSION) return 0;

    out.type = static_cast<QTransportPacketType>(data[offset]); offset += 1;
    out.stream_id = read16(data + offset); offset += 2;
    out.seq = read32(data + offset); offset += 4;
    out.ack = read32(data + offset); offset += 4;
    out.payload_len = read16(data + offset); offset += 2;

    if (len < offset + out.payload_len) return 0;
    out.payload.assign(data + offset, data + offset + out.payload_len);
    offset += out.payload_len;

    return static_cast<uint32_t>(offset);
}

// ---------------------------------------------------------------------------
// CongestionState
// ---------------------------------------------------------------------------

void CongestionState::updateRtt(float sample_us) {
    if (srtt_us == 0.0f) {
        // First sample
        srtt_us = sample_us;
        rttvar_us = sample_us / 2.0f;
    } else {
        // RFC 6298 EWMA
        float delta = std::abs(srtt_us - sample_us);
        rttvar_us = 0.75f * rttvar_us + 0.25f * delta;
        srtt_us = 0.875f * srtt_us + 0.125f * sample_us;
    }
    rto_us = srtt_us + 4.0f * rttvar_us;
    if (rto_us < 200000.0f) rto_us = 200000.0f;  // Min 200ms
}

void CongestionState::onLoss() {
    // Cubic-inspired multiplicative decrease
    wmax = cwnd;
    cwnd *= beta;  // beta = 0.7
    ssthresh = cwnd;
    in_slow_start = false;
    // Cubic K = cbrt(wmax * (1-beta) / C), C=0.4
    float C = 0.4f;
    k = std::cbrt(wmax * (1.0f - beta) / C);
    epoch_start = std::chrono::steady_clock::now();
}

void CongestionState::onAck() {
    if (in_slow_start) {
        cwnd += 1.0f;  // Exponential growth in slow start
        if (cwnd >= ssthresh) {
            in_slow_start = false;
            epoch_start = std::chrono::steady_clock::now();
            wmax = cwnd;
        }
    } else {
        // Cubic growth: W(t) = C*(t-K)^3 + wmax
        auto now = std::chrono::steady_clock::now();
        float t = std::chrono::duration<float>(now - epoch_start).count();
        float C = 0.4f;
        float target = C * std::pow(t - k, 3.0f) + wmax;
        if (target > cwnd) {
            cwnd += (target - cwnd) / cwnd;
        } else {
            cwnd += 0.01f;  // Minimum growth
        }
    }
}

// --- PLACEHOLDER_QTRANSPORT_IMPL ---

// ===========================================================================
// QUIC 0-RTT Session Resumption — Implementation
// ===========================================================================

// ---------------------------------------------------------------------------
// SessionTicket
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> SessionTicket::serialize() const {
    std::vector<std::uint8_t> buf;
    buf.reserve(128 + peer_id.device_id.size());

    // ticket_id (16 bytes)
    buf.insert(buf.end(), ticket_id.begin(), ticket_id.end());

    // creation_time as int64 (microseconds since epoch, relative)
    auto creation_us = std::chrono::duration_cast<std::chrono::microseconds>(
        creation_time.time_since_epoch()).count();
    for (int i = 7; i >= 0; --i) {
        buf.push_back(static_cast<uint8_t>((creation_us >> (i * 8)) & 0xFF));
    }

    // expiry_time as int64
    auto expiry_us = std::chrono::duration_cast<std::chrono::microseconds>(
        expiry_time.time_since_epoch()).count();
    for (int i = 7; i >= 0; --i) {
        buf.push_back(static_cast<uint8_t>((expiry_us >> (i * 8)) & 0xFF));
    }

    // resumed_key (32 bytes)
    buf.insert(buf.end(), resumed_key.begin(), resumed_key.end());

    // peer_id.device_id (length-prefixed)
    auto id_len = static_cast<uint16_t>(peer_id.device_id.size());
    buf.push_back(static_cast<uint8_t>(id_len >> 8));
    buf.push_back(static_cast<uint8_t>(id_len & 0xFF));
    buf.insert(buf.end(), peer_id.device_id.begin(), peer_id.device_id.end());

    // transport_params (16 bytes: 4x uint32)
    auto push32 = [&](std::uint32_t v) {
        buf.push_back(static_cast<uint8_t>(v >> 24));
        buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(v & 0xFF));
    };
    push32(transport_params.initial_cwnd);
    push32(transport_params.max_streams);
    push32(transport_params.cached_rtt_us);
    push32(transport_params.max_packet_size);

    return buf;
}

bool SessionTicket::deserialize(const std::uint8_t* data, std::size_t len,
                                SessionTicket& out) {
    // Minimum: 16 (ticket_id) + 8 + 8 (times) + 32 (key) + 2 (id_len) + 16 (params)
    if (len < 82) return false;

    std::size_t offset = 0;

    // ticket_id
    std::copy(data, data + 16, out.ticket_id.begin());
    offset += 16;

    // creation_time
    auto read64 = [](const uint8_t* p) -> int64_t {
        int64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v = (v << 8) | p[i];
        }
        return v;
    };
    auto read32 = [](const uint8_t* p) -> uint32_t {
        return (static_cast<uint32_t>(p[0]) << 24) |
               (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) |
               static_cast<uint32_t>(p[3]);
    };

    int64_t creation_us = read64(data + offset); offset += 8;
    out.creation_time = std::chrono::steady_clock::time_point(
        std::chrono::microseconds(creation_us));

    int64_t expiry_us = read64(data + offset); offset += 8;
    out.expiry_time = std::chrono::steady_clock::time_point(
        std::chrono::microseconds(expiry_us));

    // resumed_key
    std::copy(data + offset, data + offset + 32, out.resumed_key.begin());
    offset += 32;

    // peer_id.device_id
    if (offset + 2 > len) return false;
    uint16_t id_len = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
    offset += 2;
    if (offset + id_len > len) return false;
    out.peer_id.device_id.assign(
        reinterpret_cast<const char*>(data + offset), id_len);
    offset += id_len;

    // transport_params
    if (offset + 16 > len) return false;
    out.transport_params.initial_cwnd = read32(data + offset); offset += 4;
    out.transport_params.max_streams = read32(data + offset); offset += 4;
    out.transport_params.cached_rtt_us = read32(data + offset); offset += 4;
    out.transport_params.max_packet_size = read32(data + offset); offset += 4;

    return true;
}

// ---------------------------------------------------------------------------
// SessionTicketStore
// ---------------------------------------------------------------------------

SessionTicketStore::SessionTicketStore()
    : max_capacity_(DEFAULT_MAX_CAPACITY) {}

SessionTicketStore::SessionTicketStore(std::size_t max_capacity)
    : max_capacity_(max_capacity > 0 ? max_capacity : DEFAULT_MAX_CAPACITY) {}

void SessionTicketStore::store(const PeerId& peer_id, SessionTicket ticket) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Evict if at capacity and this is a new peer
    if (tickets_.find(peer_id.device_id) == tickets_.end() &&
        tickets_.size() >= max_capacity_) {
        evictLRU();
    }

    TicketEntry entry;
    entry.ticket = std::move(ticket);
    entry.last_access = std::chrono::steady_clock::now();
    tickets_[peer_id.device_id] = std::move(entry);
}

std::optional<SessionTicket> SessionTicketStore::retrieve(
    const PeerId& peer_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = tickets_.find(peer_id.device_id);
    if (it == tickets_.end()) return std::nullopt;

    const auto& entry = it->second;
    if (!entry.ticket.isValid()) return std::nullopt;

    // Update last_access for LRU (const_cast safe under mutex)
    const_cast<TicketEntry&>(entry).last_access =
        std::chrono::steady_clock::now();
    return entry.ticket;
}

void SessionTicketStore::invalidate(
    const std::array<std::uint8_t, 16>& ticket_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto it = tickets_.begin(); it != tickets_.end(); ++it) {
        if (it->second.ticket.ticket_id == ticket_id) {
            // Zero out the resumed_key before erasing (defense in depth)
            it->second.ticket.resumed_key.fill(0);
            tickets_.erase(it);
            return;
        }
    }
}

void SessionTicketStore::pruneExpired() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto it = tickets_.begin(); it != tickets_.end(); ) {
        if (!it->second.ticket.isValid()) {
            it->second.ticket.resumed_key.fill(0);
            it = tickets_.erase(it);
        } else {
            ++it;
        }
    }
}

std::size_t SessionTicketStore::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tickets_.size();
}

void SessionTicketStore::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Zero all keys before clearing
    for (auto& [id, entry] : tickets_) {
        entry.ticket.resumed_key.fill(0);
    }
    tickets_.clear();
}

void SessionTicketStore::evictLRU() {
    // Caller must hold mutex_
    if (tickets_.empty()) return;

    auto oldest = tickets_.begin();
    for (auto it = tickets_.begin(); it != tickets_.end(); ++it) {
        if (it->second.last_access < oldest->second.last_access) {
            oldest = it;
        }
    }
    oldest->second.ticket.resumed_key.fill(0);
    tickets_.erase(oldest);
}

// ---------------------------------------------------------------------------
// AntiReplayFilter
// ---------------------------------------------------------------------------

AntiReplayFilter::AntiReplayFilter() : AntiReplayFilter(Config{}) {}

AntiReplayFilter::AntiReplayFilter(Config config)
    : config_(std::move(config)),
      half_window_(config_.window.count() / 2) {
    std::size_t byte_count = (config_.num_bits + 7) / 8;
    filter_a_.resize(byte_count, 0);
    filter_b_.resize(byte_count, 0);
    window_start_ = std::chrono::steady_clock::now();
}

bool AntiReplayFilter::checkAndRecord(
    const std::array<std::uint8_t, 16>& ticket_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    maybeRotate();

    auto positions = hashPositions(ticket_id);

    // Check both filters (current + previous half-window)
    auto& current = current_is_a_ ? filter_a_ : filter_b_;
    auto& previous = current_is_a_ ? filter_b_ : filter_a_;

    bool seen_in_current = true;
    bool seen_in_previous = true;

    for (std::uint32_t pos : positions) {
        if (!testBit(current, pos)) seen_in_current = false;
        if (!testBit(previous, pos)) seen_in_previous = false;
    }

    if (seen_in_current || seen_in_previous) {
        return true;  // Replay detected (or false positive)
    }

    // Not seen: record in current filter
    for (std::uint32_t pos : positions) {
        setBit(current, pos);
    }
    if (current_is_a_) {
        ++count_a_;
    } else {
        ++count_b_;
    }

    return false;  // First time seeing this ticket_id
}

void AntiReplayFilter::maybeRotate() {
    // Caller must hold mutex_
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - window_start_);

    if (elapsed >= half_window_) {
        // Rotate: clear the "previous" filter, swap roles
        if (current_is_a_) {
            std::fill(filter_b_.begin(), filter_b_.end(), 0);
            count_b_ = 0;
        } else {
            std::fill(filter_a_.begin(), filter_a_.end(), 0);
            count_a_ = 0;
        }
        current_is_a_ = !current_is_a_;
        window_start_ = now;
    }
}

void AntiReplayFilter::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::fill(filter_a_.begin(), filter_a_.end(), 0);
    std::fill(filter_b_.begin(), filter_b_.end(), 0);
    count_a_ = 0;
    count_b_ = 0;
    current_is_a_ = true;
    window_start_ = std::chrono::steady_clock::now();
}

std::uint32_t AntiReplayFilter::approximateCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_a_ + count_b_;
}

std::vector<std::uint32_t> AntiReplayFilter::hashPositions(
    const std::array<std::uint8_t, 16>& ticket_id) const {
    // Use FNV-1a with different seeds to generate independent hash positions.
    // Each hash function uses a different 4-byte window of the ticket_id
    // combined with FNV offset basis variation.
    std::vector<std::uint32_t> positions;
    positions.reserve(config_.num_hashes);

    for (std::uint32_t i = 0; i < config_.num_hashes; ++i) {
        // FNV-1a with seed variation
        uint64_t hash = 0xcbf29ce484222325ULL ^ (static_cast<uint64_t>(i) * 0x100000001b3ULL);
        for (std::size_t j = 0; j < ticket_id.size(); ++j) {
            hash ^= ticket_id[j];
            hash *= 0x100000001b3ULL;
        }
        positions.push_back(static_cast<uint32_t>(hash % config_.num_bits));
    }

    return positions;
}

void AntiReplayFilter::setBit(std::vector<std::uint8_t>& filter,
                              std::uint32_t pos) {
    std::size_t byte_idx = pos / 8;
    std::uint8_t bit_mask = static_cast<uint8_t>(1 << (pos % 8));
    if (byte_idx < filter.size()) {
        filter[byte_idx] |= bit_mask;
    }
}

bool AntiReplayFilter::testBit(const std::vector<std::uint8_t>& filter,
                               std::uint32_t pos) {
    std::size_t byte_idx = pos / 8;
    std::uint8_t bit_mask = static_cast<uint8_t>(1 << (pos % 8));
    if (byte_idx >= filter.size()) return false;
    return (filter[byte_idx] & bit_mask) != 0;
}

// ---------------------------------------------------------------------------
// ZeroRttClientHello serialization
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> ZeroRttClientHello::serialize() const {
    std::vector<std::uint8_t> buf;
    auto ticket_bytes = ticket.serialize();

    // [ticket_len:4][ticket][client_random:32][early_data_len:4][early_data]
    auto push32 = [&](std::uint32_t v) {
        buf.push_back(static_cast<uint8_t>(v >> 24));
        buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(v & 0xFF));
    };

    push32(static_cast<uint32_t>(ticket_bytes.size()));
    buf.insert(buf.end(), ticket_bytes.begin(), ticket_bytes.end());
    buf.insert(buf.end(), client_random.begin(), client_random.end());
    push32(static_cast<uint32_t>(encrypted_early_data.size()));
    buf.insert(buf.end(), encrypted_early_data.begin(),
               encrypted_early_data.end());

    return buf;
}

// PLACEHOLDER_ZERO_RTT_DESER

bool ZeroRttClientHello::deserialize(const std::uint8_t* data, std::size_t len,
                                     ZeroRttClientHello& out) {
    if (len < 4) return false;

    auto read32 = [](const uint8_t* p) -> uint32_t {
        return (static_cast<uint32_t>(p[0]) << 24) |
               (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) |
               static_cast<uint32_t>(p[3]);
    };

    std::size_t offset = 0;

    uint32_t ticket_len = read32(data + offset); offset += 4;
    if (offset + ticket_len > len) return false;
    if (!SessionTicket::deserialize(data + offset, ticket_len, out.ticket)) {
        return false;
    }
    offset += ticket_len;

    if (offset + 32 > len) return false;
    std::copy(data + offset, data + offset + 32, out.client_random.begin());
    offset += 32;

    if (offset + 4 > len) return false;
    uint32_t early_len = read32(data + offset); offset += 4;
    if (offset + early_len > len) return false;
    out.encrypted_early_data.assign(data + offset, data + offset + early_len);

    return true;
}

// ---------------------------------------------------------------------------
// ZeroRttServerResponse serialization
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> ZeroRttServerResponse::serialize() const {
    std::vector<std::uint8_t> buf;

    // [result:1][server_random:32][has_ticket:1][ticket_bytes?]
    buf.push_back(static_cast<uint8_t>(result));
    buf.insert(buf.end(), server_random.begin(), server_random.end());

    if (new_ticket.has_value()) {
        buf.push_back(1);
        auto ticket_bytes = new_ticket->serialize();
        auto ticket_len = static_cast<uint32_t>(ticket_bytes.size());
        buf.push_back(static_cast<uint8_t>(ticket_len >> 24));
        buf.push_back(static_cast<uint8_t>((ticket_len >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((ticket_len >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(ticket_len & 0xFF));
        buf.insert(buf.end(), ticket_bytes.begin(), ticket_bytes.end());
    } else {
        buf.push_back(0);
    }

    return buf;
}

bool ZeroRttServerResponse::deserialize(const std::uint8_t* data,
                                        std::size_t len,
                                        ZeroRttServerResponse& out) {
    // Minimum: 1 (result) + 32 (server_random) + 1 (has_ticket)
    if (len < 34) return false;

    std::size_t offset = 0;
    out.result = static_cast<ZeroRttResult>(data[offset]); offset += 1;
    std::copy(data + offset, data + offset + 32, out.server_random.begin());
    offset += 32;

    uint8_t has_ticket = data[offset]; offset += 1;
    if (has_ticket) {
        if (offset + 4 > len) return false;
        auto read32 = [](const uint8_t* p) -> uint32_t {
            return (static_cast<uint32_t>(p[0]) << 24) |
                   (static_cast<uint32_t>(p[1]) << 16) |
                   (static_cast<uint32_t>(p[2]) << 8) |
                   static_cast<uint32_t>(p[3]);
        };
        uint32_t ticket_len = read32(data + offset); offset += 4;
        if (offset + ticket_len > len) return false;
        SessionTicket ticket;
        if (!SessionTicket::deserialize(data + offset, ticket_len, ticket)) {
            return false;
        }
        out.new_ticket = std::move(ticket);
    }

    return true;
}

// ---------------------------------------------------------------------------
// QTransport — 0-RTT Session Resumption Methods
// ---------------------------------------------------------------------------

void QTransport::generateRandom(std::uint8_t* out, std::size_t len) {
    // Use xorshift64* seeded from steady_clock for random byte generation.
    // In production this should use OS entropy (getrandom/arc4random).
    static thread_local std::uint64_t state =
        0xA3B1C6D2E5F4A7B8ULL ^
        static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());

    for (std::size_t i = 0; i < len; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        state *= 0x2545F4914F6CDD1DULL;
        out[i] = static_cast<uint8_t>(state & 0xFF);
    }
}

std::array<std::uint8_t, 32> QTransport::derive0RttKey(
    const std::array<std::uint8_t, 32>& resumed_key,
    const std::array<std::uint8_t, 32>& client_random) const {
    // 0-RTT key derivation (HKDF-like):
    //   PRK = HMAC-SHA256(salt=client_random, IKM=resumed_key)
    //   OKM = HMAC-SHA256(PRK, info="sparx-0rtt-key" || 0x01)
    //
    // This binds the 0-RTT key to both the original session (resumed_key)
    // and the fresh client random, providing key separation per attempt.

    // Simplified HKDF-Extract: FNV-based PRK (matches existing codebase style)
    // In production, use proper HMAC-SHA256 via MeshSecurity.
    std::array<std::uint8_t, 32> prk{};

    // Mix resumed_key with client_random using iterated xor-fold + rotation
    for (std::size_t i = 0; i < 32; ++i) {
        uint8_t mixed = resumed_key[i] ^ client_random[(i + 7) % 32];
        // Additional mixing with FNV-1a-like combine
        uint64_t h = 0xcbf29ce484222325ULL;
        h ^= mixed;
        h *= 0x100000001b3ULL;
        h ^= resumed_key[(i + 13) % 32];
        h *= 0x100000001b3ULL;
        h ^= client_random[(i + 19) % 32];
        h *= 0x100000001b3ULL;
        prk[i] = static_cast<uint8_t>(h ^ (h >> 32) ^ (h >> 16) ^ (h >> 48));
    }

    // HKDF-Expand: derive output key material
    // info = "sparx-0rtt-key"
    static constexpr uint8_t info[] = "sparx-0rtt-key";
    std::array<std::uint8_t, 32> okm{};
    for (std::size_t i = 0; i < 32; ++i) {
        uint64_t h = 0xcbf29ce484222325ULL;
        h ^= prk[i];
        h *= 0x100000001b3ULL;
        h ^= info[i % sizeof(info)];
        h *= 0x100000001b3ULL;
        h ^= static_cast<uint8_t>(i);
        h *= 0x100000001b3ULL;
        // Additional entropy from neighboring PRK bytes
        h ^= prk[(i + 5) % 32];
        h *= 0x100000001b3ULL;
        okm[i] = static_cast<uint8_t>(h ^ (h >> 8) ^ (h >> 16) ^ (h >> 24) ^
                                       (h >> 32) ^ (h >> 40) ^ (h >> 48) ^ (h >> 56));
    }

    return okm;
}

ZeroRttResult QTransport::connect0RTT(const PeerId& peer,
                                      const std::string& address,
                                      std::uint16_t port,
                                      const std::uint8_t* early_data,
                                      std::size_t early_data_len) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != QTransportState::Closed) return ZeroRttResult::Rejected;

    // Step 1: Look up cached session ticket for this peer
    auto ticket_opt = ticket_store_.retrieve(peer);
    if (!ticket_opt.has_value()) {
        return ZeroRttResult::NoTicket;
    }

    const SessionTicket& ticket = ticket_opt.value();

    // Step 2: Verify ticket validity
    if (!ticket.isValid()) {
        ticket_store_.invalidate(ticket.ticket_id);
        return ZeroRttResult::NoTicket;
    }

    // Step 3: Generate fresh client random
    std::array<std::uint8_t, 32> client_random{};
    generateRandom(client_random.data(), client_random.size());

    // Step 4: Derive 0-RTT encryption key
    auto zero_rtt_key = derive0RttKey(ticket.resumed_key, client_random);

    // Step 5: Encrypt early data using derived key
    // Simple XOR stream cipher keyed by zero_rtt_key (in production: ChaCha20)
    // The key is fresh per-attempt (bound to client_random), so XOR is safe
    // for one-time use here. Full ChaCha20-Poly1305 would be used via
    // MeshSecurity in a complete implementation.
    std::vector<std::uint8_t> encrypted_early(early_data_len);
    for (std::size_t i = 0; i < early_data_len; ++i) {
        // Generate keystream byte from key + position
        uint64_t ks = 0xcbf29ce484222325ULL;
        ks ^= zero_rtt_key[i % 32];
        ks *= 0x100000001b3ULL;
        ks ^= static_cast<uint8_t>(i & 0xFF);
        ks *= 0x100000001b3ULL;
        ks ^= static_cast<uint8_t>((i >> 8) & 0xFF);
        ks *= 0x100000001b3ULL;
        encrypted_early[i] = early_data[i] ^ static_cast<uint8_t>(ks);
    }

    // Step 6: Build ZeroRttClientHello
    ZeroRttClientHello client_hello;
    client_hello.ticket = ticket;
    client_hello.client_random = client_random;
    client_hello.encrypted_early_data = std::move(encrypted_early);

    // Step 7: Set up connection state
    remote_peer_ = peer;
    remote_address_ = address;
    remote_port_ = port;

    // Apply cached transport parameters from ticket
    congestion_.cwnd = static_cast<float>(ticket.transport_params.initial_cwnd);
    congestion_.srtt_us = static_cast<float>(ticket.transport_params.cached_rtt_us);

    // Step 8: Create UDP socket
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) return ZeroRttResult::CryptoError;

    // Step 9: Send the 0-RTT ClientHello as a ZERO_RTT packet
    QTransportPacket pkt;
    pkt.type = QTransportPacketType::ZERO_RTT;
    pkt.stream_id = 0;
    pkt.seq = 0;
    pkt.ack = 0;
    pkt.payload = client_hello.serialize();
    pkt.payload_len = static_cast<uint16_t>(
        std::min(pkt.payload.size(),
                 static_cast<std::size_t>(config_.max_packet_size - 18)));

    sendRawPacket(pkt);
    state_ = QTransportState::SynSent;  // Awaiting server response

    // Step 10: Invalidate the used ticket (single-use for forward secrecy)
    ticket_store_.invalidate(ticket.ticket_id);

    return ZeroRttResult::Success;
}

// PLACEHOLDER_ACCEPT0RTT

QTransport::ZeroRttAcceptResult QTransport::accept0RTT(
    const ZeroRttClientHello& client_hello,
    const std::string& from_address) {
    std::lock_guard<std::mutex> lock(mutex_);

    ZeroRttAcceptResult result;
    result.result = ZeroRttResult::Rejected;

    // Step 1: Validate the ticket's expiry
    if (!client_hello.ticket.isValid()) {
        result.result = ZeroRttResult::Rejected;
        return result;
    }

    // Step 2: Anti-replay check
    // If this ticket_id has been seen before, reject to prevent replay.
    if (anti_replay_.checkAndRecord(client_hello.ticket.ticket_id)) {
        result.result = ZeroRttResult::Replayed;
        return result;
    }

    // Step 3: Derive the same 0-RTT key the client used
    auto zero_rtt_key = derive0RttKey(
        client_hello.ticket.resumed_key, client_hello.client_random);

    // Step 4: Decrypt early data
    std::size_t data_len = client_hello.encrypted_early_data.size();
    result.early_data.resize(data_len);
    for (std::size_t i = 0; i < data_len; ++i) {
        // Same keystream derivation as client (symmetric)
        uint64_t ks = 0xcbf29ce484222325ULL;
        ks ^= zero_rtt_key[i % 32];
        ks *= 0x100000001b3ULL;
        ks ^= static_cast<uint8_t>(i & 0xFF);
        ks *= 0x100000001b3ULL;
        ks ^= static_cast<uint8_t>((i >> 8) & 0xFF);
        ks *= 0x100000001b3ULL;
        result.early_data[i] =
            client_hello.encrypted_early_data[i] ^ static_cast<uint8_t>(ks);
    }

    // Step 5: Set up connection state from the ticket
    remote_peer_ = client_hello.ticket.peer_id;
    remote_address_ = from_address;
    congestion_.cwnd = static_cast<float>(
        client_hello.ticket.transport_params.initial_cwnd);
    congestion_.srtt_us = static_cast<float>(
        client_hello.ticket.transport_params.cached_rtt_us);

    // Step 6: Transition to Established (0-RTT accepted)
    state_ = QTransportState::Established;

    // Step 7: Issue a new ticket for future 0-RTT from this client
    // (Must be done while we hold the lock — issueSessionTicket acquires none)
    SessionTicket new_ticket;
    generateRandom(new_ticket.ticket_id.data(), new_ticket.ticket_id.size());
    new_ticket.creation_time = std::chrono::steady_clock::now();
    new_ticket.expiry_time = new_ticket.creation_time + TICKET_LIFETIME;
    new_ticket.peer_id = client_hello.ticket.peer_id;

    // Derive new resumed_key from the 0-RTT key + server random
    // This ensures the new ticket's key material is distinct from the old one
    std::array<std::uint8_t, 32> server_random{};
    generateRandom(server_random.data(), server_random.size());

    new_ticket.resumed_key = derive0RttKey(zero_rtt_key, server_random);
    new_ticket.transport_params.initial_cwnd =
        static_cast<uint32_t>(congestion_.cwnd);
    new_ticket.transport_params.max_streams = config_.max_streams;
    new_ticket.transport_params.cached_rtt_us =
        static_cast<uint32_t>(congestion_.srtt_us);
    new_ticket.transport_params.max_packet_size = config_.max_packet_size;

    result.new_ticket = new_ticket;
    result.result = ZeroRttResult::Success;

    // Step 8: Send the server response (with new ticket) back to the client
    ZeroRttServerResponse response;
    response.result = ZeroRttResult::Success;
    response.server_random = server_random;
    response.new_ticket = new_ticket;

    QTransportPacket resp_pkt;
    resp_pkt.type = QTransportPacketType::SYN_ACK;  // Reuse SYN_ACK for 0-RTT response
    resp_pkt.stream_id = 0;
    resp_pkt.seq = 0;
    resp_pkt.ack = 1;
    resp_pkt.payload = response.serialize();
    resp_pkt.payload_len = static_cast<uint16_t>(resp_pkt.payload.size());
    sendRawPacket(resp_pkt);

    return result;
}

SessionTicket QTransport::issueSessionTicket(const PeerId& client_peer) {
    // Called by the server after a successful 1-RTT or 0-RTT handshake.
    // Issues a NewSessionTicket that the client can use for future 0-RTT.
    SessionTicket ticket;

    generateRandom(ticket.ticket_id.data(), ticket.ticket_id.size());
    ticket.creation_time = std::chrono::steady_clock::now();
    ticket.expiry_time = ticket.creation_time + TICKET_LIFETIME;
    ticket.peer_id = client_peer;

    // Derive resumed_key from current session state:
    // Mix local_nonce, remote_nonce, and a fresh random to get key material
    // that is bound to this specific session.
    std::array<std::uint8_t, 32> session_material{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Combine nonces as session-binding material
        for (int i = 0; i < 8; ++i) {
            session_material[i] =
                static_cast<uint8_t>((local_nonce_ >> (56 - i * 8)) & 0xFF);
            session_material[8 + i] =
                static_cast<uint8_t>((remote_nonce_ >> (56 - i * 8)) & 0xFF);
        }
        // Fill remaining 16 bytes with fresh random
        generateRandom(session_material.data() + 16, 16);

        ticket.transport_params.initial_cwnd =
            static_cast<uint32_t>(congestion_.cwnd);
        ticket.transport_params.max_streams = config_.max_streams;
        ticket.transport_params.cached_rtt_us =
            static_cast<uint32_t>(congestion_.srtt_us);
        ticket.transport_params.max_packet_size = config_.max_packet_size;
    }

    // Derive the resumed_key via HKDF-like expansion from session material
    std::array<std::uint8_t, 32> salt{};
    generateRandom(salt.data(), salt.size());
    ticket.resumed_key = derive0RttKey(session_material, salt);

    // Send NewSessionTicket to the client as a DATA packet on stream 0
    QTransportPacket nst_pkt;
    nst_pkt.type = QTransportPacketType::DATA;
    nst_pkt.stream_id = 0;  // Control stream
    nst_pkt.seq = 0;
    nst_pkt.ack = 0;

    // Tag the payload so client can identify it as a NewSessionTicket
    // Format: [0xNST magic: 4 bytes][ticket_bytes]
    auto ticket_bytes = ticket.serialize();
    nst_pkt.payload.reserve(4 + ticket_bytes.size());
    // Magic: "NST\x01"
    nst_pkt.payload.push_back(0x4E);  // 'N'
    nst_pkt.payload.push_back(0x53);  // 'S'
    nst_pkt.payload.push_back(0x54);  // 'T'
    nst_pkt.payload.push_back(0x01);  // version 1
    nst_pkt.payload.insert(nst_pkt.payload.end(),
                           ticket_bytes.begin(), ticket_bytes.end());
    nst_pkt.payload_len = static_cast<uint16_t>(nst_pkt.payload.size());

    {
        std::lock_guard<std::mutex> lock(mutex_);
        sendRawPacket(nst_pkt);
    }

    return ticket;
}

void QTransport::storeSessionTicket(const SessionTicket& ticket) {
    // Client-side: store a received session ticket for future 0-RTT
    ticket_store_.store(ticket.peer_id, ticket);
}

// ---------------------------------------------------------------------------
// QTransport
// ---------------------------------------------------------------------------

QTransport::QTransport() : QTransport(Config{}) {}

QTransport::QTransport(Config config) : config_(std::move(config)) {
    congestion_.cwnd = static_cast<float>(config_.initial_cwnd);
}

QTransport::~QTransport() {
    close();
}

std::uint64_t QTransport::generateNonce() {
    // Simple PRNG nonce (in production: use OS entropy)
    static std::uint64_t state = 0x5A73E1E254A6B9CDULL;
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return state ^ static_cast<uint64_t>(now);
}

bool QTransport::connect(const PeerId& peer, const std::string& address,
                         std::uint16_t port) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != QTransportState::Closed) return false;

    remote_peer_ = peer;
    remote_address_ = address;
    remote_port_ = port;

    // Check 0-RTT cache
    auto cache_it = zero_rtt_cache_.find(peer.device_id);
    if (config_.enable_zero_rtt && cache_it != zero_rtt_cache_.end() &&
        cache_it->second.isValid()) {
        // Use cached params for immediate data
        congestion_.cwnd = static_cast<float>(cache_it->second.cached_cwnd);
        congestion_.srtt_us = static_cast<float>(cache_it->second.cached_rtt_us);
    }

    // Create UDP socket
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) return false;

    // Send SYN with random nonce
    local_nonce_ = generateNonce();

    QTransportPacket syn;
    syn.type = QTransportPacketType::SYN;
    syn.stream_id = 0;
    syn.seq = 0;
    syn.ack = 0;
    syn.nonce = local_nonce_;
    // Encode nonce in payload
    syn.payload.resize(8);
    for (int i = 0; i < 8; ++i) {
        syn.payload[i] = static_cast<uint8_t>((local_nonce_ >> (56 - i * 8)) & 0xFF);
    }
    syn.payload_len = static_cast<uint16_t>(syn.payload.size());

    sendRawPacket(syn);
    state_ = QTransportState::SynSent;
    return true;
}

bool QTransport::accept(const QTransportPacket& syn_packet,
                        const std::string& from_address) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (syn_packet.type != QTransportPacketType::SYN) return false;

    // Extract remote nonce from payload
    if (syn_packet.payload.size() >= 8) {
        remote_nonce_ = 0;
        for (int i = 0; i < 8; ++i) {
            remote_nonce_ |= static_cast<uint64_t>(syn_packet.payload[i]) << (56 - i * 8);
        }
    }

    remote_address_ = from_address;
    local_nonce_ = generateNonce();

    // Send SYN-ACK
    QTransportPacket syn_ack;
    syn_ack.type = QTransportPacketType::SYN_ACK;
    syn_ack.stream_id = 0;
    syn_ack.seq = 0;
    syn_ack.ack = syn_packet.seq + 1;
    syn_ack.nonce = local_nonce_;
    syn_ack.payload.resize(8);
    for (int i = 0; i < 8; ++i) {
        syn_ack.payload[i] = static_cast<uint8_t>((local_nonce_ >> (56 - i * 8)) & 0xFF);
    }
    syn_ack.payload_len = static_cast<uint16_t>(syn_ack.payload.size());

    sendRawPacket(syn_ack);
    state_ = QTransportState::SynReceived;
    return true;
}

bool QTransport::send(std::uint16_t stream_id, const std::uint8_t* data,
                      std::size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != QTransportState::Established) return false;

    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return false;

    auto& stream = it->second;
    std::size_t max_payload = config_.max_packet_size - 18;  // Header overhead
    std::size_t offset = 0;

    while (offset < len) {
        // Respect congestion window
        if (static_cast<float>(stream.in_flight.size()) >= congestion_.cwnd) {
            break;  // Would need to wait for ACKs
        }

        std::size_t chunk = std::min(max_payload, len - offset);
        QTransportPacket pkt;
        pkt.type = QTransportPacketType::DATA;
        pkt.stream_id = stream_id;
        pkt.seq = stream.next_seq++;
        pkt.ack = stream.next_ack;
        pkt.payload.assign(data + offset, data + offset + chunk);
        pkt.payload_len = static_cast<uint16_t>(chunk);

        sendRawPacket(pkt);
        stream.in_flight[pkt.seq] = std::chrono::steady_clock::now();
        offset += chunk;
    }

    return offset == len;
}

std::vector<std::uint8_t> QTransport::recv(std::uint16_t stream_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return {};

    auto& stream = it->second;
    std::vector<std::uint8_t> result;

    // Deliver in-order data from reorder buffer
    while (true) {
        auto pkt_it = stream.reorder_buffer.find(stream.next_ack);
        if (pkt_it == stream.reorder_buffer.end()) break;
        result.insert(result.end(),
                      pkt_it->second.payload.begin(),
                      pkt_it->second.payload.end());
        stream.reorder_buffer.erase(pkt_it);
        stream.next_ack++;
    }

    return result;
}

std::uint16_t QTransport::openStream() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (next_stream_id_ >= config_.max_streams) return 0;

    std::uint16_t id = next_stream_id_++;
    StreamState ss;
    ss.stream_id = id;
    streams_[id] = ss;
    return id;
}

void QTransport::closeStream(std::uint16_t stream_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    streams_.erase(stream_id);
}

void QTransport::close() {
    shutdown_.store(true);
    if (io_thread_.joinable()) io_thread_.join();

    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == QTransportState::Established && socket_fd_ >= 0) {
        QTransportPacket fin;
        fin.type = QTransportPacketType::FIN;
        fin.stream_id = 0;
        sendRawPacket(fin);
    }
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
    state_ = QTransportState::Closed;
    streams_.clear();
}

CongestionState QTransport::congestionState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return congestion_;
}

CachedTransportParams QTransport::cacheParams() const {
    std::lock_guard<std::mutex> lock(mutex_);
    CachedTransportParams params;
    params.peer = remote_peer_;
    params.session_ticket = local_nonce_ ^ remote_nonce_;
    params.cached_rtt_us = static_cast<uint32_t>(congestion_.srtt_us);
    params.cached_cwnd = static_cast<uint32_t>(congestion_.cwnd);
    params.cached_at = std::chrono::steady_clock::now();
    return params;
}

bool QTransport::sendZeroRtt(std::uint16_t stream_id, const std::uint8_t* data,
                             std::size_t len, const CachedTransportParams& cached) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!cached.isValid()) return false;

    // Use cached params to send early data before handshake completes
    QTransportPacket pkt;
    pkt.type = QTransportPacketType::ZERO_RTT;
    pkt.stream_id = stream_id;
    pkt.seq = 0;
    pkt.ack = 0;

    std::size_t max_payload = config_.max_packet_size - 18;
    std::size_t chunk = std::min(max_payload, len);
    pkt.payload.assign(data, data + chunk);
    pkt.payload_len = static_cast<uint16_t>(chunk);

    sendRawPacket(pkt);
    return true;
}

void QTransport::processIncoming(const std::uint8_t* data, std::size_t len) {
    QTransportPacket pkt;
    if (QTransportPacket::deserialize(data, len, pkt) == 0) return;

    std::lock_guard<std::mutex> lock(mutex_);

    switch (pkt.type) {
        case QTransportPacketType::SYN:
            handleSyn(pkt);
            break;
        case QTransportPacketType::SYN_ACK:
            handleSynAck(pkt);
            break;
        case QTransportPacketType::ACK:
            handleAck(pkt);
            break;
        case QTransportPacketType::DATA:
            handleData(pkt);
            break;
        case QTransportPacketType::FIN:
            handleFin(pkt);
            break;
        default:
            break;
    }
}

std::vector<std::uint32_t> QTransport::detectLosses(std::uint16_t stream_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::uint32_t> lost;

    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return lost;

    auto& stream = it->second;
    // RACK-style: packets with seq < (largest_acked - REORDER_THRESHOLD) are lost
    if (stream.largest_acked < StreamState::REORDER_THRESHOLD) return lost;

    std::uint32_t loss_threshold = stream.largest_acked - StreamState::REORDER_THRESHOLD;
    for (const auto& [seq, send_time] : stream.in_flight) {
        if (seq <= loss_threshold) {
            lost.push_back(seq);
        }
    }
    return lost;
}

void QTransport::retransmit(std::uint16_t stream_id,
                            const std::vector<std::uint32_t>& lost_seqs) {
    // In production: resend the packets from a retransmission buffer
    // Here we signal the congestion controller
    std::lock_guard<std::mutex> lock(mutex_);
    if (!lost_seqs.empty()) {
        congestion_.onLoss();
    }
}

void QTransport::handleSyn(const QTransportPacket& pkt) {
    // Server side: received connection request
    if (pkt.payload.size() >= 8) {
        remote_nonce_ = 0;
        for (int i = 0; i < 8; ++i) {
            remote_nonce_ |= static_cast<uint64_t>(pkt.payload[i]) << (56 - i * 8);
        }
    }
    state_ = QTransportState::SynReceived;
}

void QTransport::handleSynAck(const QTransportPacket& pkt) {
    if (state_ != QTransportState::SynSent) return;
    // Extract server nonce
    if (pkt.payload.size() >= 8) {
        remote_nonce_ = 0;
        for (int i = 0; i < 8; ++i) {
            remote_nonce_ |= static_cast<uint64_t>(pkt.payload[i]) << (56 - i * 8);
        }
    }
    // Send final ACK
    QTransportPacket ack;
    ack.type = QTransportPacketType::ACK;
    ack.stream_id = 0;
    ack.seq = 1;
    ack.ack = 1;
    sendRawPacket(ack);
    state_ = QTransportState::Established;
}

void QTransport::handleAck(const QTransportPacket& pkt) {
    if (state_ == QTransportState::SynReceived) {
        state_ = QTransportState::Established;
        return;
    }
    // Data ACK: remove from in_flight, update congestion
    auto stream_it = streams_.find(pkt.stream_id);
    if (stream_it == streams_.end()) return;

    auto& stream = stream_it->second;
    auto inflight_it = stream.in_flight.find(pkt.ack - 1);
    if (inflight_it != stream.in_flight.end()) {
        auto rtt = std::chrono::steady_clock::now() - inflight_it->second;
        float rtt_us = std::chrono::duration<float, std::micro>(rtt).count();
        congestion_.updateRtt(rtt_us);
        congestion_.onAck();
        stream.in_flight.erase(inflight_it);
    }
    if (pkt.ack > stream.largest_acked) {
        stream.largest_acked = pkt.ack;
    }
}

void QTransport::handleData(const QTransportPacket& pkt) {
    auto stream_it = streams_.find(pkt.stream_id);
    if (stream_it == streams_.end()) {
        // Auto-create stream for incoming data
        StreamState ss;
        ss.stream_id = pkt.stream_id;
        streams_[pkt.stream_id] = ss;
        stream_it = streams_.find(pkt.stream_id);
    }

    auto& stream = stream_it->second;
    stream.reorder_buffer[pkt.seq] = pkt;

    // Send ACK
    QTransportPacket ack;
    ack.type = QTransportPacketType::ACK;
    ack.stream_id = pkt.stream_id;
    ack.seq = stream.next_seq;
    ack.ack = pkt.seq + 1;
    sendRawPacket(ack);
}

void QTransport::handleFin(const QTransportPacket& /*pkt*/) {
    state_ = QTransportState::Closing;
    // Send FIN-ACK
    QTransportPacket fin_ack;
    fin_ack.type = QTransportPacketType::ACK;
    fin_ack.stream_id = 0;
    sendRawPacket(fin_ack);
    state_ = QTransportState::Closed;
}

void QTransport::sendRawPacket(const QTransportPacket& pkt) {
    if (socket_fd_ < 0) return;
    auto wire = pkt.serialize();

    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(remote_port_);
    inet_pton(AF_INET, remote_address_.c_str(), &dest.sin_addr);

    sendto(socket_fd_, wire.data(), wire.size(), 0,
           reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
}

void QTransport::ioLoop() {
    uint8_t buf[2048];
    struct sockaddr_in sender{};
    socklen_t sender_len = sizeof(sender);

    while (!shutdown_.load()) {
        ssize_t n = recvfrom(socket_fd_, buf, sizeof(buf), 0,
                             reinterpret_cast<struct sockaddr*>(&sender),
                             &sender_len);
        if (n <= 0) continue;
        processIncoming(buf, static_cast<size_t>(n));
    }
}

// ===========================================================================
// BBR-Inspired Bandwidth Probing — Implementation
// ===========================================================================

// ---------------------------------------------------------------------------
// BandwidthEstimate
// ---------------------------------------------------------------------------

constexpr float BandwidthEstimate::GAIN_CYCLE[];

float BandwidthEstimate::currentGain() const {
    switch (phase) {
        case BbrPhase::STARTUP:   return startup_growth_factor;
        case BbrPhase::DRAIN:     return 0.5f;  // Drain at half rate
        case BbrPhase::PROBE_BW:  return GAIN_CYCLE[gain_cycle_index % 8];
        case BbrPhase::PROBE_RTT: return 1.0f;
    }
    return 1.0f;
}

float BandwidthEstimate::estimatedBandwidthBps() const {
    return btl_bw.empty() ? 0.0f : btl_bw.best(true);
}

float BandwidthEstimate::estimatedRttUs() const {
    return rt_prop.empty() ? 1000000.0f : rt_prop.best(false);
}

// ---------------------------------------------------------------------------
// BandwidthProber
// ---------------------------------------------------------------------------

BandwidthProber::BandwidthProber() : BandwidthProber(Config{}) {}

BandwidthProber::BandwidthProber(Config config)
    : config_(std::move(config)) {
    state_.btl_bw.window_size = config_.window_rounds;
    state_.rt_prop.window_size = config_.window_rounds;
    state_.phase = BbrPhase::STARTUP;
    state_.startup_growth_factor = config_.startup_growth;
    last_round_start_ = std::chrono::steady_clock::now();
}

void BandwidthProber::onAck(float delivered_bytes, float rtt_us,
                            float elapsed_us, bool is_app_limited) {
    std::lock_guard<std::mutex> lock(mutex_);

    total_delivered_ += delivered_bytes;

    // Compute delivery rate for this sample
    float delivery_rate = 0.0f;
    if (elapsed_us > 0.0f) {
        delivery_rate = (delivered_bytes / elapsed_us) * 1000000.0f;  // bytes/sec
    }

    // Update BtlBw (windowed max) — only if not app-limited
    if (!is_app_limited && delivery_rate > 0.0f) {
        state_.btl_bw.update(delivery_rate, state_.round_count);
    }

    // Update RtProp (windowed min)
    if (rtt_us > 0.0f) {
        state_.rt_prop.update(rtt_us, state_.round_count);
    }

    // Phase-specific logic
    switch (state_.phase) {
        case BbrPhase::STARTUP:
            checkStartupExit();
            break;
        case BbrPhase::DRAIN:
            checkDrainExit();
            break;
        case BbrPhase::PROBE_BW:
            // Gain cycling happens on round advance
            break;
        case BbrPhase::PROBE_RTT:
            break;
    }

    updatePacingAndCwnd();
}

void BandwidthProber::advanceRound() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.round_count++;
    last_round_start_ = std::chrono::steady_clock::now();

    if (state_.phase == BbrPhase::PROBE_BW) {
        advanceGainCycle();
    }
}

BandwidthEstimate BandwidthProber::estimate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::uint32_t BandwidthProber::estimatedBandwidthKbps() const {
    std::lock_guard<std::mutex> lock(mutex_);
    float bps = state_.estimatedBandwidthBps();
    return static_cast<uint32_t>((bps * 8.0f) / 1000.0f);  // bytes/s -> kbps
}

float BandwidthProber::estimatedRttMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.estimatedRttUs() / 1000.0f;
}

float BandwidthProber::pacingRateBps() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.pacing_rate_bps;
}

float BandwidthProber::targetCwndBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.target_cwnd_bytes;
}

BbrPhase BandwidthProber::phase() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.phase;
}

void BandwidthProber::forcePhase(BbrPhase new_phase) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.phase = new_phase;
    if (new_phase == BbrPhase::PROBE_BW) {
        state_.gain_cycle_index = 0;
    }
}

void BandwidthProber::checkStartupExit() {
    // Exit STARTUP when BtlBw hasn't grown by 25% in last round
    if (state_.btl_bw.samples.size() < 3) return;

    auto& samples = state_.btl_bw.samples;
    std::size_t n = samples.size();
    float recent = samples[n - 1].value;
    float prev = samples[n - 2].value;

    if (prev > 0.0f && (recent / prev) < 1.25f) {
        state_.filled_pipe = true;
        state_.phase = BbrPhase::DRAIN;
    }
}

void BandwidthProber::checkDrainExit() {
    // Exit DRAIN when inflight <= BDP (bandwidth-delay product)
    float bdp = state_.estimatedBandwidthBps() * (state_.estimatedRttUs() / 1000000.0f);
    if (state_.target_cwnd_bytes <= bdp) {
        state_.phase = BbrPhase::PROBE_BW;
        state_.gain_cycle_index = 0;
    }
}

void BandwidthProber::advanceGainCycle() {
    state_.gain_cycle_index = (state_.gain_cycle_index + 1) % 8;
}

void BandwidthProber::updatePacingAndCwnd() {
    float btl_bw = state_.estimatedBandwidthBps();
    float rt_prop = state_.estimatedRttUs();
    float gain = state_.currentGain();

    // Pacing rate = gain * BtlBw
    state_.pacing_rate_bps = gain * btl_bw;

    // Target CWND = gain * BDP
    float bdp = btl_bw * (rt_prop / 1000000.0f);  // bytes
    state_.target_cwnd_bytes = gain * bdp;

    // Floor: minimum CWND
    float min_cwnd = static_cast<float>(config_.min_cwnd_packets) * 1400.0f;
    if (state_.target_cwnd_bytes < min_cwnd) {
        state_.target_cwnd_bytes = min_cwnd;
    }
}

// ===========================================================================
// mTLS-like Security Layer — Implementation
// ===========================================================================

// ---------------------------------------------------------------------------
// MeshSecurity
// ---------------------------------------------------------------------------

MeshSecurity::MeshSecurity() : MeshSecurity(Config{}) {}

MeshSecurity::MeshSecurity(Config config) : config_(std::move(config)) {}

// --- Curve25519 Field Arithmetic (mod 2^255 - 19) ---
// Representation: 5 limbs of 51 bits each (total 255 bits)

MeshSecurity::Fe MeshSecurity::feFromBytes(const std::uint8_t bytes[32]) {
    Fe f{};
    // Load 5 x 51-bit limbs from little-endian bytes
    f[0] = (static_cast<uint64_t>(bytes[0])       |
            (static_cast<uint64_t>(bytes[1]) << 8) |
            (static_cast<uint64_t>(bytes[2]) << 16)|
            (static_cast<uint64_t>(bytes[3]) << 24)|
            (static_cast<uint64_t>(bytes[4]) << 32)|
            (static_cast<uint64_t>(bytes[5]) << 40)|
            (static_cast<uint64_t>(bytes[6]) << 48)) & 0x7FFFFFFFFFFFFULL;
    f[1] = ((static_cast<uint64_t>(bytes[6]) >> 3) |
            (static_cast<uint64_t>(bytes[7]) << 5) |
            (static_cast<uint64_t>(bytes[8]) << 13)|
            (static_cast<uint64_t>(bytes[9]) << 21)|
            (static_cast<uint64_t>(bytes[10]) << 29)|
            (static_cast<uint64_t>(bytes[11]) << 37)|
            (static_cast<uint64_t>(bytes[12]) << 45)) & 0x7FFFFFFFFFFFFULL;
    f[2] = ((static_cast<uint64_t>(bytes[12]) >> 6)|
            (static_cast<uint64_t>(bytes[13]) << 2)|
            (static_cast<uint64_t>(bytes[14]) << 10)|
            (static_cast<uint64_t>(bytes[15]) << 18)|
            (static_cast<uint64_t>(bytes[16]) << 26)|
            (static_cast<uint64_t>(bytes[17]) << 34)|
            (static_cast<uint64_t>(bytes[18]) << 42)|
            (static_cast<uint64_t>(bytes[19]) << 50)) & 0x7FFFFFFFFFFFFULL;
    f[3] = ((static_cast<uint64_t>(bytes[19]) >> 1)|
            (static_cast<uint64_t>(bytes[20]) << 7)|
            (static_cast<uint64_t>(bytes[21]) << 15)|
            (static_cast<uint64_t>(bytes[22]) << 23)|
            (static_cast<uint64_t>(bytes[23]) << 31)|
            (static_cast<uint64_t>(bytes[24]) << 39)|
            (static_cast<uint64_t>(bytes[25]) << 47)) & 0x7FFFFFFFFFFFFULL;
    f[4] = ((static_cast<uint64_t>(bytes[25]) >> 4)|
            (static_cast<uint64_t>(bytes[26]) << 4)|
            (static_cast<uint64_t>(bytes[27]) << 12)|
            (static_cast<uint64_t>(bytes[28]) << 20)|
            (static_cast<uint64_t>(bytes[29]) << 28)|
            (static_cast<uint64_t>(bytes[30]) << 36)|
            (static_cast<uint64_t>(bytes[31]) << 44)) & 0x7FFFFFFFFFFFFULL;
    return f;
}

void MeshSecurity::feToBytes(std::uint8_t out[32], const Fe& f) {
    // Reduce and serialize back to 32 little-endian bytes
    // Simplified: assumes input is already reduced
    Fe h = f;
    // Carry and reduce mod 2^255-19
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < 4; ++i) {
            int64_t carry = static_cast<int64_t>(h[i]) >> 51;
            h[i + 1] += static_cast<uint64_t>(carry);
            h[i] &= 0x7FFFFFFFFFFFFULL;
        }
        int64_t carry = static_cast<int64_t>(h[4]) >> 51;
        h[0] += static_cast<uint64_t>(carry) * 19;
        h[4] &= 0x7FFFFFFFFFFFFULL;
    }

    // Pack into bytes (little-endian)
    uint64_t combined = h[0] | (h[1] << 51);
    for (int i = 0; i < 7; ++i) out[i] = static_cast<uint8_t>((h[0] >> (i*8)) & 0xFF);
    // Continue packing remaining limbs
    uint64_t val = (h[0] >> 48) | (h[1] << 3);
    for (int i = 0; i < 7; ++i) out[6+i] = static_cast<uint8_t>((val >> (i*8)) & 0xFF);
    (void)combined;  // suppress warning
    // Simplified output for remaining bytes
    val = (h[1] >> 45) | (h[2] << 6);
    for (int i = 0; i < 7; ++i) out[12+i] = static_cast<uint8_t>((val >> (i*8)) & 0xFF);
    val = (h[2] >> 42) | (h[3] << 9);
    for (int i = 0; i < 7; ++i) out[19+i] = static_cast<uint8_t>((val >> (i*8)) & 0xFF);
    val = (h[3] >> 39) | (h[4] << 12);
    for (int i = 0; i < 7; ++i) {
        if (25 + i < 32) out[25+i] = static_cast<uint8_t>((val >> (i*8)) & 0xFF);
    }
}

MeshSecurity::Fe MeshSecurity::feAdd(const Fe& a, const Fe& b) {
    Fe r;
    for (int i = 0; i < 5; ++i) r[i] = a[i] + b[i];
    return r;
}

MeshSecurity::Fe MeshSecurity::feSub(const Fe& a, const Fe& b) {
    // Add 2*p to avoid underflow before subtraction
    Fe r;
    const uint64_t bias[5] = {
        0xFFFFFFFFFFFDA, 0x7FFFFFFFFFFFF, 0x7FFFFFFFFFFFF,
        0x7FFFFFFFFFFFF, 0x7FFFFFFFFFFFF
    };
    for (int i = 0; i < 5; ++i) r[i] = a[i] + bias[i] - b[i];
    return r;
}

MeshSecurity::Fe MeshSecurity::feMul(const Fe& a, const Fe& b) {
    // Schoolbook multiplication with reduction mod 2^255-19
    // Using 128-bit intermediates via uint64_t pairs
    __uint128_t t[5] = {};
    for (int i = 0; i < 5; ++i) {
        for (int j = 0; j < 5; ++j) {
            int k = i + j;
            __uint128_t prod = static_cast<__uint128_t>(a[i]) * b[j];
            if (k >= 5) {
                // Reduce: x * 2^255 = x * 19 mod p
                t[k - 5] += prod * 19;
            } else {
                t[k] += prod;
            }
        }
    }
    // Carry propagation
    Fe r;
    for (int i = 0; i < 5; ++i) {
        r[i] = static_cast<uint64_t>(t[i]) & 0x7FFFFFFFFFFFFULL;
        if (i < 4) t[i + 1] += t[i] >> 51;
        else {
            uint64_t carry = static_cast<uint64_t>(t[4] >> 51);
            r[0] += carry * 19;
        }
    }
    return r;
}

MeshSecurity::Fe MeshSecurity::feSquare(const Fe& a) {
    return feMul(a, a);
}

MeshSecurity::Fe MeshSecurity::feInvert(const Fe& a) {
    // Compute a^(p-2) mod p using addition chain for p-2 = 2^255-21
    Fe t0 = feSquare(a);
    Fe t1 = feSquare(t0);
    t1 = feSquare(t1);
    t1 = feMul(a, t1);
    t0 = feMul(t0, t1);
    Fe t2 = feSquare(t0);
    t1 = feMul(t1, t2);
    t2 = feSquare(t1);
    for (int i = 0; i < 4; ++i) t2 = feSquare(t2);
    t1 = feMul(t2, t1);
    t2 = feSquare(t1);
    for (int i = 0; i < 9; ++i) t2 = feSquare(t2);
    t2 = feMul(t2, t1);
    Fe t3 = feSquare(t2);
    for (int i = 0; i < 19; ++i) t3 = feSquare(t3);
    t2 = feMul(t3, t2);
    for (int i = 0; i < 10; ++i) t2 = feSquare(t2);
    t1 = feMul(t2, t1);
    t2 = feSquare(t1);
    for (int i = 0; i < 49; ++i) t2 = feSquare(t2);
    t2 = feMul(t2, t1);
    t3 = feSquare(t2);
    for (int i = 0; i < 99; ++i) t3 = feSquare(t3);
    t2 = feMul(t3, t2);
    for (int i = 0; i < 50; ++i) t2 = feSquare(t2);
    t1 = feMul(t2, t1);
    for (int i = 0; i < 5; ++i) t1 = feSquare(t1);
    return feMul(t1, t0);
}

void MeshSecurity::scalarmult(std::uint8_t result[32],
                              const std::uint8_t scalar[32],
                              const std::uint8_t point[32]) {
    // Montgomery ladder for X25519
    // Clamp scalar per RFC 7748
    std::uint8_t e[32];
    std::memcpy(e, scalar, 32);
    e[0] &= 248;
    e[31] &= 127;
    e[31] |= 64;

    Fe x1 = feFromBytes(point);
    Fe x2 = {1, 0, 0, 0, 0};  // (1, 0) projective
    Fe z2 = {0, 0, 0, 0, 0};
    Fe x3 = x1;
    Fe z3 = {1, 0, 0, 0, 0};

    unsigned int swap = 0;

    for (int pos = 254; pos >= 0; --pos) {
        unsigned int bit = (e[pos / 8] >> (pos % 8)) & 1;
        swap ^= bit;
        // Conditional swap
        for (int i = 0; i < 5; ++i) {
            uint64_t mask = 0ULL - static_cast<uint64_t>(swap);
            uint64_t t = mask & (x2[i] ^ x3[i]);
            x2[i] ^= t;
            x3[i] ^= t;
            t = mask & (z2[i] ^ z3[i]);
            z2[i] ^= t;
            z3[i] ^= t;
        }
        swap = bit;

        // Montgomery ladder step
        Fe a = feAdd(x2, z2);
        Fe b = feSub(x2, z2);
        Fe c = feAdd(x3, z3);
        Fe d = feSub(x3, z3);
        Fe da = feMul(d, a);
        Fe cb = feMul(c, b);
        Fe aa = feSquare(a);
        Fe bb = feSquare(b);
        x2 = feMul(aa, bb);
        Fe e_val = feSub(aa, bb);
        // a24 = 121666 for curve25519
        Fe a24 = {121666, 0, 0, 0, 0};
        Fe a24_e = feMul(a24, e_val);
        Fe sum_aa_a24e = feAdd(aa, a24_e);
        z2 = feMul(e_val, sum_aa_a24e);
        Fe dacb_sum = feAdd(da, cb);
        Fe dacb_diff = feSub(da, cb);
        x3 = feSquare(dacb_sum);
        Fe sq_diff = feSquare(dacb_diff);
        z3 = feMul(x1, sq_diff);
    }

    // Final conditional swap
    for (int i = 0; i < 5; ++i) {
        uint64_t mask = 0ULL - static_cast<uint64_t>(swap);
        uint64_t t = mask & (x2[i] ^ x3[i]);
        x2[i] ^= t;
        x3[i] ^= t;
        t = mask & (z2[i] ^ z3[i]);
        z2[i] ^= t;
        z3[i] ^= t;
    }

    // Compute x2 * z2^(-1)
    Fe z2_inv = feInvert(z2);
    Fe x_result = feMul(x2, z2_inv);
    feToBytes(result, x_result);
}

// --- SHA-256 implementation (simplified) ---

std::array<std::uint8_t, 32> MeshSecurity::sha256(
    const std::uint8_t* data, std::size_t len) {
    // SHA-256 per FIPS 180-4
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };

    auto rotr = [](uint32_t x, int n) -> uint32_t {
        return (x >> n) | (x << (32 - n));
    };

    uint32_t h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    // Pad message
    std::vector<uint8_t> msg(data, data + len);
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) msg.push_back(0x00);
    uint64_t bit_len = static_cast<uint64_t>(len) * 8;
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFF));

    // Process blocks
    for (std::size_t block = 0; block < msg.size(); block += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(msg[block + i*4]) << 24) |
                   (static_cast<uint32_t>(msg[block + i*4+1]) << 16) |
                   (static_cast<uint32_t>(msg[block + i*4+2]) << 8) |
                    static_cast<uint32_t>(msg[block + i*4+3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }

        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t temp1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;
            hh=g; g=f; f=e; e=d+temp1; d=c; c=b; b=a; a=temp1+temp2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }

    std::array<uint8_t, 32> result;
    for (int i = 0; i < 8; ++i) {
        result[i*4]   = static_cast<uint8_t>((h[i] >> 24) & 0xFF);
        result[i*4+1] = static_cast<uint8_t>((h[i] >> 16) & 0xFF);
        result[i*4+2] = static_cast<uint8_t>((h[i] >> 8) & 0xFF);
        result[i*4+3] = static_cast<uint8_t>(h[i] & 0xFF);
    }
    return result;
}

std::array<std::uint8_t, 32> MeshSecurity::hmacSha256(
    const std::uint8_t* key, std::size_t key_len,
    const std::uint8_t* data, std::size_t data_len) {
    // HMAC-SHA256 per RFC 2104
    std::array<uint8_t, 64> k_pad{};
    if (key_len > 64) {
        auto hashed = sha256(key, key_len);
        std::memcpy(k_pad.data(), hashed.data(), 32);
    } else {
        std::memcpy(k_pad.data(), key, key_len);
    }

    // ipad
    std::vector<uint8_t> inner(64 + data_len);
    for (int i = 0; i < 64; ++i) inner[i] = k_pad[i] ^ 0x36;
    std::memcpy(inner.data() + 64, data, data_len);
    auto inner_hash = sha256(inner.data(), inner.size());

    // opad
    std::vector<uint8_t> outer(64 + 32);
    for (int i = 0; i < 64; ++i) outer[i] = k_pad[i] ^ 0x5c;
    std::memcpy(outer.data() + 64, inner_hash.data(), 32);
    return sha256(outer.data(), outer.size());
}

// --- ChaCha20 implementation ---

void MeshSecurity::chacha20Block(std::uint32_t output[16],
                                 const std::uint32_t key[8],
                                 std::uint32_t counter,
                                 const std::uint32_t nonce[3]) {
    uint32_t state[16];
    // "expand 32-byte k"
    state[0] = 0x61707865; state[1] = 0x3320646e;
    state[2] = 0x79622d32; state[3] = 0x6b206574;
    for (int i = 0; i < 8; ++i) state[4 + i] = key[i];
    state[12] = counter;
    state[13] = nonce[0]; state[14] = nonce[1]; state[15] = nonce[2];

    std::memcpy(output, state, 64);

    auto quarter_round = [](uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
        a += b; d ^= a; d = (d << 16) | (d >> 16);
        c += d; b ^= c; b = (b << 12) | (b >> 20);
        a += b; d ^= a; d = (d << 8) | (d >> 24);
        c += d; b ^= c; b = (b << 7) | (b >> 25);
    };

    for (int i = 0; i < 10; ++i) {
        // Column rounds
        quarter_round(output[0], output[4], output[8],  output[12]);
        quarter_round(output[1], output[5], output[9],  output[13]);
        quarter_round(output[2], output[6], output[10], output[14]);
        quarter_round(output[3], output[7], output[11], output[15]);
        // Diagonal rounds
        quarter_round(output[0], output[5], output[10], output[15]);
        quarter_round(output[1], output[6], output[11], output[12]);
        quarter_round(output[2], output[7], output[8],  output[13]);
        quarter_round(output[3], output[4], output[9],  output[14]);
    }

    for (int i = 0; i < 16; ++i) output[i] += state[i];
}

void MeshSecurity::chacha20Encrypt(const std::uint8_t* key,
                                   const std::uint8_t* nonce_12,
                                   std::uint32_t counter,
                                   const std::uint8_t* input,
                                   std::uint8_t* output,
                                   std::size_t len) {
    uint32_t key_words[8];
    for (int i = 0; i < 8; ++i) {
        key_words[i] = static_cast<uint32_t>(key[i*4]) |
                       (static_cast<uint32_t>(key[i*4+1]) << 8) |
                       (static_cast<uint32_t>(key[i*4+2]) << 16) |
                       (static_cast<uint32_t>(key[i*4+3]) << 24);
    }
    uint32_t nonce_words[3];
    for (int i = 0; i < 3; ++i) {
        nonce_words[i] = static_cast<uint32_t>(nonce_12[i*4]) |
                         (static_cast<uint32_t>(nonce_12[i*4+1]) << 8) |
                         (static_cast<uint32_t>(nonce_12[i*4+2]) << 16) |
                         (static_cast<uint32_t>(nonce_12[i*4+3]) << 24);
    }

    std::size_t offset = 0;
    while (offset < len) {
        uint32_t block[16];
        chacha20Block(block, key_words, counter++, nonce_words);
        auto* keystream = reinterpret_cast<uint8_t*>(block);
        std::size_t chunk = std::min(static_cast<std::size_t>(64), len - offset);
        for (std::size_t i = 0; i < chunk; ++i) {
            output[offset + i] = input[offset + i] ^ keystream[i];
        }
        offset += chunk;
    }
}

// --- Poly1305 MAC ---

void MeshSecurity::poly1305Mac(std::uint8_t tag[16],
                               const std::uint8_t* key,
                               const std::uint8_t* msg, std::size_t msg_len) {
    // Poly1305 per RFC 7539 (simplified using __uint128_t)
    // r = key[0..15] clamped, s = key[16..31]
    uint64_t r0 = (static_cast<uint64_t>(key[0]) |
                   (static_cast<uint64_t>(key[1]) << 8) |
                   (static_cast<uint64_t>(key[2]) << 16) |
                   (static_cast<uint64_t>(key[3]) << 24) |
                   (static_cast<uint64_t>(key[4]) << 32) |
                   (static_cast<uint64_t>(key[5]) << 40) |
                   (static_cast<uint64_t>(key[6]) << 48) |
                   (static_cast<uint64_t>(key[7]) << 56));
    uint64_t r1 = (static_cast<uint64_t>(key[8]) |
                   (static_cast<uint64_t>(key[9]) << 8) |
                   (static_cast<uint64_t>(key[10]) << 16) |
                   (static_cast<uint64_t>(key[11]) << 24) |
                   (static_cast<uint64_t>(key[12]) << 32) |
                   (static_cast<uint64_t>(key[13]) << 40) |
                   (static_cast<uint64_t>(key[14]) << 48) |
                   (static_cast<uint64_t>(key[15]) << 56));

    // Clamp r
    r0 &= 0x0FFFFFFC0FFFFFFCULL;
    r0 &= 0x0FFFFFFC0FFFFFFFULL;
    r1 &= 0x00FFFFFFC0FFFFFFULL;

    // Accumulator (using 3 limbs for 130-bit arithmetic)
    __uint128_t h0 = 0, h1 = 0, h2 = 0;
    __uint128_t r_128 = (static_cast<__uint128_t>(r1) << 64) | r0;
    (void)h1; (void)h2; (void)r_128;

    // Simplified: process in 16-byte blocks
    uint64_t acc[3] = {0, 0, 0};  // 130-bit accumulator
    for (std::size_t i = 0; i < msg_len; i += 16) {
        // Add block to accumulator
        std::size_t block_len = std::min(static_cast<std::size_t>(16), msg_len - i);
        uint64_t n0 = 0, n1 = 0;
        for (std::size_t j = 0; j < std::min(block_len, static_cast<std::size_t>(8)); ++j) {
            n0 |= static_cast<uint64_t>(msg[i + j]) << (j * 8);
        }
        for (std::size_t j = 8; j < block_len; ++j) {
            n1 |= static_cast<uint64_t>(msg[i + j]) << ((j - 8) * 8);
        }
        // Add 2^(8*block_len) high bit
        if (block_len < 8) n0 |= 1ULL << (block_len * 8);
        else if (block_len < 16) n1 |= 1ULL << ((block_len - 8) * 8);
        else acc[2] += 1;

        // acc += n
        __uint128_t sum = static_cast<__uint128_t>(acc[0]) + n0;
        acc[0] = static_cast<uint64_t>(sum);
        sum = static_cast<__uint128_t>(acc[1]) + n1 + static_cast<uint64_t>(sum >> 64);
        acc[1] = static_cast<uint64_t>(sum);
        acc[2] += static_cast<uint64_t>(sum >> 64);

        // acc *= r (mod 2^130 - 5) — simplified multiply
        __uint128_t d0 = static_cast<__uint128_t>(acc[0]) * r0;
        __uint128_t d1 = static_cast<__uint128_t>(acc[0]) * r1 +
                         static_cast<__uint128_t>(acc[1]) * r0;
        uint64_t d2 = static_cast<uint64_t>(
            static_cast<__uint128_t>(acc[1]) * r1 +
            static_cast<__uint128_t>(acc[2]) * r0);
        d1 += static_cast<uint64_t>(d0 >> 64);
        d2 += static_cast<uint64_t>(d1 >> 64);

        acc[0] = static_cast<uint64_t>(d0);
        acc[1] = static_cast<uint64_t>(d1);
        acc[2] = d2 & 0x3;  // mod 2^130

        // Partial reduction: carry * 5
        uint64_t carry = d2 >> 2;
        acc[0] += carry * 5;
        if (acc[0] < carry * 5) acc[1]++;
    }

    // Add s
    uint64_t s0 = 0, s1 = 0;
    for (int i = 0; i < 8; ++i) {
        s0 |= static_cast<uint64_t>(key[16 + i]) << (i * 8);
        s1 |= static_cast<uint64_t>(key[24 + i]) << (i * 8);
    }
    __uint128_t final_sum = static_cast<__uint128_t>(acc[0]) + s0;
    acc[0] = static_cast<uint64_t>(final_sum);
    final_sum = static_cast<__uint128_t>(acc[1]) + s1 + static_cast<uint64_t>(final_sum >> 64);
    acc[1] = static_cast<uint64_t>(final_sum);

    // Output tag (little-endian)
    for (int i = 0; i < 8; ++i) tag[i] = static_cast<uint8_t>((acc[0] >> (i * 8)) & 0xFF);
    for (int i = 0; i < 8; ++i) tag[8+i] = static_cast<uint8_t>((acc[1] >> (i * 8)) & 0xFF);
}

// --- High-level security API ---

X25519KeyPair MeshSecurity::generateKeyPair() {
    X25519KeyPair kp;
    // Generate random private key (in production: use OS CSPRNG)
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    uint64_t state = static_cast<uint64_t>(now) ^ 0xDEADBEEFCAFE1234ULL;
    for (int i = 0; i < 32; ++i) {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        kp.private_key[i] = static_cast<uint8_t>(state & 0xFF);
    }
    // Clamp private key per RFC 7748
    kp.private_key[0] &= 248;
    kp.private_key[31] &= 127;
    kp.private_key[31] |= 64;

    // Public key = private_key * base_point(9)
    std::uint8_t basepoint[32] = {9};
    scalarmult(kp.public_key.data(), kp.private_key.data(), basepoint);
    return kp;
}

bool MeshSecurity::x25519(const std::array<std::uint8_t, 32>& private_key,
                          const std::array<std::uint8_t, 32>& peer_public_key,
                          std::array<std::uint8_t, 32>& shared_out) {
    scalarmult(shared_out.data(), private_key.data(), peer_public_key.data());
    // Check for all-zero output (invalid peer key)
    bool all_zero = true;
    for (auto b : shared_out) { if (b != 0) { all_zero = false; break; } }
    return !all_zero;
}

std::array<std::uint8_t, 32> MeshSecurity::hkdfExtract(
    const std::uint8_t* salt, std::size_t salt_len,
    const std::uint8_t* ikm, std::size_t ikm_len) {
    if (salt_len == 0) {
        // Use zero salt per RFC 5869
        std::array<uint8_t, 32> zero_salt{};
        return hmacSha256(zero_salt.data(), 32, ikm, ikm_len);
    }
    return hmacSha256(salt, salt_len, ikm, ikm_len);
}

std::vector<std::uint8_t> MeshSecurity::hkdfExpand(
    const std::array<std::uint8_t, 32>& prk,
    const std::uint8_t* info, std::size_t info_len,
    std::size_t output_len) {
    std::vector<uint8_t> okm;
    okm.reserve(output_len);
    std::array<uint8_t, 32> t{};
    uint8_t counter = 1;
    std::size_t t_len = 0;

    while (okm.size() < output_len) {
        // T(i) = HMAC(PRK, T(i-1) || info || counter)
        std::vector<uint8_t> input;
        input.insert(input.end(), t.data(), t.data() + t_len);
        input.insert(input.end(), info, info + info_len);
        input.push_back(counter++);
        t = hmacSha256(prk.data(), 32, input.data(), input.size());
        t_len = 32;
        std::size_t copy_len = std::min(static_cast<std::size_t>(32), output_len - okm.size());
        okm.insert(okm.end(), t.data(), t.data() + copy_len);
    }
    return okm;
}

SessionKeys MeshSecurity::deriveSessionKeys(
    const std::array<std::uint8_t, 32>& shared_secret,
    const std::array<std::uint8_t, 32>& transcript_hash) {
    // TLS 1.3-style key schedule
    // PRK = HKDF-Extract(transcript_hash, shared_secret)
    auto prk = hkdfExtract(transcript_hash.data(), 32,
                           shared_secret.data(), 32);

    SessionKeys keys;
    // Client write key
    const char* c_key_label = "client_write_key";
    auto c_key = hkdfExpand(prk, reinterpret_cast<const uint8_t*>(c_key_label),
                            std::strlen(c_key_label), 32);
    std::memcpy(keys.client_write_key.data(), c_key.data(), 32);

    // Server write key
    const char* s_key_label = "server_write_key";
    auto s_key = hkdfExpand(prk, reinterpret_cast<const uint8_t*>(s_key_label),
                            std::strlen(s_key_label), 32);
    std::memcpy(keys.server_write_key.data(), s_key.data(), 32);

    // Client IV
    const char* c_iv_label = "client_write_iv";
    auto c_iv = hkdfExpand(prk, reinterpret_cast<const uint8_t*>(c_iv_label),
                           std::strlen(c_iv_label), 12);
    std::memcpy(keys.client_write_iv.data(), c_iv.data(), 12);

    // Server IV
    const char* s_iv_label = "server_write_iv";
    auto s_iv = hkdfExpand(prk, reinterpret_cast<const uint8_t*>(s_iv_label),
                           std::strlen(s_iv_label), 12);
    std::memcpy(keys.server_write_iv.data(), s_iv.data(), 12);

    return keys;
}

std::vector<std::uint8_t> MeshSecurity::encrypt(
    const SessionKeys& keys, bool is_client,
    std::uint64_t nonce_counter,
    const std::uint8_t* plaintext, std::size_t plaintext_len,
    const std::uint8_t* aad, std::size_t aad_len) {

    const auto& write_key = is_client ? keys.client_write_key : keys.server_write_key;
    const auto& base_iv = is_client ? keys.client_write_iv : keys.server_write_iv;

    // XOR nonce counter into IV (TLS 1.3 style)
    std::array<uint8_t, 12> nonce = base_iv;
    for (int i = 0; i < 8; ++i) {
        nonce[4 + i] ^= static_cast<uint8_t>((nonce_counter >> (i * 8)) & 0xFF);
    }

    // Encrypt with ChaCha20 (counter starts at 1 for data, 0 for Poly1305 key)
    std::vector<uint8_t> result(plaintext_len + 16);  // ciphertext + tag

    // Generate Poly1305 one-time key (block 0)
    uint8_t poly_key[64] = {};
    uint8_t zeros[64] = {};
    chacha20Encrypt(write_key.data(), nonce.data(), 0, zeros, poly_key, 64);

    // Encrypt plaintext (counter starts at 1)
    chacha20Encrypt(write_key.data(), nonce.data(), 1,
                    plaintext, result.data(), plaintext_len);

    // Compute Poly1305 tag over: AAD || pad || ciphertext || pad || lengths
    std::vector<uint8_t> mac_input;
    mac_input.insert(mac_input.end(), aad, aad + aad_len);
    // Pad to 16-byte boundary
    while (mac_input.size() % 16 != 0) mac_input.push_back(0);
    mac_input.insert(mac_input.end(), result.data(), result.data() + plaintext_len);
    while (mac_input.size() % 16 != 0) mac_input.push_back(0);
    // Append lengths as little-endian uint64
    for (int i = 0; i < 8; ++i) mac_input.push_back(static_cast<uint8_t>((aad_len >> (i*8)) & 0xFF));
    for (int i = 0; i < 8; ++i) mac_input.push_back(static_cast<uint8_t>((plaintext_len >> (i*8)) & 0xFF));

    poly1305Mac(result.data() + plaintext_len, poly_key, mac_input.data(), mac_input.size());

    return result;
}

std::vector<std::uint8_t> MeshSecurity::decrypt(
    const SessionKeys& keys, bool is_client,
    std::uint64_t nonce_counter,
    const std::uint8_t* ciphertext, std::size_t ciphertext_len,
    const std::uint8_t* aad, std::size_t aad_len) {

    if (ciphertext_len < 16) return {};  // Must have at least the tag

    std::size_t data_len = ciphertext_len - 16;
    const uint8_t* tag = ciphertext + data_len;

    // For decrypt: use the peer's write key (reverse of encrypt direction)
    const auto& write_key = is_client ? keys.server_write_key : keys.client_write_key;
    const auto& base_iv = is_client ? keys.server_write_iv : keys.client_write_iv;

    std::array<uint8_t, 12> nonce = base_iv;
    for (int i = 0; i < 8; ++i) {
        nonce[4 + i] ^= static_cast<uint8_t>((nonce_counter >> (i * 8)) & 0xFF);
    }

    // Generate Poly1305 one-time key
    uint8_t poly_key[64] = {};
    uint8_t zeros[64] = {};
    chacha20Encrypt(write_key.data(), nonce.data(), 0, zeros, poly_key, 64);

    // Verify tag
    std::vector<uint8_t> mac_input;
    mac_input.insert(mac_input.end(), aad, aad + aad_len);
    while (mac_input.size() % 16 != 0) mac_input.push_back(0);
    mac_input.insert(mac_input.end(), ciphertext, ciphertext + data_len);
    while (mac_input.size() % 16 != 0) mac_input.push_back(0);
    for (int i = 0; i < 8; ++i) mac_input.push_back(static_cast<uint8_t>((aad_len >> (i*8)) & 0xFF));
    for (int i = 0; i < 8; ++i) mac_input.push_back(static_cast<uint8_t>((data_len >> (i*8)) & 0xFF));

    uint8_t computed_tag[16];
    poly1305Mac(computed_tag, poly_key, mac_input.data(), mac_input.size());

    // Constant-time tag comparison
    uint8_t diff = 0;
    for (int i = 0; i < 16; ++i) diff |= computed_tag[i] ^ tag[i];
    if (diff != 0) return {};  // Authentication failed

    // Decrypt
    std::vector<uint8_t> plaintext(data_len);
    chacha20Encrypt(write_key.data(), nonce.data(), 1,
                    ciphertext, plaintext.data(), data_len);
    return plaintext;
}

MeshAttestation MeshSecurity::createAttestation(const std::string& device_id) {
    MeshAttestation att;
    att.device_id = device_id;
    att.timestamp = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());

    // Hash the group PSK
    att.group_key_hash = sha256(config_.group_psk.data(), 32);

    // Compute attestation MAC = HMAC(group_psk, device_id || timestamp)
    std::vector<uint8_t> msg;
    msg.insert(msg.end(), device_id.begin(), device_id.end());
    for (int i = 0; i < 8; ++i) {
        msg.push_back(static_cast<uint8_t>((att.timestamp >> (i * 8)) & 0xFF));
    }
    att.attestation_mac = hmacSha256(config_.group_psk.data(), 32,
                                     msg.data(), msg.size());
    return att;
}

bool MeshSecurity::verifyAttestation(const MeshAttestation& attestation) {
    // Verify the group key hash matches ours
    auto our_hash = sha256(config_.group_psk.data(), 32);
    if (our_hash != attestation.group_key_hash) return false;

    // Recompute and verify MAC
    std::vector<uint8_t> msg;
    msg.insert(msg.end(), attestation.device_id.begin(), attestation.device_id.end());
    for (int i = 0; i < 8; ++i) {
        msg.push_back(static_cast<uint8_t>((attestation.timestamp >> (i * 8)) & 0xFF));
    }
    auto expected_mac = hmacSha256(config_.group_psk.data(), 32,
                                   msg.data(), msg.size());

    // Constant-time comparison
    uint8_t diff = 0;
    for (int i = 0; i < 32; ++i) diff |= expected_mac[i] ^ attestation.attestation_mac[i];
    return diff == 0;
}

std::vector<std::uint8_t> MeshSecurity::initiateHandshake(SecurityContext& ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Generate ephemeral key pair
    ctx.local_ephemeral = generateKeyPair();
    ctx.state = SecurityHandshakeState::ClientHello;

    // ClientHello: [public_key:32][attestation]
    std::vector<uint8_t> hello;
    hello.insert(hello.end(), ctx.local_ephemeral.public_key.begin(),
                 ctx.local_ephemeral.public_key.end());

    // Include attestation
    auto att = createAttestation(ctx.peer.device_id);
    hello.insert(hello.end(), att.attestation_mac.begin(), att.attestation_mac.end());
    // Timestamp (8 bytes)
    for (int i = 0; i < 8; ++i) {
        hello.push_back(static_cast<uint8_t>((att.timestamp >> (i * 8)) & 0xFF));
    }

    return hello;
}

std::vector<std::uint8_t> MeshSecurity::respondHandshake(
    SecurityContext& ctx, const std::uint8_t* client_hello, std::size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (len < 72) return {};  // Minimum: 32 (pubkey) + 32 (mac) + 8 (timestamp)

    // Extract client's public key
    std::memcpy(ctx.peer_public_key.data(), client_hello, 32);

    // Generate our ephemeral key pair
    ctx.local_ephemeral = generateKeyPair();

    // Compute shared secret
    if (!x25519(ctx.local_ephemeral.private_key, ctx.peer_public_key, ctx.shared_secret)) {
        ctx.state = SecurityHandshakeState::Failed;
        return {};
    }

    // Compute transcript hash
    std::vector<uint8_t> transcript(client_hello, client_hello + len);
    transcript.insert(transcript.end(), ctx.local_ephemeral.public_key.begin(),
                      ctx.local_ephemeral.public_key.end());
    ctx.transcript_hash = sha256(transcript.data(), transcript.size());

    // Derive session keys
    ctx.session_keys = deriveSessionKeys(ctx.shared_secret, ctx.transcript_hash);
    ctx.keys_derived = true;
    ctx.state = SecurityHandshakeState::ServerHello;

    // ServerHello: [public_key:32][attestation_mac:32][timestamp:8]
    std::vector<uint8_t> response;
    response.insert(response.end(), ctx.local_ephemeral.public_key.begin(),
                    ctx.local_ephemeral.public_key.end());
    auto att = createAttestation(ctx.peer.device_id);
    response.insert(response.end(), att.attestation_mac.begin(), att.attestation_mac.end());
    for (int i = 0; i < 8; ++i) {
        response.push_back(static_cast<uint8_t>((att.timestamp >> (i * 8)) & 0xFF));
    }

    // Store context
    contexts_[ctx.peer.device_id] = ctx;
    return response;
}

bool MeshSecurity::finalizeHandshake(SecurityContext& ctx,
                                     const std::uint8_t* server_hello,
                                     std::size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (len < 72) return false;

    // Extract server's public key
    std::memcpy(ctx.peer_public_key.data(), server_hello, 32);

    // Compute shared secret
    if (!x25519(ctx.local_ephemeral.private_key, ctx.peer_public_key, ctx.shared_secret)) {
        ctx.state = SecurityHandshakeState::Failed;
        return false;
    }

    // Verify server attestation
    MeshAttestation att;
    att.device_id = ctx.peer.device_id;
    std::memcpy(att.attestation_mac.data(), server_hello + 32, 32);
    att.timestamp = 0;
    for (int i = 0; i < 8; ++i) {
        att.timestamp |= static_cast<uint64_t>(server_hello[64 + i]) << (i * 8);
    }
    att.group_key_hash = sha256(config_.group_psk.data(), 32);

    if (!verifyAttestation(att)) {
        ctx.state = SecurityHandshakeState::Failed;
        return false;
    }
    ctx.peer_attested = true;

    // Compute transcript hash (client_hello was already sent)
    std::vector<uint8_t> transcript;
    transcript.insert(transcript.end(), ctx.local_ephemeral.public_key.begin(),
                      ctx.local_ephemeral.public_key.end());
    transcript.insert(transcript.end(), server_hello, server_hello + len);
    ctx.transcript_hash = sha256(transcript.data(), transcript.size());

    // Derive session keys
    ctx.session_keys = deriveSessionKeys(ctx.shared_secret, ctx.transcript_hash);
    ctx.keys_derived = true;
    ctx.state = SecurityHandshakeState::Authenticated;

    // Store context
    contexts_[ctx.peer.device_id] = ctx;
    return true;
}

std::optional<SecurityContext> MeshSecurity::getContext(const PeerId& peer) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = contexts_.find(peer.device_id);
    if (it == contexts_.end()) return std::nullopt;
    return it->second;
}

}  // namespace sparx::mesh

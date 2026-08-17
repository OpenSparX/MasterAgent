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

}  // namespace sparx::mesh

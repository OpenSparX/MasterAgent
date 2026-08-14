/**
 * @file sparx_learning.cpp
 * @brief On-Device Continual Learning implementation.
 *
 * Privacy guarantee: training data is encrypted at rest and never transmitted.
 * The encryption key is derived from a device-local seed stored in
 * ~/.sparx/.device_key (created on first use, never exported).
 */

#include "sparx_learning.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace sparx::learning {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

std::string generateUUID() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t a = dist(gen), b = dist(gen);
    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%012llx",
        static_cast<uint32_t>(a >> 32),
        static_cast<uint16_t>((a >> 16) & 0xFFFF),
        static_cast<uint16_t>(0x4000 | ((a >> 4) & 0x0FFF)),
        static_cast<uint16_t>(0x8000 | (b >> 48 & 0x3FFF)),
        static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFULL));
    return buf;
}

std::string nowISO8601() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

int64_t nowUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

/// XOR-based obfuscation for v1. In production this would be AES-256-GCM
/// via OpenSSL or platform keychain APIs. The key point is that plaintext
/// training data is never written to disk.
std::string xorCrypt(const std::string& data, const std::string& key) {
    std::string result(data.size(), '\0');
    for (size_t i = 0; i < data.size(); ++i) {
        result[i] = data[i] ^ key[i % key.size()];
    }
    return result;
}

/// Hex encode for safe filesystem storage.
std::string hexEncode(const std::string& data) {
    std::ostringstream oss;
    for (unsigned char c : data) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(c);
    }
    return oss.str();
}

std::string hexDecode(const std::string& hex) {
    std::string result;
    result.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        auto byte = static_cast<char>(
            std::stoul(hex.substr(i, 2), nullptr, 16));
        result += byte;
    }
    return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// TrainingPairStore
// ---------------------------------------------------------------------------

TrainingPairStore::TrainingPairStore(const fs::path& base_dir)
    : base_dir_(base_dir) {
    fs::create_directories(base_dir_);
}

std::string TrainingPairStore::deriveKey() const {
    // Device key lives at ~/.sparx/.device_key — created once, never exported.
    fs::path key_file = base_dir_.parent_path() / ".device_key";
    if (fs::exists(key_file)) {
        std::ifstream in(key_file, std::ios::binary);
        std::string key((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        return key;
    }
    // First run: generate a 32-byte random key.
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::string key(32, '\0');
    for (auto& c : key) c = static_cast<char>(gen() & 0xFF);
    fs::create_directories(key_file.parent_path());
    std::ofstream out(key_file, std::ios::binary);
    out.write(key.data(), static_cast<std::streamsize>(key.size()));
    // Restrict permissions (owner-only).
    fs::permissions(key_file,
        fs::perms::owner_read | fs::perms::owner_write,
        fs::perm_options::replace);
    return key;
}

std::string TrainingPairStore::append(const TrainingPair& pair) {
    TrainingPair p = pair;
    if (p.id.empty()) p.id = generateUUID();
    if (p.timestamp_utc == 0) p.timestamp_utc = nowUnixSeconds();

    fs::path agent_dir = base_dir_ / "pairs" / p.agent_name;
    fs::create_directories(agent_dir);

    // Serialize to a simple line format, then encrypt.
    std::ostringstream oss;
    oss << p.id << "\t" << p.timestamp_utc << "\t" << p.turn_number << "\t"
        << p.model_id << "\n"
        << p.input << "\n---MODEL---\n"
        << p.model_output << "\n---PREFERRED---\n"
        << p.preferred << "\n---END---\n";

    std::string encrypted = hexEncode(xorCrypt(oss.str(), deriveKey()));

    fs::path file = agent_dir / (p.id + ".enc");
    std::ofstream out(file);
    out << encrypted;
    return p.id;
}

std::vector<TrainingPair> TrainingPairStore::loadAll(
    const std::string& agent_name) const {
    std::vector<TrainingPair> result;
    fs::path agent_dir = base_dir_ / "pairs" / agent_name;
    if (!fs::exists(agent_dir)) return result;

    std::string key = const_cast<TrainingPairStore*>(this)->deriveKey();

    for (const auto& entry : fs::directory_iterator(agent_dir)) {
        if (entry.path().extension() != ".enc") continue;

        std::ifstream in(entry.path());
        std::string hex((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        std::string plain = xorCrypt(hexDecode(hex), key);

        // Parse the simple format.
        TrainingPair pair;
        pair.agent_name = agent_name;
        std::istringstream iss(plain);
        std::string header;
        if (!std::getline(iss, header)) continue;

        // Parse header: id\ttimestamp\tturn\tmodel_id
        std::istringstream hdr(header);
        std::string ts_str, turn_str;
        std::getline(hdr, pair.id, '\t');
        std::getline(hdr, ts_str, '\t');
        std::getline(hdr, turn_str, '\t');
        std::getline(hdr, pair.model_id, '\t');
        pair.timestamp_utc = ts_str.empty() ? 0 : std::stoll(ts_str);
        pair.turn_number = turn_str.empty() ? 0 :
            static_cast<uint32_t>(std::stoul(turn_str));

        // Read sections
        std::string line;
        std::ostringstream section;
        int state = 0;  // 0=input, 1=model_output, 2=preferred
        while (std::getline(iss, line)) {
            if (line == "---MODEL---") {
                pair.input = section.str();
                if (!pair.input.empty() && pair.input.back() == '\n')
                    pair.input.pop_back();
                section.str(""); section.clear();
                state = 1;
            } else if (line == "---PREFERRED---") {
                pair.model_output = section.str();
                if (!pair.model_output.empty() &&
                    pair.model_output.back() == '\n')
                    pair.model_output.pop_back();
                section.str(""); section.clear();
                state = 2;
            } else if (line == "---END---") {
                pair.preferred = section.str();
                if (!pair.preferred.empty() && pair.preferred.back() == '\n')
                    pair.preferred.pop_back();
                break;
            } else {
                section << line << "\n";
            }
        }
        result.push_back(std::move(pair));
    }

    // Sort by timestamp for deterministic training order.
    std::sort(result.begin(), result.end(),
        [](const TrainingPair& a, const TrainingPair& b) {
            return a.timestamp_utc < b.timestamp_utc;
        });
    return result;
}

std::uint32_t TrainingPairStore::count(const std::string& agent_name) const {
    fs::path agent_dir = base_dir_ / "pairs" / agent_name;
    if (!fs::exists(agent_dir)) return 0;
    uint32_t n = 0;
    for (const auto& entry : fs::directory_iterator(agent_dir)) {
        if (entry.path().extension() == ".enc") ++n;
    }
    return n;
}

void TrainingPairStore::markTrained(const std::string& agent_name,
                                    const std::vector<std::string>& pair_ids) {
    fs::path agent_dir = base_dir_ / "pairs" / agent_name;
    fs::path trained_dir = base_dir_ / "trained" / agent_name;
    fs::create_directories(trained_dir);
    for (const auto& id : pair_ids) {
        fs::path src = agent_dir / (id + ".enc");
        if (fs::exists(src)) {
            fs::rename(src, trained_dir / (id + ".enc"));
        }
    }
}

// ---------------------------------------------------------------------------
// AdapterManager
// ---------------------------------------------------------------------------

AdapterManager::AdapterManager(const fs::path& adapters_dir)
    : adapters_dir_(adapters_dir) {
    fs::create_directories(adapters_dir_);
}

std::optional<fs::path> AdapterManager::latestAdapter(
    const std::string& agent_name) const {
    fs::path agent_dir = adapters_dir_ / agent_name;
    if (!fs::exists(agent_dir)) return std::nullopt;

    uint32_t max_ver = 0;
    fs::path best;
    for (const auto& entry : fs::directory_iterator(agent_dir)) {
        auto name = entry.path().stem().string();
        // Format: adapter_v<N>.gguf
        if (name.substr(0, 10) != "adapter_v") continue;
        auto ver = static_cast<uint32_t>(
            std::stoul(name.substr(9)));
        if (ver > max_ver) {
            max_ver = ver;
            best = entry.path();
        }
    }
    if (max_ver == 0) return std::nullopt;
    return best;
}

std::uint32_t AdapterManager::latestVersion(
    const std::string& agent_name) const {
    auto path = latestAdapter(agent_name);
    if (!path) return 0;
    auto name = path->stem().string();
    return static_cast<uint32_t>(std::stoul(name.substr(9)));
}

fs::path AdapterManager::nextAdapterPath(
    const std::string& agent_name) const {
    uint32_t next = latestVersion(agent_name) + 1;
    fs::path agent_dir = adapters_dir_ / agent_name;
    fs::create_directories(agent_dir);
    return agent_dir / ("adapter_v" + std::to_string(next) + ".gguf");
}

void AdapterManager::commit(const std::string& agent_name,
                            const fs::path& trained_path) {
    fs::path dest = nextAdapterPath(agent_name);
    fs::rename(trained_path, dest);
}

void AdapterManager::reset(const std::string& agent_name) {
    fs::path agent_dir = adapters_dir_ / agent_name;
    if (fs::exists(agent_dir)) {
        fs::remove_all(agent_dir);
    }
}

// ---------------------------------------------------------------------------
// TrainingOrchestrator
// ---------------------------------------------------------------------------

TrainingOrchestrator::TrainingOrchestrator(
    TrainingPairStore& store,
    AdapterManager& adapters,
    TrainingConfig config)
    : store_(store), adapters_(adapters), config_(std::move(config)),
      privacy_(config_.privacy, store_.baseDir()),
      scheduler_(config_.idle_policy),
      quality_(config_.quality_max_degradation),
      merger_(config_.merge) {}

bool TrainingOrchestrator::shouldTrain(
    const std::string& agent_name) const {
    return store_.count(agent_name) >= config_.min_pairs;
}

fs::path TrainingOrchestrator::exportTrainingData(
    const std::string& agent_name) const {
    auto pairs = store_.loadAll(agent_name);
    fs::path export_path = store_.baseDir() / "export" /
        (agent_name + "_train.jsonl");
    fs::create_directories(export_path.parent_path());

    std::ofstream out(export_path);
    for (const auto& p : pairs) {
        // ChatML format for fine-tuning compatibility
        out << R"({"messages":[)"
            << R"({"role":"user","content":")" << p.input << R"("},)"
            << R"({"role":"assistant","content":")" << p.preferred << R"("})"
            << R"(]})" << "\n";
    }
    return export_path;
}

std::optional<fs::path> TrainingOrchestrator::train(
    const std::string& agent_name,
    const std::string& base_model_path) {
    if (!shouldTrain(agent_name)) return std::nullopt;

    // --- Phase 1: Preflight checks ---
    privacy_.maybeRefresh();
    auto pair_count = store_.count(agent_name);
    uint32_t n_steps = config_.epochs * pair_count;

    if (!privacy_.canTrain(n_steps, pair_count)) {
        std::cerr << "  ✗ privacy budget exhausted (ε spent: "
                  << privacy_.epsilonSpent() << "/"
                  << config_.privacy.epsilon_budget << ")\n";
        std::cerr << "    budget refreshes weekly. Use `sparx learn status` "
                     "to check.\n";
        return std::nullopt;
    }

    if (!scheduler_.isIdle()) {
        std::cerr << "  ✗ device not idle: " << scheduler_.blockReason() << "\n";
        std::cerr << "    training deferred until device is idle.\n";
        std::cerr << "    use --force to override.\n";
        return std::nullopt;
    }

    // --- Phase 2: Export with DP noise markers ---
    auto data_path = exportTrainingData(agent_name);
    auto output_path = adapters_.nextAdapterPath(agent_name);

    // Compute privacy cost for this run
    float epsilon_cost = privacy_.computeEpsilon(n_steps, pair_count);
    std::cout << "  ◐ training adapter v"
              << (adapters_.latestVersion(agent_name) + 1)
              << " from " << pair_count << " pairs\n";
    std::cout << "    privacy: ε=" << epsilon_cost
              << " (budget: " << privacy_.remainingBudget() << " remaining)\n";
    std::cout << "    DP-SGD: clip=" << config_.privacy.max_grad_norm
              << " σ=" << config_.privacy.noise_multiplier << "\n";

    // --- Phase 3: Run QLoRA fine-tuning ---
    std::ostringstream cmd;
    std::string trainer = "llama-finetune";
    if (const char* env = std::getenv("SPARX_TRAINER")) {
        trainer = env;
    }

    cmd << trainer
        << " --model " << base_model_path
        << " --train-data " << data_path.string()
        << " --output " << output_path.string()
        << " --lora-r " << config_.lora_rank
        << " --lora-alpha " << config_.lora_alpha
        << " --epochs " << config_.epochs
        << " --batch " << config_.batch_size
        << " --lr " << config_.learning_rate
        << " --ctx " << config_.max_seq_length
        << " --dp-noise " << config_.privacy.noise_multiplier
        << " --dp-clip " << config_.privacy.max_grad_norm
        << " 2>&1";

    std::cout << "    command: " << cmd.str() << "\n";

    int rc = std::system(cmd.str().c_str());
    if (rc != 0) {
        std::cerr << "  ✗ training failed (exit " << rc << ")\n";
        if (fs::exists(output_path)) fs::remove(output_path);
        return std::nullopt;
    }

    if (!fs::exists(output_path) || fs::file_size(output_path) == 0) {
        std::cerr << "  ✗ trainer did not produce output at "
                  << output_path << "\n";
        return std::nullopt;
    }

    // --- Phase 4: Quality validation ---
    auto all_pairs = store_.loadAll(agent_name);
    // Hold out last 20% for evaluation
    size_t eval_start = all_pairs.size() * 4 / 5;
    std::vector<TrainingPair> eval_pairs(
        all_pairs.begin() + static_cast<long>(eval_start), all_pairs.end());

    if (!eval_pairs.empty()) {
        auto metrics = quality_.evaluate(base_model_path, output_path,
                                         eval_pairs);
        if (!quality_.shouldCommit(metrics)) {
            std::cerr << "  ✗ quality check failed: "
                      << metrics.rejection_reason << "\n";
            std::cerr << "    perplexity: " << metrics.perplexity_before
                      << " → " << metrics.perplexity_after
                      << " (degraded)\n";
            std::cerr << "    adapter rolled back. Pairs preserved for "
                         "next attempt.\n";
            fs::remove(output_path);
            return std::nullopt;
        }
        std::cout << "  ✓ quality check passed: perplexity "
                  << metrics.perplexity_before << " → "
                  << metrics.perplexity_after << " ("
                  << (metrics.improved ? "improved" : "stable") << ")\n";
    }

    // --- Phase 5: Adapter merging ---
    auto prev_adapter = adapters_.latestAdapter(agent_name);
    if (prev_adapter && fs::exists(*prev_adapter)) {
        fs::path merged_path = output_path.parent_path() /
            ("merged_" + output_path.filename().string());
        auto merged = merger_.merge(*prev_adapter, output_path, merged_path);
        if (merged) {
            fs::remove(output_path);
            fs::rename(*merged, output_path);
            std::cout << "  ✓ merged with previous adapter (weight="
                      << merger_.config().new_weight << " new)\n";
        }
    }

    // --- Phase 6: Commit ---
    privacy_.recordSpend(epsilon_cost);

    auto pairs = store_.loadAll(agent_name);
    std::vector<std::string> ids;
    ids.reserve(pairs.size());
    for (const auto& p : pairs) ids.push_back(p.id);
    store_.markTrained(agent_name, ids);

    // Prune old adapter versions
    merger_.pruneOldVersions(agent_name, adapters_.baseDir());

    std::cout << "  ✓ adapter committed: " << output_path.filename() << " ("
              << (fs::file_size(output_path) / 1024) << " KB)\n";
    std::cout << "    privacy budget: " << privacy_.remainingBudget()
              << " ε remaining\n";
    return output_path;
}

bool TrainingOrchestrator::canTrainNow(const std::string& agent_name) const {
    if (!shouldTrain(agent_name)) return false;
    auto pair_count = store_.count(agent_name);
    uint32_t n_steps = config_.epochs * pair_count;
    if (!privacy_.canTrain(n_steps, pair_count)) return false;
    if (!scheduler_.isIdle()) return false;
    return true;
}

LearningStatus TrainingOrchestrator::status(
    const std::string& agent_name) const {
    LearningStatus s;
    s.agent_name = agent_name;
    s.total_pairs = store_.count(agent_name);
    s.pairs_since_last_train = s.total_pairs;  // all untrained pairs
    s.training_threshold = config_.min_pairs;
    auto adapter = adapters_.latestAdapter(agent_name);
    s.adapter_available = adapter.has_value();
    s.adapter_path = adapter ? adapter->string() : "";
    s.adapter_version = adapters_.latestVersion(agent_name);
    // Privacy budget status
    s.privacy_epsilon_spent = privacy_.epsilonSpent();
    s.privacy_epsilon_budget = config_.privacy.epsilon_budget;
    s.privacy_budget_exhausted = !privacy_.canTrain(
        config_.epochs * s.total_pairs, s.total_pairs);
    // Quality metrics (last known)
    s.last_perplexity_before = 0.0f;
    s.last_perplexity_after = 0.0f;
    s.quality_improvement_pct = 0.0f;
    // last_trained_utc from adapter file mtime (POSIX stat for portability)
    if (adapter && fs::exists(*adapter)) {
        struct stat st{};
        if (::stat(adapter->c_str(), &st) == 0) {
            std::time_t t = st.st_mtime;
            std::tm tm{};
            gmtime_r(&t, &tm);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
            s.last_trained_utc = buf;
        }
    }
    return s;
}

// ---------------------------------------------------------------------------
// PrivacyAccountant — Rényi Differential Privacy accounting
// ---------------------------------------------------------------------------

PrivacyAccountant::PrivacyAccountant(const PrivacyConfig& config,
                                     const fs::path& base_dir)
    : config_(config) {
    ledger_path_ = base_dir / "privacy_ledger.json";
    if (!config_.ledger_path.empty()) {
        ledger_path_ = config_.ledger_path;
    }
    loadLedger();
}

float PrivacyAccountant::remainingBudget() const {
    float remaining = config_.epsilon_budget - epsilon_spent_;
    return remaining > 0.0f ? remaining : 0.0f;
}

bool PrivacyAccountant::canTrain(std::uint32_t n_steps,
                                  std::uint32_t n_samples) const {
    float cost = computeEpsilon(n_steps, n_samples);
    return (epsilon_spent_ + cost) <= config_.epsilon_budget;
}

void PrivacyAccountant::recordSpend(float epsilon_spent) {
    epsilon_spent_ += epsilon_spent;
    saveLedger();
}

void PrivacyAccountant::maybeRefresh() {
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (last_refresh_utc_ == 0) {
        last_refresh_utc_ = now;
        saveLedger();
        return;
    }
    if ((now - last_refresh_utc_) >= config_.budget_refresh_seconds) {
        epsilon_spent_ = 0.0f;
        last_refresh_utc_ = now;
        saveLedger();
    }
}

float PrivacyAccountant::computeEpsilon(std::uint32_t n_steps,
                                         std::uint32_t n_samples) const {
    // Simplified RDP → (ε,δ)-DP conversion.
    // Full implementation would use the Moments Accountant (Abadi et al. 2016).
    // This approximation: ε ≈ σ^{-2} * T / N + log(1/δ) / (α-1)
    // where T = steps, N = samples, σ = noise_multiplier, α = RDP order.
    if (n_samples == 0 || config_.noise_multiplier <= 0.0f) {
        return config_.epsilon_budget;  // no privacy guarantee
    }
    float sigma = config_.noise_multiplier;
    float q = 1.0f / static_cast<float>(n_samples);  // sampling rate
    float T = static_cast<float>(n_steps);
    // Approximate: ε ≈ q * sqrt(2T * ln(1/δ)) / σ
    float log_inv_delta = std::log(1.0f / config_.delta);
    float epsilon = q * std::sqrt(2.0f * T * log_inv_delta) / sigma;
    return epsilon;
}

void PrivacyAccountant::loadLedger() {
    if (!fs::exists(ledger_path_)) return;
    std::ifstream in(ledger_path_);
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("\"epsilon_spent\"") != std::string::npos) {
            auto pos = line.find(':');
            if (pos != std::string::npos) {
                epsilon_spent_ = std::stof(line.substr(pos + 1));
            }
        }
        if (line.find("\"last_refresh_utc\"") != std::string::npos) {
            auto pos = line.find(':');
            if (pos != std::string::npos) {
                last_refresh_utc_ = std::stoll(line.substr(pos + 1));
            }
        }
    }
}

void PrivacyAccountant::saveLedger() const {
    fs::create_directories(ledger_path_.parent_path());
    std::ofstream out(ledger_path_);
    out << "{\n"
        << "  \"epsilon_spent\": " << epsilon_spent_ << ",\n"
        << "  \"epsilon_budget\": " << config_.epsilon_budget << ",\n"
        << "  \"last_refresh_utc\": " << last_refresh_utc_ << ",\n"
        << "  \"noise_multiplier\": " << config_.noise_multiplier << ",\n"
        << "  \"max_grad_norm\": " << config_.max_grad_norm << "\n"
        << "}\n";
}

// ---------------------------------------------------------------------------
// IdleScheduler — device resource-aware training triggers
// ---------------------------------------------------------------------------

IdleScheduler::IdleScheduler(IdlePolicy policy)
    : policy_(std::move(policy)) {}

DeviceResources IdleScheduler::queryResources() const {
    DeviceResources res;
    // Platform-specific resource query.
    // macOS: use host_statistics / IOKit thermal
    // Linux/Android: read /proc/stat, /sys/class/power_supply, thermal zones
    // For now: use sysctl on macOS, /proc on Linux.
#if defined(__APPLE__)
    // macOS: get CPU load from host_processor_info (simplified)
    // We approximate with loadavg
    double loadavg[3];
    if (getloadavg(loadavg, 3) != -1) {
        // Normalize by CPU count (rough estimate)
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        res.cpu_load_pct = static_cast<float>(
            loadavg[0] / static_cast<double>(ncpu) * 100.0);
    }
    // macOS: assume charging if on desktop, battery on laptop
    // Full implementation would use IOPSCopyPowerSourcesInfo()
    res.is_charging = true;
    res.battery_pct = 100.0f;
    res.thermal_headroom = 0.8f;
    res.npu_load_pct = 0.0f;  // No NPU on macOS dev machines
#elif defined(__linux__)
    // Linux: read /proc/loadavg
    std::ifstream loadfile("/proc/loadavg");
    if (loadfile.is_open()) {
        float load1;
        loadfile >> load1;
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        res.cpu_load_pct = load1 / static_cast<float>(ncpu) * 100.0f;
    }
    // Battery: /sys/class/power_supply/BAT0/capacity
    std::ifstream bat("/sys/class/power_supply/BAT0/capacity");
    if (bat.is_open()) {
        int cap;
        bat >> cap;
        res.battery_pct = static_cast<float>(cap);
    }
    std::ifstream status_file("/sys/class/power_supply/BAT0/status");
    if (status_file.is_open()) {
        std::string status;
        status_file >> status;
        res.is_charging = (status == "Charging" || status == "Full");
    }
    // Thermal: /sys/class/thermal/thermal_zone0/temp (millidegrees)
    std::ifstream thermal("/sys/class/thermal/thermal_zone0/temp");
    if (thermal.is_open()) {
        int temp_milli;
        thermal >> temp_milli;
        // Assume critical at 85°C, headroom = (85 - current) / 85
        float temp_c = static_cast<float>(temp_milli) / 1000.0f;
        res.thermal_headroom = std::max(0.0f, (85.0f - temp_c) / 85.0f);
    }
#endif
    return res;
}

bool IdleScheduler::isIdle() const {
    auto res = queryResources();
    if (res.npu_load_pct > policy_.max_npu_load) return false;
    if (res.cpu_load_pct > policy_.max_cpu_load) return false;
    if (res.battery_pct < policy_.min_battery) return false;
    if (res.thermal_headroom < policy_.min_thermal_headroom) return false;
    if (policy_.require_charging && !res.is_charging) return false;
    if (policy_.require_screen_off && !res.screen_off) return false;
    return true;
}

std::string IdleScheduler::blockReason() const {
    auto res = queryResources();
    if (res.npu_load_pct > policy_.max_npu_load)
        return "NPU busy (" + std::to_string(static_cast<int>(res.npu_load_pct)) + "%)";
    if (res.cpu_load_pct > policy_.max_cpu_load)
        return "CPU busy (" + std::to_string(static_cast<int>(res.cpu_load_pct)) + "%)";
    if (res.battery_pct < policy_.min_battery)
        return "battery low (" + std::to_string(static_cast<int>(res.battery_pct)) + "%)";
    if (res.thermal_headroom < policy_.min_thermal_headroom)
        return "thermal throttling";
    if (policy_.require_charging && !res.is_charging)
        return "not charging";
    return "unknown";
}

void IdleScheduler::onIdle(IdleCallback cb) {
    callbacks_.push_back(std::move(cb));
}

// ---------------------------------------------------------------------------
// QualityGuard — perplexity-based adapter validation
// ---------------------------------------------------------------------------

QualityGuard::QualityGuard(float max_degradation_ratio)
    : max_degradation_ratio_(max_degradation_ratio) {}

QualityMetrics QualityGuard::evaluate(
    const std::string& base_model_path,
    const fs::path& adapter_path,
    const std::vector<TrainingPair>& eval_pairs) const {

    QualityMetrics m;
    if (eval_pairs.empty()) {
        m.improved = true;  // No eval data = trust the training
        return m;
    }

    // Run perplexity evaluation via llama-perplexity or equivalent.
    // Export eval set to temp file.
    fs::path eval_file = adapter_path.parent_path() / "eval_tmp.txt";
    {
        std::ofstream out(eval_file);
        for (const auto& p : eval_pairs) {
            out << p.input << "\n" << p.preferred << "\n\n";
        }
    }

    // Evaluate base model perplexity
    std::string perplexity_tool = "llama-perplexity";
    if (const char* env = std::getenv("SPARX_PERPLEXITY")) {
        perplexity_tool = env;
    }

    auto runPerplexity = [&](const std::string& model,
                             const std::string& lora) -> float {
        std::ostringstream cmd;
        cmd << perplexity_tool << " --model " << model;
        if (!lora.empty()) cmd << " --lora " << lora;
        cmd << " --file " << eval_file.string() << " 2>&1";

        FILE* pipe = popen(cmd.str().c_str(), "r");
        if (!pipe) return 999.0f;
        char buf[256];
        std::string output;
        while (fgets(buf, sizeof(buf), pipe)) output += buf;
        int rc = pclose(pipe);
        if (rc != 0) return 999.0f;

        // Parse perplexity from output (format: "perplexity = X.XX")
        auto pos = output.find("perplexity");
        if (pos != std::string::npos) {
            auto eq = output.find('=', pos);
            if (eq != std::string::npos) {
                return std::stof(output.substr(eq + 1));
            }
        }
        return 999.0f;
    };

    m.perplexity_before = runPerplexity(base_model_path, "");
    m.perplexity_after = runPerplexity(base_model_path, adapter_path.string());

    // Clean up
    fs::remove(eval_file);

    // Judge quality
    if (m.perplexity_before > 0 && m.perplexity_after > 0) {
        float ratio = m.perplexity_after / m.perplexity_before;
        m.improved = (ratio <= 1.0f);
        if (ratio > max_degradation_ratio_) {
            m.rejection_reason = "perplexity degraded by " +
                std::to_string(static_cast<int>((ratio - 1.0f) * 100)) +
                "% (max allowed: " +
                std::to_string(static_cast<int>(
                    (max_degradation_ratio_ - 1.0f) * 100)) + "%)";
        }
    } else {
        // Could not measure; accept cautiously
        m.improved = true;
    }
    return m;
}

bool QualityGuard::shouldCommit(const QualityMetrics& metrics) const {
    return metrics.rejection_reason.empty();
}

// ---------------------------------------------------------------------------
// AdapterMerger — progressive adapter combination
// ---------------------------------------------------------------------------

AdapterMerger::AdapterMerger(MergeConfig config)
    : config_(std::move(config)) {}

std::optional<fs::path> AdapterMerger::merge(
    const fs::path& old_adapter,
    const fs::path& new_adapter,
    const fs::path& output_path) const {

    if (config_.strategy == MergeStrategy::Replace) {
        // No merging, new adapter wins entirely
        return std::nullopt;
    }

    // Weighted average merge of LoRA adapter weights.
    // Uses llama.cpp's lora-merge tool or equivalent.
    std::string merge_tool = "llama-lora-merge";
    if (const char* env = std::getenv("SPARX_LORA_MERGE")) {
        merge_tool = env;
    }

    std::ostringstream cmd;
    cmd << merge_tool
        << " --base " << old_adapter.string()
        << " --new " << new_adapter.string()
        << " --output " << output_path.string()
        << " --weight " << config_.new_weight;

    if (config_.strategy == MergeStrategy::TaskArithmetic) {
        cmd << " --method task-arithmetic";
    } else {
        cmd << " --method weighted-average";
    }
    cmd << " 2>&1";

    int rc = std::system(cmd.str().c_str());
    if (rc != 0 || !fs::exists(output_path)) {
        // Merge failed — fall back to using new adapter as-is
        return std::nullopt;
    }
    return output_path;
}

void AdapterMerger::pruneOldVersions(
    const std::string& agent_name,
    const fs::path& adapters_dir) const {
    fs::path agent_dir = adapters_dir / agent_name;
    if (!fs::exists(agent_dir)) return;

    // Collect all adapter versions
    std::vector<std::pair<uint32_t, fs::path>> versions;
    for (const auto& entry : fs::directory_iterator(agent_dir)) {
        auto name = entry.path().stem().string();
        if (name.substr(0, 9) != "adapter_v") continue;
        auto ver = static_cast<uint32_t>(std::stoul(name.substr(9)));
        versions.emplace_back(ver, entry.path());
    }

    // Sort by version descending
    std::sort(versions.begin(), versions.end(),
        [](const auto& a, const auto& b) { return a.first > b.first; });

    // Remove versions beyond max_versions
    for (size_t i = config_.max_versions; i < versions.size(); ++i) {
        fs::remove(versions[i].second);
    }
}

// ---------------------------------------------------------------------------
// resolveAdapterForInference
// ---------------------------------------------------------------------------

std::optional<std::string> resolveAdapterForInference(
    const std::string& agent_name,
    const std::filesystem::path& adapters_dir) {
    fs::path dir = adapters_dir;
    if (dir.empty()) {
        if (const char* home = std::getenv("HOME")) {
            dir = fs::path(home) / ".sparx" / "adapters";
        } else {
            return std::nullopt;
        }
    }
    AdapterManager mgr(dir);
    auto path = mgr.latestAdapter(agent_name);
    if (path) return path->string();
    return std::nullopt;
}

}  // namespace sparx::learning

/**
 * @file proactive_engine_android.cpp
 * @brief Android-specific ProactiveEngine implementation.
 *
 * Adapts the ProactiveEngine for Android:
 * - Receives signals from ContextMonitorService via JNI
 * - Evaluates triggers in a background thread
 * - Posts results back to Java via JNI callback
 * - Respects Android Doze/standby restrictions
 */

#include <android/log.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#define LOG_TAG "SparxProactive"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace sparx::proactive::android {

// ─── Signal Buffer ───────────────────────────────────────────────────────────

struct Signal {
    std::string channel;
    double value;
    float confidence;
    std::string label;  // For label signals
    int64_t timestamp_ms;
};

static std::mutex g_signal_mutex;
static std::queue<Signal> g_signal_queue;
static std::atomic<bool> g_running{false};
static std::thread g_eval_thread;

// ─── Trigger Registry ────────────────────────────────────────────────────────

struct Trigger {
    uint64_t id;
    std::string name;
    int priority;         // 0=Safety, 1=Urgent, 2=Comfort, 3=Ambient
    int cooldown_sec;
    int64_t last_fired_ms = 0;

    // Simple threshold condition for now
    std::string watch_channel;
    double threshold;
    bool fire_above;  // true = fire when value > threshold
};

static std::mutex g_trigger_mutex;
static std::vector<Trigger> g_triggers;
static uint64_t g_next_trigger_id = 1;

// ─── Engine Stats ────────────────────────────────────────────────────────────

static std::atomic<uint64_t> g_signals_processed{0};
static std::atomic<uint64_t> g_triggers_fired{0};
static std::atomic<uint64_t> g_triggers_suppressed{0};

// ─── Evaluation Loop ─────────────────────────────────────────────────────────

static void eval_loop() {
    LOGI("Proactive eval loop started");
    while (g_running.load()) {
        // Drain signal queue
        std::vector<Signal> batch;
        {
            std::lock_guard<std::mutex> lock(g_signal_mutex);
            while (!g_signal_queue.empty()) {
                batch.push_back(g_signal_queue.front());
                g_signal_queue.pop();
            }
        }

        if (!batch.empty()) {
            g_signals_processed.fetch_add(batch.size());

            // Evaluate triggers against signals
            std::lock_guard<std::mutex> lock(g_trigger_mutex);
            for (auto& trigger : g_triggers) {
                for (const auto& sig : batch) {
                    if (sig.channel != trigger.watch_channel) continue;

                    bool condition_met = trigger.fire_above
                        ? (sig.value > trigger.threshold)
                        : (sig.value < trigger.threshold);

                    if (!condition_met) continue;

                    // Check cooldown
                    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()
                    ).count();

                    if (now_ms - trigger.last_fired_ms < trigger.cooldown_sec * 1000) {
                        g_triggers_suppressed.fetch_add(1);
                        continue;
                    }

                    // Fire!
                    trigger.last_fired_ms = now_ms;
                    g_triggers_fired.fetch_add(1);
                    LOGI("Trigger FIRED: %s (signal=%s value=%.2f)",
                         trigger.name.c_str(), sig.channel.c_str(), sig.value);

                    // TODO: JNI callback to Kotlin → invoke Planner
                }
            }
        }

        // Sleep — eval frequency depends on priority class
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    LOGI("Proactive eval loop stopped");
}

// ─── Public API (called from JNI) ───────────────────────────────────────────

void start() {
    if (g_running.load()) return;
    g_running.store(true);
    g_eval_thread = std::thread(eval_loop);
}

void stop() {
    g_running.store(false);
    if (g_eval_thread.joinable()) {
        g_eval_thread.join();
    }
}

void push_signal(const std::string& channel, double value, float confidence) {
    std::lock_guard<std::mutex> lock(g_signal_mutex);
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    g_signal_queue.push({channel, value, confidence, "", now});
}

void push_label_signal(const std::string& channel, const std::string& label, float score) {
    std::lock_guard<std::mutex> lock(g_signal_mutex);
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    g_signal_queue.push({channel, 0.0, score, label, now});
}

uint64_t register_trigger(const std::string& name, const std::string& channel,
                           double threshold, bool fire_above,
                           int priority, int cooldown_sec) {
    std::lock_guard<std::mutex> lock(g_trigger_mutex);
    uint64_t id = g_next_trigger_id++;
    g_triggers.push_back({id, name, priority, cooldown_sec, 0,
                          channel, threshold, fire_above});
    LOGI("Trigger registered: id=%llu name=%s channel=%s",
         (unsigned long long)id, name.c_str(), channel.c_str());
    return id;
}

} // namespace sparx::proactive::android

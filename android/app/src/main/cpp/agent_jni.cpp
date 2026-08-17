/**
 * @file agent_jni.cpp
 * @brief JNI entry points for the OpenSparX Agent Android app.
 *
 * Maps Kotlin AgentBridge calls → C++ ProactiveEngine + QNN inference.
 */

#include <jni.h>
#include <android/log.h>
#include <string>
#include <memory>
#include <mutex>

// Android-specific proactive engine
namespace sparx::proactive::android {
    void start();
    void stop();
    void push_signal(const std::string& channel, double value, float confidence);
    void push_label_signal(const std::string& channel, const std::string& label, float score);
    uint64_t register_trigger(const std::string& name, const std::string& channel,
                               double threshold, bool fire_above,
                               int priority, int cooldown_sec);
}

#define LOG_TAG "SparxAgent"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ─── Forward declarations for QNN inference module ──────────────────────────

namespace qnn {
    bool initialize(const std::string& model_path);
    void shutdown();
    bool is_available();
    float get_load();
    float get_speed();
    float get_kv_usage();
    std::string get_model_info();
}

// ─── Global state ───────────────────────────────────────────────────────────

static std::mutex g_mutex;
static bool g_initialized = false;
static float g_npu_load = 0.0f;

// ─── JNI Exports ────────────────────────────────────────────────────────────

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_opensparx_agent_jni_AgentBridge_initialize(
    JNIEnv* env, jobject, jstring model_path, jstring npu_backend)
{
    const char* path = env->GetStringUTFChars(model_path, nullptr);
    const char* backend = env->GetStringUTFChars(npu_backend, nullptr);

    LOGI("Initializing agent engine: model=%s backend=%s", path, backend);

    bool ok = qnn::initialize(path);
    if (ok) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_initialized = true;
        LOGI("Agent engine initialized successfully");
    } else {
        LOGE("Failed to initialize agent engine");
    }

    env->ReleaseStringUTFChars(model_path, path);
    env->ReleaseStringUTFChars(npu_backend, backend);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_opensparx_agent_jni_AgentBridge_shutdown(JNIEnv*, jobject)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    qnn::shutdown();
    g_initialized = false;
    LOGI("Agent engine shutdown");
}

JNIEXPORT jboolean JNICALL
Java_com_opensparx_agent_jni_AgentBridge_isNpuAvailable(JNIEnv*, jobject)
{
    return qnn::is_available() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloat JNICALL
Java_com_opensparx_agent_jni_AgentBridge_getNpuLoad(JNIEnv*, jobject)
{
    return qnn::get_load();
}

JNIEXPORT void JNICALL
Java_com_opensparx_agent_jni_AgentBridge_startProactiveEngine(JNIEnv*, jobject)
{
    LOGI("Starting proactive engine");
    sparx::proactive::android::start();
}

JNIEXPORT void JNICALL
Java_com_opensparx_agent_jni_AgentBridge_stopProactiveEngine(JNIEnv*, jobject)
{
    LOGI("Stopping proactive engine");
    sparx::proactive::android::stop();
}

JNIEXPORT void JNICALL
Java_com_opensparx_agent_jni_AgentBridge_pushSignal(
    JNIEnv* env, jobject, jstring channel, jdouble value, jfloat confidence)
{
    const char* ch = env->GetStringUTFChars(channel, nullptr);
    sparx::proactive::android::push_signal(ch, value, confidence);
    env->ReleaseStringUTFChars(channel, ch);
}

JNIEXPORT void JNICALL
Java_com_opensparx_agent_jni_AgentBridge_pushLabelSignal(
    JNIEnv* env, jobject, jstring channel, jstring label, jfloat score)
{
    const char* ch = env->GetStringUTFChars(channel, nullptr);
    const char* lbl = env->GetStringUTFChars(label, nullptr);
    sparx::proactive::android::push_label_signal(ch, lbl, score);
    env->ReleaseStringUTFChars(channel, ch);
    env->ReleaseStringUTFChars(label, lbl);
}

JNIEXPORT jlong JNICALL
Java_com_opensparx_agent_jni_AgentBridge_submitQuery(
    JNIEnv* env, jobject, jstring text)
{
    const char* query = env->GetStringUTFChars(text, nullptr);
    LOGI("Reactive query: %s", query);
    // TODO: Submit to Planner → generate DAG → return plan ID
    env->ReleaseStringUTFChars(text, query);
    return 1L; // placeholder plan ID
}

JNIEXPORT jstring JNICALL
Java_com_opensparx_agent_jni_AgentBridge_getPlanDag(
    JNIEnv* env, jobject, jlong plan_id)
{
    // TODO: Return DAG as JSON for visualization
    std::string dag_json = R"({
        "nodes": [
            {"id": "planner", "status": "done"},
            {"id": "verifier", "status": "active"}
        ],
        "edges": [
            {"from": "planner", "to": "verifier"}
        ]
    })";
    return env->NewStringUTF(dag_json.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_opensparx_agent_jni_AgentBridge_getAgentsState(JNIEnv* env, jobject)
{
    return env->NewStringUTF("{}");
}

JNIEXPORT jstring JNICALL
Java_com_opensparx_agent_jni_AgentBridge_getEngineStats(JNIEnv* env, jobject)
{
    return env->NewStringUTF("{}");
}

JNIEXPORT jstring JNICALL
Java_com_opensparx_agent_jni_AgentBridge_getModelInfo(JNIEnv* env, jobject)
{
    return env->NewStringUTF(qnn::get_model_info().c_str());
}

JNIEXPORT jfloat JNICALL
Java_com_opensparx_agent_jni_AgentBridge_getInferenceSpeed(JNIEnv*, jobject)
{
    return qnn::get_speed();
}

JNIEXPORT jfloat JNICALL
Java_com_opensparx_agent_jni_AgentBridge_getKvCacheUsage(JNIEnv*, jobject)
{
    return qnn::get_kv_usage();
}

JNIEXPORT jstring JNICALL
Java_com_opensparx_agent_jni_AgentBridge_getPlanStatus(
    JNIEnv* env, jobject, jlong plan_id)
{
    return env->NewStringUTF("running");
}

JNIEXPORT jlong JNICALL
Java_com_opensparx_agent_jni_AgentBridge_registerTrigger(
    JNIEnv* env, jobject, jstring name, jstring condition_json,
    jint priority, jint cooldown)
{
    return 1L; // placeholder
}

JNIEXPORT jstring JNICALL
Java_com_opensparx_agent_jni_AgentBridge_getTriggersState(JNIEnv* env, jobject)
{
    return env->NewStringUTF("[]");
}

} // extern "C"

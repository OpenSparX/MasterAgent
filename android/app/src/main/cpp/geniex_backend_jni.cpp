/**
 * @file geniex_backend_jni.cpp
 * @brief JNI wrapper for Qualcomm GenieX SDK.
 *
 * This file encapsulates all GenieX API calls. The SDK headers are
 * included conditionally — if building without the GenieX SDK (e.g.,
 * for testing or CPU-only builds), all functions return error/fallback values.
 *
 * GenieX SDK is linked via:
 *   - Local AAR: app/libs/geniex-sdk-x.y.z.aar (development)
 *   - System lib: dlopen from /vendor/lib64/ (production on-device)
 *
 * IMPORTANT: This file does NOT contain any Qualcomm-proprietary code.
 * It only calls the public GenieX API through dlopen'd function pointers.
 * The actual SDK binaries live on the device or in a local-only AAR
 * that is NEVER committed to the repository.
 */

#include <jni.h>
#include <dlfcn.h>
#include <android/log.h>
#include <string>
#include <atomic>
#include <mutex>
#include <memory>

#define TAG "SparxGenieX"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ─── GenieX API function pointers (resolved at runtime via dlopen) ──────────

namespace geniex {

// Opaque handle types (from GenieX public headers)
using SessionHandle = void*;
using GenerationHandle = void*;

// Function pointer typedefs matching GenieX C API
using CreateSessionFn = SessionHandle(*)(const char* model_path, float npu_budget, int max_kv);
using DestroySessionFn = void(*)(SessionHandle);
using BeginGenerationFn = GenerationHandle(*)(SessionHandle, const char* prompt,
    int max_tokens, float temp, float top_p, int top_k, float rep_penalty);
using NextTokenFn = const char*(*)(GenerationHandle);
using EndGenerationFn = void(*)(GenerationHandle);
using CancelGenerationFn = void(*)(SessionHandle);
using GetLoadFn = float(*)(SessionHandle);
using GetSpeedFn = float(*)(SessionHandle);
using GetKvUsageFn = float(*)(SessionHandle);
using GetModelInfoFn = const char*(*)(SessionHandle);
using ProbeHtpFn = bool(*)();

// Loaded function pointers
static CreateSessionFn      fn_create_session = nullptr;
static DestroySessionFn     fn_destroy_session = nullptr;
static BeginGenerationFn    fn_begin_generation = nullptr;
static NextTokenFn          fn_next_token = nullptr;
static EndGenerationFn      fn_end_generation = nullptr;
static CancelGenerationFn   fn_cancel_generation = nullptr;
static GetLoadFn            fn_get_load = nullptr;
static GetSpeedFn           fn_get_speed = nullptr;
static GetKvUsageFn         fn_get_kv_usage = nullptr;
static GetModelInfoFn       fn_get_model_info = nullptr;
static ProbeHtpFn           fn_probe_htp = nullptr;

static void* g_lib_handle = nullptr;
static std::mutex g_mutex;

// Search paths for GenieX runtime
static const char* SEARCH_PATHS[] = {
    "libgeniex_runtime.so",            // app's own lib dir (AAR)
    "/vendor/lib64/libgeniex_runtime.so",
    "/system/lib64/libgeniex_runtime.so",
    "/odm/lib64/libgeniex_runtime.so",
    nullptr,
};

template<typename T>
static T load_sym(const char* name) {
    auto sym = reinterpret_cast<T>(dlsym(g_lib_handle, name));
    if (!sym) LOGW("GenieX symbol not found: %s", name);
    return sym;
}

static bool load_runtime() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_lib_handle) return true;

    for (int i = 0; SEARCH_PATHS[i]; i++) {
        g_lib_handle = dlopen(SEARCH_PATHS[i], RTLD_NOW | RTLD_LOCAL);
        if (g_lib_handle) {
            LOGI("GenieX loaded from: %s", SEARCH_PATHS[i]);
            break;
        }
    }

    if (!g_lib_handle) {
        LOGE("GenieX runtime not found on device");
        return false;
    }

    // Resolve all function pointers
    fn_create_session    = load_sym<CreateSessionFn>("geniex_create_session");
    fn_destroy_session   = load_sym<DestroySessionFn>("geniex_destroy_session");
    fn_begin_generation  = load_sym<BeginGenerationFn>("geniex_begin_generation");
    fn_next_token        = load_sym<NextTokenFn>("geniex_next_token");
    fn_end_generation    = load_sym<EndGenerationFn>("geniex_end_generation");
    fn_cancel_generation = load_sym<CancelGenerationFn>("geniex_cancel_generation");
    fn_get_load          = load_sym<GetLoadFn>("geniex_get_load");
    fn_get_speed         = load_sym<GetSpeedFn>("geniex_get_speed");
    fn_get_kv_usage      = load_sym<GetKvUsageFn>("geniex_get_kv_usage");
    fn_get_model_info    = load_sym<GetModelInfoFn>("geniex_get_model_info");
    fn_probe_htp         = load_sym<ProbeHtpFn>("geniex_probe_htp");

    bool ok = fn_create_session && fn_destroy_session &&
              fn_begin_generation && fn_next_token && fn_end_generation;
    if (!ok) {
        LOGE("GenieX: missing critical symbols");
        dlclose(g_lib_handle);
        g_lib_handle = nullptr;
    }
    return ok;
}

} // namespace geniex

// ─── JNI Exports ────────────────────────────────────────────────────────────

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_opensparx_agent_inference_GenieXBackend_nativeCreateSession(
    JNIEnv* env, jobject, jstring model_path, jfloat npu_budget, jint max_kv)
{
    if (!geniex::load_runtime() || !geniex::fn_create_session) return 0L;

    const char* path = env->GetStringUTFChars(model_path, nullptr);
    auto handle = geniex::fn_create_session(path, npu_budget, max_kv);
    env->ReleaseStringUTFChars(model_path, path);

    LOGI("GenieX session created: handle=%p budget=%.1f%% kv=%d",
         handle, npu_budget * 100, max_kv);
    return reinterpret_cast<jlong>(handle);
}

JNIEXPORT void JNICALL
Java_com_opensparx_agent_inference_GenieXBackend_nativeDestroySession(
    JNIEnv*, jobject, jlong handle)
{
    if (geniex::fn_destroy_session && handle) {
        geniex::fn_destroy_session(reinterpret_cast<geniex::SessionHandle>(handle));
    }
}

JNIEXPORT void JNICALL
Java_com_opensparx_agent_inference_GenieXBackend_nativeBeginGeneration(
    JNIEnv* env, jobject, jlong handle, jstring prompt,
    jint max_tokens, jfloat temp, jfloat top_p, jint top_k, jfloat rep_penalty)
{
    if (!geniex::fn_begin_generation || !handle) return;

    const char* p = env->GetStringUTFChars(prompt, nullptr);
    geniex::fn_begin_generation(
        reinterpret_cast<geniex::SessionHandle>(handle),
        p, max_tokens, temp, top_p, top_k, rep_penalty);
    env->ReleaseStringUTFChars(prompt, p);
}

JNIEXPORT jstring JNICALL
Java_com_opensparx_agent_inference_GenieXBackend_nativeNextToken(
    JNIEnv* env, jobject, jlong handle)
{
    if (!geniex::fn_next_token || !handle) return nullptr;

    const char* tok = geniex::fn_next_token(
        reinterpret_cast<geniex::GenerationHandle>(handle));
    return tok ? env->NewStringUTF(tok) : nullptr;
}

JNIEXPORT void JNICALL
Java_com_opensparx_agent_inference_GenieXBackend_nativeEndGeneration(
    JNIEnv*, jobject, jlong handle)
{
    if (geniex::fn_end_generation && handle) {
        geniex::fn_end_generation(reinterpret_cast<geniex::GenerationHandle>(handle));
    }
}

JNIEXPORT void JNICALL
Java_com_opensparx_agent_inference_GenieXBackend_nativeCancelGeneration(
    JNIEnv*, jobject, jlong handle)
{
    if (geniex::fn_cancel_generation && handle) {
        geniex::fn_cancel_generation(reinterpret_cast<geniex::SessionHandle>(handle));
    }
}

JNIEXPORT jfloat JNICALL
Java_com_opensparx_agent_inference_GenieXBackend_nativeGetLoad(
    JNIEnv*, jobject, jlong handle)
{
    if (!geniex::fn_get_load || !handle) return 0.0f;
    return geniex::fn_get_load(reinterpret_cast<geniex::SessionHandle>(handle));
}

JNIEXPORT jfloat JNICALL
Java_com_opensparx_agent_inference_GenieXBackend_nativeGetSpeed(
    JNIEnv*, jobject, jlong handle)
{
    if (!geniex::fn_get_speed || !handle) return 0.0f;
    return geniex::fn_get_speed(reinterpret_cast<geniex::SessionHandle>(handle));
}

JNIEXPORT jfloat JNICALL
Java_com_opensparx_agent_inference_GenieXBackend_nativeGetKvUsage(
    JNIEnv*, jobject, jlong handle)
{
    if (!geniex::fn_get_kv_usage || !handle) return 0.0f;
    return geniex::fn_get_kv_usage(reinterpret_cast<geniex::SessionHandle>(handle));
}

JNIEXPORT jstring JNICALL
Java_com_opensparx_agent_inference_GenieXBackend_nativeGetModelInfo(
    JNIEnv* env, jobject, jlong handle)
{
    if (!geniex::fn_get_model_info || !handle) {
        return env->NewStringUTF(R"({"error":"not_initialized"})");
    }
    const char* info = geniex::fn_get_model_info(
        reinterpret_cast<geniex::SessionHandle>(handle));
    return env->NewStringUTF(info ? info : "{}");
}

JNIEXPORT jboolean JNICALL
Java_com_opensparx_agent_inference_GenieXBackend_nativeProbeHtp(JNIEnv*, jobject)
{
    if (!geniex::load_runtime()) return JNI_FALSE;
    if (!geniex::fn_probe_htp) return JNI_FALSE;
    return geniex::fn_probe_htp() ? JNI_TRUE : JNI_FALSE;
}

} // extern "C"

/**
 * @file genie_model_runtime.cpp
 * @brief IModelRuntime adapter for Qualcomm AI Engine (QNN / Genie SDK).
 *
 * This adapter uses dlopen to load the Genie shared library at runtime.
 * On hosts without the Qualcomm AI stack, it reports "NPU unavailable"
 * and all inference calls return an error status — the system gracefully
 * falls back to the llama.cpp runtime.
 *
 * The actual Genie integration is device-specific and covered by the
 * Qualcomm AI Stack License. This file provides only the adapter shell
 * and dlopen probing logic, which is fully open-source.
 */

#include <iostream>
#include <string>

#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace master_agent::inference {

/// Opaque handle to a loaded Genie session.
struct GenieSession {
    void* lib_handle = nullptr;
    void* session_handle = nullptr;
    bool available = false;
};

class GenieModelRuntime {
public:
    explicit GenieModelRuntime(const std::string& model_path)
        : model_path_(model_path) {
        probe();
    }

    ~GenieModelRuntime() {
        if (session_.lib_handle) {
#ifndef _WIN32
            dlclose(session_.lib_handle);
#endif
        }
    }

    bool isAvailable() const { return session_.available; }

    std::string runtimeName() const { return "genie-npu"; }

    /// Attempt to load the Genie shared library.
    void probe() {
#ifndef _WIN32
        // Try standard Qualcomm SDK paths
        const char* lib_names[] = {
            "libQnnHtp.so",
            "libGenieRuntime.so",
            "/opt/qcom/aistack/lib/libGenieRuntime.so",
            nullptr
        };

        for (const char** name = lib_names; *name; ++name) {
            void* handle = dlopen(*name, RTLD_LAZY | RTLD_LOCAL);
            if (handle) {
                session_.lib_handle = handle;
                session_.available = true;
                return;
            }
        }
#endif
        // NPU not available on this host
        session_.available = false;
    }

    /// Run inference on the NPU. Returns empty string if unavailable.
    std::string infer(const std::string& prompt, uint32_t max_tokens) {
        if (!session_.available) {
            return "";  // Caller should fall back to llama.cpp
        }

        // In production: call into Genie SDK via resolved function pointers
        // genie_create_session(), genie_run_inference(), genie_destroy_session()
        (void)prompt;
        (void)max_tokens;
        return "";  // Placeholder — real implementation is device-specific
    }

    /// Report NPU capabilities (TOPS, supported precisions).
    struct NpuInfo {
        uint32_t tops = 0;
        bool supports_fp16 = false;
        bool supports_int8 = true;
        bool supports_int4 = false;
        std::string soc_name;
    };

    NpuInfo npuInfo() const {
        NpuInfo info;
        if (!session_.available) return info;

        // In production: query QNN system info API
        info.soc_name = "unknown";
        return info;
    }

private:
    std::string model_path_;
    GenieSession session_{};
};

}  // namespace master_agent::inference

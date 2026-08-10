/**
 * @file genie_model_runtime.cpp
 * @brief GenieModelRuntime — NPU inference through dlopen'd libGenie.so.
 */

#include "genie_model_runtime.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <dlfcn.h>

namespace master_agent::inference {

// ---------------------------------------------------------------------------
// Genie C ABI, declared locally so we never link against libGenie.so.
//
// These signatures mirror the QAIRT Genie headers. Declaring them here rather
// than including GenieDialog.h keeps the build independent of whether the
// Qualcomm SDK is installed, which is what lets one binary serve both a dev
// laptop and a device.
// ---------------------------------------------------------------------------

namespace genie_abi {

using GenieDialogConfig_Handle_t = void*;
using GenieDialog_Handle_t = void*;
using Genie_Status_t = int;

constexpr Genie_Status_t GENIE_STATUS_SUCCESS = 0;

/// Sentence codes Genie passes to the token callback.
enum GenieDialog_SentenceCode_t : int {
    GENIE_DIALOG_SENTENCE_COMPLETE = 0,
    GENIE_DIALOG_SENTENCE_BEGIN    = 1,
    GENIE_DIALOG_SENTENCE_CONTINUE = 2,
    GENIE_DIALOG_SENTENCE_END      = 3,
    GENIE_DIALOG_SENTENCE_ABORT    = 4,
    GENIE_DIALOG_SENTENCE_REWIND   = 5
};

using GenieDialog_QueryCallback_t = void (*)(const char* response,
                                            GenieDialog_SentenceCode_t code,
                                            const void* user_data);

using ConfigCreateFromJsonFn =
    Genie_Status_t (*)(const char* json, GenieDialogConfig_Handle_t* out);
using ConfigFreeFn = Genie_Status_t (*)(GenieDialogConfig_Handle_t);
using DialogCreateFn =
    Genie_Status_t (*)(GenieDialogConfig_Handle_t, GenieDialog_Handle_t* out);
using DialogFreeFn = Genie_Status_t (*)(GenieDialog_Handle_t);
using DialogQueryFn = Genie_Status_t (*)(GenieDialog_Handle_t,
                                         const char* query,
                                         GenieDialog_SentenceCode_t code,
                                         GenieDialog_QueryCallback_t callback,
                                         const void* user_data);
using DialogResetFn = Genie_Status_t (*)(GenieDialog_Handle_t);
using DialogSaveStateFn = Genie_Status_t (*)(GenieDialog_Handle_t, const char*);

}  // namespace genie_abi

// ---------------------------------------------------------------------------
// Impl: dlopen handle, resolved symbols, live dialog
// ---------------------------------------------------------------------------

struct GenieModelRuntime::Impl {
    void* library = nullptr;
    genie_abi::GenieDialogConfig_Handle_t config_handle = nullptr;
    genie_abi::GenieDialog_Handle_t dialog = nullptr;

    genie_abi::ConfigCreateFromJsonFn config_create = nullptr;
    genie_abi::ConfigFreeFn config_free = nullptr;
    genie_abi::DialogCreateFn dialog_create = nullptr;
    genie_abi::DialogFreeFn dialog_free = nullptr;
    genie_abi::DialogQueryFn dialog_query = nullptr;
    genie_abi::DialogResetFn dialog_reset = nullptr;

    ~Impl() {
        if (dialog && dialog_free) dialog_free(dialog);
        if (config_handle && config_free) config_free(config_handle);
        if (library) ::dlclose(library);
    }
};

namespace {

/// Candidate locations for libGenie.so, in the order we try them. The vendor
/// paths come first because on a real device that is where the SoC-matched
/// build lives; a copy in the app directory may be the wrong HTP arch.
std::vector<std::string> genieLibraryCandidates(const std::string& dir) {
    std::vector<std::string> out;
    if (!dir.empty()) {
        out.push_back(dir + "/libGenie.so");
    }
    out.push_back("/vendor/lib64/libGenie.so");
    out.push_back("/data/local/tmp/sparx/lib/libGenie.so");
    out.push_back("libGenie.so");  // let the loader search
    return out;
}

std::uint32_t approxTokens(const std::string& text) {
    return static_cast<std::uint32_t>((text.size() + 3U) / 4U);
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// State threaded through the Genie callback. Genie hands us a `const void*`
/// user_data, so this is the only way to reach our sink from the C callback.
struct CallbackState {
    const InferenceStreamSink* sink = nullptr;
    std::string invocation_id;
    std::string accumulated;
    /// Buffer for spans Genie has not yet marked complete. See the draft-token
    /// risk note in the header: we only emit deltas we believe are final.
    std::string pending;
    std::uint32_t chunk_index = 0;
    bool abort_requested = false;
    bool saw_end = false;
    std::int64_t first_chunk_mono_ns = 0;
};

void genieTokenCallback(const char* response,
                        genie_abi::GenieDialog_SentenceCode_t code,
                        const void* user_data) {
    auto* state = const_cast<CallbackState*>(
        static_cast<const CallbackState*>(user_data));
    if (!state) return;

    // Once we have asked Genie to stop, ignore anything still in flight rather
    // than appending it: the framework may still commit the partial output and
    // post-abort text would make raw_output disagree with what the sink saw.
    if (state->abort_requested) return;

    switch (code) {
        case genie_abi::GENIE_DIALOG_SENTENCE_ABORT:
            state->abort_requested = true;
            return;
        case genie_abi::GENIE_DIALOG_SENTENCE_REWIND:
            // Genie retracted speculative text. Drop the unemitted buffer; we
            // have not shown it to the sink, so nothing needs correcting.
            state->pending.clear();
            return;
        default:
            break;
    }

    if (response && *response) {
        state->pending += response;
    }

    const bool flush =
        code == genie_abi::GENIE_DIALOG_SENTENCE_COMPLETE ||
        code == genie_abi::GENIE_DIALOG_SENTENCE_END ||
        code == genie_abi::GENIE_DIALOG_SENTENCE_CONTINUE ||
        code == genie_abi::GENIE_DIALOG_SENTENCE_BEGIN;

    if (code == genie_abi::GENIE_DIALOG_SENTENCE_END ||
        code == genie_abi::GENIE_DIALOG_SENTENCE_COMPLETE) {
        state->saw_end = true;
    }

    if (!flush || state->pending.empty()) return;

    // Never split a UTF-8 sequence across chunks: a sink that renders text
    // (TTS, UI) would emit a replacement character for the fragment.
    size_t emit_len = state->pending.size();
    if (!state->saw_end) {
        while (emit_len > 0 &&
               (static_cast<unsigned char>(state->pending[emit_len - 1]) & 0xC0)
                   == 0x80) {
            --emit_len;
        }
        if (emit_len > 0) {
            const auto lead =
                static_cast<unsigned char>(state->pending[emit_len - 1]);
            if (lead >= 0xC0) --emit_len;  // trailing lead byte, incomplete
        }
        if (emit_len == 0) return;
    }

    std::string delta = state->pending.substr(0, emit_len);
    state->pending.erase(0, emit_len);
    state->accumulated += delta;

    if (state->first_chunk_mono_ns == 0) {
        state->first_chunk_mono_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    if (state->sink && *state->sink) {
        InferenceChunk chunk;
        chunk.invocation_id = state->invocation_id;
        chunk.chunk_index = state->chunk_index++;
        chunk.delta = std::move(delta);
        chunk.final = false;
        if ((*state->sink)(chunk) == StreamControl::Abort) {
            state->abort_requested = true;
        }
    } else {
        ++state->chunk_index;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Probe
// ---------------------------------------------------------------------------

GenieAvailability GenieModelRuntime::probe(const std::string& library_dir) {
    GenieAvailability avail;

    void* lib = nullptr;
    for (const auto& candidate : genieLibraryCandidates(library_dir)) {
        lib = ::dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (lib) {
            avail.library_found = true;
            avail.library_path = candidate;
            break;
        }
    }
    if (!lib) {
        avail.detail =
            "libGenie.so not found. on a device this ships in /vendor/lib64; "
            "on a host it is part of the QAIRT SDK.";
        return avail;
    }

    // Resolve every symbol we depend on. A partial resolution is worse than a
    // missing library: it fails at the first query instead of at startup.
    const char* required[] = {
        "GenieDialogConfig_createFromJson",
        "GenieDialogConfig_free",
        "GenieDialog_create",
        "GenieDialog_free",
        "GenieDialog_query",
    };
    std::string missing;
    for (const char* sym : required) {
        if (!::dlsym(lib, sym)) {
            if (!missing.empty()) missing += ", ";
            missing += sym;
        }
    }
    if (missing.empty()) {
        avail.symbols_resolved = true;
    } else {
        avail.detail = "libGenie.so found but missing symbols: " + missing +
                       " (SDK version mismatch?)";
    }

    ::dlclose(lib);

    // config_valid is decided by the caller who knows the config path; probe()
    // reports the library dimension only.
    avail.config_valid = true;
    return avail;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

GenieModelRuntime::GenieModelRuntime(GenieConfig config)
    : config_(std::move(config)), impl_(std::make_unique<Impl>()) {
    availability_ = probe(config_.library_dir);

    if (!config_.config_path.empty()) {
        const auto json = readFile(config_.config_path);
        if (json.empty()) {
            availability_.config_valid = false;
            availability_.detail =
                "genie config not readable: " + config_.config_path;
        }
    } else {
        availability_.config_valid = false;
        availability_.detail = "no genie config path supplied";
    }

    if (!availability_.usable()) {
        return;
    }

    // Load for real and bind the symbols.
    impl_->library = ::dlopen(availability_.library_path.c_str(),
                              RTLD_NOW | RTLD_LOCAL);
    if (!impl_->library) {
        availability_.library_found = false;
        availability_.detail = "dlopen failed: ";
        if (const char* e = ::dlerror()) availability_.detail += e;
        return;
    }

    auto bind = [this](const char* name) {
        return ::dlsym(impl_->library, name);
    };
    impl_->config_create =
        reinterpret_cast<genie_abi::ConfigCreateFromJsonFn>(
            bind("GenieDialogConfig_createFromJson"));
    impl_->config_free = reinterpret_cast<genie_abi::ConfigFreeFn>(
        bind("GenieDialogConfig_free"));
    impl_->dialog_create = reinterpret_cast<genie_abi::DialogCreateFn>(
        bind("GenieDialog_create"));
    impl_->dialog_free = reinterpret_cast<genie_abi::DialogFreeFn>(
        bind("GenieDialog_free"));
    impl_->dialog_query = reinterpret_cast<genie_abi::DialogQueryFn>(
        bind("GenieDialog_query"));
    impl_->dialog_reset = reinterpret_cast<genie_abi::DialogResetFn>(
        bind("GenieDialog_reset"));  // optional
}

GenieModelRuntime::~GenieModelRuntime() = default;

// ---------------------------------------------------------------------------
// Dialog lifecycle
// ---------------------------------------------------------------------------

Status GenieModelRuntime::ensureDialog() {
    if (impl_->dialog) return Status::Ok();

    if (!availability_.usable()) {
        return Status::Error("inference", "INFERENCE_RUNTIME_UNAVAILABLE",
                             availability_.detail.empty()
                                 ? "genie runtime unavailable"
                                 : availability_.detail);
    }

    const auto json = readFile(config_.config_path);
    if (json.empty()) {
        return Status::Error("inference", "INFERENCE_RUNTIME_CONFIG_INVALID",
                             "genie config is empty or unreadable: " +
                                 config_.config_path);
    }

    if (impl_->config_create(json.c_str(), &impl_->config_handle) !=
            genie_abi::GENIE_STATUS_SUCCESS ||
        !impl_->config_handle) {
        return Status::Error(
            "inference", "INFERENCE_RUNTIME_CONFIG_INVALID",
            "GenieDialogConfig_createFromJson failed. the usual causes are a "
            "context binary built for a different dsp_arch, or a model path in "
            "the config that does not exist on device.");
    }

    if (impl_->dialog_create(impl_->config_handle, &impl_->dialog) !=
            genie_abi::GENIE_STATUS_SUCCESS ||
        !impl_->dialog) {
        return Status::Error(
            "inference", "INFERENCE_RUNTIME_INIT_FAILED",
            "GenieDialog_create failed on soc_id " +
                std::to_string(config_.soc_id) +
                " (dsp_arch " + config_.dsp_arch + "). check that the HTP "
                "backend is present and VTCM is sufficient for this model.");
    }

    return Status::Ok();
}

void GenieModelRuntime::releaseDialog() {
    if (impl_->dialog && impl_->dialog_free) {
        impl_->dialog_free(impl_->dialog);
        impl_->dialog = nullptr;
    }
    if (impl_->config_handle && impl_->config_free) {
        impl_->config_free(impl_->config_handle);
        impl_->config_handle = nullptr;
    }
}

bool GenieModelRuntime::isReady() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return impl_ && impl_->dialog != nullptr;
}

Status GenieModelRuntime::reload(const std::string& config_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    releaseDialog();
    config_.config_path = config_path;
    const auto json = readFile(config_path);
    availability_.config_valid = !json.empty();
    if (!availability_.config_valid) {
        availability_.detail = "genie config not readable: " + config_path;
        return Status::Error("inference", "INFERENCE_RUNTIME_CONFIG_INVALID",
                             availability_.detail);
    }
    return ensureDialog();
}

// ---------------------------------------------------------------------------
// Seal handling
// ---------------------------------------------------------------------------

Status GenieModelRuntime::validateSeal(
    const InferenceRequest& request,
    const RuntimeInvocationSeal& seal) const {
    if (seal.job_id != request.job_id ||
        seal.attempt_id.empty() || seal.operation_id.empty() ||
        seal.replica_id.empty() || seal.replica_epoch == 0 ||
        seal.lease_id.empty() || seal.fencing_token == 0 ||
        seal.prompt_digest != request.prompt_digest ||
        seal.deadline_mono_ns != request.deadline_mono_ns ||
        seal.invocation_id != runtimeInvocationDigest(seal)) {
        return Status::Error(
            "inference", "INFERENCE_RUNTIME_INVOCATION_INVALID",
            "runtime invocation seal is incomplete or inconsistent");
    }
    return Status::Ok();
}

void GenieModelRuntime::echoSeal(
    InferenceOutput& out,
    const RuntimeInvocationSeal& seal,
    const InferenceRequest& request) const {
    out.model_id = request.model;
    out.model_digest = seal.model_digest;
    out.job_id = seal.job_id;
    out.operation_id = seal.operation_id;
    out.replica_id = seal.replica_id;
    out.replica_epoch = seal.replica_epoch;
    out.lease_id = seal.lease_id;
    out.fencing_token = seal.fencing_token;
    out.control_epoch = seal.control_epoch;
    out.attempt_id = seal.attempt_id;
    out.prompt_digest = seal.prompt_digest;
    out.invocation_id = seal.invocation_id;
    out.runtime_backend = config_.runtime_tag;
    out.reality = request.reality;
}

std::uint32_t GenieModelRuntime::requiredWorkUnits(
    const InferenceRequest& request) const {
    // NPU prefill is far faster per token than CPU, so the same prompt costs
    // fewer scheduling quanta. Must not invoke the model.
    const auto tokens = approxTokens(request.prompt);
    return tokens < 256U ? 1U : (tokens / 256U);
}

// ---------------------------------------------------------------------------
// Inference
// ---------------------------------------------------------------------------

Result<InferenceOutput> GenieModelRuntime::infer(
    const InferenceRequest& request,
    const RuntimeInvocationSeal& seal) {
    return inferStream(request, seal, nullptr);
}

Result<InferenceOutput> GenieModelRuntime::inferStream(
    const InferenceRequest& request,
    const RuntimeInvocationSeal& seal,
    const InferenceStreamSink& sink) {
    const auto seal_status = validateSeal(request, seal);
    if (!seal_status.ok) {
        return Result<InferenceOutput>::Failure(seal_status);
    }

    const auto start = std::chrono::steady_clock::now();

    genie_abi::GenieDialog_Handle_t dialog = nullptr;
    genie_abi::DialogQueryFn query = nullptr;
    genie_abi::DialogResetFn reset = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto ready = ensureDialog();
        if (!ready.ok) {
            return Result<InferenceOutput>::Failure(ready);
        }
        dialog = impl_->dialog;
        query = impl_->dialog_query;
        reset = impl_->dialog_reset;
    }

    // The NPU call happens outside our own lock, matching the framework's rule
    // that no external boundary is crossed while holding state locks.
    CallbackState state;
    state.sink = sink ? &sink : nullptr;
    state.invocation_id = seal.invocation_id;

    const auto rc = query(dialog,
                          request.prompt.c_str(),
                          genie_abi::GENIE_DIALOG_SENTENCE_COMPLETE,
                          &genieTokenCallback,
                          &state);

    // Flush anything Genie left buffered without a terminal code.
    if (!state.pending.empty() && !state.abort_requested) {
        state.accumulated += state.pending;
        if (state.sink && *state.sink) {
            InferenceChunk chunk;
            chunk.invocation_id = seal.invocation_id;
            chunk.chunk_index = state.chunk_index++;
            chunk.delta = state.pending;
            chunk.final = false;
            (void)(*state.sink)(chunk);
        } else {
            ++state.chunk_index;
        }
        state.pending.clear();
    }

    if (rc != genie_abi::GENIE_STATUS_SUCCESS && !state.abort_requested) {
        // Retryable: an HTP failure is usually a transient resource condition
        // (another client holding VTCM), not a permanently bad request.
        return Result<InferenceOutput>::Failure(Status::Error(
            "inference", "INFERENCE_RUNTIME_QUERY_FAILED",
            "GenieDialog_query returned " + std::to_string(rc), true));
    }

    const std::string finish_reason =
        state.abort_requested ? "abort" : (state.saw_end ? "stop" : "length");

    // Exactly one final chunk, last.
    if (sink) {
        InferenceChunk chunk;
        chunk.invocation_id = seal.invocation_id;
        chunk.chunk_index = state.chunk_index++;
        chunk.delta.clear();
        chunk.final = true;
        chunk.finish_reason = finish_reason;
        (void)sink(chunk);
    }

    if (!config_.reuse_dialog && reset) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (impl_->dialog) reset(impl_->dialog);
    }

    if (state.accumulated.empty() && !state.abort_requested) {
        return Result<InferenceOutput>::Failure(Status::Error(
            "inference", "INFERENCE_RUNTIME_EMPTY_OUTPUT",
            "genie produced no output", true));
    }

    InferenceOutput out;
    out.raw_output = state.accumulated;
    out.finish_reason = finish_reason;
    echoSeal(out, seal, request);
    out.prompt_token_count = approxTokens(request.prompt);
    out.generated_token_count = approxTokens(state.accumulated);
    out.total_latency_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count());
    out.output_digest = inferenceOutputDigest(out);
    return Result<InferenceOutput>::Success(std::move(out));
}

}  // namespace master_agent::inference

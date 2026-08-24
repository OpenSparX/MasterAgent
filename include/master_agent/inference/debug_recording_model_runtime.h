#pragma once

#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

#include <nlohmann/json.hpp>

#include "master_agent/inference/inference_framework.h"

namespace master_agent::inference {

/** Explicit local-development decorator that records raw model I/O.
 * Never enable this in production or point it at a shared directory. */
class DebugRecordingModelRuntime final : public IModelRuntime {
public:
    DebugRecordingModelRuntime(std::shared_ptr<IModelRuntime> delegate,
                               std::filesystem::path artifact_path);
    std::string runtimeTag() const override;
    std::uint32_t requiredWorkUnits(const InferenceRequest& request) const override;
    Result<InferenceOutput> infer(const InferenceRequest& request,
                                  const RuntimeInvocationSeal& seal) override;
    bool supportsStreaming() const override;
    Result<InferenceOutput> inferStream(const InferenceRequest& request,
                                        const RuntimeInvocationSeal& seal,
                                        const InferenceStreamSink& sink) override;

private:
    void recordInput(const InferenceRequest& request);
    void recordOutput(const InferenceRequest& request,
                      const Result<InferenceOutput>& output);
    void append(const nlohmann::json& record);
    std::shared_ptr<IModelRuntime> delegate_;
    std::filesystem::path artifact_path_;
    mutable std::mutex mutex_;
};

}  // namespace master_agent::inference

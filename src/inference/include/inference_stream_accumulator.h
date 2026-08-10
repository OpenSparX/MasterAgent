#pragma once

/**
 * @file inference_stream_accumulator.h
 * @brief Private chunk-contract enforcement for streamed model output.
 *
 * This header is private to Inference and is not part of the installed API.
 *
 * The accumulator runs on the generating thread, outside framework state
 * locks, and is the only component that trusts a chunk enough to forward it.
 * It exists so the framework never has to believe a runtime's account of what
 * it streamed: the accumulator rebuilds the text independently and the commit
 * fence compares that reconstruction against the sealed raw_output.
 */

#include "master_agent/inference/inference_framework.h"

#include <string>
#include <utility>

namespace master_agent::inference {

/**
 * @brief Validates the chunk contract and rebuilds streamed text.
 *
 * Chunks are speculative until the commit fence runs. Presentation sinks may
 * consume them; the decision path may not.
 */
class StreamAccumulator {
public:
    StreamAccumulator(std::string invocation_id,
                      InferenceStreamSink presentation_sink)
        : invocation_id_(std::move(invocation_id)),
          presentation_sink_(std::move(presentation_sink)) {}

    /// Handles one chunk from the runtime and returns the framework verdict.
    StreamControl accept(const InferenceChunk& chunk,
                         std::int64_t now_mono_ns) {
        if (!violation_.empty()) {
            return StreamControl::Abort;
        }
        if (saw_final_) {
            return reject("INFERENCE_STREAM_CHUNK_AFTER_FINAL");
        }
        // A chunk that does not echo the seal is stale or misrouted; it must
        // not reach a presentation sink that already showed sealed text.
        if (chunk.invocation_id != invocation_id_) {
            return reject("INFERENCE_STREAM_INVOCATION_MISMATCH");
        }
        if (chunk.chunk_index != next_index_) {
            return reject("INFERENCE_STREAM_CHUNK_OUT_OF_ORDER");
        }

        if (chunk_count_ == 0) {
            first_chunk_mono_ns_ = now_mono_ns;
        }
        accumulated_ += chunk.delta;
        ++next_index_;
        ++chunk_count_;
        saw_final_ = chunk.final;

        if (presentation_sink_) {
            // A presentation sink may request cooperative abort (user barge-in,
            // arriving P0). It cannot forge acceptance of a rejected chunk.
            if (presentation_sink_(chunk) == StreamControl::Abort) {
                abort_requested_ = true;
            }
        }
        return abort_requested_ ? StreamControl::Abort
                                : StreamControl::Continue;
    }

    /**
     * Classifies the streamed invocation against the runtime's sealed output.
     *
     * Divergence is a hard failure rather than a warning: presentation sinks
     * have already emitted the accumulated text, so committing a different
     * raw_output would let the decision path act on something the user was
     * never shown.
     */
    StreamIntegrity classify(const std::string& sealed_raw_output) const {
        if (!violation_.empty()) {
            return StreamIntegrity::Diverged;
        }
        if (abort_requested_) {
            return StreamIntegrity::Aborted;
        }
        if (chunk_count_ == 0) {
            return StreamIntegrity::NotStreamed;
        }
        if (!saw_final_) {
            return StreamIntegrity::Diverged;
        }
        return accumulated_ == sealed_raw_output
                   ? StreamIntegrity::Verified
                   : StreamIntegrity::Diverged;
    }

    const std::string& violation() const { return violation_; }
    const std::string& accumulated() const { return accumulated_; }
    std::uint32_t chunkCount() const { return chunk_count_; }
    std::int64_t firstChunkMonoNs() const {
        return first_chunk_mono_ns_;
    }
    bool abortRequested() const { return abort_requested_; }

private:
    StreamControl reject(std::string code) {
        violation_ = std::move(code);
        return StreamControl::Abort;
    }

    std::string invocation_id_;
    InferenceStreamSink presentation_sink_;
    std::string accumulated_;
    std::string violation_;
    std::uint32_t next_index_ = 0;
    std::uint32_t chunk_count_ = 0;
    std::int64_t first_chunk_mono_ns_ = 0;
    bool saw_final_ = false;
    bool abort_requested_ = false;
};

}  // namespace master_agent::inference

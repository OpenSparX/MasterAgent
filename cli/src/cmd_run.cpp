/**
 * @file cmd_run.cpp
 * @brief `sparx run` — run the agent locally on CPU.
 *
 * This is Experience A: no hardware needed, no Qualcomm SDK, just a laptop.
 * It loads agent.yaml from the current directory and enters a REPL.
 *
 * Two modes, and the banner always says which one is active:
 *
 *   REAL       a GGUF path was resolved (--model, model.path, or $SPARX_MODEL)
 *              and LlamaCppModelRuntime streams tokens from llama-server.
 *   SIMULATED  no model was resolved. Deterministic skills still run for real
 *              — they never call a model — but anything routed to inference
 *              returns a canned string.
 *
 * The distinction is printed rather than inferred because a fixed reply that
 * looks like inference is worse than no reply: it makes a broken setup read as
 * a working one.
 */

#include "sparx_commands.h"
#include "sparx_agent_config.h"
#include "sparx_skill_loader.h"

#include "llama_cpp_model_runtime.h"
#include "master_agent/common/types.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <string>

#ifndef SPARX_VERSION
#define SPARX_VERSION "0.0.0-dev"
#endif

namespace fs = std::filesystem;

namespace sparx {

namespace {

using master_agent::inference::InferenceRequest;
using master_agent::inference::LlamaCppConfig;
using master_agent::inference::LlamaCppModelRuntime;
using master_agent::inference::RuntimeInvocationSeal;
using master_agent::inference::StreamControl;
using master_agent::inference::StreamIntegrity;

std::int64_t monoNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

const char* streamIntegrityLabel(StreamIntegrity integrity) {
    switch (integrity) {
        case StreamIntegrity::Verified:    return "VERIFIED";
        case StreamIntegrity::Diverged:    return "DIVERGED";
        case StreamIntegrity::Aborted:     return "ABORTED";
        case StreamIntegrity::NotStreamed: return "NOT_STREAMED";
    }
    return "UNKNOWN";
}

/// Resolves the GGUF path from the three places a developer might put it, in
/// precedence order: an explicit flag, then agent.yaml, then the environment.
std::string resolveModelPath(const std::vector<std::string>& args,
                             const AgentConfig& config) {
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--model" && i + 1 < args.size()) {
            return args[i + 1];
        }
    }
    if (!config.model_path.empty()) {
        return config.model_path;
    }
    if (const char* env = std::getenv("SPARX_MODEL")) {
        return env;
    }
    return {};
}

/// Builds the request/seal pair for one turn. The seal exists because the
/// runtime fail-closes on an inconsistent one: prompt_digest and
/// deadline_mono_ns must match the request, and invocation_id must equal
/// runtimeInvocationDigest(seal). Here there is one local replica and no
/// control plane, so the identity fields are synthetic but non-empty and the
/// digests are computed the same way the executor computes them.
void buildTurn(const AgentConfig& config,
               const std::string& prompt,
               std::uint64_t turn,
               InferenceRequest& request,
               RuntimeInvocationSeal& seal) {
    const std::string turn_id = "sparx-run-" + std::to_string(turn);

    request.job_id = turn_id;
    request.request_id = turn_id;
    request.session_id = "sparx-run-local";
    request.prompt = prompt;
    request.prompt_digest = master_agent::stableDigest(prompt);
    request.model = config.model_id.empty() ? "local-gguf" : config.model_id;
    request.deadline_mono_ns = 0;  // no deadline: an interactive REPL waits
    request.idempotency_key = turn_id;
    request.reality = "REAL";

    seal.job_id = request.job_id;
    seal.attempt_id = turn_id + "-a1";
    seal.operation_id = turn_id + "-op";
    seal.replica_id = "local";
    seal.replica_epoch = 1;
    seal.lease_id = "sparx-run-lease";
    seal.fencing_token = 1;
    seal.control_epoch = 1;
    seal.prompt_digest = request.prompt_digest;
    seal.model_digest = master_agent::stableDigest(request.model);
    seal.deadline_mono_ns = request.deadline_mono_ns;
    seal.invocation_id = master_agent::inference::runtimeInvocationDigest(seal);
}

}  // namespace

int cmd_run(const std::vector<std::string>& args) {
    // Parse flags
    bool resume = false;
    for (const auto& arg : args) {
        if (arg == "--resume") resume = true;
    }

    // Load agent.yaml from cwd
    const fs::path config_path = fs::current_path() / "agent.yaml";
    if (!fs::exists(config_path)) {
        std::cerr << "  ✗ no agent.yaml found in current directory\n";
        std::cerr << "    run `sparx init <name>` first, then cd into it\n";
        return 1;
    }

    AgentConfig config;
    if (!loadAgentConfig(config_path.string(), config)) {
        std::cerr << "  ✗ failed to parse agent.yaml\n";
        return 1;
    }

    if (config.runtime == "npu") {
        std::cerr << "  ✗ runtime=npu requires `sparx deploy --device`\n";
        return 1;
    }

    // Resolve model path and decide REAL vs SIMULATED
    std::string model_path = resolveModelPath(args, config);
    const bool real = !model_path.empty();
    const char* reality = real ? "REAL" : "SIMULATED";
    const char* runtime_label = real ? "llama-cpp" : "none";

    std::unique_ptr<LlamaCppModelRuntime> runtime;
    if (real) {
        if (!fs::exists(model_path)) {
            std::cerr << "  ✗ model file not found: " << model_path << "\n";
            return 1;
        }
        LlamaCppConfig llama_cfg;
        llama_cfg.model_path = model_path;
        llama_cfg.endpoint = config.endpoint;
        llama_cfg.context_length = static_cast<uint32_t>(config.context_length);
        runtime = std::make_unique<LlamaCppModelRuntime>(std::move(llama_cfg));
        if (!runtime->waitForServer(std::chrono::milliseconds(15000))) {
            std::cerr << "  ✗ llama-server did not become ready within 15s\n";
            std::cerr << "    check that the model loads correctly:\n";
            std::cerr << "    llama-server -m " << model_path << " --host 127.0.0.1 --port 8080\n";
            return 1;
        }
    }

    // Banner — version comes from the compile-time stamping in CMakeLists.txt,
    // so it is always correct and cannot drift from git state.
    std::cout << "  OpenSparX v" << SPARX_VERSION << " · reality=" << reality
              << " · runtime=" << runtime_label << "\n";

    if (!real) {
        std::cout << "  note: no model configured. "
                     "deterministic skills work; inference returns a stub.\n"
                     "        configure a GGUF path via --model, model.path in agent.yaml,\n"
                     "        or the SPARX_MODEL environment variable.\n";
    }

    if (resume) {
        // WAL replay is implemented in the kernel's orchestrator. The CLI does
        // not expose it yet — pretending it works teaches nothing and hides a
        // real failure from the developer.
        std::cout << "  note: --resume is not yet wired to the WAL recovery "
                     "path in the CLI.\n"
                     "        the session starts fresh.\n";
    }

    // Load skills/<name>.yaml for every skill registered in agent.yaml.
    auto loaded_skills = loadSkills(fs::current_path(), config.skills);
    std::set<std::string> loaded_names;
    for (const auto& s : loaded_skills) loaded_names.insert(s.name);

    if (!config.skills.empty()) {
        std::cout << "  skills: " << loaded_skills.size() << "/"
                  << config.skills.size() << " loaded from skills/";
        // A registered skill with no file is the case worth naming: it is
        // silently inert otherwise, and that reads as a broken agent.
        std::vector<std::string> missing;
        for (const auto& name : config.skills) {
            if (!loaded_names.count(name)) missing.push_back(name);
        }
        if (!missing.empty()) {
            std::cout << " (no yaml: ";
            for (size_t i = 0; i < missing.size(); ++i) {
                std::cout << (i ? ", " : "") << missing[i];
            }
            std::cout << ")";
        }
        std::cout << "\n";
    }

    std::cout << "\n  Agent \"" << config.name << "\" is running. "
                 "Type a message or Ctrl+C to exit.\n\n";

    // Interactive REPL
    std::string line;
    std::uint64_t turn = 0;
    while (true) {
        std::cout << "  > " << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;
        ++turn;

        auto start = std::chrono::steady_clock::now();

        // Deterministic skills first. Loaded skills/<name>.yaml definitions win
        // over the built-in matcher so an edited skill file takes effect.
        bool matched = false;
        for (const auto& skill : loaded_skills) {
            if (!skillMatches(skill, line)) continue;

            auto elapsed = std::chrono::steady_clock::now() - start;
            auto us = std::chrono::duration_cast<
                std::chrono::microseconds>(elapsed).count();
            std::cout << "  ✓ route=deterministic  skill=" << skill.name
                      << "  " << (us / 1000.0) << "ms"
                      << "   (model not invoked)\n";

            if (skill.handler_type == "mcp") {
                // The MCP client path lives in the kernel and is not reachable
                // from `sparx run` yet. Say so rather than printing nothing,
                // which would look like the skill silently did its job.
                std::cout << "  ! handler type=mcp (service=" << skill.mcp_service
                          << " tool=" << skill.mcp_tool << ") "
                             "is not invocable from `sparx run` yet\n";
            } else if (!skill.response.empty()) {
                std::cout << "  ✓ \"" << skill.response << "\"\n";
            } else if (!skill.response_template.empty()) {
                // Template fired, but parameter extraction is not implemented,
                // so any {placeholder} is still unfilled. Print the template and
                // name the gaps instead of emitting braces as if they were text.
                std::cout << "  ✓ " << skill.response_template << "\n";
                const auto gaps = unfilledPlaceholders(skill.response_template);
                if (!gaps.empty()) {
                    std::cout << "  ! unfilled placeholders (parameter "
                                 "extraction not implemented): ";
                    for (size_t i = 0; i < gaps.size(); ++i) {
                        std::cout << (i ? ", " : "") << "{" << gaps[i] << "}";
                    }
                    std::cout << "\n";
                }
            } else {
                std::cout << "  ! skill has no handler.response — "
                             "edit " << skill.source_path << "\n";
            }
            matched = true;
            break;
        }

        // Built-in fallback for skills with no YAML file on disk.
        if (!matched) {
            for (const auto& skill : config.skills) {
                if (loaded_names.count(skill)) continue;  // already tried above
                if (!matchesDeterministicSkill(skill, line)) continue;
                auto elapsed = std::chrono::steady_clock::now() - start;
                auto us = std::chrono::duration_cast<
                    std::chrono::microseconds>(elapsed).count();
                std::cout << "  ✓ route=deterministic  skill=" << skill
                          << "  " << (us / 1000.0) << "ms"
                          << "   (model not invoked)\n";
                executeDeterministicSkill(skill, line);
                matched = true;
                break;
            }
        }

        if (!matched && real && runtime) {
            // Route to LlamaCppModelRuntime with streaming
            InferenceRequest request;
            RuntimeInvocationSeal seal;
            buildTurn(config, line, turn, request, seal);

            std::int64_t first_chunk_ns = 0;
            auto sink = [&](const master_agent::inference::InferenceChunk& chunk)
                -> StreamControl {
                if (chunk.chunk_index == 0) {
                    first_chunk_ns = monoNowNs();
                    std::cout << "  ";
                }
                std::cout << chunk.delta << std::flush;
                if (chunk.final) std::cout << "\n";
                return StreamControl::Continue;
            };

            auto result = runtime->inferStream(request, seal, sink);
            auto elapsed = std::chrono::steady_clock::now() - start;
            auto ms = std::chrono::duration_cast<
                std::chrono::milliseconds>(elapsed).count();

            if (result.status.ok && result.value) {
                auto start_ns = std::chrono::duration_cast<
                    std::chrono::nanoseconds>(
                    start.time_since_epoch()).count();
                auto ttft_ms = first_chunk_ns > 0
                    ? (first_chunk_ns - start_ns) / 1000000
                    : 0;
                // Report actual metrics rather than fabricated ones.
                std::cout << "  ✓ route=inference  ttft="
                          << ttft_ms << "ms  total=" << ms << "ms"
                          << "  tokens=" << result.value->generated_token_count
                          << "  stream="
                          << streamIntegrityLabel(result.value->stream_integrity)
                          << "\n";
            } else {
                std::cerr << "  ✗ inference failed: "
                          << result.status.error.code << " — "
                          << result.status.error.message << "\n";
            }
        } else if (!matched) {
            // Simulated mode — no model, no fake output
            auto elapsed = std::chrono::steady_clock::now() - start;
            auto us = std::chrono::duration_cast<
                std::chrono::microseconds>(elapsed).count();
            std::cout << "  · route=inference  reality=SIMULATED  "
                      << (us / 1000.0) << "ms\n";
            std::cout << "  (no model configured — inference stub)\n";
        }
        std::cout << "\n";
    }
    return 0;
}

}  // namespace sparx

#pragma once
/**
 * @file sparx_genie_config.h
 * @brief Auto-generates Genie HTP backend configuration from detected SoC capabilities.
 *
 * This replaces the error-prone manual process where developers copy-paste configs
 * with mismatched soc_id/dsp_arch/num_cores (all three of which we observed in the
 * user's existing scripts).
 */

#include "sparx_device_info.h"
#include "sparx_agent_config.h"

#include <sstream>
#include <string>

namespace sparx {

/// Generates the HTP backend extension config JSON that Genie expects.
/// Based on the pattern observed in the user's qwen3-vl-edge-quant scripts,
/// but generated correctly from the actual detected hardware.
inline std::string generateGenieConfig(const SocEntry& soc,
                                        const AgentConfig& agent) {
    std::ostringstream json;
    json << "{\n";
    json << "  \"graphs\": [\n";
    json << "    {\n";
    json << "      \"O\": 3.0,\n";
    json << "      \"vtcm_mb\": 16,\n";
    json << "      \"num_cores\": " << soc.max_cores << ",\n";
    json << "      \"fp16_relaxed_precision\": 0,\n";
    json << "      \"hvx_threads\": " << soc.hvx_threads << "\n";
    json << "    }\n";
    json << "  ],\n";
    json << "  \"devices\": [\n";
    json << "    {\n";
    json << "      \"soc_id\": " << soc.soc_id << ",\n";
    json << "      \"dsp_arch\": \"" << soc.dsp_arch << "\",\n";
    json << "      \"device_id\": 0,\n";
    json << "      \"cores\": [\n";
    for (int i = 0; i < soc.max_cores; ++i) {
        json << "        {\"core_id\": " << i
             << ", \"perf_profile\": \"burst\""
             << ", \"rpc_control_latency\": 100}";
        if (i < soc.max_cores - 1) json << ",";
        json << "\n";
    }
    json << "      ],\n";
    json << "      \"pd_session\": \"unsigned\"\n";
    json << "    }\n";
    json << "  ],\n";
    json << "  \"context\": {\n";
    json << "    \"extended_udma\": true,\n";
    json << "    \"weight_sharing_enabled\": true\n";
    json << "  },\n";
    json << "  \"memory\": {\n";
    json << "    \"mem_type\": \"shared_buffer\"\n";
    json << "  }\n";
    json << "}\n";
    return json.str();
}

/// Generates the Genie dialog JSON config for text-to-text inference.
inline std::string generateGenieDialogConfig(const SocEntry& soc,
                                              const AgentConfig& agent,
                                              const std::string& model_dir) {
    std::ostringstream json;
    json << "{\n";
    json << "  \"dialog\": {\n";
    json << "    \"version\": 1,\n";
    json << "    \"type\": \"htp\",\n";
    json << "    \"max-num-tokens\": " << agent.max_output_tokens << ",\n";
    json << "    \"embedding\": {\n";
    json << "      \"version\": 1,\n";
    json << "      \"type\": \"lut\",\n";
    json << "      \"lut-path\": \"" << model_dir << "/embedding_fp32.bin\",\n";
    json << "      \"size\": 2560,\n";
    json << "      \"datatype\": \"float32\"\n";
    json << "    },\n";
    json << "    \"context\": {\n";
    json << "      \"version\": 1,\n";
    json << "      \"size\": " << agent.context_length << ",\n";
    json << "      \"n-vocab\": 151936,\n";
    json << "      \"bos-token\": 151644,\n";
    json << "      \"eos-token\": 151645,\n";
    json << "      \"n-embd\": 2560\n";
    json << "    },\n";
    json << "    \"sampler\": {\n";
    json << "      \"version\": 1,\n";
    json << "      \"seed\": 42,\n";
    json << "      \"temp\": 0.8,\n";
    json << "      \"top-k\": 1,\n";
    json << "      \"top-p\": 1.0,\n";
    json << "      \"greedy\": true\n";
    json << "    },\n";
    json << "    \"tokenizer\": {\n";
    json << "      \"version\": 1,\n";
    json << "      \"path\": \"" << model_dir << "/tokenizer.json\"\n";
    json << "    },\n";
    json << "    \"engine\": {\n";
    json << "      \"version\": 1,\n";
    json << "      \"n-threads\": 3,\n";
    json << "      \"backend\": {\n";
    json << "        \"version\": 1,\n";
    json << "        \"type\": \"QnnHtp\",\n";
    json << "        \"QnnHtp\": {\n";
    json << "          \"version\": 1,\n";
    json << "          \"spill-fill-bufsize\": 0,\n";
    json << "          \"use-mmap\": true,\n";
    json << "          \"mmap-budget\": 0,\n";
    json << "          \"poll\": true,\n";
    json << "          \"allow-async-init\": false,\n";
    json << "          \"cpu-mask\": \"0xe0\",\n";
    json << "          \"kv-dim\": 128,\n";
    json << "          \"enable-graph-switching\": false\n";
    json << "        },\n";
    json << "        \"extensions\": \"htp_backend_ext_config.json\"\n";
    json << "      },\n";
    json << "      \"model\": {\n";
    json << "        \"version\": 1,\n";
    json << "        \"type\": \"binary\",\n";
    json << "        \"binary\": {\n";
    json << "          \"version\": 1,\n";
    json << "          \"ctx-bins\": []\n";  // Filled by deploy from model_dir
    json << "        },\n";
    json << "        \"positional-encoding\": {\n";
    json << "          \"type\": \"rope\",\n";
    json << "          \"rope-dim\": 64,\n";
    json << "          \"rope-theta\": 5000000\n";
    json << "        }\n";
    json << "      }\n";
    json << "    }\n";
    json << "  }\n";
    json << "}\n";
    return json.str();
}

}  // namespace sparx

#pragma once
#include "master_agent/runtime/reference_runtime.h"

namespace master_agent::reference {
struct HttpModelConfig {
    std::string endpoint = "http://127.0.0.1:8080/v1/chat/completions";
    std::string model = "local";
    std::string api_key;
    long timeout_ms = 30000;
};
// Connects to an existing server; never starts processes or downloads models.
// Explicit endpoint configuration is required to contact a remote server.
ModelHandler makeHttpModel(HttpModelConfig config);
}  // namespace master_agent::reference

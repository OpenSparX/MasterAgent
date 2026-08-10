#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace master_agent::memory {

struct WriteMemoryRequest {
  std::string user_id;
  std::string session_id;
  std::uint64_t turn_id = 0;
  std::uint32_t record_version = 1;
  std::uint64_t event_time = 0;
  std::string user_input;
  std::string assistant_output;
  std::string scene = "cockpit";
};

struct WriteMemoryResult {
  bool success = false;
  std::string memory_id;
  std::uint64_t revision = 0;
  bool idempotent = false;
  std::string error_code;
  std::string error_message;
};

struct GetContextRequest {
  std::string user_id;
  std::string session_id;
  std::string query;
  std::string scene;
  int token_budget = 256;
  int max_turns = 3;
};

struct ContextBlock {
  std::string memory_type = "short_term";
  std::string content;
  std::string source_memory_id;
  double relevance_score = 1.0;
  std::string session_id;
  std::uint64_t turn_id = 0;
  std::uint64_t timestamp_ms = 0;
};

struct GetContextResult {
  bool success = false;
  std::vector<ContextBlock> context_blocks;
  std::string error_code;
  std::string error_message;
};

class IMemoryClient {
 public:
  virtual ~IMemoryClient() = default;

  virtual WriteMemoryResult writeMemory(
      const WriteMemoryRequest& request) = 0;
  virtual GetContextResult getContext(
      const GetContextRequest& request) = 0;
};

inline constexpr const char* kWriteMemoryPath = "/api/memory/write";
inline constexpr const char* kGetContextPath = "/api/memory/context";

}  // namespace master_agent::memory

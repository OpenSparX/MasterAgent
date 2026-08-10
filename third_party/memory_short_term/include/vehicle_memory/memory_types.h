#pragma once

#include <string>

namespace vehicle_memory {

enum class MemoryErrorCode {
  kNone,
  kConfigurationError,
  kAuthenticationError,
  kRateLimited,
  kUpstreamTimeout,
  kUpstreamUnavailable,
  kInvalidModelOutput,
  kPreferenceValidationFailed,
  kEmbeddingFailed,
  kStorageCorrupted,
  kStorageCommitFailed,
  kRequestInvalid,
  kRevisionConflict,
};

struct MemoryError {
  MemoryErrorCode code = MemoryErrorCode::kNone;
  std::string message;
  std::string request_id;
  bool retryable = false;

  explicit operator bool() const { return code != MemoryErrorCode::kNone; }
};

}  // namespace vehicle_memory

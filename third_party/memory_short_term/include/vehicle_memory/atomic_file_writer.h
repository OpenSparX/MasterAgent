#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "vehicle_memory/memory_types.h"

namespace vehicle_memory {

enum class AtomicWriteMode {
  kCommitWithBackup,
  kRestorePreservingBackup,
};

enum class AtomicWriteStage {
  kBeforeWrite,
  kAfterTempFlushAndVerify,
  kBeforeReplace,
  kAfterBackupRotationBeforeReplace,
};

using AtomicWriteStageHook = std::function<bool(AtomicWriteStage)>;

class AtomicFileWriter {
 public:
  virtual ~AtomicFileWriter() = default;
  virtual MemoryError Replace(const std::filesystem::path& destination,
                              const std::string& bytes,
                              AtomicWriteMode mode) = 0;
};

std::shared_ptr<AtomicFileWriter> CreateWindowsAtomicFileWriter();
std::shared_ptr<AtomicFileWriter> CreateWindowsAtomicFileWriterForTesting(
    AtomicWriteStageHook hook);

}  // namespace vehicle_memory

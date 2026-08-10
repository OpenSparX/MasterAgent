#include "vehicle_memory/atomic_file_writer.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vehicle_memory {
namespace {

MemoryError CommitFailed() {
  MemoryError error;
  error.code = MemoryErrorCode::kStorageCommitFailed;
  error.message = "atomic repository write failed";
  error.retryable = true;
  return error;
}

bool IsSafeDestination(const std::filesystem::path& destination) {
  if (!destination.is_absolute() || destination.filename().empty() ||
      destination.filename() == "." || destination.filename() == "..") {
    return false;
  }
  for (const auto& component : destination) {
    if (component == "..") {
      return false;
    }
  }
  return true;
}

bool IsValidJson(const std::string& bytes) {
  try {
    return nlohmann::json::parse(bytes).is_object();
  } catch (...) {
    return false;
  }
}

bool ShouldInjectFailure(const AtomicWriteStageHook& hook,
                         AtomicWriteStage stage) {
  if (!hook) {
    return false;
  }
  try {
    return hook(stage);
  } catch (...) {
    return true;
  }
}

#ifdef _WIN32

class ScopedHandle {
 public:
  explicit ScopedHandle(HANDLE handle = INVALID_HANDLE_VALUE)
      : handle_(handle) {}
  ~ScopedHandle() {
    if (handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
  }
  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;
  HANDLE get() const { return handle_; }
  bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }

 private:
  HANDLE handle_;
};

bool WriteAndFlush(const std::filesystem::path& path,
                   const std::string& bytes) {
  ScopedHandle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                                nullptr));
  if (!file.valid()) {
    return false;
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count = static_cast<DWORD>(std::min<std::size_t>(
        bytes.size() - offset, std::numeric_limits<DWORD>::max()));
    DWORD written = 0;
    if (!WriteFile(file.get(), bytes.data() + offset, count, &written,
                   nullptr) ||
        written != count) {
      return false;
    }
    offset += written;
  }
  return FlushFileBuffers(file.get()) != FALSE;
}

bool ReadExact(const std::filesystem::path& path, std::string* bytes) {
  ScopedHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                nullptr));
  if (!file.valid()) {
    return false;
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0 ||
      static_cast<unsigned long long>(size.QuadPart) >
          static_cast<unsigned long long>(64U * 1024U * 1024U)) {
    return false;
  }
  bytes->assign(static_cast<std::size_t>(size.QuadPart), '\0');
  std::size_t offset = 0;
  while (offset < bytes->size()) {
    const auto count = static_cast<DWORD>(std::min<std::size_t>(
        bytes->size() - offset, std::numeric_limits<DWORD>::max()));
    DWORD read = 0;
    if (!ReadFile(file.get(), bytes->data() + offset, count, &read, nullptr) ||
        read != count) {
      return false;
    }
    offset += read;
  }
  return true;
}

bool IsExistingReparsePoint(const std::filesystem::path& path) {
  const auto attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
}

class WindowsAtomicFileWriter final : public AtomicFileWriter {
 public:
  explicit WindowsAtomicFileWriter(AtomicWriteStageHook hook = {})
      : hook_(std::move(hook)) {}

  MemoryError Replace(const std::filesystem::path& destination,
                      const std::string& bytes,
                      AtomicWriteMode mode) override {
    if (!IsSafeDestination(destination) || !IsValidJson(bytes)) {
      return CommitFailed();
    }
    auto temporary = destination;
    temporary += ".tmp";
    auto backup = destination;
    backup += ".bak";
    auto previous_backup = backup;
    previous_backup += ".previous";
    if (temporary.parent_path() != destination.parent_path() ||
        backup.parent_path() != destination.parent_path() ||
        (mode != AtomicWriteMode::kCommitWithBackup &&
         mode != AtomicWriteMode::kRestorePreservingBackup) ||
        IsExistingReparsePoint(destination) ||
        IsExistingReparsePoint(temporary) ||
        IsExistingReparsePoint(backup) ||
        IsExistingReparsePoint(previous_backup)) {
      return CommitFailed();
    }
    DeleteFileW(temporary.c_str());
    if (ShouldInjectFailure(hook_, AtomicWriteStage::kBeforeWrite)) {
      return CommitFailed();
    }
    if (!WriteAndFlush(temporary, bytes)) {
      DeleteFileW(temporary.c_str());
      return CommitFailed();
    }
    std::string verified;
    if (!ReadExact(temporary, &verified) || verified != bytes ||
        !IsValidJson(verified)) {
      DeleteFileW(temporary.c_str());
      return CommitFailed();
    }
    if (ShouldInjectFailure(hook_,
                            AtomicWriteStage::kAfterTempFlushAndVerify) ||
        ShouldInjectFailure(hook_, AtomicWriteStage::kBeforeReplace)) {
      DeleteFileW(temporary.c_str());
      return CommitFailed();
    }

    const auto destination_exists =
        GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES;
    BOOL replaced = FALSE;
    if (mode == AtomicWriteMode::kRestorePreservingBackup) {
      replaced = MoveFileExW(temporary.c_str(), destination.c_str(),
                             MOVEFILE_REPLACE_EXISTING |
                                 MOVEFILE_WRITE_THROUGH);
    } else if (destination_exists) {
      const bool had_backup =
          GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES;
      if (had_backup &&
          MoveFileExW(backup.c_str(), previous_backup.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ==
              FALSE) {
        DeleteFileW(temporary.c_str());
        return CommitFailed();
      }
      const bool fail_after_backup_rotation =
          had_backup && ShouldInjectFailure(
                            hook_, AtomicWriteStage::
                                       kAfterBackupRotationBeforeReplace);
      if (!fail_after_backup_rotation) {
        replaced = ReplaceFileW(destination.c_str(), temporary.c_str(),
                                backup.c_str(), REPLACEFILE_WRITE_THROUGH,
                                nullptr, nullptr);
      }
      if (replaced == FALSE && had_backup) {
        MoveFileExW(previous_backup.c_str(), backup.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
      } else if (replaced != FALSE && had_backup) {
        DeleteFileW(previous_backup.c_str());
      }
    } else {
      replaced = MoveFileExW(temporary.c_str(), destination.c_str(),
                             MOVEFILE_REPLACE_EXISTING |
                                 MOVEFILE_WRITE_THROUGH);
    }
    if (replaced == FALSE) {
      DeleteFileW(temporary.c_str());
      return CommitFailed();
    }
    return {};
  }

 private:
  AtomicWriteStageHook hook_;
};

#else

class WindowsAtomicFileWriter final : public AtomicFileWriter {
 public:
  explicit WindowsAtomicFileWriter(AtomicWriteStageHook hook = {})
      : hook_(std::move(hook)) {}

  MemoryError Replace(const std::filesystem::path& destination,
                      const std::string& bytes,
                      AtomicWriteMode mode) override {
    if (!IsSafeDestination(destination) || !IsValidJson(bytes)) {
      return CommitFailed();
    }
    auto temporary = destination;
    temporary += ".tmp";
    auto backup = destination;
    backup += ".bak";
    if (mode != AtomicWriteMode::kCommitWithBackup &&
        mode != AtomicWriteMode::kRestorePreservingBackup) {
      return CommitFailed();
    }
    std::error_code error;
    std::filesystem::remove(temporary, error);
    if (ShouldInjectFailure(hook_, AtomicWriteStage::kBeforeWrite)) {
      return CommitFailed();
    }
    {
      std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      output.flush();
      if (!output) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return CommitFailed();
      }
    }
    std::ifstream input(temporary, std::ios::binary);
    const std::string verified((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    if (verified != bytes || !IsValidJson(verified)) {
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      return CommitFailed();
    }
    if (ShouldInjectFailure(hook_,
                            AtomicWriteStage::kAfterTempFlushAndVerify) ||
        ShouldInjectFailure(hook_, AtomicWriteStage::kBeforeReplace)) {
      std::filesystem::remove(temporary, error);
      return CommitFailed();
    }
    if (mode == AtomicWriteMode::kCommitWithBackup &&
        std::filesystem::exists(destination, error)) {
      std::filesystem::remove(backup, error);
      error.clear();
      std::filesystem::rename(destination, backup, error);
      if (error) {
        return CommitFailed();
      }
    }
    std::filesystem::rename(temporary, destination, error);
    if (error) {
      std::error_code ignored;
      if (std::filesystem::exists(backup, ignored)) {
        std::filesystem::rename(backup, destination, ignored);
      }
      return CommitFailed();
    }
    return {};
  }

 private:
  AtomicWriteStageHook hook_;
};

#endif

}  // namespace

std::shared_ptr<AtomicFileWriter> CreateWindowsAtomicFileWriter() {
  return std::make_shared<WindowsAtomicFileWriter>();
}

std::shared_ptr<AtomicFileWriter> CreateWindowsAtomicFileWriterForTesting(
    AtomicWriteStageHook hook) {
  return std::make_shared<WindowsAtomicFileWriter>(std::move(hook));
}

}  // namespace vehicle_memory

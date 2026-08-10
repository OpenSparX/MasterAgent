#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "vehicle_memory/atomic_file_writer.h"
#include "vehicle_memory/memory_types.h"

namespace vehicle_memory {

enum class ExtractionState {
  kPending,
  kProcessed,
  kSuperseded,
};

struct ConversationTurn {
  std::string user_id;
  std::string session_id;
  std::uint64_t turn_id = 0;
  std::uint32_t record_version = 1;
  std::uint64_t timestamp_ms = 0;
  std::string user_text;
  std::string assistant_text;
  std::string scene = "cockpit";
  ExtractionState extraction_state = ExtractionState::kPending;
  std::string extraction_batch_id;
};

struct ConversationJournalSnapshot {
  std::uint64_t revision = 0;
  std::vector<ConversationTurn> turns;
};

struct ConversationJournalInspection {
  bool exists = false;
  std::string file_name;
  std::uintmax_t file_bytes = 0;
  std::string format = "vehicle-conversation-journal";
  std::uint32_t schema_version = 1;
  std::string checksum;
  ConversationJournalSnapshot snapshot;
  MemoryError error;
};

struct ConversationJournalLoadResult {
  ConversationJournalSnapshot snapshot;
  bool recovered_from_backup = false;
  MemoryError error;
};

struct AppendConversationTurnResult {
  bool appended = false;
  bool idempotent = false;
  std::uint64_t revision = 0;
  MemoryError error;
};

struct RecentConversationResult {
  std::vector<ConversationTurn> turns;
  MemoryError error;
};

struct PendingExtractionBatch {
  std::string batch_id;
  std::string user_id;
  std::vector<ConversationTurn> turns;
};

struct PreparePendingBatchResult {
  PendingExtractionBatch batch;
  MemoryError error;
};

struct MarkExtractionProcessedResult {
  bool committed = false;
  bool idempotent = false;
  std::uint64_t revision = 0;
  MemoryError error;
};

struct ConversationJournalState {
  std::uint64_t revision = 0;
  std::size_t total_turns = 0;
  std::size_t pending_turns = 0;
  std::size_t processed_turns = 0;
  std::size_t superseded_turns = 0;
  std::uintmax_t file_bytes = 0;
};

class ConversationJournal {
 public:
  virtual ~ConversationJournal() = default;

  virtual ConversationJournalLoadResult Load() = 0;
  virtual ConversationJournalSnapshot Snapshot() const = 0;
  virtual ConversationJournalInspection InspectPersisted() const = 0;
  virtual AppendConversationTurnResult Append(
      const ConversationTurn& turn) = 0;
  virtual RecentConversationResult Recent(
      const std::string& user_id, const std::string& session_id,
      std::size_t max_turns) const = 0;
  virtual PreparePendingBatchResult PreparePending(
      const std::string& user_id, std::size_t max_turns) const = 0;
  virtual MarkExtractionProcessedResult MarkProcessed(
      const PendingExtractionBatch& batch) = 0;
  virtual ConversationJournalState State(
      const std::string& user_id = {}) const = 0;
};

std::shared_ptr<ConversationJournal> CreateJsonConversationJournal(
    std::filesystem::path data_directory,
    std::filesystem::path relative_file_name,
    std::shared_ptr<AtomicFileWriter> writer);

}  // namespace vehicle_memory

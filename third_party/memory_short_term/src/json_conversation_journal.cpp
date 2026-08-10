#include "vehicle_memory/conversation_journal.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>

#include <nlohmann/json.hpp>

#include "vehicle_memory/checksum.h"

namespace vehicle_memory {
namespace {

constexpr std::size_t kMaximumJournalBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaximumUserIdBytes = 256U;
constexpr std::size_t kMaximumSessionIdBytes = 256U;
constexpr std::size_t kMaximumSceneBytes = 256U;
constexpr std::size_t kMaximumTextBytes = 64U * 1024U;

MemoryError Error(MemoryErrorCode code, const char* message,
                  bool retryable = false) {
  MemoryError error;
  error.code = code;
  error.message = message;
  error.retryable = retryable;
  return error;
}

MemoryError InvalidRequest(const char* message) {
  return Error(MemoryErrorCode::kRequestInvalid, message);
}

MemoryError CommitFailed() {
  return Error(MemoryErrorCode::kStorageCommitFailed,
               "conversation journal commit failed", true);
}

MemoryError Corrupted() {
  return Error(MemoryErrorCode::kStorageCorrupted,
               "conversation journal is corrupted");
}

const char* ToString(ExtractionState state) {
  switch (state) {
    case ExtractionState::kPending:
      return "pending";
    case ExtractionState::kProcessed:
      return "processed";
    case ExtractionState::kSuperseded:
      return "superseded";
  }
  return "pending";
}

bool ParseExtractionState(const std::string& text, ExtractionState* state) {
  if (text == "pending") {
    *state = ExtractionState::kPending;
    return true;
  }
  if (text == "processed") {
    *state = ExtractionState::kProcessed;
    return true;
  }
  if (text == "superseded") {
    *state = ExtractionState::kSuperseded;
    return true;
  }
  return false;
}

bool IsValidTurn(const ConversationTurn& turn) {
  if (turn.user_id.empty() || turn.user_id.size() > kMaximumUserIdBytes ||
      turn.session_id.empty() ||
      turn.session_id.size() > kMaximumSessionIdBytes ||
      turn.turn_id == 0U || turn.record_version == 0U ||
      turn.timestamp_ms == 0U || turn.user_text.empty() ||
      turn.user_text.size() > kMaximumTextBytes ||
      turn.assistant_text.size() > kMaximumTextBytes ||
      turn.scene.size() > kMaximumSceneBytes) {
    return false;
  }
  if (turn.extraction_state == ExtractionState::kPending &&
      !turn.extraction_batch_id.empty()) {
    return false;
  }
  if (turn.extraction_state == ExtractionState::kProcessed &&
      turn.extraction_batch_id.empty()) {
    return false;
  }
  return true;
}

auto Identity(const ConversationTurn& turn) {
  return std::tie(turn.user_id, turn.session_id, turn.turn_id,
                  turn.record_version);
}

bool SameIdentity(const ConversationTurn& left,
                  const ConversationTurn& right) {
  return Identity(left) == Identity(right);
}

bool SameRawContent(const ConversationTurn& left,
                    const ConversationTurn& right) {
  return SameIdentity(left, right) &&
         left.timestamp_ms == right.timestamp_ms &&
         left.user_text == right.user_text &&
         left.assistant_text == right.assistant_text &&
         left.scene == right.scene;
}

bool ChronologicalLess(const ConversationTurn& left,
                       const ConversationTurn& right) {
  return std::tie(left.timestamp_ms, left.session_id, left.turn_id,
                  left.record_version) <
         std::tie(right.timestamp_ms, right.session_id, right.turn_id,
                  right.record_version);
}

nlohmann::json TurnToJson(const ConversationTurn& turn) {
  return nlohmann::json{
      {"user_id", turn.user_id},
      {"session_id", turn.session_id},
      {"turn_id", turn.turn_id},
      {"record_version", turn.record_version},
      {"timestamp_ms", turn.timestamp_ms},
      {"user_text", turn.user_text},
      {"assistant_text", turn.assistant_text},
      {"scene", turn.scene},
      {"extraction_state", ToString(turn.extraction_state)},
      {"extraction_batch_id", turn.extraction_batch_id},
  };
}

bool TurnFromJson(const nlohmann::json& json, ConversationTurn* turn) {
  try {
    if (!json.is_object()) {
      return false;
    }
    turn->user_id = json.at("user_id").get<std::string>();
    turn->session_id = json.at("session_id").get<std::string>();
    turn->turn_id = json.at("turn_id").get<std::uint64_t>();
    turn->record_version =
        json.at("record_version").get<std::uint32_t>();
    turn->timestamp_ms = json.at("timestamp_ms").get<std::uint64_t>();
    turn->user_text = json.at("user_text").get<std::string>();
    turn->assistant_text =
        json.at("assistant_text").get<std::string>();
    turn->scene = json.at("scene").get<std::string>();
    if (!ParseExtractionState(
            json.at("extraction_state").get<std::string>(),
            &turn->extraction_state)) {
      return false;
    }
    turn->extraction_batch_id =
        json.at("extraction_batch_id").get<std::string>();
    return IsValidTurn(*turn);
  } catch (...) {
    return false;
  }
}

std::string Serialize(const ConversationJournalSnapshot& snapshot,
                      MemoryError* error) {
  try {
    nlohmann::json document{
        {"format", "vehicle-conversation-journal"},
        {"schema_version", 1},
        {"revision", snapshot.revision},
        {"turns", nlohmann::json::array()},
    };
    for (const auto& turn : snapshot.turns) {
      if (!IsValidTurn(turn)) {
        *error = Corrupted();
        return {};
      }
      document["turns"].push_back(TurnToJson(turn));
    }
    document["checksum"] = Sha256Hex(document.dump());
    return document.dump(2);
  } catch (...) {
    *error = CommitFailed();
    return {};
  }
}

struct DecodeResult {
  ConversationJournalSnapshot snapshot;
  std::string checksum;
  MemoryError error;
};

DecodeResult Deserialize(const std::string& bytes) {
  DecodeResult result;
  try {
    auto document = nlohmann::json::parse(bytes);
    if (!document.is_object() ||
        document.at("format") != "vehicle-conversation-journal" ||
        document.at("schema_version") != 1 ||
        !document.at("revision").is_number_unsigned() ||
        !document.at("turns").is_array() ||
        !document.at("checksum").is_string()) {
      result.error = Corrupted();
      return result;
    }
    const auto expected = document.at("checksum").get<std::string>();
    document.erase("checksum");
    if (Sha256Hex(document.dump()) != expected) {
      result.error = Corrupted();
      return result;
    }
    result.checksum = expected;
    result.snapshot.revision =
        document.at("revision").get<std::uint64_t>();
    std::set<std::tuple<std::string, std::string, std::uint64_t,
                        std::uint32_t>>
        identities;
    for (const auto& encoded : document.at("turns")) {
      ConversationTurn turn;
      if (!TurnFromJson(encoded, &turn) ||
          !identities.emplace(turn.user_id, turn.session_id, turn.turn_id,
                              turn.record_version)
               .second) {
        result.error = Corrupted();
        result.snapshot = {};
        return result;
      }
      result.snapshot.turns.push_back(std::move(turn));
    }
    return result;
  } catch (...) {
    result.error = Corrupted();
    return result;
  }
}

struct FileLoad {
  bool exists = false;
  bool valid = false;
  bool io_error = false;
  std::string bytes;
  std::string checksum;
  ConversationJournalSnapshot snapshot;
};

FileLoad LoadFile(const std::filesystem::path& path) {
  FileLoad loaded;
  try {
    std::error_code file_error;
    loaded.exists = std::filesystem::exists(path, file_error);
    if (file_error) {
      loaded.io_error = true;
      return loaded;
    }
    if (!loaded.exists) {
      return loaded;
    }
    const auto is_regular =
        std::filesystem::is_regular_file(path, file_error);
    if (file_error) {
      loaded.io_error = true;
      return loaded;
    }
    if (!is_regular) {
      return loaded;
    }
    const auto size = std::filesystem::file_size(path, file_error);
    if (file_error) {
      loaded.io_error = true;
      return loaded;
    }
    if (size > kMaximumJournalBytes) {
      return loaded;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
      return loaded;
    }
    loaded.bytes.assign(std::istreambuf_iterator<char>(input),
                        std::istreambuf_iterator<char>());
    if (input.bad() || loaded.bytes.size() != size) {
      loaded.bytes.clear();
      return loaded;
    }
    auto decoded = Deserialize(loaded.bytes);
    if (decoded.error) {
      return loaded;
    }
    loaded.valid = true;
    loaded.checksum = std::move(decoded.checksum);
    loaded.snapshot = std::move(decoded.snapshot);
    return loaded;
  } catch (...) {
    loaded.io_error = true;
    return loaded;
  }
}

bool ContainsParentTraversal(const std::filesystem::path& path) {
  return std::any_of(path.begin(), path.end(), [](const auto& component) {
    return component == "..";
  });
}

bool IsContainedBy(const std::filesystem::path& root,
                   const std::filesystem::path& candidate) {
  auto root_iterator = root.begin();
  auto candidate_iterator = candidate.begin();
  for (; root_iterator != root.end();
       ++root_iterator, ++candidate_iterator) {
    if (candidate_iterator == candidate.end() ||
        *candidate_iterator != *root_iterator) {
      return false;
    }
  }
  return candidate_iterator != candidate.end();
}

bool ResolveTarget(const std::filesystem::path& configured_directory,
                   const std::filesystem::path& relative_file_name,
                   std::filesystem::path* root,
                   std::filesystem::path* target) {
  try {
    if (configured_directory.empty() || relative_file_name.empty() ||
        relative_file_name.is_absolute() ||
        relative_file_name.has_root_name() ||
        relative_file_name.has_root_directory() ||
        ContainsParentTraversal(relative_file_name)) {
      return false;
    }
    std::error_code error;
    auto resolved_root =
        std::filesystem::weakly_canonical(configured_directory, error);
    if (error || !resolved_root.is_absolute() ||
        !std::filesystem::is_directory(resolved_root, error) || error) {
      return false;
    }
    const auto root_status =
        std::filesystem::symlink_status(resolved_root, error);
    if (error || std::filesystem::is_symlink(root_status)) {
      return false;
    }
    auto resolved_target =
        (resolved_root / relative_file_name).lexically_normal();
    if (!IsContainedBy(resolved_root, resolved_target) ||
        resolved_target == resolved_root ||
        resolved_target.parent_path() != resolved_root) {
      return false;
    }
    if (std::filesystem::exists(resolved_target, error)) {
      if (error ||
          std::filesystem::is_symlink(
              std::filesystem::symlink_status(resolved_target, error)) ||
          error) {
        return false;
      }
    } else if (error) {
      return false;
    }
    *root = std::move(resolved_root);
    *target = std::move(resolved_target);
    return true;
  } catch (...) {
    return false;
  }
}

std::string BatchIdFor(const std::string& user_id,
                       const std::vector<ConversationTurn>& turns) {
  nlohmann::json identity{
      {"user_id", user_id},
      {"turns", nlohmann::json::array()},
  };
  for (const auto& turn : turns) {
    identity["turns"].push_back(
        {{"session_id", turn.session_id},
         {"turn_id", turn.turn_id},
         {"record_version", turn.record_version},
         {"timestamp_ms", turn.timestamp_ms},
         {"user_text", turn.user_text},
         {"assistant_text", turn.assistant_text}});
  }
  return "batch-" + Sha256Hex(identity.dump()).substr(0, 24);
}

class JsonConversationJournal final : public ConversationJournal {
 public:
  JsonConversationJournal(std::filesystem::path data_directory,
                          std::filesystem::path relative_file_name,
                          std::shared_ptr<AtomicFileWriter> writer)
      : configured_data_directory_(std::move(data_directory)),
        relative_file_name_(std::move(relative_file_name)),
        writer_(std::move(writer)) {
    path_is_safe_ =
        ResolveTarget(configured_data_directory_, relative_file_name_,
                      &data_directory_, &path_);
  }

  ConversationJournalLoadResult Load() override {
    std::unique_lock lock(mutex_);
    ConversationJournalLoadResult result;
    if (!PathIsSafe() || writer_ == nullptr) {
      result.error =
          Error(MemoryErrorCode::kConfigurationError,
                "conversation journal configuration is invalid");
      return result;
    }
    const auto main = LoadFile(path_);
    auto backup_path = path_;
    backup_path += ".bak";
    if (main.valid) {
      snapshot_ = main.snapshot;
      result.snapshot = snapshot_;
      return result;
    }
    const auto backup = LoadFile(backup_path);
    if (!main.exists && !backup.exists) {
      snapshot_ = {};
      result.snapshot = snapshot_;
      return result;
    }
    if (!backup.valid) {
      result.error = Corrupted();
      return result;
    }
    const auto restored = writer_->Replace(
        path_, backup.bytes, AtomicWriteMode::kRestorePreservingBackup);
    if (restored) {
      result.error = CommitFailed();
      return result;
    }
    const auto verified = LoadFile(path_);
    if (!verified.valid || verified.bytes != backup.bytes) {
      result.error = CommitFailed();
      return result;
    }
    snapshot_ = backup.snapshot;
    result.snapshot = snapshot_;
    result.recovered_from_backup = true;
    return result;
  }

  ConversationJournalSnapshot Snapshot() const override {
    std::shared_lock lock(mutex_);
    return snapshot_;
  }

  ConversationJournalInspection InspectPersisted() const override {
    std::shared_lock lock(mutex_);
    ConversationJournalInspection result;
    result.file_name = relative_file_name_.generic_string();
    if (!PathIsSafe()) {
      result.error =
          Error(MemoryErrorCode::kConfigurationError,
                "conversation journal configuration is invalid");
      return result;
    }
    const auto loaded = LoadFile(path_);
    if (loaded.io_error) {
      result.error = Corrupted();
      return result;
    }
    if (!loaded.exists) {
      if (snapshot_.revision != 0U || !snapshot_.turns.empty()) {
        result.error = Corrupted();
      }
      return result;
    }
    if (!loaded.valid) {
      result.error = Corrupted();
      return result;
    }
    result.exists = true;
    result.file_bytes = loaded.bytes.size();
    result.checksum = loaded.checksum;
    result.snapshot = loaded.snapshot;
    return result;
  }

  AppendConversationTurnResult Append(
      const ConversationTurn& incoming) override {
    std::unique_lock lock(mutex_);
    AppendConversationTurnResult result;
    result.revision = snapshot_.revision;
    if (!PathIsSafe() || writer_ == nullptr) {
      result.error =
          Error(MemoryErrorCode::kConfigurationError,
                "conversation journal configuration is invalid");
      return result;
    }
    ConversationTurn turn = incoming;
    turn.extraction_state = ExtractionState::kPending;
    turn.extraction_batch_id.clear();
    if (!IsValidTurn(turn)) {
      result.error = InvalidRequest("conversation turn is invalid");
      return result;
    }
    const auto exact = std::find_if(
        snapshot_.turns.begin(), snapshot_.turns.end(),
        [&turn](const ConversationTurn& current) {
          return SameIdentity(current, turn);
        });
    if (exact != snapshot_.turns.end()) {
      if (SameRawContent(*exact, turn)) {
        result.idempotent = true;
      } else {
        result.error =
            Error(MemoryErrorCode::kRevisionConflict,
                  "conversation turn identity already has different content");
      }
      return result;
    }
    const auto newer = std::find_if(
        snapshot_.turns.begin(), snapshot_.turns.end(),
        [&turn](const ConversationTurn& current) {
          return current.user_id == turn.user_id &&
                 current.session_id == turn.session_id &&
                 current.turn_id == turn.turn_id &&
                 current.record_version > turn.record_version;
        });
    if (newer != snapshot_.turns.end()) {
      result.error =
          Error(MemoryErrorCode::kRevisionConflict,
                "newer conversation turn version already exists");
      return result;
    }

    auto next = snapshot_;
    for (auto& current : next.turns) {
      if (current.user_id == turn.user_id &&
          current.session_id == turn.session_id &&
          current.turn_id == turn.turn_id &&
          current.record_version < turn.record_version &&
          current.extraction_state == ExtractionState::kPending) {
        current.extraction_state = ExtractionState::kSuperseded;
        current.extraction_batch_id.clear();
      }
    }
    next.turns.push_back(std::move(turn));
    ++next.revision;
    const auto persisted = Persist(next);
    if (persisted) {
      result.error = persisted;
      return result;
    }
    snapshot_ = std::move(next);
    result.appended = true;
    result.revision = snapshot_.revision;
    return result;
  }

  RecentConversationResult Recent(
      const std::string& user_id, const std::string& session_id,
      std::size_t max_turns) const override {
    std::shared_lock lock(mutex_);
    RecentConversationResult result;
    if (user_id.empty() || max_turns == 0U) {
      result.error = InvalidRequest("recent conversation query is invalid");
      return result;
    }
    for (const auto& turn : snapshot_.turns) {
      if (turn.user_id == user_id &&
          (session_id.empty() || turn.session_id == session_id) &&
          turn.extraction_state != ExtractionState::kSuperseded) {
        result.turns.push_back(turn);
      }
    }
    std::sort(result.turns.begin(), result.turns.end(),
              ChronologicalLess);
    if (result.turns.size() > max_turns) {
      result.turns.erase(
          result.turns.begin(),
          result.turns.end() -
              static_cast<std::ptrdiff_t>(max_turns));
    }
    return result;
  }

  PreparePendingBatchResult PreparePending(
      const std::string& user_id, std::size_t max_turns) const override {
    std::shared_lock lock(mutex_);
    PreparePendingBatchResult result;
    if (user_id.empty() || max_turns == 0U) {
      result.error = InvalidRequest("pending extraction query is invalid");
      return result;
    }
    result.batch.user_id = user_id;
    for (const auto& turn : snapshot_.turns) {
      if (turn.user_id == user_id &&
          turn.extraction_state == ExtractionState::kPending) {
        result.batch.turns.push_back(turn);
      }
    }
    std::sort(result.batch.turns.begin(), result.batch.turns.end(),
              ChronologicalLess);
    if (result.batch.turns.size() > max_turns) {
      result.batch.turns.resize(max_turns);
    }
    if (!result.batch.turns.empty()) {
      result.batch.batch_id =
          BatchIdFor(user_id, result.batch.turns);
    }
    return result;
  }

  MarkExtractionProcessedResult MarkProcessed(
      const PendingExtractionBatch& batch) override {
    std::unique_lock lock(mutex_);
    MarkExtractionProcessedResult result;
    result.revision = snapshot_.revision;
    if (batch.user_id.empty() || batch.turns.empty() ||
        batch.batch_id != BatchIdFor(batch.user_id, batch.turns)) {
      result.error = InvalidRequest("extraction batch is invalid");
      return result;
    }

    bool all_already_processed = true;
    for (const auto& source : batch.turns) {
      const auto found = std::find_if(
          snapshot_.turns.begin(), snapshot_.turns.end(),
          [&source](const ConversationTurn& current) {
            return SameIdentity(current, source);
          });
      if (found == snapshot_.turns.end() ||
          !SameRawContent(*found, source)) {
        result.error =
            Error(MemoryErrorCode::kRevisionConflict,
                  "extraction source changed before commit", true);
        return result;
      }
      if (found->extraction_state == ExtractionState::kProcessed &&
          found->extraction_batch_id == batch.batch_id) {
        continue;
      }
      all_already_processed = false;
      if (found->extraction_state != ExtractionState::kPending) {
        result.error =
            Error(MemoryErrorCode::kRevisionConflict,
                  "extraction source is no longer pending", true);
        return result;
      }
    }
    if (all_already_processed) {
      result.idempotent = true;
      return result;
    }

    auto next = snapshot_;
    for (const auto& source : batch.turns) {
      auto found = std::find_if(
          next.turns.begin(), next.turns.end(),
          [&source](const ConversationTurn& current) {
            return SameIdentity(current, source);
          });
      found->extraction_state = ExtractionState::kProcessed;
      found->extraction_batch_id = batch.batch_id;
    }
    ++next.revision;
    const auto persisted = Persist(next);
    if (persisted) {
      result.error = persisted;
      return result;
    }
    snapshot_ = std::move(next);
    result.committed = true;
    result.revision = snapshot_.revision;
    return result;
  }

  ConversationJournalState State(
      const std::string& user_id) const override {
    std::shared_lock lock(mutex_);
    ConversationJournalState state;
    state.revision = snapshot_.revision;
    for (const auto& turn : snapshot_.turns) {
      if (!user_id.empty() && turn.user_id != user_id) {
        continue;
      }
      ++state.total_turns;
      switch (turn.extraction_state) {
        case ExtractionState::kPending:
          ++state.pending_turns;
          break;
        case ExtractionState::kProcessed:
          ++state.processed_turns;
          break;
        case ExtractionState::kSuperseded:
          ++state.superseded_turns;
          break;
      }
    }
    std::error_code error;
    if (std::filesystem::exists(path_, error) && !error) {
      state.file_bytes = std::filesystem::file_size(path_, error);
      if (error) {
        state.file_bytes = 0;
      }
    }
    return state;
  }

 private:
  bool PathIsSafe() const {
    std::filesystem::path checked_root;
    std::filesystem::path checked_target;
    return path_is_safe_ &&
           ResolveTarget(configured_data_directory_, relative_file_name_,
                         &checked_root, &checked_target) &&
           checked_root == data_directory_ && checked_target == path_;
  }

  MemoryError Persist(
      const ConversationJournalSnapshot& snapshot) const {
    MemoryError serialization_error;
    const auto bytes = Serialize(snapshot, &serialization_error);
    if (serialization_error || bytes.empty()) {
      return serialization_error ? serialization_error : CommitFailed();
    }
    const auto persisted = writer_->Replace(
        path_, bytes, AtomicWriteMode::kCommitWithBackup);
    return persisted ? CommitFailed() : MemoryError{};
  }

  std::filesystem::path configured_data_directory_;
  std::filesystem::path relative_file_name_;
  std::filesystem::path data_directory_;
  std::filesystem::path path_;
  std::shared_ptr<AtomicFileWriter> writer_;
  mutable std::shared_mutex mutex_;
  ConversationJournalSnapshot snapshot_;
  bool path_is_safe_ = false;
};

}  // namespace

std::shared_ptr<ConversationJournal> CreateJsonConversationJournal(
    std::filesystem::path data_directory,
    std::filesystem::path relative_file_name,
    std::shared_ptr<AtomicFileWriter> writer) {
  return std::make_shared<JsonConversationJournal>(
      std::move(data_directory), std::move(relative_file_name),
      std::move(writer));
}

}  // namespace vehicle_memory

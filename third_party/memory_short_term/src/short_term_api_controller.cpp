#include "vehicle_memory/short_term_api_controller.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace vehicle_memory {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kDefaultRecentTurns = 3U;
constexpr std::size_t kMaximumRecentTurns = 100U;

std::string RequestId() {
  static std::atomic<std::uint64_t> sequence{0U};
  std::ostringstream output;
  output << "short-term-" << sequence.fetch_add(1U);
  return output.str();
}

ShortTermApiResponse Failure(int status, const std::string& code,
                             const std::string& message,
                             bool retryable = false) {
  return {
      status,
      {{"success", false},
       {"error",
        {{"code", code},
         {"message", message},
         {"request_id", RequestId()},
         {"retryable", retryable}}}},
  };
}

ShortTermApiResponse FromStorageError(const MemoryError& error) {
  return Failure(error.retryable ? 503 : 500, "storage_error",
                 error.message.empty() ? "short-term storage failed"
                                       : error.message,
                 error.retryable);
}

bool ParseObject(const std::string& text, Json* body) {
  try {
    *body = Json::parse(text);
    return body->is_object();
  } catch (...) {
    return false;
  }
}

bool HasOnly(const Json& body, const std::set<std::string>& allowed) {
  for (auto item = body.begin(); item != body.end(); ++item) {
    if (allowed.count(item.key()) == 0U) return false;
  }
  return true;
}

bool ReadRequiredString(const Json& body, const char* key,
                        std::string* value) {
  const auto found = body.find(key);
  if (found == body.end() || !found->is_string()) return false;
  *value = found->get<std::string>();
  return !value->empty();
}

template <typename Integer>
bool ReadRequiredUnsigned(const Json& body, const char* key, Integer* value) {
  const auto found = body.find(key);
  if (found == body.end() || !found->is_number_unsigned()) return false;
  const auto raw = found->get<std::uint64_t>();
  if (raw == 0U ||
      raw > static_cast<std::uint64_t>(
                std::numeric_limits<Integer>::max())) {
    return false;
  }
  *value = static_cast<Integer>(raw);
  return true;
}

bool ReadMaxTurns(const Json& body, std::size_t* max_turns) {
  const auto found = body.find("max_turns");
  if (found == body.end()) {
    *max_turns = kDefaultRecentTurns;
    return true;
  }
  if (!found->is_number_unsigned()) return false;
  const auto raw = found->get<std::uint64_t>();
  if (raw == 0U || raw > kMaximumRecentTurns) return false;
  *max_turns = static_cast<std::size_t>(raw);
  return true;
}

const char* ExtractionStateText(ExtractionState state) {
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

Json TurnJson(const ConversationTurn& turn) {
  return {
      {"user_id", turn.user_id},
      {"session_id", turn.session_id},
      {"turn_id", turn.turn_id},
      {"record_version", turn.record_version},
      {"timestamp_ms", turn.timestamp_ms},
      {"user_text", turn.user_text},
      {"assistant_text", turn.assistant_text},
      {"scene", turn.scene},
      {"extraction_state", ExtractionStateText(turn.extraction_state)},
      {"extraction_batch_id", turn.extraction_batch_id},
  };
}

ShortTermApiResponse Append(
    const std::shared_ptr<ConversationJournal>& journal, const Json& body,
    bool facade) {
  static const std::set<std::string> kRawFields{
      "user_id",       "session_id",   "turn_id", "record_version",
      "timestamp_ms",  "user_text",    "assistant_text", "scene"};
  static const std::set<std::string> kFacadeFields{
      "user_id",      "session_id",      "turn_id", "record_version",
      "event_time",   "user_input",      "assistant_output", "scene"};
  if (!HasOnly(body, facade ? kFacadeFields : kRawFields)) {
    return Failure(400, "invalid_request", "request contains unknown fields");
  }

  ConversationTurn turn;
  const char* timestamp_key = facade ? "event_time" : "timestamp_ms";
  const char* user_text_key = facade ? "user_input" : "user_text";
  const char* assistant_text_key =
      facade ? "assistant_output" : "assistant_text";
  if (!ReadRequiredString(body, "user_id", &turn.user_id) ||
      !ReadRequiredString(body, "session_id", &turn.session_id) ||
      !ReadRequiredUnsigned(body, "turn_id", &turn.turn_id) ||
      !ReadRequiredUnsigned(body, timestamp_key, &turn.timestamp_ms) ||
      !ReadRequiredString(body, user_text_key, &turn.user_text) ||
      !ReadRequiredString(body, assistant_text_key, &turn.assistant_text)) {
    return Failure(400, "invalid_request",
                   "writeMemory request is incomplete or invalid");
  }
  const auto version = body.find("record_version");
  if (version != body.end()) {
    if (!ReadRequiredUnsigned(body, "record_version",
                              &turn.record_version)) {
      return Failure(400, "invalid_request", "record_version is invalid");
    }
  }
  const auto scene = body.find("scene");
  if (scene != body.end()) {
    if (!scene->is_string() || scene->get<std::string>().empty()) {
      return Failure(400, "invalid_request", "scene is invalid");
    }
    turn.scene = scene->get<std::string>();
  }

  const auto appended = journal->Append(turn);
  if (appended.error) return FromStorageError(appended.error);
  Json response{
      {"success", true},
      {"revision", appended.revision},
      {"idempotent", appended.idempotent},
  };
  if (facade) {
    response["memory_id"] = turn.user_id + ":" + turn.session_id + ":" +
                            std::to_string(turn.turn_id) + ":" +
                            std::to_string(turn.record_version);
    response["merged"] = appended.idempotent;
  } else {
    response["appended"] = appended.appended;
  }
  return {200, std::move(response)};
}

ShortTermApiResponse Recent(
    const std::shared_ptr<ConversationJournal>& journal, const Json& body,
    bool facade) {
  static const std::set<std::string> kRawFields{
      "user_id", "session_id", "max_turns"};
  static const std::set<std::string> kFacadeFields{
      "user_id", "session_id", "scene", "query", "token_budget",
      "max_turns"};
  if (!HasOnly(body, facade ? kFacadeFields : kRawFields)) {
    return Failure(400, "invalid_request", "request contains unknown fields");
  }
  std::string user_id;
  std::string session_id;
  std::size_t max_turns = 0U;
  if (!ReadRequiredString(body, "user_id", &user_id) ||
      !ReadRequiredString(body, "session_id", &session_id) ||
      !ReadMaxTurns(body, &max_turns)) {
    return Failure(400, "invalid_request",
                   "getContext request is incomplete or invalid");
  }
  const auto recent = journal->Recent(user_id, session_id, max_turns);
  if (recent.error) return FromStorageError(recent.error);

  Json turns = Json::array();
  Json blocks = Json::array();
  for (const auto& turn : recent.turns) {
    turns.push_back(TurnJson(turn));
    if (facade) {
      blocks.push_back(
          {{"content", "USER_ORIGINAL: " + turn.user_text +
                           "\nVEHICLE_REPLY: " + turn.assistant_text},
           {"source_memory_id",
            "short-term:" + turn.user_id + ":" + turn.session_id + ":" +
                std::to_string(turn.turn_id) + ":" +
                std::to_string(turn.record_version)},
           {"relevance_score", 1.0},
           {"memory_type", "short_term"},
           {"session_id", turn.session_id},
           {"turn_id", turn.turn_id},
           {"timestamp_ms", turn.timestamp_ms}});
    }
  }
  if (facade) {
    return {200,
            {{"success", true},
             {"context_blocks", std::move(blocks)},
             {"turn_count", recent.turns.size()}}};
  }
  return {200,
          {{"success", true},
           {"turns", std::move(turns)},
           {"count", recent.turns.size()}}};
}

}  // namespace

ShortTermApiController::ShortTermApiController(
    std::shared_ptr<ConversationJournal> journal)
    : journal_(std::move(journal)) {}

ShortTermApiResponse ShortTermApiController::Handle(
    const ShortTermApiRequest& request) const {
  if (!journal_) {
    return Failure(503, "configuration_error",
                   "short-term journal is not configured");
  }
  if (request.path == "/api/health") {
    if (request.method != "GET") {
      return Failure(405, "method_not_allowed", "GET is required");
    }
    const auto state = journal_->State();
    return {200,
            {{"success", true},
             {"status", "ready"},
             {"mode", "short_term_only"},
             {"storage",
              {{"status", "ready"},
               {"revision", state.revision},
               {"file_bytes", state.file_bytes}}}}};
  }

  if (request.path == "/api/conversation/state") {
    if (request.method != "GET") {
      return Failure(405, "method_not_allowed", "GET is required");
    }
    const auto found = request.query.find("user_id");
    if (found == request.query.end() || found->second.empty() ||
        request.query.size() != 1U) {
      return Failure(400, "invalid_request", "user_id query is required");
    }
    const auto state = journal_->State(found->second);
    return {200,
            {{"success", true},
             {"revision", state.revision},
             {"total_turns", state.total_turns},
             {"pending_turns", state.pending_turns},
             {"processed_turns", state.processed_turns},
             {"superseded_turns", state.superseded_turns},
             {"file_bytes", state.file_bytes}}};
  }

  if (request.path == "/api/conversation/journal") {
    if (request.method != "GET") {
      return Failure(405, "method_not_allowed", "GET is required");
    }
    if (request.query.size() > 2U ||
        std::any_of(request.query.begin(), request.query.end(),
                    [](const auto& item) {
                      return item.first != "user_id" &&
                             item.first != "session_id";
                    })) {
      return Failure(400, "invalid_request", "journal query is invalid");
    }
    const auto user = request.query.find("user_id");
    const auto session = request.query.find("session_id");
    if (session != request.query.end() &&
        (user == request.query.end() || user->second.empty())) {
      return Failure(400, "invalid_request",
                     "session_id requires user_id");
    }
    const auto inspected = journal_->InspectPersisted();
    if (inspected.error) return FromStorageError(inspected.error);
    Json all_turns = Json::array();
    Json matching = Json::array();
    for (const auto& turn : inspected.snapshot.turns) {
      const auto serialized = TurnJson(turn);
      all_turns.push_back(serialized);
      const bool user_matches =
          user == request.query.end() || turn.user_id == user->second;
      const bool session_matches =
          session == request.query.end() ||
          turn.session_id == session->second;
      if (user_matches && session_matches) matching.push_back(serialized);
    }
    return {200,
            {{"success", true},
             {"exists", inspected.exists},
             {"file_name", inspected.file_name},
             {"file_bytes", inspected.file_bytes},
             {"document",
              {{"format", inspected.format},
               {"schema_version", inspected.schema_version},
               {"revision", inspected.snapshot.revision},
               {"checksum", inspected.checksum},
               {"turns", std::move(all_turns)}}},
             {"matching_count", matching.size()},
             {"matching_turns", std::move(matching)}}};
  }

  const bool is_write = request.path == "/api/memory/write" ||
                        request.path == "/api/conversation/append";
  const bool is_context = request.path == "/api/memory/context" ||
                          request.path == "/api/conversation/recent";
  if (is_write || is_context) {
    if (request.method != "POST") {
      return Failure(405, "method_not_allowed", "POST is required");
    }
    Json body;
    if (!ParseObject(request.body, &body)) {
      return Failure(400, "invalid_json", "request body must be a JSON object");
    }
    if (is_write) {
      return Append(journal_, body, request.path == "/api/memory/write");
    }
    return Recent(journal_, body, request.path == "/api/memory/context");
  }

  return Failure(404, "not_found", "API route was not found");
}

}  // namespace vehicle_memory

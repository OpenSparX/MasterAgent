#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "test_support.h"
#include "vehicle_memory/atomic_file_writer.h"
#include "vehicle_memory/conversation_journal.h"
#include "vehicle_memory/short_term_api_controller.h"

namespace {

using vehicle_memory::CreateJsonConversationJournal;
using vehicle_memory::ShortTermApiController;
using vehicle_memory::ShortTermApiRequest;
using vehicle_memory::test_support::Expect;

class ScopedTempDirectory {
 public:
  ScopedTempDirectory() {
    static std::atomic<std::uint64_t> sequence{0U};
    const auto token = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    path_ = std::filesystem::temp_directory_path() /
            ("short-term-api-" + std::to_string(token) + "-" +
             std::to_string(sequence.fetch_add(1U)));
    std::filesystem::create_directories(path_);
  }

  ~ScopedTempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

ShortTermApiRequest Post(std::string path, nlohmann::json body) {
  return {"POST", std::move(path), {}, body.dump()};
}

nlohmann::json WriteBody(std::uint64_t turn_id, std::string user_input) {
  return {
      {"user_id", "driver-a"},
      {"session_id", "session-a"},
      {"turn_id", turn_id},
      {"record_version", 1},
      {"event_time", 1785200000000ULL + turn_id},
      {"user_input", std::move(user_input)},
      {"assistant_output", u8"已收到"},
      {"scene", "cockpit"},
  };
}

void TestFacadeAndRawRoutes() {
  ScopedTempDirectory temp;
  auto journal = CreateJsonConversationJournal(
      temp.path(), "conversation_journal.json",
      vehicle_memory::CreateWindowsAtomicFileWriter());
  Expect(!journal->Load().error, "empty short-term journal must load");
  ShortTermApiController controller(journal);

  const auto health = controller.Handle({"GET", "/api/health", {}, {}});
  Expect(health.status == 200 && health.body.at("mode") == "short_term_only",
         "short-term health route failed");

  const auto first =
      controller.Handle(Post("/api/memory/write",
                             WriteBody(1U, u8"第一轮用户原话")));
  const auto second =
      controller.Handle(Post("/api/memory/write",
                             WriteBody(2U, u8"第二轮用户原话")));
  Expect(first.status == 200 && second.status == 200 &&
             first.body.at("success") == true &&
             second.body.at("revision") == 2U,
         "writeMemory facade did not append two turns");

  const auto replay =
      controller.Handle(Post("/api/memory/write",
                             WriteBody(2U, u8"第二轮用户原话")));
  Expect(replay.status == 200 && replay.body.at("idempotent") == true &&
             replay.body.at("revision") == 2U,
         "writeMemory facade is not idempotent");

  const auto context = controller.Handle(Post(
      "/api/memory/context",
      {{"user_id", "driver-a"},
       {"session_id", "session-a"},
       {"query", u8"继续刚才的话题"},
       {"scene", "cockpit"},
       {"token_budget", 256},
       {"max_turns", 1}}));
  Expect(context.status == 200 && context.body.at("turn_count") == 1U &&
             context.body.at("context_blocks").size() == 1U &&
             context.body.at("context_blocks")[0].at("memory_type") ==
                 "short_term" &&
             context.body.at("context_blocks")[0]
                     .at("content")
                     .get<std::string>()
                     .find(u8"第二轮用户原话") != std::string::npos,
         "getContext facade did not return only the newest short-term turn");

  const auto recent = controller.Handle(Post(
      "/api/conversation/recent",
      {{"user_id", "driver-a"},
       {"session_id", "session-a"},
       {"max_turns", 10}}));
  Expect(recent.status == 200 && recent.body.at("count") == 2U,
         "raw recent route failed");

  const auto state = controller.Handle(
      {"GET", "/api/conversation/state", {{"user_id", "driver-a"}}, {}});
  Expect(state.status == 200 && state.body.at("total_turns") == 2U &&
             state.body.at("pending_turns") == 2U,
         "short-term state route failed");

  const auto inspected = controller.Handle(
      {"GET",
       "/api/conversation/journal",
       {{"user_id", "driver-a"}, {"session_id", "session-a"}},
       {}});
  Expect(inspected.status == 200 &&
             inspected.body.at("matching_count") == 2U &&
             inspected.body.at("document").at("checksum")
                     .get<std::string>()
                     .size() == 64U,
         "persisted Journal inspection failed");
}

void TestValidationAndIsolation() {
  ScopedTempDirectory temp;
  auto journal = CreateJsonConversationJournal(
      temp.path(), "conversation_journal.json",
      vehicle_memory::CreateWindowsAtomicFileWriter());
  Expect(!journal->Load().error, "validation journal must load");
  ShortTermApiController controller(journal);

  const auto malformed =
      controller.Handle({"POST", "/api/memory/write", {}, "{"});
  Expect(malformed.status == 400, "malformed JSON was accepted");

  auto unknown = WriteBody(1U, u8"原话");
  unknown["unexpected"] = true;
  Expect(controller.Handle(Post("/api/memory/write", unknown)).status == 400,
         "unknown writeMemory field was accepted");

  Expect(controller.Handle({"GET", "/api/memory/write", {}, {}}).status ==
             405,
         "wrong method was accepted");
  Expect(controller.Handle({"GET", "/api/memory/recall", {}, {}}).status ==
             404,
         "long-term recall route must not exist");
}

}  // namespace

int main() {
  TestFacadeAndRawRoutes();
  TestValidationAndIsolation();
  return 0;
}

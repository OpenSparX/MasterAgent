#pragma once

#include <map>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "vehicle_memory/conversation_journal.h"

namespace vehicle_memory {

struct ShortTermApiRequest {
  std::string method;
  std::string path;
  std::map<std::string, std::string> query;
  std::string body;
};

struct ShortTermApiResponse {
  int status = 200;
  nlohmann::json body;
};

class ShortTermApiController {
 public:
  explicit ShortTermApiController(
      std::shared_ptr<ConversationJournal> journal);

  ShortTermApiResponse Handle(const ShortTermApiRequest& request) const;

 private:
  std::shared_ptr<ConversationJournal> journal_;
};

}  // namespace vehicle_memory

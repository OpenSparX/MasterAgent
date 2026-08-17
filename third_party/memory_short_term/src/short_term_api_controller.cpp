// short_term_api_controller.cpp — REST/gRPC API surface for short-term memory
#include "vehicle_memory/conversation_journal.h"
#include <string>
#include <memory>
#include <unordered_map>

namespace sparx::memory {

/// Manages multiple journal sessions via API calls
class ShortTermApiController {
    std::unordered_map<std::string, std::shared_ptr<ConversationJournal>> sessions_;

public:
    bool createSession(const std::string& session_id,
                       std::shared_ptr<ConversationJournal> journal) {
        if (sessions_.count(session_id)) return false;
        if (!journal->open(session_id)) return false;
        sessions_[session_id] = std::move(journal);
        return true;
    }

    bool closeSession(const std::string& session_id) {
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) return false;
        it->second->close();
        sessions_.erase(it);
        return true;
    }

    ConversationJournal* getSession(const std::string& session_id) {
        auto it = sessions_.find(session_id);
        return it != sessions_.end() ? it->second.get() : nullptr;
    }

    size_t activeSessionCount() const { return sessions_.size(); }
};

} // namespace sparx::memory

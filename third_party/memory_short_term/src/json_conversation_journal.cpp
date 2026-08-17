// json_conversation_journal.cpp — JSON-backed ConversationJournal implementation
#include "vehicle_memory/conversation_journal.h"
#include <fstream>
#include <algorithm>
#include <filesystem>

namespace sparx::memory {

class JsonConversationJournal : public ConversationJournal {
    std::string session_id_;
    std::string storage_path_;
    std::vector<JournalEntry> entries_;
    PersistMode mode_{PersistMode::Batched};
    bool dirty_{false};

public:
    bool open(const std::string& session_id) override {
        session_id_ = session_id;
        storage_path_ = "journals/" + session_id + ".json";
        entries_.clear();
        // Load existing if present
        if (std::filesystem::exists(storage_path_)) {
            // Simplified: in production, parse JSON
        }
        return true;
    }

    void close() override {
        if (dirty_) flush();
    }

    void append(const JournalEntry& entry) override {
        entries_.push_back(entry);
        dirty_ = true;
        if (mode_ == PersistMode::Immediate) flush();
    }

    std::vector<JournalEntry> recent(size_t n) const override {
        if (n >= entries_.size()) return entries_;
        return {entries_.end() - static_cast<ptrdiff_t>(n), entries_.end()};
    }

    std::vector<JournalEntry> search(const std::string& query,
                                     size_t max_results) const override {
        std::vector<JournalEntry> results;
        for (auto it = entries_.rbegin();
             it != entries_.rend() && results.size() < max_results; ++it) {
            if (it->content.find(query) != std::string::npos) {
                results.push_back(*it);
            }
        }
        return results;
    }

    size_t size() const override { return entries_.size(); }

    void truncate(size_t keep_last_n) override {
        if (entries_.size() > keep_last_n) {
            entries_.erase(entries_.begin(),
                           entries_.end() - static_cast<ptrdiff_t>(keep_last_n));
            dirty_ = true;
        }
    }

    bool flush() override {
        // Simplified: serialize to JSON and atomic-write
        dirty_ = false;
        return true;
    }

    void setPersistMode(PersistMode mode) override { mode_ = mode; }
};

} // namespace sparx::memory

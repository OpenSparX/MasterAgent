#pragma once
// conversation_journal.h — Short-term memory journal interface
// Part of the OpenSparX Agent OS memory subsystem

#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include <functional>

namespace sparx::memory {

/// A single journal entry representing one conversation turn
struct JournalEntry {
    uint64_t    sequence_id{0};
    std::string role;           // "user", "assistant", "system", "tool"
    std::string content;
    std::string model_id;       // which model produced this
    std::chrono::system_clock::time_point timestamp;
    double      importance{0.5};
    std::vector<std::string> tags;
};

/// Persistence mode for the journal
enum class PersistMode { Immediate, Batched, OnClose };

/// Abstract interface for conversation journals
class ConversationJournal {
public:
    virtual ~ConversationJournal() = default;

    virtual bool open(const std::string& session_id) = 0;
    virtual void close() = 0;

    virtual void append(const JournalEntry& entry) = 0;
    virtual std::vector<JournalEntry> recent(size_t n) const = 0;
    virtual std::vector<JournalEntry> search(const std::string& query,
                                             size_t max_results = 10) const = 0;

    virtual size_t size() const = 0;
    virtual void truncate(size_t keep_last_n) = 0;
    virtual bool flush() = 0;

    virtual void setPersistMode(PersistMode mode) = 0;
};

} // namespace sparx::memory

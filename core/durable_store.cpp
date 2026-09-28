#include "durable_store.h"
#include <filesystem>
#include <stdexcept>
#ifdef MASTER_AGENT_HAS_SQLITE
#include <sqlite3.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace master_agent::reference::detail {
#ifdef MASTER_AGENT_HAS_SQLITE
namespace {
constexpr int application_id = 0x4D415254; // MART, distinct from unrelated SQLite databases.
struct Statement {
    sqlite3_stmt* stmt = nullptr;
    sqlite3* db;
    Statement(sqlite3* connection, const char* sql) : db(connection) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(stmt); }
    void bind(int index, const std::string& value) {
        if (sqlite3_bind_text(stmt, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    bool next() {
        int code = sqlite3_step(stmt);
        if (code == SQLITE_ROW) return true;
        if (code != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
        return false;
    }
    std::string text(int column) const {
        const auto* bytes = sqlite3_column_text(stmt, column);
        if (!bytes) throw std::runtime_error("Unexpected NULL in store");
        return {reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(sqlite3_column_bytes(stmt, column))};
    }
};
void sql(sqlite3* db, const char* statement) {
    if (sqlite3_exec(db, statement, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
}
struct Transaction {
    sqlite3* db;
    bool committed = false;
    explicit Transaction(sqlite3* connection) : db(connection) { sql(db, "BEGIN IMMEDIATE"); }
    ~Transaction() { if (!committed) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { sql(db, "COMMIT"); committed = true; }
};
int scalar(sqlite3* db, const char* query) {
    Statement statement(db, query);
    if (!statement.next()) throw std::runtime_error("Missing store metadata");
    return sqlite3_column_int(statement.stmt, 0);
}
Json encode(const Result<Reply>& result) {
    if (result) {
        const auto& r = *result;
        return {{"ok", true}, {"session", r.session_id}, {"request", r.request_id},
                {"route", r.route}, {"tool", r.tool}, {"output", r.output}};
    }
    const auto e = result.error.value_or(StructuredError{"INTERNAL", "Missing result", "", 500});
    return {{"ok", false}, {"code", e.code}, {"message", e.message}, {"detail", e.detail}, {"http_status", e.http_status}};
}
Result<Reply> decode(const Json& value) {
    if (value.at("ok").get<bool>())
        return Result<Reply>::success({value.at("session").get<std::string>(), value.at("request").get<std::string>(),
            value.at("route").get<std::string>(), value.at("tool").get<std::string>(), value.at("output"), false});
    return Result<Reply>::failure({value.at("code").get<std::string>(), value.at("message").get<std::string>(),
        value.at("detail").get<std::string>(), value.at("http_status").get<int>()});
}
Result<Reply> unknown() {
    return Result<Reply>::failure({"UNKNOWN", "Interrupted request; reconcile the external outcome before retrying", "", 409});
}
std::string stateFor(const Result<Reply>& result) {
    if (result) return "COMMITTED";
    return result.error && result.error->code == "UNKNOWN" ? "UNKNOWN" : "FAILED";
}
void event(sqlite3* db, const Turn& turn, const std::string& kind, const std::string& note = "") {
    Statement statement(db, "INSERT INTO events(session_id,request_id,event,note) VALUES(?,?,?,?)");
    statement.bind(1, turn.session_id); statement.bind(2, turn.request_id);
    statement.bind(3, kind); statement.bind(4, note); statement.next();
}
Status error(const std::exception& e) { return Status::Error("STORAGE_ERROR", e.what()); }
}
struct DurableStore::Impl {
    sqlite3* db = nullptr;
    int lock_fd = -1;
    ~Impl() {
        if (db) sqlite3_close(db);
        if (lock_fd >= 0) { flock(lock_fd, LOCK_UN); close(lock_fd); }
    }
};
DurableStore::DurableStore() : impl_(std::make_unique<Impl>()) {}
DurableStore::~DurableStore() = default;
Status DurableStore::open(const std::string& path) {
    try {
        if (path.empty()) return Status::Error("INVALID_STORE", "Store path must not be empty");
        auto parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        // Keep an independent lock file: Apple's SQLite VFS can use flock on
        // the database itself. Canonical parents and rejected aliases keep the
        // ownership lock stable across relative paths and symlinked parents.
        const auto normalized = (std::filesystem::weakly_canonical(parent.empty() ? std::filesystem::path(".") : parent) /
            std::filesystem::path(path).filename()).string();
        impl_->lock_fd = ::open((normalized + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (impl_->lock_fd < 0) return Status::Error("STORAGE_ERROR", "Cannot open store ownership lock");
        struct stat info{};
        if (fstat(impl_->lock_fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1)
            return Status::Error("INVALID_STORE", "Ownership lock must be a regular unaliased file");
        if (flock(impl_->lock_fd, LOCK_EX | LOCK_NB) != 0)
            return Status::Error("STORAGE_BUSY", "Another runtime owns this store");
        const int db_fd = ::open(normalized.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (db_fd < 0) return Status::Error("STORAGE_ERROR", "Cannot open regular store file");
        const bool regular = fstat(db_fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_nlink == 1;
        close(db_fd);
        if (!regular) return Status::Error("INVALID_STORE", "Store must be a regular file without hard links");
        if (sqlite3_open_v2(normalized.c_str(), &impl_->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
            return Status::Error("STORAGE_ERROR", "Cannot open SQLite store");
        sqlite3_busy_timeout(impl_->db, 1000);
        const int id = scalar(impl_->db, "PRAGMA application_id");
        const int version = scalar(impl_->db, "PRAGMA user_version");
        if (id != application_id && !(id == 0 && version == 0 && scalar(impl_->db, "SELECT count(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'") == 0))
            return Status::Error("INVALID_STORE", "Not a MasterAgent request store");
        if ((id == application_id && version != 1) || (id == 0 && version != 0))
            return Status::Error("STORE_VERSION", "Unsupported store schema version");
        Statement check(impl_->db, "PRAGMA quick_check");
        if (!check.next() || check.text(0) != "ok") return Status::Error("STORAGE_ERROR", "Store integrity check failed");
        // Finish the read before changing journal mode.
        while (check.next()) {}
        if (fchmod(impl_->lock_fd, 0600) != 0 || chmod(normalized.c_str(), 0600) != 0) return Status::Error("STORAGE_ERROR", "Cannot restrict store permissions");
        sql(impl_->db, "PRAGMA journal_mode=WAL");
        sql(impl_->db, "PRAGMA synchronous=FULL");
        sql(impl_->db, "PRAGMA foreign_keys=ON");
        if (id == 0) {
            Transaction transaction(impl_->db);
            sql(impl_->db, "CREATE TABLE sessions(session_id TEXT PRIMARY KEY,history TEXT NOT NULL)");
            sql(impl_->db, "CREATE TABLE requests(session_id TEXT NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,request_id TEXT NOT NULL,input TEXT NOT NULL,state TEXT NOT NULL,result TEXT NOT NULL,tool TEXT NOT NULL DEFAULT '',arguments TEXT NOT NULL DEFAULT '{}',PRIMARY KEY(session_id,request_id))");
            sql(impl_->db, "CREATE TABLE events(sequence INTEGER PRIMARY KEY,session_id TEXT NOT NULL,request_id TEXT NOT NULL,event TEXT NOT NULL,note TEXT NOT NULL,created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
            sql(impl_->db, "PRAGMA application_id=1296126548");
            sql(impl_->db, "PRAGMA user_version=1");
            transaction.commit();
        }
        return Status::Ok();
    } catch (const std::exception& e) { return error(e); }
}
Result<Snapshot> DurableStore::load(const Limits& limits) {
    try {
        Snapshot snapshot;
        Transaction transaction(impl_->db);
        Statement sessions(impl_->db, "SELECT session_id,history FROM sessions");
        while (sessions.next()) {
            if (snapshot.histories.size() >= limits.sessions) throw std::runtime_error("Stored sessions exceed configured limit");
            auto id = sessions.text(0);
            auto history = Json::parse(sessions.text(1));
            if (id.empty() || id.size() > 256 || !history.is_array() || history.size() % 2 != 0)
                throw std::runtime_error("Invalid stored session");
            for (const auto& message : history)
                if (!message.is_object() || !message.contains("role") || !message["role"].is_string() ||
                    !message.contains("content") || !message["content"].is_string() ||
                    (message["role"] != "user" && message["role"] != "assistant"))
                    throw std::runtime_error("Invalid stored history");
            auto& target = snapshot.histories[id];
            for (const auto& message : history) target.push_back(message);
            while (target.size() / 2 > limits.history_turns) target.erase(target.begin(), target.begin() + 2);
        }
        std::map<std::string, std::size_t> counts;
        Statement requests(impl_->db, "SELECT session_id,request_id,input,state,result,tool,arguments FROM requests ORDER BY rowid");
        while (requests.next()) {
            StoredRequest r;
            r.turn = {requests.text(0), requests.text(1), requests.text(2)};
            r.state = requests.text(3); r.tool = requests.text(5); r.arguments = Json::parse(requests.text(6));
            if (!snapshot.histories.count(r.turn.session_id) || r.turn.request_id.empty() || r.turn.request_id.size() > 256 ||
                r.turn.input.empty() || r.turn.input.size() > 65536 || !r.arguments.is_object() ||
                ++counts[r.turn.session_id] > limits.requests_per_session)
                throw std::runtime_error("Invalid stored request or exceeded request limit");
            if (r.state == "STARTED" || r.state == "DISPATCHED") r.result = unknown();
            else {
                r.result = decode(Json::parse(requests.text(4)));
                if (stateFor(r.result) != r.state || (r.result && (r.result.value->session_id != r.turn.session_id || r.result.value->request_id != r.turn.request_id)))
                    throw std::runtime_error("Inconsistent stored outcome");
            }
            snapshot.requests.push_back(std::move(r));
        }
        for (auto& r : snapshot.requests) {
            if (r.state != "STARTED" && r.state != "DISPATCHED") continue;
            Statement update(impl_->db, "UPDATE requests SET state='UNKNOWN',result=? WHERE session_id=? AND request_id=?");
            update.bind(1, encode(r.result).dump()); update.bind(2, r.turn.session_id); update.bind(3, r.turn.request_id); update.next();
            event(impl_->db, r.turn, "RECOVERED_UNKNOWN"); r.state = "UNKNOWN";
        }
        transaction.commit();
        return Result<Snapshot>::success(std::move(snapshot));
    } catch (const std::exception& e) { return Result<Snapshot>::failure({"STORAGE_ERROR", e.what(), "", 500}); }
}
Status DurableStore::begin(const Turn& turn) {
    try {
        Transaction transaction(impl_->db);
        Statement session(impl_->db, "INSERT OR IGNORE INTO sessions(session_id,history) VALUES(?,'[]')");
        session.bind(1, turn.session_id); session.next();
        Statement request(impl_->db, "INSERT INTO requests(session_id,request_id,input,state,result) VALUES(?,?,?,'STARTED','{}')");
        request.bind(1, turn.session_id); request.bind(2, turn.request_id); request.bind(3, turn.input); request.next();
        event(impl_->db, turn, "STARTED"); transaction.commit(); return Status::Ok();
    } catch (const std::exception& e) { return error(e); }
}
Status DurableStore::dispatch(const Turn& turn, const std::string& tool, const Json& arguments) {
    try {
        Transaction transaction(impl_->db);
        Statement update(impl_->db, "UPDATE requests SET state='DISPATCHED',tool=?,arguments=? WHERE session_id=? AND request_id=? AND state='STARTED'");
        update.bind(1, tool); update.bind(2, arguments.dump()); update.bind(3, turn.session_id); update.bind(4, turn.request_id); update.next();
        if (sqlite3_changes(impl_->db) != 1) throw std::runtime_error("Invalid dispatch transition");
        event(impl_->db, turn, "DISPATCHED"); transaction.commit(); return Status::Ok();
    } catch (const std::exception& e) { return error(e); }
}
Status DurableStore::complete(const Turn& turn, const Result<Reply>& result,
                              const std::vector<Json>& history, const std::string& note) {
    try {
        Transaction transaction(impl_->db);
        Statement update(impl_->db, "UPDATE requests SET state=?,result=? WHERE session_id=? AND request_id=? AND state IN ('STARTED','DISPATCHED','UNKNOWN')");
        update.bind(1, stateFor(result)); update.bind(2, encode(result).dump());
        update.bind(3, turn.session_id); update.bind(4, turn.request_id); update.next();
        if (sqlite3_changes(impl_->db) != 1) throw std::runtime_error("Invalid completion transition");
        Statement session(impl_->db, "UPDATE sessions SET history=? WHERE session_id=?");
        session.bind(1, Json(history).dump()); session.bind(2, turn.session_id); session.next();
        event(impl_->db, turn, note.empty() ? stateFor(result) : "RECONCILED_" + stateFor(result), note);
        transaction.commit(); return Status::Ok();
    } catch (const std::exception& e) { return error(e); }
}
Status DurableStore::clear(const std::string& id) {
    try {
        Transaction transaction(impl_->db);
        Statement pending(impl_->db, "SELECT 1 FROM requests WHERE session_id=? AND state IN ('STARTED','DISPATCHED','UNKNOWN')");
        pending.bind(1, id);
        if (pending.next()) return Status::Error("UNRESOLVED_REQUESTS", "Reconcile unknown outcomes before clearing the session");
        Statement remove(impl_->db, "DELETE FROM sessions WHERE session_id=?"); remove.bind(1, id); remove.next();
        event(impl_->db, {id, "", ""}, "SESSION_CLEARED"); transaction.commit(); return Status::Ok();
    } catch (const std::exception& e) { return error(e); }
}
#else
struct DurableStore::Impl {};
DurableStore::DurableStore() : impl_(std::make_unique<Impl>()) {}
DurableStore::~DurableStore() = default;
static Status disabled() { return Status::Error("STORAGE_UNAVAILABLE", "Build with MASTER_AGENT_ENABLE_STORAGE=ON"); }
Status DurableStore::open(const std::string&) { return disabled(); }
Result<Snapshot> DurableStore::load(const Limits&) { return Result<Snapshot>::failure({"STORAGE_UNAVAILABLE", "Storage disabled", "", 500}); }
Status DurableStore::begin(const Turn&) { return disabled(); }
Status DurableStore::dispatch(const Turn&, const std::string&, const Json&) { return disabled(); }
Status DurableStore::complete(const Turn&, const Result<Reply>&, const std::vector<Json>&, const std::string&) { return disabled(); }
Status DurableStore::clear(const std::string&) { return disabled(); }
#endif
}  // namespace master_agent::reference::detail

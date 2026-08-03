/*
 * sqlite_store.cpp -- see sqlite_store.hpp. Only compiled with OPENCODE_USE_SQLITE.
 */
#include "store/sqlite_store.hpp"

#include <chrono>
#include <cstring>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include "core/arena.h"
#include "msg/codec.h"

namespace opencode::store {
namespace {

constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS sessions(
  id TEXT PRIMARY KEY,
  title TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS messages(
  id TEXT PRIMARY KEY,
  session_id TEXT NOT NULL,
  seq INTEGER NOT NULL,
  blob BLOB NOT NULL);
CREATE TABLE IF NOT EXISTS files(
  id TEXT PRIMARY KEY,
  session_id TEXT NOT NULL,
  path TEXT NOT NULL,
  content TEXT NOT NULL,
  version INTEGER NOT NULL,
  created_at INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS idx_messages_session ON messages(session_id, seq);
CREATE INDEX IF NOT EXISTS idx_files_session ON files(session_id, path);
)sql";

/* RAII reset: binds/stepping must always reset the statement afterwards. */
struct StmtReset {
    sqlite3_stmt* s;
    ~StmtReset() {
        if (s) sqlite3_reset(s);
    }
};

void log_err(core::Logger* log, const char* what, sqlite3* db) noexcept {
    if (log) {
        log->error("store: sqlite failure", "op", what, "err",
                   sqlite3_errmsg(db));
    }
}

int64_t epoch_ms() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
        .count();
}

} /* namespace */

struct SqliteStore::Impl {
    sqlite3* db = nullptr;
    sqlite3_stmt* q_session_get = nullptr;
    sqlite3_stmt* q_session_save = nullptr;
    sqlite3_stmt* q_msgs = nullptr;
    sqlite3_stmt* q_next_seq = nullptr;
    sqlite3_stmt* q_msg_save = nullptr;
    sqlite3_stmt* q_msg_del = nullptr;
    sqlite3_stmt* q_file_save = nullptr;
    sqlite3_stmt* q_files = nullptr;
    core::Logger* log = nullptr;
    uint64_t seq = 0;
    core::Arena arena; /* scratch for binary message encoding */
    mutable std::mutex mtx; /* prepared statements are not thread-shared */

    Impl() = default;
    ~Impl() {
        if (db) {
            sqlite3_finalize(q_files);
            sqlite3_finalize(q_file_save);
            sqlite3_finalize(q_msg_del);
            sqlite3_finalize(q_msg_save);
            sqlite3_finalize(q_next_seq);
            sqlite3_finalize(q_msgs);
            sqlite3_finalize(q_session_save);
            sqlite3_finalize(q_session_get);
            sqlite3_close(db);
        }
    }

    std::string next_id() {
        return "id" + std::to_string(epoch_ms()) + "-" +
               std::to_string(++seq);
    }
};

SqliteStore::SqliteStore(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

SqliteStore::~SqliteStore() = default;

Session SqliteStore::session_get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    Session s;
    sqlite3_stmt* st = impl_->q_session_get;
    StmtReset rr{st};
    sqlite3_clear_bindings(st);
    sqlite3_bind_text(st, 1, id.c_str(), static_cast<int>(id.size()),
                      SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        s.id = id;
        s.title = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        s.created_at =
            static_cast<uint64_t>(sqlite3_column_int64(st, 1));
        s.updated_at =
            static_cast<uint64_t>(sqlite3_column_int64(st, 2));
    }
    return s;
}

Session SqliteStore::session_save(Session s) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    Session stored = std::move(s);
    if (stored.id.empty()) stored.id = impl_->next_id();
    sqlite3_stmt* st = impl_->q_session_save;
    StmtReset rr{st};
    sqlite3_bind_text(st, 1, stored.id.c_str(),
                      static_cast<int>(stored.id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, stored.title.c_str(),
                      static_cast<int>(stored.title.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, static_cast<int64_t>(stored.created_at));
    sqlite3_bind_int64(st, 4, static_cast<int64_t>(stored.updated_at));
    if (sqlite3_step(st) != SQLITE_DONE) {
        log_err(impl_->log, "session_save", impl_->db);
    }
    if (listener_) listener_->on_session_saved(stored);
    return stored;
}

std::vector<msg::Message> SqliteStore::messages_by_session(
    const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    std::vector<msg::Message> out;
    sqlite3_stmt* st = impl_->q_msgs;
    StmtReset rr{st};
    sqlite3_clear_bindings(st);
    sqlite3_bind_text(st, 1, session_id.c_str(),
                      static_cast<int>(session_id.size()), SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const void* data = sqlite3_column_blob(st, 0);
        const int n = sqlite3_column_bytes(st, 0);
        if (data == nullptr || n <= 0) continue;
        const auto* bytes =
            static_cast<const std::byte*>(data);
        msg::Message m;
        const core::error_code ec =
            msg::decode_message(std::span<const std::byte>(bytes, n), m);
        if (!ec.ok()) {
            log_err(impl_->log, "messages_by_session: decode", impl_->db);
            continue; /* never hand corrupted rows to callers */
        }
        out.push_back(std::move(m));
    }
    return out;
}

msg::Message SqliteStore::message_save(msg::Message m) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    msg::Message stored = std::move(m);
    if (stored.id.empty()) stored.id = impl_->next_id();

    const std::span<std::byte> bytes =
        msg::encode_message(stored, impl_->arena);
    if (bytes.empty()) {
        log_err(impl_->log, "message_save: encode", impl_->db);
        impl_->arena.reset();
        return stored; /* nothing stored; OOM path is logged, never swallowed */
    }

    int64_t seq = 0;
    {
        sqlite3_stmt* st = impl_->q_next_seq;
        StmtReset rr{st};
        sqlite3_bind_text(st, 1, stored.session_id.c_str(),
                          static_cast<int>(stored.session_id.size()),
                          SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            seq = sqlite3_column_int64(st, 0);
        }
    }
    sqlite3_stmt* st = impl_->q_msg_save;
    StmtReset rr{st};
    sqlite3_bind_text(st, 1, stored.id.c_str(),
                      static_cast<int>(stored.id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, stored.session_id.c_str(),
                      static_cast<int>(stored.session_id.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, seq);
    sqlite3_bind_blob(st, 4, bytes.data(), static_cast<int>(bytes.size()),
                      SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_DONE) {
        log_err(impl_->log, "message_save", impl_->db);
    }
    impl_->arena.reset();

    if (listener_) listener_->on_message_saved(stored);
    return stored;
}

void SqliteStore::message_delete(const std::string& id) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    sqlite3_stmt* st = impl_->q_msg_del;
    StmtReset rr{st};
    sqlite3_bind_text(st, 1, id.c_str(), static_cast<int>(id.size()),
                      SQLITE_TRANSIENT);
    const int rc = sqlite3_step(st);
    const bool removed = (rc == SQLITE_DONE) && (sqlite3_changes(impl_->db) > 0);
    if (rc != SQLITE_DONE) log_err(impl_->log, "message_delete", impl_->db);
    if (removed && listener_) listener_->on_message_deleted(id);
}

File SqliteStore::file_save_version(File f) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    File stored = std::move(f);
    if (stored.id.empty()) stored.id = impl_->next_id();
    if (stored.version == 0) stored.version = 1;
    sqlite3_stmt* st = impl_->q_file_save;
    StmtReset rr{st};
    sqlite3_bind_text(st, 1, stored.id.c_str(),
                      static_cast<int>(stored.id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, stored.session_id.c_str(),
                      static_cast<int>(stored.session_id.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, stored.path.c_str(),
                      static_cast<int>(stored.path.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, stored.content.c_str(),
                      static_cast<int>(stored.content.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, static_cast<int>(stored.version));
    sqlite3_bind_int64(st, 6, static_cast<int64_t>(stored.created_at));
    if (sqlite3_step(st) != SQLITE_ROW) {
        log_err(impl_->log, "file_save_version", impl_->db);
    } else {
        stored.version =
            static_cast<uint32_t>(sqlite3_column_int64(st, 0));
        stored.path =
            reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        stored.created_at =
            static_cast<uint64_t>(sqlite3_column_int64(st, 2));
    }
    if (listener_) listener_->on_file_saved(stored);
    return stored;
}

std::vector<File> SqliteStore::files_by_session(
    const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    std::vector<File> out;
    sqlite3_stmt* st = impl_->q_files;
    StmtReset rr{st};
    sqlite3_clear_bindings(st);
    sqlite3_bind_text(st, 1, session_id.c_str(),
                      static_cast<int>(session_id.size()), SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        File f;
        f.id = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        f.path = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        f.content =
            reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
        f.version =
            static_cast<uint32_t>(sqlite3_column_int(st, 3));
        f.created_at =
            static_cast<uint64_t>(sqlite3_column_int64(st, 4));
        f.session_id = session_id;
        out.push_back(std::move(f));
    }
    return out;
}

core::error_code create_sqlite_store(const char* path,
                                     std::unique_ptr<Store>& out,
                                     core::Logger* log) {
    auto impl = std::make_unique<SqliteStore::Impl>();
    impl->log = log;

    const int rc = sqlite3_open_v2(
        path, &impl->db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (rc != SQLITE_OK) {
        if (log) {
            log->error("store: cannot open sqlite db", "path", path, "err",
                       sqlite3_errmsg(impl->db));
        }
        sqlite3_close(impl->db);
        impl->db = nullptr;
        return core::make_error_code(core::Err::e_internal,
                                     static_cast<uint32_t>(rc));
    }
    sqlite3_busy_timeout(impl->db, 5000);
    const char* setup[] = {"PRAGMA journal_mode=WAL;", kSchema,
                           "PRAGMA user_version=1;"};
    for (const char* sql : setup) {
        if (sqlite3_exec(impl->db, sql, nullptr, nullptr, nullptr) !=
            SQLITE_OK) {
            log_err(impl->log, "setup", impl->db);
        }
    }

    auto prep = [&](const char* sql, sqlite3_stmt** st) -> bool {
        return sqlite3_prepare_v2(impl->db, sql, -1, st, nullptr) ==
               SQLITE_OK;
    };
    const bool ok = prep(
                        "SELECT title, created_at, updated_at FROM sessions"
                        " WHERE id=?1",
                        &impl->q_session_get) &&
                    prep("INSERT OR REPLACE INTO sessions"
                         " (id,title,created_at,updated_at)"
                         " VALUES(?1,?2,?3,?4)",
                         &impl->q_session_save) &&
                    prep("SELECT blob FROM messages WHERE session_id=?1"
                         " ORDER BY seq",
                         &impl->q_msgs) &&
                    prep("SELECT COALESCE(MAX(seq),-1)+1 FROM messages"
                         " WHERE session_id=?1",
                         &impl->q_next_seq) &&
                    prep("INSERT INTO messages (id,session_id,seq,blob)"
                         " VALUES(?1,?2,?3,?4)",
                         &impl->q_msg_save) &&
                    prep("DELETE FROM messages WHERE id=?1",
                         &impl->q_msg_del) &&
                    prep("INSERT INTO files"
                         " (id,session_id,path,content,version,created_at)"
                         " VALUES(?1,?2,?3,?4,?5,?6)"
                         " ON CONFLICT(id) DO UPDATE SET"
                         " content=excluded.content, version=files.version+1"
                         " RETURNING version, path, created_at",
                         &impl->q_file_save) &&
                    prep("SELECT id,path,content,version,created_at FROM files"
                         " WHERE session_id=?1 ORDER BY rowid",
                         &impl->q_files);
    if (!ok) {
        log_err(impl->log, "prepare", impl->db);
        impl.reset();
        return core::make_error_code(core::Err::e_internal);
    }
    out = std::unique_ptr<Store>(new SqliteStore(std::move(impl)));
    return core::ok();
}

} /* namespace opencode::store */

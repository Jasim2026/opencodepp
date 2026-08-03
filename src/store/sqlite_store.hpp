/*
 * sqlite_store.hpp -- durable Store backend (optional).
 *
 * Compiled and linked ONLY when OPENCODE_USE_SQLITE=ON (OFF by default, see
 * 04_DEPENDENCY_POLICY.md Section 3). This header and sqlite_store.cpp are the
 * only TUs that ever include <sqlite3.h>; the rest of the engine never sees
 * sqlite. Requires SQLite >= 3.35 (for INSERT ... ON CONFLICT ... RETURNING).
 *
 * Messages are stored as binary-codec BLOBs (msg/codec.h), so a session that
 * is saved and reloaded comes back byte-identical. Schema is versioned via
 * PRAGMA user_version (currently 1). WAL journaling is enabled at open.
 */
#ifndef OPENCODE_STORE_SQLITE_STORE_HPP
#define OPENCODE_STORE_SQLITE_STORE_HPP

#include <memory>
#include <string>

#include <sqlite3.h>

#include "core/error.h"
#include "core/log.h"
#include "store/store.h"

namespace opencode::store {

class SqliteStore final : public Store {
public:
    SqliteStore(const SqliteStore&) = delete;
    SqliteStore& operator=(const SqliteStore&) = delete;
    ~SqliteStore() override;

    Session session_get(const std::string& id) const override;
    Session session_save(Session s) override;
    std::vector<msg::Message> messages_by_session(
        const std::string& session_id) const override;
    msg::Message message_save(msg::Message m) override;
    void message_delete(const std::string& id) override;
    File file_save_version(File f) override;
    std::vector<File> files_by_session(
        const std::string& session_id) const override;

private:
    struct Impl;
    explicit SqliteStore(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;

    friend core::error_code create_sqlite_store(const char* path,
                                                std::unique_ptr<Store>& out,
                                                core::Logger* log);
};

/* Open (creating if needed) the database at `path`. Returns e_internal with
 * the sqlite result code in detail() when the open or schema setup fails. */
core::error_code create_sqlite_store(const char* path,
                                     std::unique_ptr<Store>& out,
                                     core::Logger* log = nullptr);

} /* namespace opencode::store */

#endif /* OPENCODE_STORE_SQLITE_STORE_HPP */

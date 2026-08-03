/*
 * store.h -- the persistence interface (03_ARCHITECTURE.md Section 4).
 *
 * A Store owns sessions, per-session message history, and per-session file
 * versions. Implementations:
 *   - MemStore    (mem_store.hpp)   in-memory, the default; zero dependencies.
 *   - SqliteStore (sqlite_store.hpp) durable backend, built only when
 *     OPENCODE_USE_SQLITE=ON; sqlite3 is never imported by this header or by
 *     any non-sqlite TU (04_DEPENDENCY_POLICY.md Section 3).
 *
 * All instances are self-contained (no globals). Methods that return objects
 * return a default/empty value when the key is missing (empty id = not found).
 * Mutations are reported to an optional StoreListener (host/agent hook); the
 * listener is a single instance member, never a global.
 */
#ifndef OPENCODE_STORE_STORE_H
#define OPENCODE_STORE_STORE_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "msg/message.h"

namespace opencode::store {

struct Session {
    std::string id;
    std::string title;
    uint64_t created_at = 0; /* wall-clock seconds */
    uint64_t updated_at = 0;
};

struct File {
    std::string id;
    std::string session_id;
    std::string path;   /* repo-relative path */
    std::string content;
    uint32_t version = 1;
    uint64_t created_at = 0;
};

/* Reacts to store mutations. All callbacks are no-op by default. */
class StoreListener {
public:
    virtual void on_session_saved(const Session&) noexcept {}
    virtual void on_message_saved(const msg::Message&) noexcept {}
    virtual void on_message_deleted(std::string_view) noexcept {}
    virtual void on_file_saved(const File&) noexcept {}
    virtual ~StoreListener() = default;
};

class Store {
public:
    virtual ~Store() = default;

    virtual Session session_get(const std::string& id) const = 0;
    /* Saves; assigns a generated id when `s.id` is empty. Returns the stored
     * copy (with the final id). */
    virtual Session session_save(Session s) = 0;

    /* All messages of a session, in insertion order. */
    virtual std::vector<msg::Message> messages_by_session(
        const std::string& session_id) const = 0;
    /* Saves; assigns a generated id when `m.id` is empty. */
    virtual msg::Message message_save(msg::Message m) = 0;
    virtual void message_delete(const std::string& id) = 0;

    /* Saves a new version: same id -> content replaced, version bumped. */
    virtual File file_save_version(File f) = 0;
    /* Current version of each file in the session, in first-save order. */
    virtual std::vector<File> files_by_session(
        const std::string& session_id) const = 0;

    void set_listener(StoreListener* l) noexcept { listener_ = l; }
    StoreListener* listener() const noexcept { return listener_; }

protected:
    StoreListener* listener_ = nullptr;
};

} /* namespace opencode::store */

#endif /* OPENCODE_STORE_STORE_H */

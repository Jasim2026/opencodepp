/*
 * mem_store.hpp -- the default, zero-dependency Store implementation.
 *
 * Fully instance-scoped maps; every public method is guarded by a single
 * mutex (single-writer semantics) so host threads may call safely. Messages
 * are kept per session in insertion order. Designed to run a full session
 * with no files on disk at all (constrained-host / Android T2 story).
 */
#ifndef OPENCODE_STORE_MEM_STORE_HPP
#define OPENCODE_STORE_MEM_STORE_HPP

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "store/store.h"

namespace opencode::store {

class MemStore final : public Store {
public:
    MemStore() = default;

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
    std::string next_id() noexcept;

    mutable std::mutex m_;
    std::unordered_map<std::string, Session> sessions_;
    /* session id -> messages in insertion order */
    std::unordered_map<std::string, std::vector<msg::Message>> msgs_;
    /* message id -> owning session (for O(1) delete) */
    std::unordered_map<std::string, std::string> msg_session_;
    std::unordered_map<std::string, File> files_;
    /* session id -> ordered file ids */
    std::unordered_map<std::string, std::vector<std::string>> files_order_;
    uint64_t seq_ = 0;
};

/* The default store; heap-allocated so hosts can own it via unique_ptr. */
std::unique_ptr<Store> create_mem_store();

} /* namespace opencode::store */

#endif /* OPENCODE_STORE_MEM_STORE_HPP */

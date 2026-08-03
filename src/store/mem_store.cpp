/*
 * mem_store.cpp -- see mem_store.hpp.
 */
#include "store/mem_store.hpp"

namespace opencode::store {

std::string MemStore::next_id() noexcept {
    return "id" + std::to_string(++seq_);
}

Session MemStore::session_get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(m_);
    const auto it = sessions_.find(id);
    return it == sessions_.end() ? Session{} : it->second;
}

Session MemStore::session_save(Session s) {
    Session stored = std::move(s);
    if (stored.id.empty()) stored.id = next_id();
    {
        std::lock_guard<std::mutex> lock(m_);
        sessions_[stored.id] = stored;
    }
    if (listener_) listener_->on_session_saved(stored);
    return stored;
}

std::vector<msg::Message> MemStore::messages_by_session(
    const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(m_);
    const auto it = msgs_.find(session_id);
    return it == msgs_.end() ? std::vector<msg::Message>{} : it->second;
}

msg::Message MemStore::message_save(msg::Message m) {
    msg::Message stored = std::move(m);
    if (stored.id.empty()) stored.id = next_id();
    {
        std::lock_guard<std::mutex> lock(m_);
        msgs_[stored.session_id].push_back(stored);
        msg_session_[stored.id] = stored.session_id;
    }
    if (listener_) listener_->on_message_saved(stored);
    return stored;
}

void MemStore::message_delete(const std::string& id) {
    std::string session_id;
    {
        std::lock_guard<std::mutex> lock(m_);
        const auto it = msg_session_.find(id);
        if (it == msg_session_.end()) return;
        session_id = it->second;
        auto& v = msgs_[session_id];
        for (auto jt = v.begin(); jt != v.end(); ++jt) {
            if (jt->id == id) {
                v.erase(jt);
                break;
            }
        }
        msg_session_.erase(it);
    }
    if (listener_) listener_->on_message_deleted(id);
}

File MemStore::file_save_version(File f) {
    File stored = std::move(f);
    {
        std::lock_guard<std::mutex> lock(m_);
        const auto it = files_.find(stored.id);
        if (it != files_.end() && !stored.id.empty()) {
            File& old = it->second;
            old.content = stored.content;
            old.version += 1;
            stored = old; /* keep the original path and timestamps */
        } else {
            if (stored.id.empty()) stored.id = next_id();
            if (stored.version == 0) stored.version = 1;
            files_[stored.id] = stored;
            files_order_[stored.session_id].push_back(stored.id);
        }
    }
    if (listener_) listener_->on_file_saved(stored);
    return stored;
}

std::vector<File> MemStore::files_by_session(
    const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(m_);
    std::vector<File> out;
    const auto it = files_order_.find(session_id);
    if (it == files_order_.end()) return out;
    out.reserve(it->second.size());
    for (const std::string& id : it->second) {
        const auto jt = files_.find(id);
        if (jt != files_.end()) out.push_back(jt->second);
    }
    return out;
}

std::unique_ptr<Store> create_mem_store() {
    return std::make_unique<MemStore>();
}

} /* namespace opencode::store */

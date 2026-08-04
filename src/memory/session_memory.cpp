/*
 * session_memory.cpp -- checkpoint/resume over the Store (see header).
 */
#include "memory/session_memory.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/clock.h"
#include "memory/entry.h"
#include "util/json.h"
#include "util/sha1.h"

namespace opencode::memory {

namespace {

namespace fs = std::filesystem;

/* Task-state entry keys stored per session checkpoint. */
constexpr std::string_view kAppliedEdits = "applied_edits";
constexpr std::string_view kTokensUsed = "tokens_used";
constexpr std::string_view kWorkspaceHash = "workspace_hash";

std::string session_scope(std::string_view session_id) {
    return "session:" + std::string(session_id);
}

/* Read all entries whose File.session_id == scope, in insertion order. */
std::vector<Entry> entries_by_scope(store::Store* store, std::string_view scope) {
    std::vector<Entry> out;
    if (store == nullptr) return out;
    for (const store::File& f : store->files_by_session(std::string(scope))) {
        Entry e;
        if (from_json(f.content, e).ok()) out.push_back(std::move(e));
    }
    return out;
}

/* Upsert one entry as a File row under its scope. */
void save_entry(store::Store* store, Entry e) {
    if (store == nullptr) return;
    store::File f;
    f.id = entry_id(e.scope, e.key);
    f.session_id = e.scope;
    f.path = "memory/" + e.key;
    f.content = to_json(e);
    store->file_save_version(f);
}

const Entry* find_entry(const std::vector<Entry>& entries, std::string_view key) {
    for (const Entry& e : entries)
        if (e.key == key) return &e;
    return nullptr;
}

} /* namespace */

std::string checkpoint(store::Store* store, std::string_view session_id,
                       const std::vector<msg::Message>& messages,
                       std::uint64_t tokens_used,
                       const std::vector<std::string>& applied_edits,
                       std::string_view workspace_hash) {
    if (store == nullptr) return "";
    const std::uint64_t now = core::now_wall_sec();

    store::Session s;
    s.id = std::string(session_id);
    s.title = messages.empty() ? "" : messages.front().content_text();
    const store::Session saved = store->session_save(s);
    if (saved.id.empty()) return "";

    for (const msg::Message& m : messages) store->message_save(m);

    const std::string scope = session_scope(session_id);

    /* applied_edits -> JSON array of paths */
    {
        std::vector<util::JVal> arr;
        arr.reserve(applied_edits.size());
        for (const std::string& p : applied_edits) arr.push_back(util::JVal::Str(p));
        Entry e;
        e.kind = Kind::task_state;
        e.scope = scope;
        e.key = std::string(kAppliedEdits);
        e.value = util::to_json(util::JVal::Array(std::move(arr)));
        e.created_at = now;
        save_entry(store, std::move(e));
    }
    /* tokens_used -> decimal string */
    {
        Entry e;
        e.kind = Kind::task_state;
        e.scope = scope;
        e.key = std::string(kTokensUsed);
        e.value = std::to_string(tokens_used);
        e.created_at = now;
        save_entry(store, std::move(e));
    }
    /* workspace_hash */
    if (!workspace_hash.empty()) {
        Entry e;
        e.kind = Kind::task_state;
        e.scope = scope;
        e.key = std::string(kWorkspaceHash);
        e.value = std::string(workspace_hash);
        e.created_at = now;
        save_entry(store, std::move(e));
    }
    return saved.id;
}

SessionCheckpoint resume(store::Store* store, std::string_view session_id,
                         std::string_view workspace_dir) {
    SessionCheckpoint out;
    if (store == nullptr) return out;
    out.session_id = std::string(session_id);

    const store::Session s = store->session_get(std::string(session_id));
    if (s.id.empty()) return out; /* nothing persisted */
    out.found = true;
    out.messages = store->messages_by_session(std::string(session_id));

    const std::string scope = session_scope(session_id);
    const std::vector<Entry> entries = entries_by_scope(store, scope);

    if (const Entry* e = find_entry(entries, kAppliedEdits)) {
        /* value is a JSON array of strings */
        util::JVal root;
        std::size_t pos = 0;
        if (util::parse_json(e->value, root, &pos).ok() &&
            root.kind == util::JVal::Kind::array) {
            for (const util::JVal& v : root.arr) {
                if (v.kind == util::JVal::Kind::string)
                    out.applied_edits.emplace_back(v.str);
            }
        }
    }
    if (const Entry* e = find_entry(entries, kTokensUsed)) {
        try {
            out.tokens_used = static_cast<std::uint64_t>(
                std::stoull(e->value, nullptr, 10));
        } catch (...) {
            out.tokens_used = 0;
        }
    }
    if (const Entry* e = find_entry(entries, kWorkspaceHash))
        out.workspace_hash = e->value;

    if (!out.workspace_hash.empty()) {
        std::string now;
        if (workspace_hash(workspace_dir, now).ok())
            out.workspace_matches = (now == out.workspace_hash);
    }
    return out;
}

core::error_code workspace_hash(std::string_view dir, std::string& out) {
    out.clear();
    std::set<std::string> lines;
    const fs::path root(dir);

    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec))
        return core::make_error_code(core::Err::e_tool_reject);

    for (fs::recursive_directory_iterator it(root, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) return core::make_error_code(core::Err::e_tool_reject);
        std::error_code fec;
        if (!it->is_regular_file(fec) || fec) continue;

        const fs::path rel = fs::relative(it->path(), root);
        const std::string rels = rel.generic_string();
        if (rels.rfind(".git/", 0) == 0 || rels == ".git") continue;

        std::FILE* f = std::fopen(it->path().c_str(), "rb");
        if (f == nullptr) continue;
        std::string content;
        char buf[8192];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
            content.append(buf, n);
        std::fclose(f);

        lines.insert(rels + ":" + util::sha1_hex(content));
    }
    if (ec) return core::make_error_code(core::Err::e_tool_reject);

    std::string acc;
    acc.reserve(lines.size() * 64);
    for (const std::string& l : lines) {
        acc += l;
        acc += '\n';
    }
    out = util::sha1_hex(acc);
    return core::ok();
}

} /* namespace opencode::memory */

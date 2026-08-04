/*
 * workspace_memory.cpp -- workspace memory implementation (see header).
 */
#include "memory/workspace_memory.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/clock.h"
#include "msg/tokens.h"

namespace opencode::memory {

namespace {

using core::Err;
using core::make_error_code;

bool contains_ci(std::string_view haystack, std::string_view needle) {
    if (needle.empty() || haystack.empty()) return false;
    std::string h(haystack.size(), '\0');
    for (std::size_t i = 0; i < haystack.size(); ++i)
        h[i] = static_cast<char>(std::tolower(
            static_cast<unsigned char>(haystack[i])));
    std::string n(needle.size(), '\0');
    for (std::size_t i = 0; i < needle.size(); ++i)
        n[i] = static_cast<char>(std::tolower(
            static_cast<unsigned char>(needle[i])));
    return h.find(n) != std::string::npos;
}

/* Deterministic insertion order: entries already arrive in first-save order
 * from files_by_session(); the oldest entry is the first one at the cap. */
const Entry* oldest(const std::vector<Entry>& entries) {
    const Entry* out = nullptr;
    for (const Entry& e : entries) {
        if (out == nullptr || e.created_at < out->created_at) out = &e;
    }
    return out;
}

} /* namespace */

std::string workspace_scope(std::string_view workspace_root) {
    return "workspace:" + std::string(workspace_root);
}

core::error_code write_entry(store::Store* store, std::string_view workspace_root,
                             Entry e, const config::MemoryCfg& caps,
                             std::string* out_id) {
    if (out_id != nullptr) out_id->clear();
    if (store == nullptr) return core::ok();

    e.scope = workspace_scope(workspace_root);
    if (const core::error_code c = validate_entry(e, caps); !c.ok()) return c;

    std::vector<Entry> existing = read_entries(store, workspace_root, caps);
    if (existing.size() >= static_cast<std::size_t>(caps.max_entries)) {
        /* ring buffer: reuse the oldest entry's key so the scope stays at
         * max_entries without a store delete API. */
        if (const Entry* oldest_entry = oldest(existing))
            e.key = oldest_entry->key;
    }

    store::File f;
    f.id = entry_id(e.scope, e.key);
    f.session_id = e.scope;
    f.path = "memory/" + e.key;
    f.content = to_json(e);
    const store::File saved = store->file_save_version(f);
    if (out_id != nullptr) out_id->assign(saved.id);
    return core::ok();
}

std::vector<Entry> read_entries(store::Store* store,
                                std::string_view workspace_root,
                                const config::MemoryCfg& caps,
                                std::optional<Kind> kind) {
    std::vector<Entry> out;
    if (store == nullptr) return out;
    (void)caps;
    const std::string scope = workspace_scope(workspace_root);
    for (const store::File& f : store->files_by_session(scope)) {
        Entry e;
        if (from_json(f.content, e).ok() && e.scope == scope &&
            (!kind.has_value() || e.kind == *kind)) {
            out.push_back(std::move(e));
        }
    }
    return out;
}

std::vector<Entry> match_entries(store::Store* store,
                                 std::string_view workspace_root,
                                 const std::vector<std::string>& keywords,
                                 const config::MemoryCfg& caps) {
    std::vector<Entry> out;
    if (store == nullptr || keywords.empty()) return out;
    for (Entry& e : read_entries(store, workspace_root, caps)) {
        for (const std::string& kw : keywords) {
            if (contains_ci(e.key, kw) || contains_ci(e.value, kw))
                goto matched;
            for (const std::string& t : e.tags) {
                if (contains_ci(t, kw)) goto matched;
            }
        }
        continue;
    matched:
        out.push_back(std::move(e));
    }
    return out;
}

std::string entries_to_context(const std::vector<Entry>& entries,
                               std::size_t max_entries,
                               std::uint32_t max_tokens,
                               std::uint32_t max_chars) {
    std::string out;
    std::size_t count = 0;
    std::uint32_t tokens = 0;
    for (const Entry& e : entries) {
        if (max_entries != 0 && count >= max_entries) break;
        const std::string line = to_env_text(e, max_chars);
        if (line.empty()) continue;
        const std::uint32_t line_tokens =
            static_cast<std::uint32_t>(msg::estimate_tokens(line));
        if (max_tokens != 0 && tokens + line_tokens > max_tokens) break;
        if (!out.empty()) out += '\n';
        out += line;
        tokens += line_tokens;
        ++count;
    }
    return out;
}

std::vector<std::string> keywords_from_text(std::string_view text) {
    static const std::set<std::string> kStop = {
        "about", "after", "again", "also", "because", "before", "being",
        "build", "change", "could", "does", "edit", "from", "have", "into",
        "just", "like", "make", "need", "only", "other", "please", "should",
        "some", "than", "that", "their", "them", "then", "there", "these",
        "they", "this", "those", "through", "under", "very", "want", "were",
        "what", "when", "where", "which", "while", "with", "would", "your"};
    std::vector<std::string> out;
    std::string cur;
    const auto flush = [&]() {
        if (cur.size() >= 4 && kStop.find(cur) == kStop.end()) {
            if (std::find(out.begin(), out.end(), cur) == out.end())
                out.push_back(cur);
        }
        cur.clear();
    };
    for (const char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            flush();
        }
    }
    flush();
    return out;
}

} /* namespace opencode::memory */

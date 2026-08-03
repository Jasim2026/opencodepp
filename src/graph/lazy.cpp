/*
 * lazy.cpp -- lazy / incremental indexing (Phase 7).
 *
 * ensure_indexed() parses a file only when its (mtime,size) fingerprint is
 * unchanged since the last parse -- the key to fast repeated context builds.
 * The parse cache is capped (IndexLimits.cache_files); the LRU victim is
 * evicted (its syms and deps tombstoned, reclaimed lazily) so long sessions
 * stay bounded.
 *
 * mtime is nanoseconds-resolution where available; second-resolution filesystems
 * (some FUSE mounts) would otherwise make same-second edits look unchanged.
 */
#include "graph/index.h"

#include <limits>
#include <string>

namespace opencode::graph {

core::error_code SymbolIndex::ensure_indexed(const std::string& file,
                                             std::string* changed) {
    /* Single open: fstat fingerprint + read together (no separate stat, no
     * re-read on the parse path). */
    std::uint64_t mtime = 0, size = 0;
    std::string text;
    if (!read_file_stat(file, text, mtime, size))
        return core::make_error_code(core::Err::e_missing_cfg);

    const auto it = files_.find(file);
    if (it != files_.end() && it->second.mtime == mtime &&
        it->second.size == size) {
        touch_[file] = ++touch_clock_;
        if (changed != nullptr) changed->clear();
        return core::ok();
    }

    core::error_code ec = index_text(file, detect_lang(file), text);
    if (!ec.ok()) return ec;

    const auto pf = files_.find(file);
    if (pf != files_.end()) {
        pf->second.mtime = mtime;
        pf->second.size = size;
    }
    touch_[file] = ++touch_clock_;
    if (changed != nullptr) *changed = file;

    evict_lru();
    return core::ok();
}

void SymbolIndex::evict_lru() noexcept {
    if (files_.size() <= static_cast<size_t>(limits_.cache_files)) return;
    std::uint64_t min = std::numeric_limits<std::uint64_t>::max();
    std::string victim;
    for (const auto& [file, t] : touch_) {
        if (t < min) {
            min = t;
            victim = file;
        }
    }
    if (!victim.empty()) remove_file(victim);
}

} /* namespace opencode::graph */

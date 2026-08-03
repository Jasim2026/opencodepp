/*
 * lazy.cpp -- lazy / incremental indexing (Phase 7).
 *
 * ensure_indexed() parses a file only when its (mtime,size) fingerprint is
 * unchanged since the last parse -- the key to fast repeated context builds.
 * The parse cache is capped (IndexLimits.cache_files); the LRU victim is
 * evicted (its syms tombstoned, deps dropped) so long sessions stay bounded.
 *
 * mtime is nanoseconds-resolution where available; second-resolution filesystems
 * (some FUSE mounts) would otherwise make same-second edits look unchanged.
 */
#include "graph/index.h"

#include <sys/stat.h>

#include <limits>
#include <string>

namespace opencode::graph {

namespace {

bool stat_file(const std::string& path, std::uint64_t& mtime,
               std::uint64_t& size) {
    struct ::stat st;
    if (::stat(path.c_str(), &st) != 0) return false;
#ifdef __APPLE__
    mtime = static_cast<std::uint64_t>(st.st_mtimespec.tv_sec) * 1000000000ULL +
            static_cast<std::uint64_t>(st.st_mtimespec.tv_nsec);
#else
    mtime = static_cast<std::uint64_t>(st.st_mtim.tv_sec) * 1000000000ULL +
            static_cast<std::uint64_t>(st.st_mtim.tv_nsec);
#endif
    size = static_cast<std::uint64_t>(st.st_size);
    return true;
}

} /* namespace */

core::error_code SymbolIndex::ensure_indexed(const std::string& file,
                                             std::string* changed) {
    std::uint64_t mtime = 0, size = 0;
    if (!stat_file(file, mtime, size))
        return core::make_error_code(core::Err::e_missing_cfg);

    const auto it = files_.find(file);
    if (it != files_.end() && it->second.mtime == mtime &&
        it->second.size == size) {
        touch_[file] = ++touch_clock_;
        if (changed != nullptr) changed->clear();
        return core::ok();
    }

    core::error_code ec = extract_file(file);
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

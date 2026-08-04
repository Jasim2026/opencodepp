/*
 * workspace_memory.h -- durable, keyed workspace memory (Phase 11 Task 3).
 *
 * Entries are scoped to a workspace root and keyed deterministically so a
 * re-write replaces the previous value (no append, no history). Reads are
 * keyword/kind-scoped, never scored: match_entries() returns entries whose
 * key/value/tags contain an intent keyword, and entries_to_context() bounds
 * the injection to the MemoryCfg budget (max entries + max tokens). The
 * engine writes Lesson at task-done and RepoRule on permission-denied; the
 * agent writes explicitly through the memory.write tool. Never throws.
 */
#ifndef OPENCODE_MEMORY_WORKSPACE_MEMORY_H
#define OPENCODE_MEMORY_WORKSPACE_MEMORY_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.hpp"
#include "core/error.h"
#include "memory/entry.h"
#include "store/store.h"

namespace opencode::memory {

/* Stable scope string for a workspace root: "workspace:<root>". */
std::string workspace_scope(std::string_view workspace_root);

/* Upsert a workspace-scoped entry (scope is forced to the workspace root).
 * Validates against caps (key charset/length, value cap, secret filter) and
 * enforces the per-scope cap by recycling the oldest entry slot once the
 * scope holds max_entries rows (a deterministic ring buffer, since the Store
 * has no file-delete API). On success *out_id (when non-null) receives the
 * entry id. Returns e_invalid_cfg on validation failure. No-op (ok) with a
 * null Store. */
core::error_code write_entry(store::Store* store, std::string_view workspace_root,
                             Entry e, const config::MemoryCfg& caps,
                             std::string* out_id = nullptr);

/* All workspace-scoped entries in insertion order. When `kind` is set, only
 * entries of that kind are returned. */
std::vector<Entry> read_entries(store::Store* store,
                                std::string_view workspace_root,
                                const config::MemoryCfg& caps,
                                std::optional<Kind> kind = std::nullopt);

/* Entries whose key, value, or tags contain any of `keywords`
 * (case-insensitive), oldest first. Empty keywords match nothing. */
std::vector<Entry> match_entries(store::Store* store,
                                 std::string_view workspace_root,
                                 const std::vector<std::string>& keywords,
                                 const config::MemoryCfg& caps);

/* Bound a matched entry set to the injection budget: at most `max_entries`
 * entries (oldest first) whose joined text stays under `max_tokens`.
 * Returns concatenated "[kind] key: value" lines (see to_env_text). */
std::string entries_to_context(const std::vector<Entry>& entries,
                               std::size_t max_entries,
                               std::uint32_t max_tokens,
                               std::uint32_t max_chars);

/* Deterministic intent keywords from a user turn: lowercase words >= 4 chars,
 * with a small stopword set. Used to scope Tier-2 memory injection. */
std::vector<std::string> keywords_from_text(std::string_view text);

} /* namespace opencode::memory */

#endif /* OPENCODE_MEMORY_WORKSPACE_MEMORY_H */

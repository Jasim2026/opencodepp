/*
 * entry.h -- the memory entry model and validation (Phase 11 Task 1).
 *
 * An Entry is a curated, structured fact persisted in the Store. Entries are
 * scoped (workspace or per-session) and kind-tagged for deterministic matching;
 * no scoring, no embeddings. The secret filter blocks tokens/passwords/sk-...
 * at write time. Validation enforces per-entry size caps from MemoryCfg.
 * Never throws.
 */
#ifndef OPENCODE_MEMORY_ENTRY_H
#define OPENCODE_MEMORY_ENTRY_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.hpp"
#include "core/error.h"

namespace opencode::memory {

/* Memory entry kinds (the agent writes these explicitly; the engine writes
 * Lesson/RepoRule at defined points). */
enum class Kind : uint8_t {
    decision = 0, /* why a choice was made                              */
    fact = 1,     /* durable workspace fact (e.g. "build uses cmake")   */
    task_state = 2, /* session-scoped task state (applied edits, etc.)  */
    repo_rule = 3,  /* workspace rule (permission denied, repo norm)    */
    lesson = 4,     /* engine-generated lesson (task done, outcome)     */
    user_pref = 5,  /* user-stated preference                           */
};

/* Convert Kind to/from a stable string (for JSON / env keys). */
std::string_view kind_name(Kind k) noexcept;
Kind kind_from_name(std::string_view s) noexcept;

/* One memory entry. */
struct Entry {
    std::string id;            /* stable id (caller-assigned or generated) */
    Kind kind = Kind::fact;
    std::string scope;         /* "workspace" or "session:<session-id>"   */
    std::string key;           /* short identifier; [a-z0-9._-]{1,64}    */
    std::string value;         /* free-form text (<= max_value_chars)     */
    std::string source;        /* turn/ref provenance                    */
    std::uint64_t created_at = 0; /* wall-clock seconds                  */
    std::uint64_t ttl_s = 0;   /* 0 = no expiry                          */
    std::vector<std::string> tags; /* flat labels for kind/scope matching */
};

/* Returns true if the key or value contains a secret pattern.
 * Patterns: "token", "password", "secret" (case-insensitive substring),
 * or starts with "sk-" / contains "key=sk-". This is the write-time
 * filter the plan requires. */
bool has_secret_value(std::string_view key, std::string_view value);

/* Validate an entry against MemoryCfg caps. Returns ok() when valid, or:
 *   e_invalid_cfg  - key/value too long, key bad charset, secret detected
 *   e_model_unsup  - kind is out of range */
core::error_code validate_entry(const Entry& e, const config::MemoryCfg& caps);

/* Serialize entry to JSON (a flat object). Caller owns the string. */
std::string to_json(const Entry& e);

/* Parse a JSON object into an Entry. Ignores unknown keys. */
core::error_code from_json(std::string_view json, Entry& out);

/* Convert an entry to an EnvEntry text snippet for Tier-2 context
 * injection. Format: "[<kind>] <key>: <value>" (truncated to
 * max_value_chars if needed). */
std::string to_env_text(const Entry& e, std::uint32_t max_chars = 0);

/* Generate a deterministic entry id from scope + key (for workspace
 * memory where ids must be stable across writes). */
std::string entry_id(std::string_view scope, std::string_view key);

} /* namespace opencode::memory */

#endif /* OPENCODE_MEMORY_ENTRY_H */

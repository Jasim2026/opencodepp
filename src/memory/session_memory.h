/*
 * session_memory.h -- session checkpoint / resume (Phase 11 Task 2).
 *
 * A checkpoint is a Store snapshot: the session row + all messages + a few
 * task-state entries (applied edits, tokens used, workspace hash). Resume
 * loads the snapshot and verifies the workspace hash still matches; on a
 * mismatch the caller starts a fresh task with a note, never blind-applies.
 * Storeless calls are no-ops. Never throws.
 */
#ifndef OPENCODE_MEMORY_SESSION_MEMORY_H
#define OPENCODE_MEMORY_SESSION_MEMORY_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.h"
#include "msg/message.h"
#include "store/store.h"

namespace opencode::memory {

/* Everything needed to continue a task from its last clean point. */
struct SessionCheckpoint {
    std::string session_id;
    std::vector<msg::Message> messages;   /* full history, insertion order  */
    std::vector<std::string> applied_edits; /* tool paths, in apply order   */
    std::uint64_t tokens_used = 0;
    std::string workspace_hash;            /* hash recorded at checkpoint   */
    bool workspace_matches = false;        /* current dir hash == stored    */
    bool found = false;                    /* a checkpoint exists            */
};

/* Persist a checkpoint for `session_id`: session row, all messages, and the
 * task-state entries. Returns the session id on success, "" when no Store is
 * attached or a write failed. */
std::string checkpoint(store::Store* store, std::string_view session_id,
                       const std::vector<msg::Message>& messages,
                       std::uint64_t tokens_used,
                       const std::vector<std::string>& applied_edits,
                       std::string_view workspace_hash);

/* Load the checkpoint for `session_id` and verify the CURRENT workspace hash
 * against the stored one (workspace_matches). No-op when no Store. */
SessionCheckpoint resume(store::Store* store, std::string_view session_id,
                         std::string_view workspace_dir);

/* SHA-1 over sorted "relpath:hex" lines for every regular file under `dir`
 * (relpath + file digest). Returns e_tool_reject on directory walk failure;
 * an empty dir hashes to the empty-string digest. */
core::error_code workspace_hash(std::string_view dir, std::string& out);

} /* namespace opencode::memory */

#endif /* OPENCODE_MEMORY_SESSION_MEMORY_H */

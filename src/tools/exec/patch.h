/*
 * exec/patch.h -- unified-diff parser + applier (the primary edit primitive).
 *
 * file.patch is the engine's main edit tool: apply hunks with context, atomically
 * (either every hunk lands or none does), rollback-capable via reverse. The
 * parser understands standard "@@ -o,n +n,n @@" hunks with ' '/'+'/'-' lines and
 * tolerates ---/+++ file headers. Matching is context-based (header line numbers
 * are a hint), so patches survive small whitespace drift. Never throws.
 */
#ifndef OPENCODE_TOOLS_EXEC_PATCH_H
#define OPENCODE_TOOLS_EXEC_PATCH_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.h"

namespace opencode::tools::exec::patch {

struct Hunk {
    std::uint64_t orig_start = 0; /* 1-based first old line; 0 = unanchored */
    std::uint64_t new_start = 0;  /* 1-based first new line                  */
    std::vector<std::string> ctx; /* ' ' context lines (no prefix)          */
    std::vector<std::string> rem; /* '-' removed lines (no prefix)          */
    std::vector<std::string> add; /* '+' added lines (no prefix)            */
    /* Ordered bodies preserving the original interleaving of ctx/rem/add.
     * old_body = all non-'+' lines in patch order (the original lines).
     * new_body = all non-'-' lines in patch order (the replacement lines). */
    std::vector<std::string> old_body;
    std::vector<std::string> new_body;
};

core::error_code parse(std::string_view text, std::vector<Hunk>& out);

/* Apply all hunks to `original`; returns the new content only when EVERY hunk
 * matched (never a partial apply). e_tool_reject on context mismatch. */
core::error_code apply(const std::vector<Hunk>& hunks, std::string_view original,
                       std::string& out);

/* Emit the reverse of `patch_text` (removed/added swapped), for rollback and
 * round-trip tests. */
core::error_code reverse(std::string_view patch_text, std::string& out);

/* Convenience: apply `patch_text` to the file at `path` atomically; the file
 * is untouched on any parse/match error. `report` gets "applied N hunks". */
core::error_code apply_file(const std::string& path,
                            std::string_view patch_text, std::string& report);

} /* namespace opencode::tools::exec::patch */

#endif /* OPENCODE_TOOLS_EXEC_PATCH_H */

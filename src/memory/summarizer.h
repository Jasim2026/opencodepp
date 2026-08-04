/*
 * summarizer.h -- lossy turn summary for long sessions (Phase 11 Task 4).
 *
 * When message history exceeds the context cap, fold_oldest() replaces the
 * oldest turns with a single Summary message: either via an optional LLM
 * callback (one extra call in-budget) or via a deterministic local fold
 * (first/last line per turn + outcome). Every fold is explicit -- the reason
 * and folded message count are surfaced so hosts can report the loss; the
 * engine emits a "fold" session event. Nothing here ever throws.
 */
#ifndef OPENCODE_MEMORY_SUMMARIZER_H
#define OPENCODE_MEMORY_SUMMARIZER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "msg/message.h"

namespace opencode::memory {

/* Optional LLM-backed fold. Receives the turns being folded plus the locally
 * produced fallback text; returns the summary text, or "" to fall back to the
 * local fold (e.g. the budget forbids an extra call). */
using SummaryFn = std::function<std::string(
    const std::vector<msg::Message>& folded, std::string_view local_text)>;

/* One fold: the oldest turns are replaced by a single Summary message prepended
 * to the kept tail. Loss is never silent -- folded_count/folded_tokens and the
 * reason are always populated when folded==true. */
struct FoldResult {
    std::vector<msg::Message> messages; /* projected history               */
    msg::Message summary;               /* the inserted Summary message     */
    std::size_t folded_count = 0;       /* messages folded in               */
    std::uint32_t folded_tokens = 0;    /* tokens replaced by the summary   */
    std::string event_what;             /* e.g. "history fold: 12 messages" */
    std::string event_reason;           /* why the fold happened / LLM off  */
    bool folded = false;
};

/* Token estimate of a message history (text + tool frames). */
std::uint32_t estimate_history_tokens(const std::vector<msg::Message>& msgs);

/* True when the history should be folded: more than `keep_recent` messages
 * exist AND their token cost cannot fit the context window once the reserved
 * `max_output_tokens` are deducted. Deterministic. */
bool needs_fold(const std::vector<msg::Message>& msgs,
                std::uint32_t context_window, std::uint32_t max_output_tokens,
                std::size_t keep_recent);

/* Deterministic local fold: per turn, "[role] first line ... last line" with
 * tool-frame counts, then a final "outcome:" line from the newest folded turn.
 * No provider involved. */
std::string local_fold_text(const std::vector<msg::Message>& msgs);

/* Fold the oldest turns (everything before the newest `keep_recent`) into one
 * Summary message. Uses `fn` when non-null and its result is non-empty,
 * otherwise the local fold. Returns folded=false when there is nothing to
 * fold. The Summary is a user-role message (safe on every wire family). */
FoldResult fold_oldest(const std::vector<msg::Message>& msgs,
                       std::uint32_t context_window,
                       std::uint32_t max_output_tokens,
                       std::size_t keep_recent,
                       const SummaryFn& fn = nullptr);

} /* namespace opencode::memory */

#endif /* OPENCODE_MEMORY_SUMMARIZER_H */

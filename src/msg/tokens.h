/*
 * tokens.h -- approximate BPE token counting (drives T1 budgeting and the
 * retry budget). Constants live in tokens.cpp with a comment; the labeled
 * corpus in tests/fixtures/tokens/ asserts the < 10% error bound.
 */
#ifndef OPENCODE_MSG_TOKENS_H
#define OPENCODE_MSG_TOKENS_H

#include <cstddef>
#include <string_view>

#include "msg/message.h"

namespace opencode::msg {

/* Estimate the token count of a piece of text (never allocates):
 *   - prose:  words * 1.3
 *   - code:   chars / 4
 *   - CJK:    1 token per ideograph (+ ascii / 4)
 * A single cheap pass classifies the text into one of the regimes. */
std::size_t estimate_tokens(std::string_view s) noexcept;

/* Sum over a message's parts, memoized per content-hash for short repeated
 * strings (e.g. repeated tool output). Never allocates. */
std::size_t estimate_message_tokens(const Message& m) noexcept;

} /* namespace opencode::msg */

#endif /* OPENCODE_MSG_TOKENS_H */

/*
 * finish.h -- finish reasons (why a turn ended).
 *
 * Mirrors the provider wire values (Phase 5 maps 1:1); `unknown` is the
 * fallback for absent/unrecognized reasons so callers never hit undefined
 * behavior. `from_string<FinishReason>` specializes the shared template.
 */
#ifndef OPENCODE_MSG_FINISH_H
#define OPENCODE_MSG_FINISH_H

#include <cstdint>
#include <optional>
#include <string_view>

#include "msg/role.h" /* shared from_string template */

namespace opencode::msg {

enum class FinishReason : uint8_t {
    end_turn = 0,
    max_tokens = 1,
    tool_use = 2,
    canceled = 3,
    error = 4,
    permission_denied = 5,
    unknown = 6,
};

constexpr std::string_view to_string(FinishReason r) noexcept {
    switch (r) {
        case FinishReason::end_turn: return "end_turn";
        case FinishReason::max_tokens: return "max_tokens";
        case FinishReason::tool_use: return "tool_use";
        case FinishReason::canceled: return "canceled";
        case FinishReason::error: return "error";
        case FinishReason::permission_denied: return "permission_denied";
        case FinishReason::unknown: return "unknown";
    }
    return "unknown";
}

template <>
constexpr std::optional<FinishReason> from_string<FinishReason>(
    std::string_view s) noexcept {
    if (s == "end_turn") return FinishReason::end_turn;
    if (s == "max_tokens") return FinishReason::max_tokens;
    if (s == "tool_use") return FinishReason::tool_use;
    if (s == "canceled") return FinishReason::canceled;
    if (s == "error") return FinishReason::error;
    if (s == "permission_denied") return FinishReason::permission_denied;
    if (s == "unknown") return FinishReason::unknown;
    return std::nullopt;
}

} /* namespace opencode::msg */

#endif /* OPENCODE_MSG_FINISH_H */

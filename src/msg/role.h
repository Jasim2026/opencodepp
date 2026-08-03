/*
 * role.h -- message roles (the message metadata vocabulary).
 *
 * Used by the wire formats in Phase 5 and by the store. `to_string` is the
 * canonical serialized name; `from_string<Role>` is case-sensitive ASCII for
 * now (Phase 5 may widen it). `from_string` is a template specialized per
 * small enum because C++ cannot overload on return type alone.
 */
#ifndef OPENCODE_MSG_ROLE_H
#define OPENCODE_MSG_ROLE_H

#include <cstdint>
#include <optional>
#include <string_view>

namespace opencode::msg {

enum class Role : uint8_t {
    user = 0,
    assistant = 1,
    system = 2,
    tool = 3,
};

constexpr std::string_view to_string(Role r) noexcept {
    switch (r) {
        case Role::user: return "user";
        case Role::assistant: return "assistant";
        case Role::system: return "system";
        case Role::tool: return "tool";
    }
    return "unknown";
}

/* Generic deleted; explicit specializations provide the per-enum parsing. */
template <class T>
constexpr std::optional<T> from_string(std::string_view) noexcept = delete;

template <>
constexpr std::optional<Role> from_string<Role>(std::string_view s) noexcept {
    if (s == "user") return Role::user;
    if (s == "assistant") return Role::assistant;
    if (s == "system") return Role::system;
    if (s == "tool") return Role::tool;
    return std::nullopt;
}

} /* namespace opencode::msg */

#endif /* OPENCODE_MSG_ROLE_H */

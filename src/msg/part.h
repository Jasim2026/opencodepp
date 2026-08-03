/*
 * part.h -- typed content parts (the unit of a message body).
 *
 * A Part is one of a small closed set of payload types (text, reasoning,
 * image URL, binary blob, a tool call, a tool result, or a turn finish).
 * `part_size_estimate()` gives an upper-ish bound of the heap bytes a part
 * will occupy -- used by store/agent budgeting. No source file needed.
 */
#ifndef OPENCODE_MSG_PART_H
#define OPENCODE_MSG_PART_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "msg/finish.h"

namespace opencode::msg {

enum class PartKind : uint8_t {
    text = 0,
    reasoning = 1,
    image_url = 2,
    binary = 3,
    tool_call = 4,
    tool_result = 5,
    finish = 6,
};

constexpr std::string_view to_string(PartKind k) noexcept {
    switch (k) {
        case PartKind::text: return "text";
        case PartKind::reasoning: return "reasoning";
        case PartKind::image_url: return "image_url";
        case PartKind::binary: return "binary";
        case PartKind::tool_call: return "tool_call";
        case PartKind::tool_result: return "tool_result";
        case PartKind::finish: return "finish";
    }
    return "unknown";
}

struct Text {
    std::string content;
    static constexpr PartKind part_kind() noexcept { return PartKind::text; }
};

struct Reasoning {
    std::string content;
    static constexpr PartKind part_kind() noexcept { return PartKind::reasoning; }
};

struct ImageUrl {
    std::string url;
    static constexpr PartKind part_kind() noexcept { return PartKind::image_url; }
};

struct Binary {
    std::string mime;
    std::vector<std::uint8_t> data;
    static constexpr PartKind part_kind() noexcept { return PartKind::binary; }
};

struct ToolCall {
    std::string id;
    std::string name;
    std::string input_json; /* JSON serialization of the arguments */
    bool finished = false;  /* provider marked the call complete */
    static constexpr PartKind part_kind() noexcept { return PartKind::tool_call; }
};

struct ToolResult {
    std::string call_id; /* ToolCall::id this answers */
    std::string content;
    bool is_error = false;
    static constexpr PartKind part_kind() noexcept { return PartKind::tool_result; }
};

struct Finish {
    FinishReason reason = FinishReason::unknown;
    static constexpr PartKind part_kind() noexcept { return PartKind::finish; }
};

using Part = std::variant<Text, Reasoning, ImageUrl, Binary, ToolCall, ToolResult, Finish>;

inline constexpr size_t kPartCount = std::variant_size_v<Part>;

/* Discriminator of a part (cheap, no allocation). */
inline PartKind part_kind(const Part& p) noexcept {
    return std::visit([](const auto& v) { return v.part_kind(); }, p);
}

/* Nullable accessor, e.g. `as<Text>(p)`; returns nullptr when kind mismatches. */
template <class T>
const T* as(const Part& p) noexcept {
    return std::get_if<T>(&p);
}
template <class T>
T* as(Part& p) noexcept {
    return std::get_if<T>(&p);
}

/* True when `p` holds exactly type T. */
template <class T>
bool holds(const Part& p) noexcept {
    return std::holds_alternative<T>(p);
}

/* Forwarding visitor: `visit([](auto&& v){...}, part)`. */
template <class F>
decltype(auto) visit(F&& f, const Part& p) {
    return std::visit(std::forward<F>(f), p);
}
template <class F>
decltype(auto) visit(F&& f, Part& p) {
    return std::visit(std::forward<F>(f), p);
}

/* Estimated heap footprint of a part in bytes (string lengths + data + a
 * per-object overhead term). Used for budgeting; may slightly over-count. */
inline size_t part_size_estimate(const Part& p) noexcept {
    return std::visit(
        [](const auto& v) -> size_t {
            using V = std::remove_cvref_t<decltype(v)>;
            size_t n = sizeof(V) + 16; /* object + slack */
            if constexpr (std::is_same_v<V, Text> || std::is_same_v<V, Reasoning>) {
                n += v.content.size();
            } else if constexpr (std::is_same_v<V, ImageUrl>) {
                n += v.url.size();
            } else if constexpr (std::is_same_v<V, Binary>) {
                n += v.mime.size() + v.data.size();
            } else if constexpr (std::is_same_v<V, ToolCall>) {
                n += v.id.size() + v.name.size() + v.input_json.size();
            } else if constexpr (std::is_same_v<V, ToolResult>) {
                n += v.call_id.size() + v.content.size();
            }
            return n;
        },
        p);
}

} /* namespace opencode::msg */

#endif /* OPENCODE_MSG_PART_H */

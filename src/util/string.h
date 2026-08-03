/*
 * string.h — small string helpers. Allocation-light: most routines operate on
 * string_views; only the *cat/to_* variants touch a caller-provided buffer or
 * std::string.
 */
#ifndef OPENCODE_UTIL_STRING_H
#define OPENCODE_UTIL_STRING_H

#include <cstdint>
#include <string>
#include <string_view>

namespace opencode::util {

constexpr std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                          s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

constexpr bool starts_with(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}
constexpr bool ends_with(std::string_view s, std::string_view suffix) noexcept {
    return s.size() >= suffix.size() &&
           s.substr(s.size() - suffix.size()) == suffix;
}

constexpr bool contains(std::string_view s, std::string_view needle) noexcept {
    return s.find(needle) != std::string_view::npos;
}

/* Case-insensitive ASCII comparison. */
constexpr char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}
constexpr char ascii_upper(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
}

inline std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(ascii_lower(c));
    return out;
}

/* Split on any character in `delims`. Emits non-empty tokens. */
template <typename F>
void split(std::string_view s, std::string_view delims, F&& fn) {
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t next = s.find_first_of(delims, pos);
        if (next == std::string_view::npos) next = s.size();
        std::string_view tok = s.substr(pos, next - pos);
        if (!tok.empty()) fn(tok);
        pos = next + 1;
    }
}

/* Integer parsing that reports failure (never throws). */
inline bool parse_u64(std::string_view s, uint64_t& out) noexcept {
    if (s.empty()) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        uint64_t d = static_cast<uint64_t>(c - '0');
        if (v > (UINT64_MAX - d) / 10) return false;
        v = v * 10 + d;
    }
    out = v;
    return true;
}
inline bool parse_i64(std::string_view s, int64_t& out) noexcept {
    if (s.empty()) return false;
    bool neg = s.front() == '-';
    if (neg || s.front() == '+') s.remove_prefix(1);
    uint64_t mag = 0;
    if (!parse_u64(s, mag)) return false;
    if (neg) {
        if (mag > (static_cast<uint64_t>(INT64_MAX) + 1)) return false;
        out = mag == (static_cast<uint64_t>(INT64_MAX) + 1)
                  ? INT64_MIN
                  : -static_cast<int64_t>(mag);
    } else {
        if (mag > static_cast<uint64_t>(INT64_MAX)) return false;
        out = static_cast<int64_t>(mag);
    }
    return true;
}

} /* namespace opencode::util */

#endif /* OPENCODE_UTIL_STRING_H */

/*
 * path.h -- allocation-light path helpers (POSIX-style, '/'-separated).
 */
#ifndef OPENCODE_UTIL_PATH_H
#define OPENCODE_UTIL_PATH_H

#include <string>
#include <string_view>

namespace opencode::util {

/* Everything after the last '/'. "a/b.c" -> "b.c", "b.c" -> "b.c". */
inline std::string_view basename(std::string_view p) noexcept {
    size_t i = p.find_last_of('/');
    return i == std::string_view::npos ? p : p.substr(i + 1);
}

/* Everything before the last '/'. "a/b.c" -> "a", "b.c" -> "". */
inline std::string_view dirname(std::string_view p) noexcept {
    size_t i = p.find_last_of('/');
    return i == std::string_view::npos ? std::string_view{} : p.substr(0, i);
}

/* File extension (without the dot). "a/b.c" -> "c", "b" -> "". */
inline std::string_view extension(std::string_view p) noexcept {
    std::string_view base = basename(p);
    size_t i = base.find_last_of('.');
    if (i == std::string_view::npos || i == 0) return {};
    return base.substr(i + 1);
}

/* "a/b" + "c" -> "a/b/c". Skips double slashes; keeps trailing slash if root. */
inline std::string join(std::string_view a, std::string_view b) {
    std::string out;
    out.reserve(a.size() + b.size() + 1);
    out.append(a);
    if (!out.empty() && out.back() != '/' && !b.empty()) out.push_back('/');
    out.append(b);
    return out;
}

inline bool is_absolute(std::string_view p) noexcept {
    return !p.empty() && p.front() == '/';
}

} /* namespace opencode::util */

#endif /* OPENCODE_UTIL_PATH_H */

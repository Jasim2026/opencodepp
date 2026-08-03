#include "core/log.h"

#include <cstring>

namespace opencode::core {

namespace {

/* Escape logfmt values: wrap in quotes when they contain whitespace/quotes. */
size_t append_escaped(char* out, size_t cap, std::string_view v) noexcept {
    const bool needs_quote =
        v.find_first_of(" \t\r\n\"") != std::string_view::npos;
    size_t n = 0;
    if (needs_quote) {
        if (n < cap) out[n++] = '"';
    }
    for (char c : v) {
        if (c == '"') {
            if (n + 1 < cap) { out[n++] = '\\'; out[n++] = '"'; }
        } else if (c == '\\') {
            if (n + 1 < cap) { out[n++] = '\\'; out[n++] = '\\'; }
        } else if (c == '\n') {
            if (n + 1 < cap) { out[n++] = '\\'; out[n++] = 'n'; }
        } else if (c == '\t') {
            if (n + 1 < cap) { out[n++] = '\\'; out[n++] = 't'; }
        } else {
            if (n < cap) out[n++] = c;
        }
    }
    if (needs_quote) {
        if (n < cap) out[n++] = '"';
    }
    return n;
}

} /* namespace */

void Logger::deliver(Level level, const char* buf, size_t n) noexcept {
    const std::string_view line(buf, n);
    if (sink_ != nullptr) sink_(userdata_, level, line);
    if (file_ != nullptr) {
        std::fwrite(buf, 1, n, file_);
        std::fputc('\n', file_);
    }
}

size_t Logger::write_msg(char* out, size_t cap, std::string_view msg) noexcept {
    if (cap == 0) return 0;
    size_t n = 0;
    const std::string_view key = "msg";
    for (char c : key) { if (n < cap - 1) out[n++] = c; }
    if (n < cap - 1) out[n++] = '=';
    n += append_escaped(out + n, cap > n ? cap - n : 0, msg);
    return n;
}

size_t Logger::write_kv(char* out, size_t cap, std::string_view k,
                        std::string_view v) noexcept {
    if (cap == 0) return 0;
    size_t n = 0;
    if (n < cap - 1) out[n++] = ' ';
    for (char c : k) { if (n < cap - 1) out[n++] = c; }
    if (n < cap - 1) out[n++] = '=';
    n += append_escaped(out + n, cap > n ? cap - n : 0, v);
    return n;
}

size_t Logger::write_kv(char* out, size_t cap, std::string_view k,
                        int64_t v) noexcept {
    char num[32];
    int len = std::snprintf(num, sizeof num, "%lld",
                            static_cast<long long>(v));
    if (len < 0) len = 0;
    return write_kv(out, cap, k, std::string_view(num, static_cast<size_t>(len)));
}

size_t Logger::write_kv(char* out, size_t cap, std::string_view k,
                        uint64_t v) noexcept {
    char num[32];
    int len = std::snprintf(num, sizeof num, "%llu",
                            static_cast<unsigned long long>(v));
    if (len < 0) len = 0;
    return write_kv(out, cap, k, std::string_view(num, static_cast<size_t>(len)));
}

size_t Logger::write_kv(char* out, size_t cap, std::string_view k,
                        double v) noexcept {
    char num[48];
    int len = std::snprintf(num, sizeof num, "%.6g", v);
    if (len < 0) len = 0;
    return write_kv(out, cap, k, std::string_view(num, static_cast<size_t>(len)));
}

size_t Logger::write_kv(char* out, size_t cap, std::string_view k,
                        bool v) noexcept {
    return write_kv(out, cap, k, v ? std::string_view("true")
                                   : std::string_view("false"));
}

} /* namespace opencode::core */

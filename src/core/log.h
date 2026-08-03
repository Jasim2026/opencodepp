/*
 * log.h — per-instance logfmt logger. Zero globals; zero heap in the hot path.
 *
 * Lines are formatted into a fixed stack buffer (logfmt: message then
 * key=value pairs) and delivered to a registered sink. Sinks: a C callback
 * (host hook, ABI opencode_log_fn) and/or a FILE* (behind a flag). No
 * std::string, no static buffers, no allocation in the format path.
 */
#ifndef OPENCODE_CORE_LOG_H
#define OPENCODE_CORE_LOG_H

#include <cstdint>
#include <cstdio>
#include <string_view>

namespace opencode::core {

enum class Level : uint8_t { trace = 0, debug = 1, info = 2, warn = 3, error = 4 };

constexpr std::string_view level_name(Level l) noexcept {
    switch (l) {
        case Level::trace: return "trace";
        case Level::debug: return "debug";
        case Level::info:  return "info";
        case Level::warn:  return "warn";
        case Level::error: return "error";
    }
    return "?";
}

class Logger {
public:
    /* Sink receives (userdata, level, formatted line WITHOUT trailing '\n'). */
    using Sink = void (*)(void* userdata, Level level, std::string_view line);

    Logger(Sink sink = nullptr, void* userdata = nullptr,
           Level min_level = Level::info) noexcept
        : sink_(sink), userdata_(userdata), min_level_(min_level), file_(nullptr) {}

    void set_sink(Sink s, void* u) noexcept { sink_ = s; userdata_ = u; }
    void set_min_level(Level l) noexcept { min_level_ = l; }
    Level min_level() const noexcept { return min_level_; }
    void set_file(FILE* f) noexcept { file_ = f; } /* optional file sink */
    bool enabled(Level l) const noexcept { return l >= min_level_; }

    /* Emit a logfmt line: message first, then alternating key/value pairs. */
    template <typename... KV>
    void log(Level level, std::string_view msg, KV&&... kv) noexcept {
        if (!enabled(level)) return;
        char buf[kBuf];
        size_t n = write_msg(buf, sizeof buf, msg);
        write_pair_seq(buf, sizeof buf, n, std::forward<KV>(kv)...);
        deliver(level, buf, n);
    }

    template <typename... KV>
    void trace(std::string_view m, KV&&... kv) noexcept { log(Level::trace, m, std::forward<KV>(kv)...); }
    template <typename... KV>
    void debug(std::string_view m, KV&&... kv) noexcept { log(Level::debug, m, std::forward<KV>(kv)...); }
    template <typename... KV>
    void info(std::string_view m, KV&&... kv) noexcept { log(Level::info, m, std::forward<KV>(kv)...); }
    template <typename... KV>
    void warn(std::string_view m, KV&&... kv) noexcept { log(Level::warn, m, std::forward<KV>(kv)...); }
    template <typename... KV>
    void error(std::string_view m, KV&&... kv) noexcept { log(Level::error, m, std::forward<KV>(kv)...); }

private:
    static constexpr size_t kBuf = 2048;
    Sink sink_;
    void* userdata_;
    Level min_level_;
    FILE* file_;

    void deliver(Level level, const char* buf, size_t n) noexcept;

    static size_t write_msg(char* out, size_t cap, std::string_view msg) noexcept;
    static size_t write_kv(char* out, size_t cap, std::string_view k,
                           std::string_view v) noexcept;
    static size_t write_kv(char* out, size_t cap, std::string_view k,
                           const char* v) noexcept {
        return write_kv(out, cap, k,
                        v == nullptr ? std::string_view{}
                                     : std::string_view(v));
    }
    template <size_t N>
    static size_t write_kv(char* out, size_t cap, std::string_view k,
                           const char (&v)[N]) noexcept {
        return write_kv(out, cap, k, std::string_view(v, N - 1));
    }
    static size_t write_kv(char* out, size_t cap, std::string_view k,
                           int64_t v) noexcept;
    static size_t write_kv(char* out, size_t cap, std::string_view k,
                           uint64_t v) noexcept;
    static size_t write_kv(char* out, size_t cap, std::string_view k,
                           double v) noexcept;
    static size_t write_kv(char* out, size_t cap, std::string_view k,
                           bool v) noexcept;

    /* Convert a key/value pair into the buffer (typed overload resolution). */
    template <typename V>
    static size_t append_pair(char* out, size_t cap, std::string_view k,
                              const V& v) noexcept {
        return write_kv(out, cap, k, v);
    }

    template <typename... KV>
    static void write_pair_seq(char* out, size_t cap, size_t& n) noexcept {
        (void)out; (void)cap; (void)n;
    }
    template <typename K, typename V, typename... Rest>
    static void write_pair_seq(char* out, size_t cap, size_t& n, K&& k, V&& v,
                               Rest&&... rest) noexcept {
        n += append_pair(out + n, cap > n ? cap - n : 0, to_key(k), v);
        write_pair_seq(out, cap, n, std::forward<Rest>(rest)...);
    }

    static std::string_view to_key(std::string_view s) noexcept { return s; }
    static std::string_view to_key(const char* s) noexcept { return s == nullptr ? "" : std::string_view(s); }
};

} /* namespace opencode::core */

#endif /* OPENCODE_CORE_LOG_H */

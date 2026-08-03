/*
 * clock.h — monotonic + wall-clock helpers. No allocation, no global state.
 */
#ifndef OPENCODE_CORE_CLOCK_H
#define OPENCODE_CORE_CLOCK_H

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>

namespace opencode::core {

/* Monotonic clock (steady_clock): for timers, timeouts, benchmarks. */
inline std::chrono::steady_clock::time_point now_mono() noexcept {
    return std::chrono::steady_clock::now();
}

/* Milliseconds since an arbitrary but stable epoch. */
inline uint64_t now_mono_ms() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

inline uint64_t now_mono_us() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

/* Wall-clock seconds since the Unix epoch (double for sub-second precision). */
inline double now_wall_sec() noexcept {
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/* Format wall time into buf as "YYYY-MM-DDTHH:MM:SS" (UTC). Returns length. */
inline int format_wall_utc(char* buf, size_t cap, double secs) noexcept {
    const auto s = static_cast<time_t>(secs);
    tm tm{};
    gmtime_r(&s, &tm);
    return std::snprintf(buf, cap, "%04d-%02d-%02dT%02d:%02d:%02d",
                         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                         tm.tm_hour, tm.tm_min, tm.tm_sec);
}

} /* namespace opencode::core */

#endif /* OPENCODE_CORE_CLOCK_H */

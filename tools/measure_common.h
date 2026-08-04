// measure_common.h -- shared measurement helpers for the Phase 13 tools
// (bench_engine --profile, tools/measure). Inline, stdlib-only, never throws.
#ifndef OPENCODE_TOOLS_MEASURE_COMMON_H
#define OPENCODE_TOOLS_MEASURE_COMMON_H

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace opencode::measure {

using Clock = std::chrono::steady_clock;

template <typename F>
double measure_us(F&& f) {
    const auto t0 = Clock::now();
    f();
    const auto t1 = Clock::now();
    return std::chrono::duration<double, std::micro>(t1 - t0).count();
}

/* Best (minimum) wall time across `trials` runs of `fn`. Taking the minimum
 * rejects scheduler noise; outliers are the *slow* side, so min is the right
 * estimator for a latency budget check. */
inline double min_us(const std::function<void()>& fn, int trials) {
    double best = -1.0;
    for (int i = 0; i < trials; ++i) {
        const double us = measure_us(fn);
        if (best < 0.0 || us < best) best = us;
    }
    return best;
}

/* Current RSS in KiB via /proc/self/status; -1 when unavailable. */
inline long rss_kb() {
    FILE* f = std::fopen("/proc/self/status", "r");
    if (f == nullptr) return -1;
    char line[256];
    long v = -1;
    while (std::fgets(line, sizeof line, f) != nullptr) {
        if (std::sscanf(line, "VmRSS: %ld kB", &v) == 1) break;
    }
    std::fclose(f);
    return v;
}

/* Peak RSS in KiB; -1 when unavailable. */
inline long hwm_kb() {
    FILE* f = std::fopen("/proc/self/status", "r");
    if (f == nullptr) return -1;
    char line[256];
    long v = -1;
    while (std::fgets(line, sizeof line, f) != nullptr) {
        if (std::sscanf(line, "VmHWM: %ld kB", &v) == 1) break;
    }
    std::fclose(f);
    return v;
}

/* Size in bytes of a regular file; -1 when missing/unreadable. */
inline long file_size(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return -1;
    return static_cast<long>(st.st_size);
}

/* Stable RSS estimate: sample every 10 ms `n` times, return the minimum.
 * The min rejects allocator/GC noise; the *slow/leaky* side is a rising RSS,
 * so min is the right estimator for a floor check. */
inline long rss_kb_min(int n = 3) {
    long best = -1;
    for (int i = 0; i < n; ++i) {
        const long v = rss_kb();
        if (best < 0 || (v >= 0 && v < best)) best = v;
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 10'000'000;
        ::nanosleep(&ts, nullptr);
    }
    return best;
}

/* Try to release freed pages back to the OS before an RSS sample. */
inline void trim_allocator() {
#if defined(__GLIBC__)
    ::malloc_trim(0);
#endif
}

struct PerfRow {
    const char* name;
    double min_us;   /* best measured wall time, microseconds */
    double limit_ms; /* the budget (target) in milliseconds    */
    double slack_ms; /* assertion limit = limit + slack        */
    bool pass;
};

inline void print_rows(const PerfRow* rows, int n) {
    std::printf("%-24s %12s %14s %10s\n", "metric", "measured", "target", "status");
    for (int i = 0; i < n; ++i) {
        const double measured_ms = rows[i].min_us / 1000.0;
        std::printf("%-24s %9.3f ms %11.1f ms %10s\n", rows[i].name,
                    measured_ms, rows[i].limit_ms, rows[i].pass ? "PASS" : "FAIL");
    }
}

inline bool rows_ok(const PerfRow* rows, int n) {
    for (int i = 0; i < n; ++i)
        if (!rows[i].pass) return false;
    return true;
}

} /* namespace opencode::measure */

#endif /* OPENCODE_TOOLS_MEASURE_COMMON_H */

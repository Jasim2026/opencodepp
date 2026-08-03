/*
 * metrics.h — per-instance counters, gauges, histograms.
 *
 * Instance-scoped (zero globals). Histograms use fixed exponential buckets so
 * p50/p95 are O(buckets) and allocation-free at observe time. Names must be
 * stable string literals (stored as string_view, never copied).
 */
#ifndef OPENCODE_CORE_METRICS_H
#define OPENCODE_CORE_METRICS_H

#include <cstdint>
#include <string_view>

namespace opencode::core {

class Metrics {
public:
    enum class Kind : uint8_t { counter = 0, gauge = 1, histogram = 2 };

    /* Buckets: 64 slots, exponential in microseconds: slot i covers
     * (2^(i-1) us, 2^i us]; slot 0 covers [0, 1 us]. Enough for ~1 us..2 h. */
    static constexpr size_t kBuckets = 64;

    Metrics() = default;
    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;

    void inc(std::string_view name, uint64_t by = 1) noexcept;
    void dec(std::string_view name, uint64_t by = 1) noexcept;
    void set(std::string_view name, int64_t value) noexcept;
    void observe(std::string_view name, double seconds) noexcept;

    /* Snapshot sink: (name, kind, value, count).
     * For counters/gauges, value = current value, count = 0.
     * For histograms, value = p50 upper edge (seconds), count = samples. */
    using Sink = void (*)(void* userdata, std::string_view name, Kind kind,
                          double value, uint64_t count);

    template <typename F>
    void snapshot(F&& f) const {
        using Fn = std::remove_reference_t<F>;
        Fn* p = &f;
        snapshot_raw(p, [](void* u, std::string_view n, Kind k, double v,
                           uint64_t c) {
            (*static_cast<Fn*>(u))(n, k, v, c);
        });
    }

    /* Percentile of a histogram by name; -1.0 if unknown. */
    double percentile(std::string_view name, double pct) const noexcept;

private:
    struct Entry {
        std::string_view name;
        Kind kind;
        int64_t value;             /* counter/gauge value */
        uint64_t hcount[kBuckets]; /* histogram buckets */
        uint64_t hsamples;
        double hsum;
    };
    Entry* find(std::string_view name) noexcept;
    const Entry* find(std::string_view name) const noexcept;
    Entry* ensure(std::string_view name, Kind kind) noexcept;

    static size_t bucket_for(double seconds) noexcept;
    static double upper_edge(size_t bucket) noexcept; /* seconds */
    double percentile_entry(const Entry& e, double pct) const noexcept;

    void snapshot_raw(void* userdata, Sink sink) const noexcept;

    Entry entries_[64]; /* fixed capacity: 64 distinct metrics per engine */
    size_t count_ = 0;
};

} /* namespace opencode::core */

#endif /* OPENCODE_CORE_METRICS_H */

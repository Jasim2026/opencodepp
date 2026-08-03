/*
 * meter.h -- per-engine network counters (05_NETWORK_RESILIENCE.md Section 6).
 *
 * Exposed to the ABI metrics surface and to tools/bench_engine: bytes/tokens,
 * rtt percentiles (p50/p95 over a capped sample window), retries, reconnects,
 * offline_ms, round_trips, timeouts. Counters are atomics (the metrics reader
 * may be a different thread); the rtt histogram is single-threaded.
 */
#ifndef OPENCODE_NET_METER_H
#define OPENCODE_NET_METER_H

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <vector>

namespace opencode::net {

class Meter {
public:
    static constexpr size_t kMaxRttSamples = 1024;

    void record_rtt(uint64_t ms);
    void add_bytes_out(uint64_t n) noexcept { bytes_out_.fetch_add(n); }
    void add_bytes_in(uint64_t n) noexcept { bytes_in_.fetch_add(n); }
    void add_tokens_in(uint64_t n) noexcept { tokens_in_.fetch_add(n); }
    void add_tokens_out(uint64_t n) noexcept { tokens_out_.fetch_add(n); }
    void inc_round_trip() noexcept { round_trips_.fetch_add(1); }
    void inc_retry() noexcept { retries_.fetch_add(1); }
    void inc_reconnect() noexcept { reconnects_.fetch_add(1); }
    void inc_timeout() noexcept { timeouts_.fetch_add(1); }
    void add_offline_ms(uint64_t ms) noexcept { offline_ms_.fetch_add(ms); }
    void reset() noexcept;

    uint64_t rtt_p50() const noexcept;
    uint64_t rtt_p95() const noexcept;
    size_t rtt_samples() const noexcept { return rtts_.size(); }

    uint64_t bytes_out() const noexcept { return bytes_out_.load(); }
    uint64_t bytes_in() const noexcept { return bytes_in_.load(); }
    uint64_t tokens_in() const noexcept { return tokens_in_.load(); }
    uint64_t tokens_out() const noexcept { return tokens_out_.load(); }
    uint32_t round_trips() const noexcept { return round_trips_.load(); }
    uint32_t retries() const noexcept { return retries_.load(); }
    uint32_t reconnects() const noexcept { return reconnects_.load(); }
    uint32_t timeouts() const noexcept { return timeouts_.load(); }
    uint64_t offline_ms() const noexcept { return offline_ms_.load(); }

    struct Snapshot {
        uint64_t bytes_out = 0;
        uint64_t bytes_in = 0;
        uint64_t tokens_in = 0;
        uint64_t tokens_out = 0;
        uint32_t round_trips = 0;
        uint32_t retries = 0;
        uint32_t reconnects = 0;
        uint32_t timeouts = 0;
        uint64_t offline_ms = 0;
        uint64_t rtt_p50 = 0;
        uint64_t rtt_p95 = 0;
        size_t rtt_samples = 0;
    };
    Snapshot snapshot() const noexcept;

private:
    uint64_t percentile(double p) const noexcept;

    std::atomic<uint64_t> bytes_out_{0};
    std::atomic<uint64_t> bytes_in_{0};
    std::atomic<uint64_t> tokens_in_{0};
    std::atomic<uint64_t> tokens_out_{0};
    std::atomic<uint64_t> offline_ms_{0};
    std::atomic<uint32_t> round_trips_{0};
    std::atomic<uint32_t> retries_{0};
    std::atomic<uint32_t> reconnects_{0};
    std::atomic<uint32_t> timeouts_{0};
    std::vector<uint64_t> rtts_;
};

} /* namespace opencode::net */

#endif /* OPENCODE_NET_METER_H */

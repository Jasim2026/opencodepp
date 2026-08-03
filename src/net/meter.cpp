#include "net/meter.h"

#include <algorithm>

namespace opencode::net {

void Meter::record_rtt(uint64_t ms) {
    if (rtts_.size() >= kMaxRttSamples) {
        /* Ring: drop the oldest sample. */
        rtts_.erase(rtts_.begin());
    }
    rtts_.push_back(ms);
}

void Meter::reset() noexcept {
    bytes_out_ = 0;
    bytes_in_ = 0;
    tokens_in_ = 0;
    tokens_out_ = 0;
    offline_ms_ = 0;
    round_trips_ = 0;
    retries_ = 0;
    reconnects_ = 0;
    timeouts_ = 0;
    rtts_.clear();
}

uint64_t Meter::percentile(double p) const noexcept {
    if (rtts_.empty()) return 0;
    std::vector<uint64_t> sorted = rtts_;
    std::sort(sorted.begin(), sorted.end());
    size_t idx = static_cast<size_t>(p * static_cast<double>(sorted.size()));
    if (idx >= sorted.size()) idx = sorted.size() - 1;
    return sorted[idx];
}

uint64_t Meter::rtt_p50() const noexcept { return percentile(0.50); }
uint64_t Meter::rtt_p95() const noexcept { return percentile(0.95); }

Meter::Snapshot Meter::snapshot() const noexcept {
    Snapshot s;
    s.bytes_out = bytes_out();
    s.bytes_in = bytes_in();
    s.tokens_in = tokens_in();
    s.tokens_out = tokens_out();
    s.round_trips = round_trips();
    s.retries = retries();
    s.reconnects = reconnects();
    s.timeouts = timeouts();
    s.offline_ms = offline_ms();
    s.rtt_p50 = rtt_p50();
    s.rtt_p95 = rtt_p95();
    s.rtt_samples = rtt_samples();
    return s;
}

} /* namespace opencode::net */

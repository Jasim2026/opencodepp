#include "net/policy.h"

namespace opencode::net {

namespace {

/* Deterministic xorshift; `s` must be non-zero. */
uint32_t next_random(uint32_t& s) noexcept {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

} /* namespace */

uint32_t RetryPolicy::jitter_ms(uint64_t base) const noexcept {
    if (base == 0) return 0;
    const uint64_t lo = base - static_cast<uint64_t>(base * budget_.jitter);
    const uint64_t hi = base + static_cast<uint64_t>(base * budget_.jitter);
    if (lo >= hi) return static_cast<uint32_t>(base);
    const uint32_t span = static_cast<uint32_t>(hi - lo + 1);
    return static_cast<uint32_t>(lo) + next_random(seed_) % span;
}

bool RetryPolicy::retryable(core::error_code ec) noexcept {
    return core::retry_class(ec.code()) == core::Retry::retryable;
}

uint64_t RetryPolicy::next_delay_ms(uint32_t attempt) const noexcept {
    uint64_t d = budget_.base_delay_ms;
    for (uint32_t i = 1; i < attempt && d < budget_.max_delay_ms; ++i) {
        d *= 2;
        if (d > budget_.max_delay_ms) d = budget_.max_delay_ms;
    }
    if (d > budget_.max_delay_ms) d = budget_.max_delay_ms;
    return jitter_ms(d);
}

RetryDecision RetryPolicy::decide(core::error_code ec,
                                  uint32_t attempt) const noexcept {
    RetryDecision d;
    if (ec.ok()) {
        d.retry = false;
        d.reason = "ok";
        return d;
    }
    if (!retryable(ec)) {
        d.exhausted = true;
        d.reason = "non-retryable";
        return d;
    }
    if (attempt >= budget_.max_retries) {
        d.exhausted = true;
        d.reason = "exhausted";
        return d;
    }
    d.retry = true;
    d.delay_ms = next_delay_ms(attempt);
    d.reason = "backoff";
    return d;
}

RetryDecision RetryPolicy::decide_retry_after(
    uint32_t retry_after_s, uint32_t attempt) const noexcept {
    RetryDecision d;
    if (attempt >= budget_.max_retries) {
        d.exhausted = true;
        d.reason = "exhausted";
        return d;
    }
    const uint64_t after_ms = static_cast<uint64_t>(retry_after_s) * 1000;
    const uint64_t backoff = next_delay_ms(attempt);
    d.retry = true;
    d.delay_ms = after_ms > backoff ? after_ms : backoff;
    d.reason = "rate-limit";
    return d;
}

} /* namespace opencode::net */

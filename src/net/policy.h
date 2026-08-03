/*
 * policy.h -- retry classification + deterministic backoff.
 *
 * RetryPolicy implements the Phase 4 backoff doctrine
 * (05_NETWORK_RESILIENCE.md Section 3): base 300 ms, factor 2, deterministic
 * jitter seeded per request, cap 30 s, maxRetries 8. A decision is a plain
 * value (RetryDecision) -- never throws, no allocation.
 */
#ifndef OPENCODE_NET_POLICY_H
#define OPENCODE_NET_POLICY_H

#include <cstdint>
#include <string_view>

#include "core/clock.h"
#include "core/error.h"

namespace opencode::net {

struct RetryBudget {
    uint32_t max_retries = 8;
    uint64_t time_cap_ms = 0; /* overall attempt window; 0 = unlimited */
    uint32_t base_delay_ms = 300;
    double jitter = 0.30;     /* +/- ratio on every delay */
    uint32_t max_delay_ms = 30'000;
};

struct RetryDecision {
    bool retry = false;
    uint64_t delay_ms = 0;
    bool exhausted = false;
    std::string_view reason; /* "ok", "non-retryable", "exhausted", "backoff",
                                "rate-limit" */
};

class RetryPolicy {
public:
    explicit RetryPolicy(RetryBudget budget = {}) noexcept : budget_(budget) {}

    /* Seed the jitter stream deterministically for one request (derive from
     * the stable request_id). 0 keeps the default seed. */
    void set_seed(uint32_t seed) noexcept { seed_ = seed ? seed : 0x9e37u; }

    static bool retryable(core::error_code ec) noexcept;

    /* Backoff delay for `attempt` (1-based), jittered and capped. */
    uint64_t next_delay_ms(uint32_t attempt) const noexcept;

    /* Whether to retry `ec` after `attempt` failures of this request. */
    RetryDecision decide(core::error_code ec, uint32_t attempt) const noexcept;

    /* Rate-limit path: the server asked us to wait `retry_after_s` seconds. */
    RetryDecision decide_retry_after(uint32_t retry_after_s,
                                     uint32_t attempt) const noexcept;

    const RetryBudget& budget() const noexcept { return budget_; }

private:
    uint32_t jitter_ms(uint64_t base) const noexcept;

    RetryBudget budget_;
    mutable uint32_t seed_ = 0x9e37u;
};

} /* namespace opencode::net */

#endif /* OPENCODE_NET_POLICY_H */

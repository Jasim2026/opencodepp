/*
 * budget.h -- per-task token + retry budgeting (T1, Phase 6).
 *
 * TokenBudget is the one place the engine turns the T1 caps into a decision:
 *   - hard caps come from config (BudgetCfg::max_tokens_per_task) or the
 *     catalog (context_window, default_max_tokens);
 *   - a floor is enforced: never allocate a budget below what correctness
 *     needs (the request itself), mirroring the "floor, not just cap" rule;
 *   - cost projection uses the catalog prices (model::cost_estimate);
 *   - the retry hook answers "may this failed request be retried?" under the
 *     remaining token budget (budget-aware retry policy).
 *
 * All functions are allocation-free where the public surface is concerned.
 */
#ifndef OPENCODE_PROMPT_BUDGET_H
#define OPENCODE_PROMPT_BUDGET_H

#include <cstdint>
#include <string_view>

#include "core/error.h"
#include "model/catalog.hpp"

namespace opencode::prompt {

/* T1 constants (01_TARGETS_AND_PRINCIPLES.md). */
inline constexpr uint32_t kTargetContextTokens = 3'500;   /* edge profile */
inline constexpr uint32_t kHardCapTokens = 12'000;        /* default task cap */
inline constexpr uint32_t kMinOutputTokens = 1'024;       /* floor for answers */
inline constexpr uint32_t kRetryTokensPerAttempt = 1'500; /* retry allowance */

struct BudgetCaps {
    uint32_t context_target = kTargetContextTokens; /* budget the assembler aims at */
    uint32_t hard_cap = kHardCapTokens;             /* absolute per-task ceiling */
    uint32_t max_output = kMinOutputTokens;         /* tokens reserved for the reply */
    uint32_t context_window = 0;                    /* model cap; 0 = unknown */
    bool edge_mode = false;
};

struct CostProjection {
    uint64_t cents = 0;      /* estimated USD cents for the mix */
    uint64_t input_tokens = 0;
    uint64_t output_tokens = 0;
    uint64_t cached_input_tokens = 0;
};

struct TokenBudget {
    uint64_t total_tokens = 0;      /* granted for this task */
    uint64_t used_tokens = 0;       /* consumed so far */
    uint64_t retry_tokens = 0;      /* consumed by retries */
    uint32_t attempts = 0;          /* request attempts made */
    uint32_t max_attempts = 8;      /* matches net::RetryPolicy default */

    bool is_exhausted() const noexcept { return used_tokens >= total_tokens; }
    uint64_t remaining() const noexcept { return is_exhausted() ? 0u
                                                                : total_tokens - used_tokens; }
};

/* Resolve the per-request caps for a model id (alias-aware). Falls back to
 * defaults when the model is not in the catalog (never fails). */
BudgetCaps caps_for(std::string_view model_id, uint32_t config_hard_cap = 0);

/* Derive a floor: the context budget must at least hold the request window. */
uint32_t budget_floor(uint32_t context_target) noexcept;

/* Create the task budget. */
TokenBudget make_budget(const BudgetCaps& caps) noexcept;

/* Register `n` tokens as consumed; returns false and does not change the
 * budget when the reservation would exceed `total_tokens`. */
bool consume(TokenBudget& b, uint64_t n) noexcept;

/* Cost projection for a planned mix (uses the catalog when the model is
 * known; else a coarse default). */
CostProjection project_cost(std::string_view model_id, uint64_t input_tokens,
                            uint64_t output_tokens,
                            uint64_t cached_input_tokens) noexcept;

/* Retry hook: may this failed attempt be retried under the remaining budget?
 * Mirrors core::retry_class but also reserves the retry allowance. */
bool retry_allowed(const TokenBudget& b, core::error_code ec) noexcept;

} /* namespace opencode::prompt */

#endif /* OPENCODE_PROMPT_BUDGET_H */

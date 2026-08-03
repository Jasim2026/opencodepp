/*
 * budget.cpp -- budget resolution (see budget.h).
 */
#include "prompt/budget.h"

#include <cstdint>

#include "core/error.h"
#include "model/catalog.hpp"

namespace opencode::prompt {

BudgetCaps caps_for(std::string_view model_id, uint32_t config_hard_cap) {
    BudgetCaps caps;
    if (const model::ModelInfo* m = model::find_model(model_id)) {
        caps.context_window = m->context_window;
        caps.max_output = m->default_max_tokens;
    }
    if (config_hard_cap) caps.hard_cap = config_hard_cap;
    /* the floor: context must fit next to a minimal output reservation */
    if (caps.context_window) {
        const uint32_t reserved =
            caps.max_output ? caps.max_output : kMinOutputTokens;
        if (caps.context_window > reserved) {
            const uint32_t window_room = caps.context_window - reserved;
            if (caps.hard_cap > window_room) caps.hard_cap = window_room;
        }
    }
    caps.context_target =
        caps.context_target < caps.hard_cap ? caps.context_target : caps.hard_cap;
    return caps;
}

uint32_t budget_floor(uint32_t context_target) noexcept {
    /* the assembler reserves kNewestUserFloor; keep at least that + output */
    const uint32_t floor = kMinOutputTokens + 800;
    return context_target < floor ? floor : context_target;
}

TokenBudget make_budget(const BudgetCaps& caps) noexcept {
    TokenBudget b;
    const uint64_t floor = budget_floor(caps.context_target);
    b.total_tokens = caps.edge_mode
                         ? (floor > caps.hard_cap ? floor : caps.hard_cap)
                         : caps.hard_cap;
    if (b.total_tokens < floor) b.total_tokens = floor;
    return b;
}

bool consume(TokenBudget& b, uint64_t n) noexcept {
    if (b.used_tokens > b.total_tokens ||
        b.total_tokens - b.used_tokens < n) {
        return false;
    }
    b.used_tokens += n;
    return true;
}

CostProjection project_cost(std::string_view model_id, uint64_t input_tokens,
                            uint64_t output_tokens,
                            uint64_t cached_input_tokens) noexcept {
    CostProjection p{0, input_tokens, output_tokens, cached_input_tokens};
    if (const model::ModelInfo* m = model::find_model(model_id)) {
        p.cents = model::cost_estimate(*m, input_tokens, output_tokens,
                                       cached_input_tokens);
        return p;
    }
    /* coarse default when the model is unknown: assume ~$0.10 per 1M mix */
    p.cents = (input_tokens + output_tokens + cached_input_tokens) / 10'000;
    return p;
}

bool retry_allowed(const TokenBudget& b, core::error_code ec) noexcept {
    if (ec.retry() != core::Retry::retryable) return false;
    /* reserve the retry allowance against the remaining budget */
    if (b.remaining() < kRetryTokensPerAttempt) return false;
    if (b.attempts >= b.max_attempts) return false;
    return true;
}

} /* namespace opencode::prompt */

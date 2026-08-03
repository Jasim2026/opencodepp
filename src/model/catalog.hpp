/*
 * catalog.hpp -- compile-time model & provider catalog.
 *
 * constexpr table of the models the engine plans around: ids, wire names,
 * costs (USD cents per 1M tokens), context windows and capabilities. This
 * drives the Phase 6 budget (T1) and the retry-budget accounting.
 *
 * PRICING: recorded 2025-10 from the providers' public pricing pages.
 * Sub-cent cached-input prices are rounded to the nearest cent. Prices change;
 * re-verify before a shipping release and update the date-stamp + table. Keep
 * the table additive (never renumber a row) so old configs keep working.
 *
 * Everything is header-only: constexpr functions must be visible in every TU
 * that uses them, so there is deliberately no catalog.cpp.
 */
#ifndef OPENCODE_MODEL_CATALOG_HPP
#define OPENCODE_MODEL_CATALOG_HPP

#include <cstdint>
#include <string_view>

namespace opencode::model {

struct ModelInfo {
    std::string_view id;               /* catalog id (also a config alias) */
    std::string_view provider;         /* provider id: "anthropic"|"openai"|"google" */
    std::string_view api_model_name;   /* wire name the provider expects */
    uint32_t in_cents_per_1m;          /* USD cents per 1M input tokens */
    uint32_t out_cents_per_1m;         /* USD cents per 1M output tokens */
    uint32_t cached_in_cents_per_1m;   /* cached-input price (0 = no cache) */
    uint32_t context_window;           /* tokens */
    uint32_t default_max_tokens;       /* default per-call output cap */
    bool can_reason;                   /* chain-of-thought style reasoning */
    bool supports_attachments;         /* image/binary parts */
};

struct ProviderInfo {
    std::string_view id;
    std::string_view api_family; /* wire family: anthropic | openai | google */
    std::string_view base_url;
};

inline constexpr ModelInfo kModels[] = {
    /* anthropic */
    {"claude-sonnet-4-5", "anthropic", "claude-sonnet-4-5",
     300, 1500, 30, 200'000, 64'000, true, true},
    {"claude-haiku-4-5", "anthropic", "claude-haiku-4-5",
     100, 500, 10, 200'000, 32'768, false, true},
    /* openai */
    {"gpt-4.1", "openai", "gpt-4.1",
     200, 800, 50, 1'047'576, 32'768, true, true},
    {"gpt-4.1-mini", "openai", "gpt-4.1-mini",
     40, 160, 10, 1'047'576, 32'768, false, true},
    {"gpt-4.1-nano", "openai", "gpt-4.1-nano",
     10, 40, 0, 1'047'576, 32'768, false, true},
    {"gpt-4o", "openai", "gpt-4o",
     250, 1000, 125, 128'000, 16'384, false, true},
    {"o4-mini", "openai", "o4-mini",
     110, 440, 55, 200'000, 100'000, true, true},
    {"gpt-5", "openai", "gpt-5",
     125, 1000, 13, 400'000, 100'000, true, true},
    /* google */
    {"gemini-2.5-pro", "google", "gemini-2.5-pro",
     125, 1000, 31, 1'048'576, 65'536, true, true},
    {"gemini-2.5-flash", "google", "gemini-2.5-flash",
     30, 250, 8, 1'048'576, 65'536, false, true},
    {"gemini-2.5-flash-lite", "google", "gemini-2.5-flash-lite",
     10, 40, 0, 1'048'576, 65'536, false, true},
};

inline constexpr ProviderInfo kProviders[] = {
    {"anthropic", "anthropic", "https://api.anthropic.com"},
    {"openai", "openai", "https://api.openai.com/v1"},
    {"google", "google",
     "https://generativelanguage.googleapis.com/v1beta"},
};

/* alias -> canonical catalog id (first match wins; scan is short and constexpr) */
inline constexpr std::string_view kAliases[][2] = {
    {"claude", "claude-sonnet-4-5"},
    {"claude-sonnet", "claude-sonnet-4-5"},
    {"sonnet", "claude-sonnet-4-5"},
    {"claude-haiku", "claude-haiku-4-5"},
    {"haiku", "claude-haiku-4-5"},
    {"gpt-4o", "gpt-4o"},
    {"o4-mini", "o4-mini"},
    {"gemini", "gemini-2.5-flash"},
    {"gemini-2.5-pro", "gemini-2.5-pro"},
    {"gemini-2.5-flash", "gemini-2.5-flash"},
};

/* Resolve an id (including aliases) to its canonical catalog id. Empty when
 * the id is unknown. */
inline constexpr std::string_view canonical_id(
    std::string_view id) noexcept {
    for (const ModelInfo& m : kModels) {
        if (m.id == id) return m.id;
    }
    for (const auto& a : kAliases) {
        if (a[0] == id) return a[1];
    }
    return {};
}

/* Catalog lookup by id or alias. Returns nullptr when unknown. */
inline constexpr const ModelInfo* find_model(std::string_view id) noexcept {
    if (const std::string_view canon = canonical_id(id); !canon.empty()) {
        for (const ModelInfo& m : kModels) {
            if (m.id == canon) return &m;
        }
    }
    return nullptr;
}

/* Provider lookup. Returns nullptr when unknown. */
inline constexpr const ProviderInfo* find_provider(
    std::string_view id) noexcept {
    for (const ProviderInfo& p : kProviders) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

/* Provider of a model (by catalog id or alias). nullptr when unknown. */
inline constexpr const ProviderInfo* find_provider_by_model(
    std::string_view model_id) noexcept {
    const ModelInfo* m = find_model(model_id);
    if (m == nullptr) return nullptr;
    return find_provider(m->provider);
}

/* Estimated cost in whole USD cents for a token mix, ceiling-rounded.
 *    cost = ceil((in*in_c + out*out_c + cached*cached_c) / 1_000_000)
 * Integer-only; cannot overflow for any realistic token count. */
inline constexpr uint64_t cost_estimate(const ModelInfo& m, uint64_t in,
                                        uint64_t out,
                                        uint64_t cached_in) noexcept {
    const uint64_t sum = in * m.in_cents_per_1m +
                         out * m.out_cents_per_1m +
                         cached_in * m.cached_in_cents_per_1m;
    return (sum + 999'999) / 1'000'000;
}

} /* namespace opencode::model */

#endif /* OPENCODE_MODEL_CATALOG_HPP */

/*
 * resolver.cpp -- model id -> wire projection.
 *
 * Resolution order per 11_PHASE_05.md Task 6: exact catalog id -> alias ->
 * (model_name, provider). Unknown ids yield e_model_unsup with close-match
 * suggestions (small edit distance over catalog ids + aliases) so the host can
 * present "did you mean ...". Never throws.
 */
#include "provider/provider.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "model/catalog.hpp"

namespace opencode::provider {
namespace {

using namespace opencode::core;

/* Levenshtein distance (bounded: stop early once past `limit`). */
uint32_t edit_distance(std::string_view a, std::string_view b,
                       uint32_t limit) {
    if (a.empty()) return static_cast<uint32_t>(b.size());
    if (b.empty()) return static_cast<uint32_t>(a.size());
    std::vector<uint32_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = static_cast<uint32_t>(j);
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = static_cast<uint32_t>(i);
        uint32_t row_min = cur[0];
        for (size_t j = 1; j <= b.size(); ++j) {
            const uint32_t del = prev[j] + 1;
            const uint32_t ins = cur[j - 1] + 1;
            const uint32_t sub =
                prev[j - 1] + (a[i - 1] == b[j - 1] ? 0u : 1u);
            cur[j] = std::min({del, ins, sub});
            row_min = std::min(row_min, cur[j]);
        }
        if (row_min > limit) return row_min;
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

void suggest(std::string_view id, std::vector<std::string>& out) {
    struct Cand {
        uint32_t dist;
        std::string id;
    };
    std::vector<Cand> cands;
    for (const model::ModelInfo& m : model::kModels)
        cands.push_back({edit_distance(id, m.id, 6), std::string(m.id)});
    for (const auto& a : model::kAliases)
        cands.push_back({edit_distance(id, a[0], 6), std::string(a[0])});
    std::stable_sort(cands.begin(), cands.end(),
                     [](const Cand& a, const Cand& b) {
                         return a.dist < b.dist;
                     });
    for (const Cand& c : cands) {
        if (c.dist == 0 || c.dist > 3) continue;
        bool dup = false;
        for (const std::string& s : out) {
            if (s == c.id) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            out.push_back(c.id);
            if (out.size() >= 3) break;
        }
    }
}

} /* namespace */

error_code resolve_model(std::string_view model_id, ModelSpec& out,
                         std::vector<std::string>* suggestions) {
    const model::ModelInfo* m = model::find_model(model_id);
    if (m == nullptr) {
        if (suggestions != nullptr) suggest(model_id, *suggestions);
        return make_error_code(Err::e_model_unsup);
    }
    const model::ProviderInfo* p = model::find_provider(m->provider);
    out = {};
    out.provider = std::string(m->provider);
    out.api_family = p != nullptr ? std::string(p->api_family) : out.provider;
    out.api_model_name = std::string(m->api_model_name);
    out.base_url = p != nullptr ? std::string(p->base_url) : "";
    out.can_reason = m->can_reason;
    out.supports_attachments = m->supports_attachments;
    out.context_window = m->context_window;
    out.default_max_tokens = m->default_max_tokens;
    return ok();
}

error_code split_url(std::string_view url, UrlParts& out) {
    out = {};
    const size_t sep = url.find("://");
    if (sep == std::string_view::npos || sep == 0)
        return make_error_code(Err::e_invalid_cfg);
    out.scheme = std::string(url.substr(0, sep));
    if (out.scheme != "http" && out.scheme != "https")
        return make_error_code(Err::e_invalid_cfg);
    std::string_view rest = url.substr(sep + 3);
    const size_t slash = rest.find('/');
    const std::string_view authority =
        slash == std::string_view::npos ? rest : rest.substr(0, slash);
    if (slash != std::string_view::npos) {
        out.path = std::string(rest.substr(slash));
        while (!out.path.empty() && out.path.back() == '/')
            out.path.pop_back();
    }
    if (authority.empty()) return make_error_code(Err::e_invalid_cfg);

    const size_t colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
        const std::string_view port_s = authority.substr(colon + 1);
        bool all_digit = !port_s.empty();
        for (const char c : port_s)
            if (c < '0' || c > '9') all_digit = false;
        if (!all_digit) return make_error_code(Err::e_invalid_cfg);
        long v = 0;
        for (const char c : port_s) v = v * 10 + (c - '0');
        if (v < 1 || v > 65535) return make_error_code(Err::e_invalid_cfg);
        out.port = static_cast<uint16_t>(v);
        out.host = std::string(authority.substr(0, colon));
    } else {
        out.host = std::string(authority);
    }
    if (out.host.empty()) return make_error_code(Err::e_invalid_cfg);
    return ok();
}

} /* namespace opencode::provider */

/*
 * factory.cpp -- builds a Provider from a ProviderConfig.
 *
 * The provider id selects the adapter; base_url/path/default_max_tokens come
 * from the config (overriding the catalog defaults). The factory does not read
 * model ids -- the caller resolves the model (resolver.cpp) and passes the
 * merged ModelSpec down to build_request. Never throws.
 */
#include "provider/provider.h"

#include <memory>
#include <string>
#include <string_view>

#include "model/catalog.hpp"

namespace opencode::provider {
namespace {

using namespace opencode::core;

} /* namespace */

error_code make_provider(const ProviderConfig& cfg,
                         std::unique_ptr<Provider>& out) {
    std::string base_url = cfg.base_url;
    uint32_t default_max_tokens = cfg.default_max_tokens;

    if (cfg.id == "openai_compat") {
        if (base_url.empty())
            return make_error_code(Err::e_invalid_cfg);
        return make_openai_compat(std::move(base_url), cfg.api_key,
                                  default_max_tokens, cfg.path, out);
    }

    const model::ProviderInfo* p = model::find_provider(cfg.id);
    if (p == nullptr)
        return make_error_code(Err::e_model_unsup);
    if (base_url.empty()) base_url = std::string(p->base_url);

    if (cfg.id == "anthropic") {
        return make_anthropic(std::move(base_url), cfg.api_key,
                              default_max_tokens, out);
    }
    if (cfg.id == "openai") {
        return make_openai(std::move(base_url), cfg.api_key,
                           default_max_tokens, "/chat/completions", out);
    }
    if (cfg.id == "google") {
        return make_gemini(std::move(base_url), cfg.api_key,
                           default_max_tokens, out);
    }
    return make_error_code(Err::e_model_unsup);
}

} /* namespace opencode::provider */

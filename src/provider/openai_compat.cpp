/*
 * openai_compat.cpp -- OpenAI-compatible endpoint adapter.
 *
 * A thin projection over the OpenAI wire family for providers that speak the
 * /chat/completions dialect but live elsewhere: vLLM, Ollama, Together, LM
 * Studio, etc. Identical to make_openai except the base URL and endpoint path
 * are caller-supplied (config-driven), so token budgeting must stay honest:
 * model catalog costs only apply when the model is known, else the host's
 * config-provided costs are used (Phase 6/8). Never throws.
 */
#include "provider/provider.h"

#include <memory>
#include <string>
#include <string_view>

namespace opencode::provider {

using core::error_code;

error_code make_openai_compat(std::string base_url, std::string api_key,
                              uint32_t default_max_tokens,
                              std::string_view endpoint_path,
                              std::unique_ptr<Provider>& out) {
    return make_openai(std::move(base_url), std::move(api_key),
                       default_max_tokens, endpoint_path, out);
}

} /* namespace opencode::provider */

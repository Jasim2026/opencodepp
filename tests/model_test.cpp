// model_test.cpp -- Phase 3: catalog lookup, aliases, cost math.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "model/catalog.hpp"

namespace {
int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #cond);                                             \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

using opencode::model::cost_estimate;
using opencode::model::find_model;
using opencode::model::find_provider;
using opencode::model::find_provider_by_model;

void test_lookup() {
    const auto* sonnet = find_model("claude-sonnet-4-5");
    CHECK(sonnet != nullptr);
    CHECK(std::string_view(sonnet->provider) == "anthropic");
    CHECK(std::string_view(sonnet->api_model_name) == "claude-sonnet-4-5");
    CHECK(sonnet->context_window == 200'000);
    CHECK(sonnet->can_reason);
    CHECK(sonnet->supports_attachments);

    /* aliases resolve to the same row */
    CHECK(find_model("claude") == sonnet);
    CHECK(find_model("claude-sonnet") == sonnet);
    CHECK(find_model("sonnet") == sonnet);
    CHECK(find_model("haiku") == find_model("claude-haiku-4-5"));
    CHECK(find_model("gpt-4.1") != nullptr);
    CHECK(find_model("gpt-5") != nullptr);
    CHECK(find_model("gemini-2.5-flash-lite") != nullptr);
    CHECK(find_model("gemini") == find_model("gemini-2.5-flash"));

    /* unknown id -> nullptr */
    CHECK(find_model("no-such-model") == nullptr);
    CHECK(find_model("") == nullptr);

    /* case-sensitive */
    CHECK(find_model("CLAUDE-SONNET-4-5") == nullptr);
}

void test_providers() {
    const auto* a = find_provider("anthropic");
    CHECK(a != nullptr);
    CHECK(std::string_view(a->api_family) == "anthropic");
    CHECK(std::string_view(a->base_url) == "https://api.anthropic.com");
    CHECK(find_provider("openai") != nullptr);
    CHECK(find_provider("google") != nullptr);
    CHECK(find_provider("nope") == nullptr);

    CHECK(find_provider_by_model("claude") == a);
    CHECK(find_provider_by_model("gpt-5") == find_provider("openai"));
    CHECK(find_provider_by_model("no-such-model") == nullptr);
}

void test_cost_math() {
    const auto* m = find_model("claude-sonnet-4-5"); /* $3/$15, cache $0.30 */
    CHECK(m != nullptr);

    /* exact 1M-token buckets land on the table prices, in cents */
    CHECK(cost_estimate(*m, 1'000'000, 0, 0) == 300);
    CHECK(cost_estimate(*m, 0, 1'000'000, 0) == 1500);
    CHECK(cost_estimate(*m, 0, 0, 1'000'000) == 30);
    CHECK(cost_estimate(*m, 1'000'000, 1'000'000, 0) == 1800);

    /* partial buckets use ceiling so we never undercharge */
    CHECK(cost_estimate(*m, 500'000, 0, 0) == 150);
    CHECK(cost_estimate(*m, 1, 0, 0) == 1);

    /* a different provider's rate is used per model */
    const auto* flash = find_model("gemini-2.5-flash"); /* $0.30/$2.50 */
    CHECK(cost_estimate(*flash, 1'000'000, 0, 0) == 30);
    CHECK(cost_estimate(*flash, 0, 1'000'000, 0) == 250);

    /* zero-cost model math stays stable */
    const auto* nano = find_model("gpt-4.1-nano"); /* $0.10/$0.40, no cache */
    CHECK(cost_estimate(*nano, 0, 0, 1'000'000) == 0);
}
} /* namespace */

int main() {
    test_lookup();
    test_providers();
    test_cost_math();
    if (failures == 0) {
        std::printf("model_test: OK\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "model_test: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}

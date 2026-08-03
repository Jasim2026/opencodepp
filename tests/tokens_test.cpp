// tokens_test.cpp -- Phase 2: token estimator vs the hand-labeled corpus.
// Gate: estimate error < 10% on every corpus entry (asserted), plus message
// aggregation and the empty/edge cases.
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>

#include "core/error.h"
#include "msg/message.h"
#include "msg/tokens.h"
#include "util/json.h"

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

/* |est - label| / label < 0.10, integer math to avoid fp rounding. */
bool within_10pct(std::size_t est, std::size_t label) {
    if (label == 0) return est == 0;
    const std::size_t diff = est > label ? est - label : label - est;
    return diff * 10 < label;
}

void test_corpus(const char* path) {
    using namespace opencode::core;
    using namespace opencode::util;
    std::string src;
    {
        FILE* f = std::fopen(path, "rb");
        CHECK(f != nullptr);
        if (!f) return;
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        src.resize(static_cast<std::size_t>(n));
        if (n > 0) {
            const std::size_t got =
                std::fread(src.data(), 1, static_cast<std::size_t>(n), f);
            CHECK(got == static_cast<std::size_t>(n));
        }
        std::fclose(f);
    }

    JVal root;
    const error_code ec = parse_json(src, root);
    CHECK(ec.ok());
    if (!ec.ok()) return;
    CHECK(root.kind == JVal::Kind::array);

    for (const JVal& entry : root.arr) {
        const JVal* t = entry.find("t");
        const JVal* s = entry.find("s");
        CHECK(t != nullptr && s != nullptr);
        if (!t || !s) continue;
        CHECK(t->kind == JVal::Kind::number);
        CHECK(s->kind == JVal::Kind::string);
        const std::size_t label = static_cast<std::size_t>(t->num);
        const std::size_t est = opencode::msg::estimate_tokens(s->str);
        if (!within_10pct(est, label)) {
            std::fprintf(stderr,
                         "  corpus %.60s... est=%zu label=%zu\n",
                         std::string(s->str).substr(0, 60).c_str(), est, label);
        }
        CHECK(within_10pct(est, label));
    }
}

void test_message_aggregation() {
    using namespace opencode::msg;
    Message m;
    m.id = "m1";
    m.role = Role::assistant;
    m.parts.push_back(Text{"The rain in Spain falls mainly on the plain."});
    m.parts.push_back(Text{"The rain in Spain falls mainly on the plain."});
    m.parts.push_back(Text{"Go."});
    m.parts.push_back(Finish{FinishReason::end_turn});

    const std::size_t one = estimate_tokens(
        "The rain in Spain falls mainly on the plain.");
    const std::size_t total = estimate_message_tokens(m);
    /* two identical prose parts (cached) + "Go." + frame + finish */
    CHECK(total == 4 + one * 2 + estimate_tokens("Go.") + 1);
    CHECK(estimate_message_tokens(m) == total); /* deterministic */
}

void test_edge_cases() {
    using namespace opencode::msg;
    CHECK(estimate_tokens("") == 0);
    CHECK(estimate_tokens("   ") == 0);
    CHECK(estimate_tokens("hello") == 1);
    CHECK(estimate_tokens("!!!" ) == 1);
    /* pure whitespace message sums to just the frame */
    Message m;
    m.parts.push_back(Text{"   "});
    m.parts.push_back(Text{""});
    CHECK(estimate_message_tokens(m) == 4);
}
} /* namespace */

int main() {
    test_corpus("fixtures/tokens/corpus.json");
    test_message_aggregation();
    test_edge_cases();
    if (failures == 0) {
        std::printf("tokens_test: OK\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "tokens_test: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}

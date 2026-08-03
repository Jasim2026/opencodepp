// prompt_test.cpp -- Phase 6: template compiler, registry, provider-native tool
// schema projection, tiered context assembly, and the T1 budget.
//
// Locally: build and run from the repo root so `src/prompt/templates` and
// `tests/fixtures/responses` resolve (ctest sets WORKING_DIRECTORY to the
// source root).
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.h"
#include "msg/message.h"
#include "msg/tokens.h"
#include "prompt/budget.h"
#include "prompt/compiler.h"
#include "prompt/context.h"
#include "prompt/registry.h"
#include "util/json.h"
#include "util/sha1.h"

namespace {

using namespace opencode;
using namespace opencode::core;
using namespace opencode::msg;
using namespace opencode::provider;
using namespace opencode::prompt;
using opencode::util::JVal;
using opencode::util::sha1_hex;

int failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,   \
                         #cond);                                           \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

std::string read_file(const char* path) {
    std::string out;
    if (FILE* f = std::fopen(path, "rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
            out.append(buf, n);
        std::fclose(f);
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

void add_part(Message& m, Part p) { m.parts.push_back(std::move(p)); }

/* The same golden toolset the Phase 5 request fixtures are built from. */
ToolsSpec golden_tools() {
    ToolsSpec tools;
    ToolSpec t;
    t.id = "add";
    t.name = "add";
    t.description = "Add two numbers";
    t.input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"integer\"},"
        "\"b\":{\"type\":\"integer\"}}}";
    tools.push_back(std::move(t));
    return tools;
}

MsgList small_history() {
    MsgList msgs;
    {
        Message m;
        m.id = "m1";
        m.role = Role::user;
        add_part(m, Text{"Please help."});
        msgs.push_back(std::move(m));
    }
    {
        Message m;
        m.id = "m2";
        m.role = Role::assistant;
        add_part(m, Text{"I can help."});
        msgs.push_back(std::move(m));
    }
    {
        Message m;
        m.id = "m3";
        m.role = Role::user;
        add_part(m, Text{"Now fix the bug."});
        msgs.push_back(std::move(m));
    }
    return msgs;
}

MsgList big_history() {
    MsgList msgs = small_history();
    std::string filler;
    for (int i = 0; i < 20; ++i)
        filler += "This is filler history content that repeats. ";
    for (int i = 0; i < 60; ++i) {
        Message m;
        m.id = "old_" + std::to_string(i);
        m.role = (i % 2) ? Role::user : Role::assistant;
        add_part(m, Text{filler});
        msgs.push_back(std::move(m));
    }
    return msgs;
}

/* ---- sha1 (RFC 3174 vectors) ---- */

void test_sha1() {
    struct V {
        const char* in;
        const char* want;
    };
    const V vec[] = {
        {"", "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
        {"abc", "a9993e364706816aba3e25717850c26c9cd0d89d"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "84983e441c3bd26ebaae4aa1f95129e5e54670f1"},
        {"a", "86f7e437faa5a7fce15d1ddcb9eaeaea377667b8"},
        {"hello world", "2aae6c35c94fcfb415dbe95f408b9ce91ee846ed"},
    };
    for (const V& v : vec) {
        CHECK(sha1_hex(v.in) == v.want);
    }
    std::printf("  sha1 vectors: OK\n");
}

/* ---- compiler ---- */

void test_compiler() {
    const std::string src =
        "System rules.\n{{TOOLS_SCHEMA}}\n{{LANG_STYLE}}\n";
    PromptRef ref;
    CHECK(compile_prompt("system_base", src, ref).ok());
    CHECK(ref.id == "system_base");
    CHECK(ref.parts.size() == 1);
    CHECK(ref.parts[0].kind == PromptPartKind::template_);
    CHECK(ref.parts[0].placeholders.size() == 2);
    CHECK(ref.parts[0].placeholders[0] == "TOOLS_SCHEMA");
    CHECK(ref.parts[0].placeholders[1] == "LANG_STYLE");
    CHECK(ref.sha1.size() == 40);
    CHECK(ref.estimated_tokens > 0);

    /* determinism: same source -> same sha1 */
    PromptRef ref2;
    CHECK(compile_prompt("system_base", src, ref2).ok());
    CHECK(ref2.sha1 == ref.sha1);
    /* different source -> different sha1 */
    PromptRef ref3;
    CHECK(compile_prompt("system_base", src + "x", ref3).ok());
    CHECK(ref3.sha1 != ref.sha1);

    /* file compile + missing file error */
    CHECK(!compile_prompt_file("t", "/nonexistent/prompt.md", ref).ok());
    CHECK(compile_prompt_file("t", "src/prompt/templates/SYSTEM_BASE.md", ref)
              .ok());
    CHECK(ref.id == "t");
    CHECK(ref.estimated_tokens > 0);
    std::printf("  compiler: OK\n");
}

/* ---- registry + load_templates ---- */

void test_registry() {
    PromptRegistry reg;
    PromptRef ref;
    CHECK(compile_prompt("a", "one", ref).ok());
    CHECK(reg.add(std::move(ref)).ok());
    CHECK(reg.find("a") != nullptr);
    CHECK(reg.find("b") == nullptr);
    CHECK(reg.ids().size() == 1 && reg.ids()[0] == "a");

    /* same id replaces */
    PromptRef ref2;
    CHECK(compile_prompt("a", "two", ref2).ok());
    CHECK(reg.add(std::move(ref2)).ok());
    CHECK(reg.ids().size() == 1);
    CHECK(reg.find("a")->parts[0].text == "two");

    /* load_templates: missing dir -> e_missing_cfg; real dir -> stems */
    CHECK(load_templates("/nonexistent/templates", reg) ==
          core::Err::e_missing_cfg);
    CHECK(load_templates("src/prompt/templates", reg).ok());
    const PromptRegistry& c = reg;
    CHECK(c.find("SYSTEM_BASE") != nullptr);
    CHECK(c.find("SYSTEM_VERIFY") != nullptr);
    CHECK(c.find("SYSTEM_MEMORY") != nullptr);
    CHECK(c.find("TOOLS") != nullptr);
    std::printf("  registry: OK\n");
}

/* ---- tool schema projection vs the golden wire fixtures ---- */

void test_tools_schema() {
    const ToolsSpec tools = golden_tools();

    struct V {
        const char* family;
        const char* fixture;
    };
    const V vec[] = {
        {"anthropic", "tests/fixtures/responses/anthropic_request.json"},
        {"openai", "tests/fixtures/responses/openai_request.json"},
        {"google", "tests/fixtures/responses/google_request.json"},
        {"openai_compat", "tests/fixtures/responses/openai_compat_request.json"},
    };
    for (const V& v : vec) {
        std::string generated;
        CHECK(tools_schema_json(tools, v.family, generated).ok());

        const std::string raw = read_file(v.fixture);
        JVal parsed;
        CHECK(parse_json(raw, parsed).ok());
        const JVal* tools_arr = parsed.find("tools");
        CHECK(tools_arr != nullptr);
        if (tools_arr == nullptr) continue;
        const std::string fixture_norm = util::to_json(*tools_arr);
        JVal gen;
        CHECK(parse_json(generated, gen).ok());
        CHECK(util::to_json(gen) == fixture_norm);
    }

    /* token estimate is monotone and nonzero */
    const uint32_t one = tools_schema_tokens(tools, "openai");
    CHECK(one > 0);
    ToolsSpec more = tools;
    ToolSpec t2;
    t2.id = "read_file";
    t2.name = "read_file";
    t2.description = "Read a file";
    t2.input_schema_json = "{\"type\":\"object\"}";
    more.push_back(std::move(t2));
    CHECK(tools_schema_tokens(more, "openai") > one);
    std::printf("  tools_schema: OK\n");
}

/* ---- tiered context assembly ---- */

void test_context() {
    PromptRegistry reg;
    CHECK(load_templates("src/prompt/templates", reg).ok());
    const MsgList hist = small_history();

    ContextInput in;
    in.registry = &reg;
    in.messages = &hist;
    in.tools = nullptr;
    ContextPlan plan;
    CHECK(assemble_context(in, plan).ok());

    /* system message first, newest user message present */
    CHECK(plan.messages.size() >= 3);
    CHECK(plan.messages[0].role == Role::system);
    CHECK(plan.messages.back().content_text().find("Now fix the bug.") !=
          std::string::npos);
    CHECK(plan.estimated_tokens > 0);
    CHECK(plan.bytes > 0);
    CHECK(plan.under_target);
    CHECK(plan.events.empty());

    /* budget gating: older messages omitted, newest kept */
    const MsgList big = big_history();
    ContextInput in2;
    in2.registry = &reg;
    in2.messages = &big;
    in2.available_tokens = 1'200; /* small cap forces Tier-2 omissions */
    ContextPlan plan2;
    CHECK(assemble_context(in2, plan2).ok());
    CHECK(!plan2.events.empty());
    bool saw_omit = false;
    for (const ContextEvent& e : plan2.events)
        if (e.kind == ContextEvent::Kind::omitted) saw_omit = true;
    CHECK(saw_omit);
    CHECK(!plan2.omitted.empty());
    CHECK(plan2.messages.size() < big.size()); /* history was cut */

    /* truncation: huge newest user message -> truncated, suffix present */
    MsgList hugo;
    {
        Message m;
        m.id = "huge";
        m.role = Role::user;
        std::string blob;
        blob.reserve(200'000);
        for (int i = 0; i < 40'000; ++i) blob += "word ";
        blob += "END MARKER";
        add_part(m, Text{std::move(blob)});
        hugo.push_back(std::move(m));
    }
    ContextInput in3;
    in3.registry = &reg;
    in3.messages = &hugo;
    ContextPlan plan3;
    CHECK(assemble_context(in3, plan3).ok());
    bool saw_trunc = false;
    for (const ContextEvent& e : plan3.events)
        if (e.kind == ContextEvent::Kind::truncated) saw_trunc = true;
    CHECK(saw_trunc);
    CHECK(!plan3.truncated.empty());
    CHECK(plan3.messages.back().content_text().find(
              " [context truncated]") != std::string::npos);

    /* errors: no messages, missing registry */
    ContextInput bad;
    ContextPlan p;
    CHECK(assemble_context(bad, p) == core::Err::e_missing_cfg);
    std::printf("  context: OK\n");
}

/* ---- budget ---- */

void test_budget() {
    const BudgetCaps caps = caps_for("claude-haiku-4-5");
    CHECK(caps.context_window == 200'000);
    CHECK(caps.max_output == 32'768);
    CHECK(caps.hard_cap == kHardCapTokens);
    CHECK(caps.context_target == kTargetContextTokens);

    const BudgetCaps capped = caps_for("claude-haiku-4-5", 5'000);
    CHECK(capped.hard_cap == 5'000);
    CHECK(capped.context_target == kTargetContextTokens);

    const BudgetCaps floor_capped = caps_for("claude-haiku-4-5", 1'000);
    CHECK(floor_capped.hard_cap == 1'000);
    /* floor never drops below what the request window needs */
    const uint32_t floor = budget_floor(floor_capped.context_target);
    CHECK(floor >= kMinOutputTokens + 800);

    TokenBudget b = make_budget(caps);
    CHECK(b.total_tokens == kHardCapTokens);
    CHECK(b.remaining() == kHardCapTokens);
    CHECK(!b.is_exhausted());
    CHECK(consume(b, 100));
    CHECK(b.used_tokens == 100);
    CHECK(!consume(b, kHardCapTokens)); /* over budget -> false */
    CHECK(b.used_tokens == 100);

    /* cost projection: known and unknown models */
    const CostProjection p =
        project_cost("claude-haiku-4-5", 10'000, 2'000, 5'000);
    CHECK(p.cents > 0);
    CHECK(p.input_tokens == 10'000 && p.output_tokens == 2'000);
    const CostProjection p2 = project_cost("not-a-real-model", 10'000, 0, 0);
    CHECK(p2.cents > 0);

    /* retry hook */
    TokenBudget rb = make_budget(caps);
    CHECK(!retry_allowed(rb, core::ok()));
    CHECK(retry_allowed(rb, core::make_error_code(core::Err::e_net_timeout)));
    rb.attempts = rb.max_attempts;
    CHECK(!retry_allowed(rb, core::make_error_code(core::Err::e_net_timeout)));
    rb.attempts = 0;
    rb.used_tokens = rb.total_tokens;
    CHECK(!retry_allowed(rb, core::make_error_code(core::Err::e_net_timeout)));
    std::printf("  budget: OK\n");
}

} /* namespace */

int main() {
    test_sha1();
    test_compiler();
    test_registry();
    test_tools_schema();
    test_context();
    test_budget();
    if (failures == 0) {
        std::printf("prompt_test: OK\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "prompt_test: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}

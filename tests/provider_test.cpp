/*
 * provider_test.cpp -- wire-contract tests for the Phase 5 provider layer.
 *
 * Golden-byte request fixtures (tests/fixtures/responses/) pin each adapter's
 * serialization; streaming fixtures pin the normalised StreamEvent sequence;
 * negative + fuzz cases prove no frame can crash or leak (ASan runs it on CI).
 * Local verification: plain g++ build-and-run from the tests/ directory so the
 * fixture paths resolve.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/error.h"
#include "msg/message.h"
#include "provider/provider.h"

namespace {

using namespace opencode;
using namespace opencode::core;
using namespace opencode::msg;
using namespace opencode::provider;

int failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,   \
                         #cond);                                           \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

/* ---- golden conversation (must match the fixtures below) ---- */

void add_part(Message& m, Part p) { m.parts.push_back(std::move(p)); }

MsgList golden_msgs() {
    MsgList msgs;
    {
        Message m;
        m.role = Role::system;
        add_part(m, Text{"You are a test assistant."});
        msgs.push_back(std::move(m));
    }
    {
        Message m;
        m.role = Role::user;
        add_part(m, Text{"Please help."});
        msgs.push_back(std::move(m));
    }
    {
        Message m;
        m.role = Role::assistant;
        add_part(m, Text{"I can help."});
        add_part(m, Reasoning{"think about the sum"});
        ToolCall tc;
        tc.id = "tc_1";
        tc.name = "add";
        tc.input_json = "{\"a\":1,\"b\":2}";
        add_part(m, tc);
        msgs.push_back(std::move(m));
    }
    {
        Message m;
        m.role = Role::user;
        ToolResult tr;
        tr.call_id = "tc_1";
        tr.content = "3";
        add_part(m, tr);
        msgs.push_back(std::move(m));
    }
    return msgs;
}

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

ModelSpec golden_model() {
    ModelSpec m;
    m.api_model_name = "claude-sonnet-4-5";
    m.default_max_tokens = 4096;
    return m;
}

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

std::string header_value(const net::HttpHeaders& hs, std::string_view name) {
    for (const net::HttpHeader& h : hs)
        if (h.name == name) return h.value;
    return "";
}

/* ---- golden request bytes ---- */

void test_golden_requests() {
    const MsgList msgs = golden_msgs();
    const ToolsSpec tools = golden_tools();
    const ModelSpec model = golden_model();
    const Budget budget;
    const std::string expected[] = {
        "fixtures/responses/anthropic_request.json",
        "fixtures/responses/openai_request.json",
        "fixtures/responses/google_request.json",
        "fixtures/responses/openai_compat_request.json",
    };

    struct Case {
        std::string name;
        std::string provider_name;
        error_code (*make)(std::unique_ptr<Provider>&);
        std::string path;
        std::string host;
        int fixture;
    };
    const auto mk_anthropic = [](std::unique_ptr<Provider>& o) {
        return make_anthropic("https://api.anthropic.com", "test-key", 0, o);
    };
    const auto mk_openai = [](std::unique_ptr<Provider>& o) {
        return make_openai("https://api.openai.com/v1", "test-key", 0,
                           "/chat/completions", o);
    };
    const auto mk_gemini = [](std::unique_ptr<Provider>& o) {
        return make_gemini("https://generativelanguage.googleapis.com/v1beta",
                           "test-key", 0, o);
    };
    const auto mk_compat = [](std::unique_ptr<Provider>& o) {
        return make_openai_compat("http://127.0.0.1:8123", "test-key", 0,
                                  "/v1/chat/completions", o);
    };

    const Case cases[] = {
        {"anthropic", "anthropic", mk_anthropic, "/v1/messages",
         "api.anthropic.com", 0},
        {"openai", "openai", mk_openai, "/v1/chat/completions",
         "api.openai.com", 1},
        {"google", "gemini", mk_gemini,
         "/v1beta/models/claude-sonnet-4-5:streamGenerateContent?alt=json"
         "&key=test-key",
         "generativelanguage.googleapis.com", 2},
        {"openai_compat", "openai", mk_compat, "/v1/chat/completions",
         "127.0.0.1:8123", 3},
    };

    for (const Case& c : cases) {
        std::unique_ptr<Provider> p;
        CHECK(c.make(p).ok());
        CHECK(p->name() == c.provider_name);
        CHECK(p->supports_native_tools());
        CHECK(p->stream_kind() == net::StreamKind::sse ||
              c.name == "google");

        RequestBytes rb;
        const error_code ec = p->build_request(msgs, tools, model, budget, rb);
        CHECK(ec.ok());
        if (!ec.ok()) continue;

        const std::string want = read_file(expected[c.fixture].c_str());
        if (rb.body != want) {
            std::fprintf(stderr, "  %s: golden bytes differ (%zu vs %zu)\n",
                         c.name.c_str(), rb.body.size(), want.size());
            ++failures;
        }
        CHECK(rb.method == "POST");
        CHECK(rb.path == c.path);
        CHECK(header_value(rb.headers, "Host") == c.host);
        CHECK(header_value(rb.headers, "Content-Type") == "application/json");
        if (c.name == "google")
            CHECK(header_value(rb.headers, "Authorization").empty());
        else
            CHECK(!header_value(rb.headers, "Authorization").empty() ||
                  !header_value(rb.headers, "x-api-key").empty());
    }
    std::printf("  golden request bytes: OK\n");
}

/* ---- anthropic stream ---- */

void test_anthropic_stream() {
    std::unique_ptr<Provider> p;
    CHECK(make_anthropic("https://api.anthropic.com", "k", 0, p).ok());

    std::vector<StreamEvent> ev;
    CHECK(p->parse_frame(StreamFrame{"message_start",
                                     "{\"type\":\"message_start\","
                                     "\"message\":{\"id\":\"m1\",\"usage\":{"
                                     "\"input_tokens\":12,\"output_tokens\":1}}}",
                                     },
                         ev)
              .ok());
    CHECK(ev.size() == 1 && holds<MessageStart>(ev[0]));

    ev.clear();
    CHECK(p->parse_frame(StreamFrame{"content_block_start",
                                     "{\"type\":\"content_block_start\","
                                     "\"index\":0,\"content_block\":{"
                                     "\"type\":\"tool_use\",\"id\":\"tu1\","
                                     "\"name\":\"read_file\",\"input\":{}}}",
                                     },
                         ev)
              .ok());
    CHECK(ev.empty());

    ev.clear();
    CHECK(p->parse_frame(StreamFrame{"content_block_delta",
                                     "{\"type\":\"content_block_delta\","
                                     "\"index\":0,\"delta\":{"
                                     "\"type\":\"input_json_delta\","
                                     "\"partial_json\":\"{\\\"path\\\":\\\"\"}}",
                                     },
                         ev)
              .ok());
    CHECK(ev.size() == 1);
    const ToolCallDelta* d0 = as<ToolCallDelta>(ev[0]);
    CHECK(d0 != nullptr && d0->id == "tu1" && d0->name == "read_file" &&
          d0->args_fragment == "{\"path\":\"");

    ev.clear();
    CHECK(p->parse_frame(StreamFrame{"content_block_delta",
                                     "{\"type\":\"content_block_delta\","
                                     "\"index\":0,\"delta\":{"
                                     "\"type\":\"input_json_delta\","
                                     "\"partial_json\":\"/etc/hosts\\\"}\"}}",
                                     },
                         ev)
              .ok());
    CHECK(ev.size() == 1 && as<ToolCallDelta>(ev[0]) != nullptr);

    ev.clear();
    CHECK(p->parse_frame(
              StreamFrame{"content_block_stop",
                          "{\"type\":\"content_block_stop\",\"index\":0}"}, ev)
              .ok());
    CHECK(ev.size() == 1);
    const ToolCallDone* done = as<ToolCallDone>(ev[0]);
    CHECK(done != nullptr && done->id == "tu1" && done->name == "read_file" &&
          done->input_json == "{\"path\":\"/etc/hosts\"}");

    ev.clear();
    CHECK(p->parse_frame(StreamFrame{"message_delta",
                                     "{\"type\":\"message_delta\","
                                     "\"delta\":{\"stop_reason\":\"tool_use\"},"
                                     "\"usage\":{\"output_tokens\":22}}"},
                         ev)
              .ok());
    CHECK(ev.empty());

    ev.clear();
    CHECK(p->parse_frame(StreamFrame{"message_stop",
                                     "{\"type\":\"message_stop\"}"}, ev)
              .ok());
    CHECK(ev.size() == 1);
    const MessageDone* md = as<MessageDone>(ev[0]);
    CHECK(md != nullptr && md->finish == FinishReason::tool_use);
    CHECK(md->usage.input_tokens == 12 && md->usage.output_tokens == 22);

    /* Interleaved text + reasoning deltas across blocks. */
    p->reset_stream();
    ev.clear();
    CHECK(p->parse_frame(StreamFrame{"message_start",
                                     "{\"message\":{\"usage\":{}}}"}, ev)
              .ok());
    CHECK(p->parse_frame(StreamFrame{"content_block_delta",
                                     "{\"index\":0,\"delta\":{"
                                     "\"type\":\"text_delta\",\"text\":\"Hi\"}}"},
                         ev)
              .ok());
    CHECK(p->parse_frame(StreamFrame{"content_block_delta",
                                     "{\"index\":1,\"delta\":{"
                                     "\"type\":\"thinking_delta\","
                                     "\"thinking\":\"hmm\"}}"},
                         ev)
              .ok());
    CHECK(ev.size() == 3); /* MessageStart + TextDelta + ReasoningDelta */
    CHECK(as<TextDelta>(ev[1]) != nullptr &&
          as<TextDelta>(ev[1])->text == "Hi");
    CHECK(as<ReasoningDelta>(ev[2]) != nullptr &&
          as<ReasoningDelta>(ev[2])->text == "hmm");
    std::printf("  anthropic stream: OK\n");
}

/* ---- openai stream + partial JSON stitching ---- */

void test_openai_stream() {
    std::unique_ptr<Provider> p;
    CHECK(make_openai("https://api.openai.com/v1", "k", 0,
                      "/chat/completions", p)
              .ok());

    std::vector<StreamEvent> ev;
    CHECK(p->parse_frame(StreamFrame{"",
                                     "{\"id\":\"c1\",\"choices\":[{\"index\":0,"
                                     "\"delta\":{\"role\":\"assistant\","
                                     "\"content\":\"Let me\"}}]}"},
                         ev)
              .ok());
    CHECK(ev.size() == 1);
    const TextDelta* t1 = as<TextDelta>(ev[0]);
    CHECK(t1 != nullptr && t1->text == "Let me");

    ev.clear();
    CHECK(p->parse_frame(StreamFrame{"",
                                     "{\"id\":\"c2\",\"choices\":[{\"index\":0,"
                                     "\"delta\":{\"content\":\" check.\"}}]}"},
                         ev)
              .ok());
    CHECK(ev.size() == 1 && as<TextDelta>(ev[0])->text == " check.");

    ev.clear();
    CHECK(p->parse_frame(StreamFrame{"",
                                     "{\"id\":\"c3\",\"choices\":[{\"index\":0,"
                                     "\"delta\":{\"tool_calls\":[{\"index\":0,"
                                     "\"id\":\"call_1\",\"type\":\"function\","
                                     "\"function\":{\"name\":\"read_file\","
                                     "\"arguments\":\"{\\\"path\\\":\"}}]}}]}"},
                         ev)
              .ok());
    CHECK(ev.size() == 1);
    const ToolCallDelta* d1 = as<ToolCallDelta>(ev[0]);
    CHECK(d1 != nullptr && d1->id == "call_1" && d1->name == "read_file" &&
          d1->args_fragment == "{\"path\":");

    ev.clear();
    CHECK(p->parse_frame(StreamFrame{"",
                                     "{\"id\":\"c4\",\"choices\":[{\"index\":0,"
                                     "\"delta\":{\"tool_calls\":[{\"index\":0,"
                                     "\"function\":{\"arguments\":\"\\\"/etc/"
                                     "hosts\\\"}\"}}]}}]}"},
                         ev)
              .ok());
    CHECK(ev.size() == 1 && as<ToolCallDelta>(ev[0]) != nullptr);

    ev.clear();
    CHECK(p->parse_frame(StreamFrame{"",
                                     "{\"id\":\"c5\",\"choices\":[{\"index\":0,"
                                     "\"delta\":{},\"finish_reason\":"
                                     "\"tool_calls\"}],\"usage\":{"
                                     "\"prompt_tokens\":30,"
                                     "\"completion_tokens\":15}}"},
                         ev)
              .ok());
    /* ToolCallDone + MessageDone */
    CHECK(ev.size() == 2);
    const ToolCallDone* done = as<ToolCallDone>(ev[0]);
    CHECK(done != nullptr && done->id == "call_1" && done->name == "read_file" &&
          done->input_json == "{\"path\":\"/etc/hosts\"}");
    const MessageDone* md = as<MessageDone>(ev[1]);
    CHECK(md != nullptr && md->finish == FinishReason::tool_use);
    CHECK(md->usage.input_tokens == 30 && md->usage.output_tokens == 15);

    /* Plain stop completion. */
    p->reset_stream();
    ev.clear();
    CHECK(p->parse_frame(StreamFrame{"",
                                     "{\"choices\":[{\"index\":0,\"delta\":{"
                                     "\"content\":\"Bye\"},"
                                     "\"finish_reason\":\"stop\"}]}"},
                         ev)
              .ok());
    CHECK(ev.size() == 2);
    CHECK(as<TextDelta>(ev[0])->text == "Bye");
    CHECK(as<MessageDone>(ev[1]) != nullptr &&
          as<MessageDone>(ev[1])->finish == FinishReason::end_turn);
    std::printf("  openai stream + stitching: OK\n");
}

/* ---- gemini jsonl stream ---- */

void test_gemini_stream() {
    std::unique_ptr<Provider> p;
    CHECK(make_gemini("https://generativelanguage.googleapis.com/v1beta",
                      "k", 0, p)
              .ok());

    std::vector<StreamEvent> ev;
    CHECK(p->parse_frame(StreamFrame{"",
                                     "{\"candidates\":[{\"content\":{\"parts\":["
                                     "{\"text\":\"Hi\"},{\"text\":\" there\"}]},"
                                     "\"finishReason\":\"\"}],"
                                     "\"usageMetadata\":{\"promptTokenCount\":5,"
                                     "\"candidatesTokenCount\":3}}"},
                         ev)
              .ok());
    CHECK(ev.size() == 2);
    CHECK(as<TextDelta>(ev[0]) != nullptr && as<TextDelta>(ev[0])->text == "Hi");
    CHECK(as<TextDelta>(ev[1]) != nullptr &&
          as<TextDelta>(ev[1])->text == " there");

    ev.clear();
    CHECK(p->parse_frame(StreamFrame{"",
                                     "{\"candidates\":[{\"content\":{\"parts\":["
                                     "{\"functionCall\":{\"name\":\"lookup\","
                                     "\"args\":{\"key\":\"x\"}}}]},"
                                     "\"finishReason\":\"STOP\"}],"
                                     "\"usageMetadata\":{\"promptTokenCount\":5,"
                                     "\"candidatesTokenCount\":10}}"},
                         ev)
              .ok());
    CHECK(ev.size() == 2);
    const ToolCallDone* done = as<ToolCallDone>(ev[0]);
    CHECK(done != nullptr && done->name == "lookup" &&
          done->input_json == "{\"key\":\"x\"}");
    const MessageDone* md = as<MessageDone>(ev[1]);
    CHECK(md != nullptr && md->finish == FinishReason::end_turn);
    CHECK(md->usage.input_tokens == 5 && md->usage.output_tokens == 10);

    /* Thought part -> ReasoningDelta. */
    p->reset_stream();
    ev.clear();
    CHECK(p->parse_frame(StreamFrame{"",
                                     "{\"candidates\":[{\"content\":{\"parts\":["
                                     "{\"text\":\"hmm\",\"thought\":true}]}}]}"},
                         ev)
              .ok());
    CHECK(ev.size() == 1 && as<ReasoningDelta>(ev[0]) != nullptr &&
          as<ReasoningDelta>(ev[0])->text == "hmm");
    std::printf("  gemini jsonl stream: OK\n");
}

/* ---- negative + errors ---- */

void test_negative() {
    std::unique_ptr<Provider> oa, an, gm;
    CHECK(make_openai("https://api.openai.com/v1", "k", 0, "/chat/completions",
                      oa)
              .ok());
    CHECK(make_anthropic("https://api.anthropic.com", "k", 0, an).ok());
    CHECK(make_gemini("https://generativelanguage.googleapis.com/v1beta", "k",
                      0, gm)
              .ok());

    std::vector<StreamEvent> ev;
    /* Bad JSON in every adapter -> ProviderError (never a crash). */
    for (Provider* p : {oa.get(), an.get(), gm.get()}) {
        p->reset_stream();
        ev.clear();
        const error_code ec = p->parse_frame(StreamFrame{"", "not json"}, ev);
        CHECK(ec.ok());
        CHECK(ev.size() == 1);
        const ProviderError* err = as<ProviderError>(ev[0]);
        CHECK(err != nullptr && err->ec.code() == Err::e_proto_parse);
    }

    /* Anthropic unknown event -> ProviderError e_proto_parse. */
    ev.clear();
    CHECK(an->parse_frame(StreamFrame{"bogus_event", "{}"}, ev).ok());
    CHECK(ev.size() == 1 && as<ProviderError>(ev[0]) != nullptr &&
          as<ProviderError>(ev[0])->ec.code() == Err::e_proto_parse);

    /* Anthropic rate-limit error event -> e_rate_limit. */
    ev.clear();
    CHECK(an->parse_frame(StreamFrame{"error",
                                      "{\"error\":{\"type\":"
                                      "\"rate_limit_error\","
                                      "\"message\":\"slow down\"}}"},
                          ev)
              .ok());
    CHECK(ev.size() == 1);
    const ProviderError* rl = as<ProviderError>(ev[0]);
    CHECK(rl != nullptr && rl->ec.code() == Err::e_rate_limit);

    /* Anthropic auth error event -> e_auth. */
    ev.clear();
    CHECK(an->parse_frame(StreamFrame{"error",
                                      "{\"error\":{\"type\":"
                                      "\"authentication_error\","
                                      "\"message\":\"bad key\"}}"},
                          ev)
              .ok());
    CHECK(ev.size() == 1 && as<ProviderError>(ev[0])->ec.code() == Err::e_auth);

    /* Missing fields tolerated (empty message_delta etc.). */
    ev.clear();
    CHECK(an->parse_frame(StreamFrame{"message_delta", "{}"}, ev).ok());
    CHECK(ev.empty());
    ev.clear();
    CHECK(oa->parse_frame(StreamFrame{"", "{}"}, ev).ok());
    CHECK(ev.empty());
    std::printf("  negative fixtures: OK\n");
}

/* ---- usage parsing ---- */

void test_usage_parse() {
    std::unique_ptr<Provider> oa, an, gm;
    CHECK(make_openai("https://api.openai.com/v1", "k", 0, "/chat/completions",
                      oa)
              .ok());
    CHECK(make_anthropic("https://api.anthropic.com", "k", 0, an).ok());
    CHECK(make_gemini("https://generativelanguage.googleapis.com/v1beta", "k",
                      0, gm)
              .ok());

    util::JVal j;
    Usage u;
    CHECK(util::parse_json(
              "{\"usage\":{\"input_tokens\":1,\"output_tokens\":2,"
              "\"cache_read_input_tokens\":3,"
              "\"cache_creation_input_tokens\":4}}",
              j)
              .ok());
    CHECK(an->parse_usage(j, u).ok());
    CHECK(u.input_tokens == 1 && u.output_tokens == 2 &&
          u.cached_input_tokens == 7);

    CHECK(util::parse_json(
              "{\"prompt_tokens\":5,\"completion_tokens\":6,"
              "\"prompt_tokens_details\":{\"cached_tokens\":2}}",
              j)
              .ok());
    CHECK(oa->parse_usage(j, u).ok());
    CHECK(u.input_tokens == 5 && u.output_tokens == 6 &&
          u.cached_input_tokens == 2);

    CHECK(util::parse_json(
              "{\"usageMetadata\":{\"promptTokenCount\":7,"
              "\"candidatesTokenCount\":8,"
              "\"cachedContentTokenCount\":1}}",
              j)
              .ok());
    CHECK(gm->parse_usage(j, u).ok());
    CHECK(u.input_tokens == 7 && u.output_tokens == 8 &&
          u.cached_input_tokens == 1);
    std::printf("  usage parsing: OK\n");
}

/* ---- resolver + url ---- */

void test_resolver() {
    ModelSpec m;
    std::vector<std::string> sug;
    CHECK(resolve_model("claude-sonnet-4-5", m, &sug).ok());
    CHECK(m.provider == "anthropic" && m.api_family == "anthropic");
    CHECK(m.api_model_name == "claude-sonnet-4-5");
    CHECK(m.base_url == "https://api.anthropic.com");
    CHECK(m.context_window > 0 && m.default_max_tokens > 0);
    CHECK(m.can_reason);

    CHECK(resolve_model("claude", m, &sug).ok());
    CHECK(m.api_model_name == "claude-sonnet-4-5");

    CHECK(resolve_model("gemini", m, &sug).ok());
    CHECK(m.provider == "google" && m.api_family == "google");

    sug.clear();
    const error_code ec = resolve_model("gpt-4", m, &sug);
    CHECK(ec.code() == Err::e_model_unsup);
    CHECK(!sug.empty());
    bool has_gpt4o = false;
    for (const std::string& s : sug)
        if (s == "gpt-4o") has_gpt4o = true;
    CHECK(has_gpt4o);
    std::printf("  resolver: OK\n");
}

void test_split_url() {
    UrlParts u;
    CHECK(split_url("https://api.anthropic.com", u).ok());
    CHECK(u.scheme == "https" && u.host == "api.anthropic.com" && u.port == 0);
    CHECK(u.path.empty());

    CHECK(split_url("http://127.0.0.1:8123", u).ok());
    CHECK(u.host == "127.0.0.1" && u.port == 8123 && u.path.empty());

    CHECK(split_url("https://api.openai.com/v1/", u).ok());
    CHECK(u.host == "api.openai.com" && u.path == "/v1");

    CHECK(split_url("ftp://x.example", u).code() == Err::e_invalid_cfg);
    CHECK(split_url("http://host:abc", u).code() == Err::e_invalid_cfg);
    CHECK(split_url("no-scheme", u).code() == Err::e_invalid_cfg);
    std::printf("  split_url: OK\n");
}

/* ---- fuzz: 10k frames per adapter ---- */

uint32_t rng_state = 0x1234567u;
uint32_t rnd() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

void test_fuzz() {
    std::unique_ptr<Provider> adapters[3];
    CHECK(make_openai("https://api.openai.com/v1", "k", 0, "/chat/completions",
                      adapters[0])
              .ok());
    CHECK(make_anthropic("https://api.anthropic.com", "k", 0, adapters[1]).ok());
    CHECK(make_gemini("https://generativelanguage.googleapis.com/v1beta", "k",
                      0, adapters[2])
              .ok());

    const char* events[] = {"", "message_start", "message_delta",
                            "message_stop", "content_block_start",
                            "content_block_delta", "content_block_stop",
                            "error", "ping", "bogus_event"};
    const char* junk = "{}[],\"\\n0123456789abcdef:/_ true falsenull";

    for (auto& ap : adapters) {
        Provider* p = ap.get();
        for (int i = 0; i < 10000; ++i) {
            p->reset_stream();
            std::vector<StreamEvent> ev;
            const std::string event = events[rnd() % 10];
            const size_t n = rnd() % 256;
            std::string data;
            data.reserve(n);
            for (size_t k = 0; k < n; ++k) data += junk[rnd() % 40];
            const error_code ec = p->parse_frame(StreamFrame{event, data}, ev);
            CHECK(ec.ok()); /* errors surface as ProviderError events */
        }
    }

    /* Truncated golden frames must never crash either. */
    const std::string bodies[] = {
        read_file("fixtures/responses/anthropic_request.json"),
        read_file("fixtures/responses/openai_request.json"),
        read_file("fixtures/responses/google_request.json"),
    };
    for (size_t ai = 0; ai < 3; ++ai) {
        for (int i = 0; i < 2000; ++i) {
            adapters[ai]->reset_stream();
            std::vector<StreamEvent> ev;
            const std::string& body = bodies[ai];
            const size_t cut = rnd() % (body.size() + 1);
            (void)adapters[ai]->parse_frame(StreamFrame{"", body.substr(0, cut)},
                                            ev);
        }
    }
    std::printf("  fuzz 10k frames/adapter + truncation: OK\n");
}

} /* namespace */

int main() {
    test_golden_requests();
    test_anthropic_stream();
    test_openai_stream();
    test_gemini_stream();
    test_negative();
    test_usage_parse();
    test_resolver();
    test_split_url();
    test_fuzz();
    if (failures == 0) {
        std::printf("provider_test: OK\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "provider_test: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}

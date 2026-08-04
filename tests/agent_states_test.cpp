/*
 * agent_states_test.cpp -- Phase 10 commit 1: states, session, intent.
 *
 * Covers: the main-lane transition table, orthogonal lanes, terminal exits,
 * Session budget accounting + message history + event emission + provider
 * binding, the intent classifier table, and path-hint extraction.
 *
 * No network is touched (provider binding only constructs the adapter).
 * Runs from the repo root.
 */
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "agent/intent.h"
#include "agent/session.h"
#include "agent/states.h"
#include "config/config.hpp"
#include "msg/finish.h"
#include "provider/provider.h"

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

using opencode::agent::AgentEvent;
using opencode::agent::AgentLane;
using opencode::agent::AgentState;
using opencode::agent::Intent;
using opencode::agent::IntentPlan;
using opencode::agent::Session;
using opencode::agent::SessionOptions;
using opencode::agent::classify_intent;
using opencode::agent::extract_path_hints;
using opencode::agent::format_event;
using opencode::agent::is_terminal;
using opencode::agent::lane_allowed;
using opencode::agent::session_options_from_config;
using opencode::agent::to_string;
using opencode::agent::valid_transition;

void test_state_transitions() {
    CHECK(valid_transition(AgentState::idle, AgentState::preparing));
    CHECK(valid_transition(AgentState::preparing, AgentState::connecting));
    CHECK(valid_transition(AgentState::connecting, AgentState::streaming));
    CHECK(valid_transition(AgentState::streaming, AgentState::tool_phase));
    CHECK(valid_transition(AgentState::streaming, AgentState::verifying));
    CHECK(valid_transition(AgentState::tool_phase, AgentState::verifying));
    CHECK(valid_transition(AgentState::verifying, AgentState::applying));
    CHECK(valid_transition(AgentState::applying, AgentState::done));
    CHECK(valid_transition(AgentState::applying, AgentState::idle));

    /* exits to terminals from any non-terminal state */
    CHECK(valid_transition(AgentState::connecting, AgentState::done));
    CHECK(valid_transition(AgentState::streaming, AgentState::error));
    CHECK(valid_transition(AgentState::tool_phase, AgentState::cancelled));

    /* reset from terminals back to idle */
    CHECK(valid_transition(AgentState::error, AgentState::idle));
    CHECK(valid_transition(AgentState::cancelled, AgentState::idle));

    /* no skipped states / illegal hops */
    CHECK(!valid_transition(AgentState::idle, AgentState::streaming));
    CHECK(!valid_transition(AgentState::idle, AgentState::done));
    CHECK(!valid_transition(AgentState::preparing, AgentState::tool_phase));
    CHECK(!valid_transition(AgentState::streaming, AgentState::preparing));
    CHECK(!valid_transition(AgentState::verifying, AgentState::connecting));
    CHECK(!valid_transition(AgentState::done, AgentState::done));
    CHECK(!valid_transition(AgentState::idle, AgentState::error));

    CHECK(is_terminal(AgentState::done));
    CHECK(is_terminal(AgentState::error));
    CHECK(is_terminal(AgentState::cancelled));
    CHECK(!is_terminal(AgentState::idle));
    CHECK(!is_terminal(AgentState::streaming));

    CHECK(std::string(to_string(AgentState::applying)) == "applying");
    CHECK(std::string(to_string(AgentLane::backoff)) == "backoff");

    CHECK(lane_allowed(AgentState::connecting, AgentLane::backoff));
    CHECK(lane_allowed(AgentState::streaming, AgentLane::offline));
    CHECK(lane_allowed(AgentState::verifying, AgentLane::paused));
    CHECK(!lane_allowed(AgentState::idle, AgentLane::backoff));
    CHECK(!lane_allowed(AgentState::preparing, AgentLane::offline));
    CHECK(!lane_allowed(AgentState::applying, AgentLane::paused));

    AgentEvent ev;
    ev.state = AgentState::streaming;
    ev.lane = AgentLane::none;
    ev.phase = "stream";
    ev.tokens_used = 42;
    const std::string s = format_event(ev);
    CHECK(s.find("streaming") != std::string::npos);
    CHECK(s.find("tokens=42") != std::string::npos);

    std::printf("  states: OK\n");
}

void test_session_budget() {
    opencode::config::Config cfg;
    cfg.budget.max_tokens_per_task = 1000;
    SessionOptions opts;
    opts.workspace = "/tmp/agent_t1";
    opts.config = cfg;
    Session s(opts);

    CHECK(s.budget_remaining() == 1000);
    CHECK(!s.budget_exhausted());
    CHECK(s.account_tokens(600) == 400);
    CHECK(s.tokens_used() == 600);
    CHECK(s.budget_remaining() == 400);
    CHECK(s.account_tokens(400) == 0);
    CHECK(s.budget_exhausted());
    CHECK(s.budget_remaining() == 0);

    std::printf("  session budget: OK\n");
}

void test_session_messages() {
    opencode::config::Config cfg;
    SessionOptions opts;
    opts.workspace = "/tmp/agent_t1";
    opts.config = cfg;
    Session s(opts);

    const std::string uid = s.append_user("hello there");
    CHECK(!uid.empty());
    CHECK(s.messages().size() == 1);
    CHECK(s.messages()[0].role == opencode::msg::Role::user);
    CHECK(s.messages()[0].content_text() == "hello there");

    opencode::msg::ToolCall call;
    call.id = "call-1";
    call.name = "file.write";
    call.input_json = "{\"path\":\"a.txt\"}";
    call.finished = true;
    opencode::provider::Usage usage;
    usage.input_tokens = 10;
    usage.output_tokens = 5;
    s.append_assistant("editing", {call}, opencode::msg::FinishReason::tool_use,
                       usage);
    CHECK(s.messages().size() == 2);
    CHECK(s.messages()[1].role == opencode::msg::Role::assistant);
    CHECK(s.messages()[1].content_text() == "editing");
    CHECK(s.messages()[1].finish_reason() == opencode::msg::FinishReason::tool_use);
    CHECK(s.messages()[1].tool_calls().size() == 1);
    CHECK(s.tokens_used() == 15);

    s.append_tool_result("call-1", "wrote a.txt", false);
    CHECK(s.messages().size() == 3);
    CHECK(s.messages()[2].role == opencode::msg::Role::user);
    CHECK(s.messages()[2].tool_results().size() == 1);
    CHECK(s.messages()[2].tool_results()[0]->call_id == "call-1");

    std::printf("  session messages: OK\n");
}

struct Sink {
    void* ud = nullptr;
    std::vector<AgentEvent> events;
};
void on_event(void* userdata, const AgentEvent& ev) {
    auto* s = static_cast<Sink*>(userdata);
    s->events.push_back(ev);
}

void test_session_events() {
    opencode::config::Config cfg;
    SessionOptions opts;
    opts.workspace = "/tmp/agent_t1";
    opts.config = cfg;
    Session s(opts);

    Sink sink;
    s.set_event_fn(on_event, &sink);

    s.set_state(AgentState::preparing);
    s.emit("intent", "classified", 0);
    s.set_state(AgentState::connecting);
    s.emit("connect", "dialing", 0);
    s.set_state(AgentState::streaming);
    s.account_tokens(50);
    s.emit("stream", "first delta", 0);

    CHECK(sink.events.size() == 3);
    CHECK(sink.events[0].state == AgentState::preparing);
    CHECK(sink.events[0].phase == "intent");
    CHECK(sink.events[0].tokens_used == 0);
    CHECK(sink.events[1].state == AgentState::connecting);
    CHECK(sink.events[1].phase == "connect");
    CHECK(sink.events[2].state == AgentState::streaming);
    CHECK(sink.events[2].phase == "stream");
    CHECK(sink.events[2].tokens_used == 50);

    /* invalid transitions are dropped */
    s.set_state(AgentState::streaming);
    s.set_state(AgentState::idle); /* illegal hop -> dropped */
    CHECK(s.state() == AgentState::streaming);
    s.set_lane(AgentLane::backoff);
    CHECK(s.lane() == AgentLane::backoff);
    s.set_state(AgentState::tool_phase); /* stream -> tool_phase, lane clears */
    CHECK(s.state() == AgentState::tool_phase);
    CHECK(s.lane() == AgentLane::backoff); /* lane still set (allowed in tool_phase) */

    /* request ids: stable per attempt, distinct per attempt */
    const std::string r1 = s.begin_request();
    const std::string r2 = s.begin_request();
    CHECK(!r1.empty() && r1 != r2);

    std::printf("  session events: OK\n");
}

void test_session_provider() {
    opencode::config::Config cfg;
    cfg.providers.push_back({});
    cfg.providers.back().id = "anthropic";
    cfg.providers.back().api_key = "k";
    cfg.agents.push_back({});
    cfg.agents.back().id = "default";
    cfg.agents.back().model = "claude-haiku-4-5";

    SessionOptions opts;
    opts.workspace = "/tmp/agent_t1";
    opts.config = cfg;
    CHECK(session_options_from_config(cfg, "default", "/tmp/agent_t1", opts).ok());
    CHECK(opts.provider_id == "anthropic");
    CHECK(opts.model.api_model_name == "claude-haiku-4-5");

    Session s(opts);
    CHECK(s.ensure_provider().ok());
    CHECK(s.provider() != nullptr);
    CHECK(s.provider()->name() == std::string_view("anthropic"));

    /* openai_compat (mock) binding via explicit options */
    SessionOptions m;
    m.workspace = "/tmp/agent_t1";
    m.config = cfg;
    m.provider_id = "openai_compat";
    m.model.api_family = "openai";
    m.model.api_model_name = "mock";
    m.model.base_url = "http://127.0.0.1:8123";
    Session sm(m);
    CHECK(sm.ensure_provider().ok());
    CHECK(sm.provider()->name() == std::string_view("openai"));

    /* unknown provider id -> clean error */
    SessionOptions bad;
    bad.workspace = "/tmp/agent_t1";
    bad.config = cfg;
    bad.provider_id = "nope";
    Session sb(bad);
    CHECK(!sb.ensure_provider().ok());

    std::printf("  session provider: OK\n");
}

void check_intent(std::string_view text, Intent expected) {
    const IntentPlan p = classify_intent(text);
    if (p.intent != expected) {
        std::fprintf(stderr, "  intent mismatch for '%s': got %s want %s\n",
                     std::string(text).c_str(),
                     std::string(opencode::agent::to_string(p.intent)).c_str(),
                     std::string(opencode::agent::to_string(expected)).c_str());
        ++failures;
    }
}

void test_intent_classifier() {
    check_intent("can you remember to use tabs", Intent::meta);
    check_intent("run the tests for core", Intent::shell);
    check_intent("please run ./build.sh", Intent::shell);
    check_intent("write a unit test for TaskScheduler", Intent::test);
    check_intent("add tests for the agent loop", Intent::test);
    check_intent("fix the crash in session.cpp", Intent::bugfix);
    check_intent("this function is broken", Intent::bugfix);
    check_intent("please fix the failing output", Intent::bugfix);
    check_intent("refactor the network layer", Intent::refactor);
    check_intent("explore where the rate limiter lives", Intent::explore);
    check_intent("show me the files in src/agent", Intent::explore);
    check_intent("explain what event_loop does", Intent::explain);
    check_intent("what is the token budget?", Intent::ask);
    check_intent("edit src/agent/session.cpp to log more", Intent::edit);
    check_intent("change the timeout to 5s", Intent::edit);
    check_intent("rewrite the policy module", Intent::edit);
    check_intent("create a new module for metrics", Intent::edit);
    check_intent("implement a retry loop", Intent::edit);
    /* no keyword, but a file path -> edit */
    check_intent("review src/net/policy.h carefully", Intent::edit);
    /* nothing matched -> unknown (Uncertain), never guessed */
    check_intent("good morning", Intent::unknown);
    check_intent("please continue", Intent::unknown);

    /* intent plans carry the right profiles */
    const IntentPlan ask = classify_intent("what is x?");
    CHECK(ask.budget_profile == "minimal");
    CHECK(!ask.wants_tools);
    CHECK(!ask.write_allowed);
    const IntentPlan ed = classify_intent("edit src/agent/loop.cpp to handle 404");
    CHECK(ed.budget_profile == "edit");
    CHECK(ed.wants_tools);
    CHECK(ed.write_allowed);

    std::printf("  intent classifier: OK\n");
}

void test_path_hints() {
    const auto h1 = extract_path_hints("read src/agent/session.cpp and docs/api.md");
    CHECK(h1.size() == 2);
    CHECK(h1[0] == "src/agent/session.cpp");
    CHECK(h1[1] == "docs/api.md");

    const auto h2 = extract_path_hints("open \"src/net/policy.h\" please");
    CHECK(h2.size() == 1);
    CHECK(h2[0] == "src/net/policy.h");

    const auto h3 = extract_path_hints("no files mentioned here at all");
    CHECK(h3.empty());

    std::printf("  path hints: OK\n");
}

} /* namespace */

int main() {
    test_state_transitions();
    test_session_budget();
    test_session_messages();
    test_session_events();
    test_session_provider();
    test_intent_classifier();
    test_path_hints();

    if (failures != 0) {
        std::fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    std::printf("agent_states_test: all OK\n");
    return 0;
}

/*
 * session.h -- one agent session: the mutable state of a task (Phase 10 T1).
 *
 * A Session owns the message history, the token budget, the provider binding,
 * the workspace handle, and the event sink. It is created from a Config +
 * resolved model and can be serialized through the Store (Phase 3) for resume.
 * All methods are non-throwing and follow the engine conventions.
 *
 * The Session is deliberately the ONLY place the loop keeps task state: the
 * loop itself (loop.cpp) holds no globals and stays deterministic given a
 * session + injected provider/gate.
 */
#ifndef OPENCODE_AGENT_SESSION_H
#define OPENCODE_AGENT_SESSION_H

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "agent/states.h"
#include "config/config.hpp"
#include "core/error.h"
#include "core/log.h"
#include "msg/message.h"
#include "provider/provider.h"
#include "store/store.h"

namespace opencode::agent {

/* Everything the loop needs to construct a Session. `store` is optional:
 * when set, persist() writes the session + messages so the host can resume. */
struct SessionOptions {
    std::string workspace;          /* sandbox base for tools/gate          */
    std::string session_id;         /* "" = engine-generated                */
    config::Config config;
    provider::ModelSpec model;      /* resolved model (resolver.cpp)        */
    std::string provider_id;        /* provider cfg id to bind              */
    std::string api_key;            /* from the provider cfg / env          */
    core::Logger* log = nullptr;
    store::Store* store = nullptr;
};

class Session {
public:
    explicit Session(SessionOptions opts);
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session();

    /* ---- identity / config ---- */
    const std::string& id() const noexcept { return id_; }
    const std::string& workspace() const noexcept { return workspace_; }
    const config::Config& config() const noexcept { return cfg_; }
    const provider::ModelSpec& model() const noexcept { return model_; }
    core::Logger* log() const noexcept { return log_; }

    /* ---- message history ---- */
    const std::vector<msg::Message>& messages() const noexcept {
        return messages_;
    }
    /* Append the user's text as a user message; returns its id. */
    std::string append_user(std::string_view text);
    /* Append the assistant's reply (text + optional tool calls + finish). */
    std::string append_assistant(std::string text,
                                 std::vector<msg::ToolCall> calls,
                                 msg::FinishReason finish,
                                 const provider::Usage& usage);
    /* Append one tool result answering a ToolCall by id. */
    void append_tool_result(std::string_view call_id, std::string_view content,
                            bool is_error);

    /* ---- token budget (T1) ---- */
    std::uint64_t tokens_used() const noexcept { return tokens_used_; }
    std::uint64_t budget_remaining() const noexcept;
    bool budget_exhausted() const noexcept;
    /* Account `n` consumed tokens; returns the new remaining. */
    std::uint64_t account_tokens(std::uint64_t n);

    /* ---- provider binding ---- */
    /* Build the provider from the options (idempotent). e_model_unsup /
     * e_invalid_cfg on bad config. Never connects the network. */
    core::error_code ensure_provider();
    provider::Provider* provider() noexcept { return provider_.get(); }
    const provider::Provider* provider() const noexcept { return provider_.get(); }

    /* ---- idempotency key (Phase 4 retry) ---- */
    /* Begin a new request attempt window; the returned id is stable for all
     * retries of the same attempt and differs per attempt. */
    std::string begin_request();
    const std::string& request_id() const noexcept { return request_id_; }

    /* ---- state machine + events ---- */
    AgentState state() const noexcept { return state_; }
    AgentLane lane() const noexcept { return lane_; }
    /* Transition to `to`; invalid transitions are dropped with a log (the
     * loop guards its calls with valid_transition; this is a safety net). */
    void set_state(AgentState to);
    void set_lane(AgentLane l);
    bool transition(AgentState from, AgentState to);

    using EventFn = void (*)(void* userdata, const AgentEvent& ev);
    void set_event_fn(EventFn fn, void* userdata) noexcept {
        event_fn_ = fn;
        event_ud_ = userdata;
    }
    /* Emit an event with the current state/lane + token count. */
    void emit(std::string_view phase, std::string_view detail,
              std::int32_t status = 0);
    void emit(AgentState state, std::string_view phase,
              std::string_view detail, std::int32_t status = 0);

    /* ---- persistence (optional Store) ---- */
    /* Persist the session + all messages; no-op when no Store is attached.
     * Returns the session id ("" on failure). */
    std::string persist();

    /* Access the optional Store (nullable). Used by memory checkpoint/resume. */
    store::Store* store() const noexcept { return store_; }

private:
    std::string next_msg_id();
    std::uint64_t budget_cap() const noexcept;
    void emit_locked(std::string_view phase, std::string_view detail,
                     std::int32_t status);

    std::string id_;
    std::string workspace_;
    config::Config cfg_;
    provider::ModelSpec model_;
    std::string provider_id_;
    std::string api_key_;
    core::Logger* log_;
    store::Store* store_;

    std::vector<msg::Message> messages_;
    std::uint64_t tokens_used_ = 0;
    std::uint64_t msg_seq_ = 0;

    std::unique_ptr<provider::Provider> provider_;
    std::string request_id_;
    std::uint64_t req_seq_ = 0;

    AgentState state_ = AgentState::idle;
    AgentLane lane_ = AgentLane::none;
    EventFn event_fn_ = nullptr;
    void* event_ud_ = nullptr;
};

/* Convenience: build SessionOptions from a Config. Resolves `agent_id`
 * (first agent by default) and its model, then the provider cfg for the
 * model's provider id (first provider with that id; provider_id is required).
 * Returns e_invalid_cfg when the pieces are missing. */
core::error_code session_options_from_config(
    const config::Config& cfg, const std::string& agent_id,
    const std::string& workspace, SessionOptions& out);

} /* namespace opencode::agent */

#endif /* OPENCODE_AGENT_SESSION_H */

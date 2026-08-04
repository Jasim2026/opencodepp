/*
 * session.cpp -- Session implementation (see session.h).
 *
 * Message ids and request ids are deterministic within a session (a monotonic
 * per-session counter) so tests and resume logs are stable. The event sink is
 * a plain C callback pair (ABI-friendly); emit() never throws and never
 * recurses. persist() is best-effort: a missing/optional Store is a no-op.
 */
#include "agent/session.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/clock.h"

namespace opencode::agent {

namespace {

using namespace opencode::core;

} /* namespace */

Session::Session(SessionOptions opts)
    : id_(opts.session_id.empty() ? "sess-" + std::to_string(core::now_wall_sec())
                                  : opts.session_id),
      workspace_(opts.workspace),
      cfg_(std::move(opts.config)),
      model_(std::move(opts.model)),
      provider_id_(std::move(opts.provider_id)),
      api_key_(std::move(opts.api_key)),
      log_(opts.log),
      store_(opts.store) {}

Session::~Session() = default;

std::string Session::next_msg_id() {
    return "m" + std::to_string(++msg_seq_);
}

std::uint64_t Session::budget_cap() const noexcept {
    std::uint64_t cap = cfg_.budget.max_tokens_per_task;
    for (const config::AgentCfg& a : cfg_.agents) {
        if (a.max_tokens != 0) {
            cap = a.max_tokens;
            break;
        }
    }
    return cap;
}

std::uint64_t Session::budget_remaining() const noexcept {
    const std::uint64_t cap = budget_cap();
    return tokens_used_ >= cap ? 0 : cap - tokens_used_;
}

bool Session::budget_exhausted() const noexcept { return budget_remaining() == 0; }

std::uint64_t Session::account_tokens(std::uint64_t n) {
    tokens_used_ += n;
    return budget_remaining();
}

std::string Session::append_user(std::string_view text) {
    msg::Message m;
    m.id = next_msg_id();
    m.session_id = id_;
    m.role = msg::Role::user;
    m.created_at = static_cast<std::uint64_t>(core::now_wall_sec());
    m.parts.push_back(msg::Text{std::string(text)});
    messages_.push_back(m);
    if (store_ != nullptr) store_->message_save(m);
    return m.id;
}

std::string Session::append_assistant(std::string text,
                                      std::vector<msg::ToolCall> calls,
                                      msg::FinishReason finish,
                                      const provider::Usage& usage) {
    msg::Message m;
    m.id = next_msg_id();
    m.session_id = id_;
    m.role = msg::Role::assistant;
    m.model = model_.api_model_name;
    m.created_at = static_cast<std::uint64_t>(core::now_wall_sec());
    if (!text.empty()) m.parts.push_back(msg::Text{std::move(text)});
    for (msg::ToolCall& c : calls) m.parts.push_back(std::move(c));
    if (finish != msg::FinishReason::unknown)
        m.parts.push_back(msg::Finish{finish});
    messages_.push_back(m);
    if (store_ != nullptr) store_->message_save(m);
    if (usage.input_tokens != 0 || usage.output_tokens != 0)
        account_tokens(usage.input_tokens + usage.output_tokens);
    return m.id;
}

void Session::append_tool_result(std::string_view call_id,
                                 std::string_view content, bool is_error) {
    msg::Message m;
    m.id = next_msg_id();
    m.session_id = id_;
    /* Tool results are not a wire role for any provider family: they live
     * inside user messages as ToolResult parts (the adapters split them into
     * role:tool / tool_result / functionResponse wire blocks). */
    m.role = msg::Role::user;
    m.created_at = static_cast<std::uint64_t>(core::now_wall_sec());
    m.parts.push_back(msg::ToolResult{std::string(call_id), std::string(content),
                                      is_error});
    messages_.push_back(m);
    if (store_ != nullptr) store_->message_save(m);
}

std::string Session::begin_request() {
    request_id_ = id_ + "-req" + std::to_string(++req_seq_);
    return request_id_;
}

core::error_code Session::ensure_provider() {
    if (provider_ != nullptr) return core::ok();
    provider::ProviderConfig pc;
    pc.id = provider_id_;
    pc.api_key = api_key_;
    pc.base_url = model_.base_url;
    pc.default_max_tokens = model_.default_max_tokens;
    if (pc.id.empty()) return core::make_error_code(core::Err::e_invalid_cfg);
    return provider::make_provider(pc, provider_);
}

void Session::set_state(AgentState to) {
    if (!valid_transition(state_, to)) {
        if (log_ != nullptr)
            log_->warn("drop invalid transition",
                       "from", agent::to_string(state_),
                       "to", agent::to_string(to));
        return;
    }
    state_ = to;
}

bool Session::transition(AgentState from, AgentState to) {
    if (state_ != from) return false;
    set_state(to);
    return state_ == to;
}

void Session::set_lane(AgentLane l) {
    if (!lane_allowed(state_, l)) {
        if (log_ != nullptr)
            log_->warn("drop invalid lane", "lane", agent::to_string(l));
        return;
    }
    lane_ = l;
}

void Session::emit_locked(std::string_view phase, std::string_view detail,
                          std::int32_t status) {
    if (event_fn_ == nullptr) return;
    AgentEvent ev;
    ev.state = state_;
    ev.lane = lane_;
    ev.phase = std::string(phase);
    ev.detail = std::string(detail);
    ev.tokens_used = tokens_used_;
    ev.status = status;
    event_fn_(event_ud_, ev);
}

void Session::emit(std::string_view phase, std::string_view detail,
                   std::int32_t status) {
    emit_locked(phase, detail, status);
}

void Session::emit(AgentState state, std::string_view phase,
                   std::string_view detail, std::int32_t status) {
    if (valid_transition(state_, state)) state_ = state;
    emit_locked(phase, detail, status);
}

std::string Session::persist() {
    if (store_ == nullptr) return "";
    store::Session s;
    s.id = id_;
    s.title = messages_.empty() ? "" : messages_.front().content_text();
    const store::Session saved = store_->session_save(s);
    if (saved.id.empty()) return "";
    for (const msg::Message& m : messages_) store_->message_save(m);
    return saved.id;
}

core::error_code session_options_from_config(
    const config::Config& cfg, const std::string& agent_id,
    const std::string& workspace, SessionOptions& out) {
    if (cfg.providers.empty())
        return core::make_error_code(core::Err::e_invalid_cfg);
    const config::AgentCfg* agent = nullptr;
    for (const config::AgentCfg& a : cfg.agents) {
        if (a.id == agent_id) {
            agent = &a;
            break;
        }
    }
    if (agent == nullptr && !cfg.agents.empty()) agent = &cfg.agents.front();
    const std::string model_id =
        agent != nullptr && !agent->model.empty() ? agent->model : "";
    if (model_id.empty())
        return core::make_error_code(core::Err::e_invalid_cfg);

    provider::ModelSpec model;
    const core::error_code ec = provider::resolve_model(model_id, model);
    if (!ec.ok()) {
        /* Not a catalog model. Fall back to the first openai_compat provider
         * (mock/hosted endpoints such as the Phase 4 mock_api) and project the
         * id straight through as the wire model name. */
        const config::ProviderCfg* compat = nullptr;
        for (const config::ProviderCfg& p : cfg.providers) {
            if (p.id == "openai_compat") {
                compat = &p;
                break;
            }
        }
        if (compat == nullptr || compat->base_url.empty())
            return ec;
        model.api_family = "openai";
        model.provider = "openai_compat";
        model.api_model_name = model_id;
        model.base_url = compat->base_url;
    }

    const config::ProviderCfg* pc = nullptr;
    for (const config::ProviderCfg& p : cfg.providers) {
        if (p.id == model.provider) {
            pc = &p;
            break;
        }
    }
    if (pc == nullptr) {
        /* openai_compat hosts name the provider; fall back to the first. */
        pc = &cfg.providers.front();
        if (!model.base_url.empty() && !pc->base_url.empty())
            model.base_url = pc->base_url;
    } else {
        if (!pc->base_url.empty()) model.base_url = pc->base_url;
    }

    out = SessionOptions{};
    out.workspace = workspace;
    out.config = cfg;
    out.model = std::move(model);
    out.provider_id = pc->id;
    out.api_key = pc->api_key;
    return core::ok();
}

} /* namespace opencode::agent */

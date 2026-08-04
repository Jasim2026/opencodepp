/*
 * loop.cpp -- the agent drive loop (see loop.h).
 *
 * The 5-stage pipeline per iteration: intent -> context assembly -> provider
 * prompt -> single cloud call (streamed) -> native verify of write proposals.
 * One cloud call per iteration; tool rounds append results and iterate again,
 * each round bounded by the Phase 6 token budget. Edits reach the workspace
 * ONLY through the Phase 9 gate.
 *
 * Failure doctrine (05_NETWORK_RESILIENCE.md): every retryable transport
 * failure is classified by RetryPolicy and retried through the backoff lane;
 * gate failures become deduped tool feedback; the only exits are done /
 * e_budget / e_cancelled / a clean non-retryable error. Never throws.
 */
#include "agent/loop.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <sys/socket.h>

#include "agent/feedback.h"
#include "agent/intent.h"
#include "core/clock.h"
#include "memory/session_memory.h"
#include "net/http1.h"
#include "net/socket.h"
#include "net/sse.h"
#include "net/transport.h"
#include "prompt/context.h"
#include "util/json.h"

namespace opencode::agent {

namespace {

using core::Err;
using core::make_error_code;

/* Deterministic seed for the per-request jitter stream (no std::hash so the
 * retry timing is stable across toolchains). */
std::uint32_t fnv1a32(std::string_view s) noexcept {
    std::uint32_t h = 2166136261u;
    for (const unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

/* Flatten a core::error_code onto the AgentEvent status field. */
std::int32_t status_of(core::error_code ec) noexcept {
    return static_cast<std::int32_t>(ec.code());
}

/* tools::ToolSpec -> provider::ToolSpec (the provider layer only needs the
 * wire projection). */
provider::ToolsSpec to_provider_specs(
    const std::vector<const tools::Tool*>& reg) {
    provider::ToolsSpec out;
    out.reserve(reg.size());
    for (const tools::Tool* t : reg) {
        const tools::ToolSpec& s = t->spec();
        provider::ToolSpec ps;
        ps.id = s.id;
        ps.name = s.name;
        ps.description = s.description;
        ps.input_schema_json = s.params_schema;
        out.push_back(std::move(ps));
    }
    return out;
}

/* The active toolset for `plan`: minimal -> none; read -> read-only tools;
 * edit -> everything registered. */
provider::ToolsSpec toolset_for(const IntentPlan& plan,
                                tools::ToolRegistry* reg) {
    provider::ToolsSpec out;
    if (reg == nullptr || !plan.wants_tools) return out;
    if (plan.budget_profile == "edit") {
        return to_provider_specs(reg->all());
    }
    std::vector<const tools::Tool*> ro;
    for (const tools::Tool* t : reg->all()) {
        if (t->spec().is_read_only) ro.push_back(t);
    }
    return to_provider_specs(ro);
}

} /* namespace */

Agent::Agent(Session& session, LoopOptions opts)
    : session_(session), opts_(std::move(opts)) {}

Agent::~Agent() = default;

void Agent::sleep_backoff(std::uint64_t ms) {
    const std::uint64_t slice = 100;
    while (ms > 0 && !cancel_.load()) {
        const std::uint64_t step = ms < slice ? ms : slice;
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
        ms -= step;
    }
}

void Agent::checkpoint(const std::vector<std::string>& applied_edits) {
    if (session_.store() == nullptr) return;
    std::string hash;
    if (!memory::workspace_hash(session_.workspace(), hash).ok()) hash.clear();
    memory::checkpoint(session_.store(), session_.id(), session_.messages(),
                       session_.tokens_used(), applied_edits, hash);
    session_.emit("checkpoint", session_.id());
}

core::error_code Agent::assemble(const IntentPlan& plan, provider::MsgList& msgs,
                                 provider::ToolsSpec& tools,
                                 provider::Budget& budget) {
    if (opts_.prompt == nullptr)
        return make_error_code(Err::e_missing_cfg);

    tools = toolset_for(plan, opts_.tools);

    const config::Config& cfg = session_.config();
    const provider::ModelSpec& model = session_.model();

    prompt::ContextInput in;
    in.registry = opts_.prompt;
    in.messages = &session_.messages();
    in.tools = &tools;
    in.wire_family = model.api_family;
    in.edge_mode = (plan.budget_profile == "minimal");
    in.context_window = model.context_window;
    in.max_output_tokens = model.default_max_tokens;
    in.hard_cap_tokens = cfg.budget.max_tokens_per_task;
    in.available_tokens =
        static_cast<std::uint32_t>(session_.budget_remaining());

    prompt::ContextPlan plan_out;
    const core::error_code ec = prompt::assemble_context(in, plan_out);
    if (!ec.ok()) return ec;

    msgs = std::move(plan_out.messages);
    budget.max_output_tokens = model.default_max_tokens;
    budget.max_context_tokens = model.context_window;
    budget.max_tool_args_bytes = 0;
    return core::ok();
}

core::error_code Agent::stream_call(
    const provider::MsgList& msgs, const provider::ToolsSpec& tools,
    const provider::Budget& budget,
    const std::function<void(const provider::StreamEvent&)>& sink) {
    if (opts_.loop == nullptr)
        return make_error_code(Err::e_invalid_cfg);
    if (cancel_.load()) return make_error_code(Err::e_cancelled);

    provider::Provider* p = session_.provider();
    if (p == nullptr) return make_error_code(Err::e_invalid_cfg);

    const provider::ModelSpec& model = session_.model();

    provider::RequestBytes req;
    core::error_code ec = p->build_request(msgs, tools, model, budget, req);
    if (!ec.ok()) return ec;

    provider::UrlParts url;
    ec = provider::split_url(model.base_url, url);
    if (!ec.ok()) return ec;
    if (url.scheme != "http")
        return make_error_code(Err::e_not_impl); /* TLS is Phase 12/13 work */
    if (url.port == 0) url.port = 80;

    std::string path = url.path;
    if (!path.empty() && path.back() == '/') path.pop_back();
    path += req.path;

    net::HttpHeaders headers = req.headers;
    if (net::http_header(headers, "Host").empty())
        headers.push_back({"Host", url.host + ":" + std::to_string(url.port)});

    const std::uint64_t timeout_ms =
        opts_.request_timeout_ms != 0
            ? opts_.request_timeout_ms
            : session_.config().network.timeout_ms;
    const std::uint64_t deadline =
        core::now_mono_ms() + static_cast<std::uint64_t>(timeout_ms);

    session_.set_state(AgentState::connecting);
    session_.emit("connect",
                  url.host + ":" + std::to_string(url.port) + path);

    net::Socket sock;
    ec = sock.open(url.host.find(':') != std::string::npos ? AF_INET6
                                                           : AF_INET);
    if (!ec.ok()) return ec;
    ec = sock.connect(*opts_.loop, net::Addr{url.host, url.port},
                      opts_.connect_timeout_ms);
    if (!ec.ok()) {
        session_.emit("connect", "failed " + std::string(ec.message()),
                      status_of(ec));
        return ec;
    }

    net::HttpRequest hr;
    hr.method = req.method;
    hr.path = path;
    hr.headers = headers;
    hr.body = req.body;
    hr.request_id = session_.request_id();

    std::string wire;
    ec = net::http_build_request(hr, wire);
    if (!ec.ok()) return ec;

    std::size_t off = 0;
    while (off < wire.size()) {
        if (cancel_.load()) return make_error_code(Err::e_cancelled);
        ssize_t sent = 0;
        ec = sock.send(*opts_.loop,
                       reinterpret_cast<const std::uint8_t*>(wire.data() + off),
                       wire.size() - off, deadline, sent);
        if (!ec.ok()) return ec;
        if (sent <= 0) return make_error_code(Err::e_net_connect);
        off += static_cast<std::size_t>(sent);
    }

    net::Transport t;
    ec = t.attach(std::move(sock), nullptr);
    if (!ec.ok()) return ec;

    net::HttpParser hp;
    char scratch[16 * 1024];
    for (;;) {
        if (cancel_.load()) return make_error_code(Err::e_cancelled);
        ssize_t got = 0;
        ec = t.read(*opts_.loop, reinterpret_cast<std::uint8_t*>(scratch),
                    sizeof scratch, deadline, got);
        if (!ec.ok()) return ec;
        if (got == 0)
            return make_error_code(Err::e_net_connect); /* EOF before head */
        ec = hp.feed(std::string_view(scratch, static_cast<std::size_t>(got)));
        if (!ec.ok()) return ec;
        if (hp.head_done()) break;
    }

    const net::HttpResponse& resp = hp.head();
    ec = net::http_status_error(resp);
    if (!ec.ok()) {
        session_.emit("stream",
                      "http " + std::to_string(resp.code), status_of(ec));
        return ec; /* e_rate_limit / e_auth / e_provider_err: classified next */
    }

    session_.set_state(AgentState::streaming);
    session_.emit("stream", "open");

    net::SseParser sp(p->stream_kind());
    sp.set_abort_flag(&cancel_);

    std::vector<provider::StreamEvent> evs;
    core::error_code parse_err;
    ec = net::sse_stream(
        *opts_.loop, t, hp, deadline, sp,
        [&](const net::SseEvent& se) {
            if (!parse_err.ok()) return;
            provider::StreamFrame frame{se.event, se.data};
            evs.clear();
            parse_err = p->parse_frame(frame, evs);
            for (const provider::StreamEvent& ev : evs) sink(ev);
        });
    if (!ec.ok()) {
        session_.emit("stream",
                      ec == Err::e_cancelled ? "cancelled" : "closed",
                      status_of(ec));
        return ec;
    }
    if (!parse_err.ok()) return parse_err;
    session_.emit("stream", "closed");
    return core::ok();
}

core::error_code Agent::one_round(
    const provider::ToolsSpec& tools, const provider::Budget& budget,
    std::vector<provider::ToolCallDone>& calls_out, std::string& text_out,
    provider::Usage& usage_out, msg::FinishReason& finish_out) {
    core::error_code ec = session_.ensure_provider();
    if (!ec.ok()) return ec;

    provider::Provider* p = session_.provider();
    if (p == nullptr) return make_error_code(Err::e_invalid_cfg);

    net::RetryPolicy policy(opts_.retry);
    policy.set_seed(fnv1a32(session_.id()));
    p->reset_stream();
    session_.begin_request();

    std::vector<provider::ToolCallDone> calls;
    std::string text;
    provider::Usage usage;
    msg::FinishReason finish = msg::FinishReason::unknown;
    core::error_code provider_err;

    auto sink = [&](const provider::StreamEvent& ev) {
        if (const auto* d = provider::as<provider::TextDelta>(ev)) {
            text += d->text;
        } else if (const auto* d = provider::as<provider::ToolCallDone>(ev)) {
            calls.push_back(*d);
        } else if (const auto* d = provider::as<provider::MessageDone>(ev)) {
            usage = d->usage;
            finish = d->finish;
        } else if (const auto* err =
                       provider::as<provider::ProviderError>(ev)) {
            provider_err = err->ec.ok()
                               ? make_error_code(Err::e_provider_err)
                               : err->ec;
        }
        /* ToolCallDelta/ReasoningDelta/MessageStart are transient state. */
    };

    for (std::uint32_t attempt = 1;; ++attempt) {
        if (cancel_.load()) return make_error_code(Err::e_cancelled);
        ec = stream_call(session_.messages(), tools, budget, sink);
        if (ec.ok()) {
            if (provider_err.ok()) break;
            return provider_err;  // clean transport, provider-level error
        }
        if (ec == Err::e_cancelled) return ec;
        if (!provider_err.ok()) return provider_err;

        const net::RetryDecision d = policy.decide(ec, attempt);
        if (!d.retry) return ec;

        session_.set_lane(AgentLane::backoff);
        session_.emit("backoff", d.reason, status_of(ec));
        sleep_backoff(d.delay_ms);
        session_.set_lane(AgentLane::none);
    }

    calls_out = std::move(calls);
    text_out = std::move(text);
    usage_out = usage;
    finish_out = finish;
    return core::ok();
}

core::error_code Agent::build_proposal(const provider::ToolCallDone& call,
                                       verify::EditProposal& out) {
    out = verify::EditProposal{};
    out.tool_name = call.name;
    out.args_json = call.input_json;

    util::JVal args;
    core::error_code ec = util::parse_json(call.input_json, args);
    if (!ec.ok()) return ec;
    const util::JVal* path = args.find("path");
    if (path == nullptr || path->kind != util::JVal::Kind::string ||
        path->str.empty())
        return make_error_code(Err::e_tool_reject);
    const std::string_view p = path->str;
    if (p[0] == '/' || p.find("..") != std::string_view::npos)
        return make_error_code(Err::e_tool_reject); /* stay in the sandbox */
    out.path = std::string(p);

    if (call.name == "file.write") {
        const util::JVal* content = args.find("content");
        if (content == nullptr || content->kind != util::JVal::Kind::string)
            return make_error_code(Err::e_tool_reject);
        out.after_content = std::string(content->str);
        const std::string full =
            opts_.verify_ctx.workspace_root.empty()
                ? out.path
                : opts_.verify_ctx.workspace_root + "/" + out.path;
        std::error_code fec;
        const std::string current = [&]() -> std::string {
            std::ifstream in(full, std::ios::binary);
            if (!in) return "";
            return std::string(std::istreambuf_iterator<char>(in),
                               std::istreambuf_iterator<char>());
        }();
        out.before_content = current;
    } else if (call.name == "file.patch") {
        const util::JVal* patch = args.find("patch");
        if (patch == nullptr || patch->kind != util::JVal::Kind::string)
            return make_error_code(Err::e_tool_reject);
        out.patch_text = std::string(patch->str);
    } else {
        return make_error_code(Err::e_tool_notfound);
    }
    return core::ok();
}

tools::ToolResult Agent::run_tool(const provider::ToolCallDone& call,
                                  std::vector<std::string>& applied_edits,
                                  std::string& feedback_out) {
    tools::ToolResult r;
    r.tool_id = call.name;

    if (opts_.tools == nullptr || opts_.permission == nullptr ||
        opts_.verify == nullptr) {
        r.status = tools::ToolStatus::error;
        r.content = "agent loop missing registry/permission/verify";
        r.content_is_error = true;
        return r;
    }

    const tools::Tool* t = opts_.tools->find(call.name);
    if (t == nullptr) {
        r.status = tools::ToolStatus::error;
        r.content = "unknown tool: " + call.name;
        r.content_is_error = true;
        return r;
    }

    const tools::ToolSpec& spec = t->spec();
    tools::Invocation inv;
    inv.tool_name = call.name;
    inv.args_json = call.input_json;
    inv.span_id = 0;
    tools::ToolContext ctx;
    ctx.span_id = 0;
    ctx.cancel = &tool_cancel_;

    const bool is_write = spec.category == tools::ToolCategory::write;

    if (is_write) {
        session_.set_state(AgentState::verifying);
        session_.emit("verify", call.name);

        verify::EditProposal prop;
        const core::error_code pec = build_proposal(call, prop);
        if (!pec.ok()) {
            feedback_out = "verify invalid " + call.name + ": " +
                           std::string(pec.message());
            r.status = tools::ToolStatus::error;
            r.content = feedback_out;
            r.content_is_error = true;
            return r;
        }

        std::vector<verify::GateResult> results =
            opts_.verify->run_all(prop, opts_.verify_ctx);
        const verify::GateResult* fail = nullptr;
        for (const verify::GateResult& g : results) {
            if (!g.pass) {
                fail = &g;
                break;
            }
        }
        if (fail != nullptr) {
            const VerifyFeedback fb = to_feedback(*fail);
            const std::string text = gate_feedback_text(*fail);
            const std::string key = feedback_key(fb);
            session_.emit("verify", text, status_of(make_error_code(Err::e_verify_fail)));
            if (std::find(feedback_seen_.begin(), feedback_seen_.end(), key) !=
                feedback_seen_.end()) {
                aborted_by_feedback_ = true;
                feedback_out = "verify aborted: " + text;
            } else {
                feedback_seen_.push_back(key);
                feedback_out = text;
            }
            r.status = tools::ToolStatus::error;
            r.content = feedback_out;
            r.content_is_error = true;
            return r;
        }

        session_.set_state(AgentState::applying);
        session_.emit("apply", call.name);
        r = opts_.permission->run(*opts_.tools, call.name, inv, ctx);
        if (r.status == tools::ToolStatus::ok) {
            applied_edits.push_back(
                prop.path.empty() ? call.name : prop.path + " (" + call.name + ")");
        } else if (r.status == tools::ToolStatus::error) {
            feedback_out = tool_error_text(r);
        }
        return r;
    }

    r = opts_.permission->run(*opts_.tools, call.name, inv, ctx);
    if (r.status != tools::ToolStatus::ok) {
        if (r.content == "permission_denied")
            feedback_out = permission_text(call.name);
        else
            feedback_out = tool_error_text(r);
    }
    return r;
}

DriveResult Agent::drive(std::string_view user_input) {
    DriveResult result;

    if (is_terminal(session_.state())) session_.set_state(AgentState::idle);
    if (session_.state() != AgentState::idle) {
        result.ec = make_error_code(Err::e_busy);
        return result;
    }
    if (cancel_.load()) {
        result.ec = make_error_code(Err::e_cancelled);
        return result;
    }

    feedback_seen_.clear();
    aborted_by_feedback_ = false;

    session_.set_state(AgentState::preparing);
    session_.emit("intent", "classifying");
    const IntentPlan plan = classify_intent(user_input);
    if (session_.log() != nullptr)
        session_.log()->info("intent classified", "intent",
                             to_string(plan.intent),
                             "profile", plan.budget_profile);

    session_.append_user(user_input);

    for (;;) {
        if (cancel_.load()) break;

        /* Round-boundary normalization: a finished tool round sits in
         * tool_phase/verifying/applying; route through the remaining gates
         * back to idle so the next round can start from preparing. */
        if (session_.state() == AgentState::tool_phase) {
            session_.set_state(AgentState::verifying);
            session_.emit("verify", "no write proposals");
        }
        if (session_.state() == AgentState::verifying) {
            session_.set_state(AgentState::applying);
            session_.emit("apply", "round complete");
        }
        if (session_.state() == AgentState::applying) {
            session_.set_state(AgentState::idle);
            session_.emit("idle", "round boundary");
        }

        session_.set_state(AgentState::preparing);
        if (session_.budget_exhausted()) {
            result.ec = make_error_code(Err::e_budget);
            break;
        }

        provider::MsgList msgs;
        provider::ToolsSpec tools;
        provider::Budget budget;
        core::error_code ec = assemble(plan, msgs, tools, budget);
        if (!ec.ok()) {
            result.ec = ec;
            break;
        }
        session_.emit("assemble", "tier=" + plan.budget_profile +
                                     " tools=" + std::to_string(tools.size()));

        std::vector<provider::ToolCallDone> calls;
        std::string text;
        provider::Usage usage;
        msg::FinishReason finish = msg::FinishReason::unknown;
        ec = one_round(tools, budget, calls, text, usage, finish);
        if (!ec.ok()) {
            result.ec = ec;
            break;
        }
        ++result.iterations;

        std::vector<msg::ToolCall> parts;
        parts.reserve(calls.size());
        for (const provider::ToolCallDone& c : calls) {
            msg::ToolCall tc;
            tc.id = c.id;
            tc.name = c.name;
            tc.input_json = c.input_json;
            tc.finished = true;
            parts.push_back(std::move(tc));
        }
        session_.append_assistant(text, std::move(parts), finish, usage);

        if (finish == msg::FinishReason::permission_denied) {
            result.ec = make_error_code(Err::e_tool_reject);
            break;
        }

        if (calls.empty()) {
            result.summary = text;
            session_.set_state(AgentState::done);
            session_.emit("done", "summary");
            break;
        }

        session_.set_state(AgentState::tool_phase);
        session_.emit("tools", std::to_string(calls.size()) + " calls");

        for (const provider::ToolCallDone& call : calls) {
            if (cancel_.load()) break;
            std::string fb;
            const tools::ToolResult r =
                run_tool(call, result.applied_edits, fb);
            if (!fb.empty()) {
                result.feedback.push_back(fb);
                session_.append_tool_result(call.id, fb, true);
            } else {
                const std::string content =
                    r.content.empty() ? "ok" : r.content;
                session_.append_tool_result(
                    call.id, content,
                    r.status != tools::ToolStatus::ok || r.content_is_error);
            }
            if (r.status == tools::ToolStatus::canceled) {
                result.ec = make_error_code(Err::e_cancelled);
                break;
            }
            if (aborted_by_feedback_) break;
        }

        checkpoint(result.applied_edits);

        if (result.ec == Err::e_cancelled) break;
        if (aborted_by_feedback_) {
            result.ec = make_error_code(Err::e_aborted);
            break;
        }
    }

    checkpoint(result.applied_edits);

    result.tokens_used = session_.tokens_used();

    if (result.ec == Err::e_cancelled) {
        session_.set_state(AgentState::cancelled);
        session_.emit("cancelled", "host cancelled");
    } else if (result.ec == Err::ok) {
        /* falls through: done already set when the summary round landed */
        if (session_.state() != AgentState::done) {
            session_.set_state(AgentState::done);
            session_.emit("done", "summary");
        }
    } else {
        session_.set_state(AgentState::error);
        session_.emit("error", std::string(result.ec.message()),
                      status_of(result.ec));
    }
    return result;
}

} /* namespace opencode::agent */

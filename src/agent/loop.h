/*
 * loop.h -- the agent drive loop (Phase 10 Task 4).
 *
 * The 5-stage pipeline: intent -> context -> prompt -> single cloud call ->
 * native verify. The loop holds no global state: everything lives in the
 * Session (history/budget/provider binding) plus the injected dependencies
 * (event loop, tools, permission gate, verify gate). One cloud call per
 * iteration; extra iterations exist only for tool rounds, each bounded by
 * max_tokens_per_task (Phase 6). Edits reach the workspace ONLY through the
 * Phase 9 gate.
 *
 * Never-abort guarantee: every retryable transport failure goes through the
 * backoff lane and resumes; every gate failure becomes a feedback tool_result
 * (deduped); the only exits are done / ERR_BUDGET / ERR_CANCELLED / a clean
 * non-retryable error. Never throws.
 */
#ifndef OPENCODE_AGENT_LOOP_H
#define OPENCODE_AGENT_LOOP_H

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "agent/intent.h"
#include "agent/session.h"
#include "core/error.h"
#include "core/event_loop.h"
#include "net/policy.h"
#include "prompt/registry.h"
#include "provider/provider.h"
#include "tools/permission.h"
#include "tools/registry.h"
#include "verify/gate.h"

namespace opencode::agent {

struct LoopOptions {
    core::EventLoop* loop = nullptr;         /* required */
    prompt::PromptRegistry* prompt = nullptr; /* required (SYSTEM_BASE etc.) */
    tools::ToolRegistry* tools = nullptr;    /* required (includes writes) */
    tools::Gate* permission = nullptr;       /* required */
    verify::Gate* verify = nullptr;          /* required */
    verify::Context verify_ctx;              /* workspace + graph wiring   */
    net::RetryBudget retry;                  /* backoff budget (Phase 4)   */
    std::uint32_t connect_timeout_ms = 10'000;
    std::uint32_t request_timeout_ms = 0;    /* 0 = config.network.timeout_ms */
};

struct DriveResult {
    core::error_code ec;
    std::string summary;         /* final model text (empty on tool tasks) */
    std::vector<std::string> applied_edits; /* "tool path" per applied write */
    std::vector<std::string> feedback;      /* gate feedback produced       */
    std::uint64_t tokens_used = 0;
    std::uint32_t iterations = 0; /* cloud-call rounds                     */
};

class Agent {
public:
    Agent(Session& session, LoopOptions opts);
    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;
    ~Agent();

    /* Run one task from the user's message. Returns when the task is done,
     * budget-exhausted, cancelled, or a non-retryable error surfaces. */
    DriveResult drive(std::string_view user_input);

    /* Cooperative cancellation; checked at every await. */
    void request_cancel() noexcept {
        cancel_.store(true);
        tool_cancel_.cancel();
    }
    bool cancelled() const noexcept { return cancel_.load(); }

private:
    /* One cloud call round: assemble context, send, collect stream events,
     * return tool calls + text + usage. Handles retry/backoff internally. */
    core::error_code one_round(const provider::ToolsSpec& tools,
                               const provider::Budget& budget,
                               std::vector<provider::ToolCallDone>& calls_out,
                               std::string& text_out, provider::Usage& usage_out,
                               msg::FinishReason& finish_out);

    /* Transport + provider stream for one request. Never throws. */
    core::error_code stream_call(
        const provider::MsgList& msgs, const provider::ToolsSpec& tools,
        const provider::Budget& budget,
        const std::function<void(const provider::StreamEvent&)>& sink);

    /* Execute one tool call. Write tools are verified by the Phase 9 gate
     * BEFORE apply; on gate failure, `feedback` carries the exact message and
     * nothing is written. Returns the ToolResult. */
    tools::ToolResult run_tool(const provider::ToolCallDone& call,
                               std::vector<std::string>& applied_edits,
                               std::string& feedback_out);

    /* Build a verify::EditProposal from a write-tool call (reads current file
     * for before_content; file.patch computes after_content in memory). */
    core::error_code build_proposal(const provider::ToolCallDone& call,
                                    verify::EditProposal& out);

    /* Sleep the backoff lane duration (blocking sleep; the loop owns the
     * thread during a retry pause). */
    void sleep_backoff(std::uint64_t ms);

    /* Budget-aware context assembly for one round. */
    core::error_code assemble(const IntentPlan& plan, provider::MsgList& msgs,
                              provider::ToolsSpec& tools, provider::Budget& budget);

    Session& session_;
    LoopOptions opts_;
    std::atomic<bool> cancel_{false};
    tools::CancellationToken tool_cancel_;
    /* Dedupe keys for gate feedback seen this task (loop aborts on repeats). */
    std::vector<std::string> feedback_seen_;
    bool aborted_by_feedback_ = false;
};

} /* namespace opencode::agent */

#endif /* OPENCODE_AGENT_LOOP_H */

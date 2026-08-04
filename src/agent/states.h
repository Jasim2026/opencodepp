/*
 * states.h -- the agent-level state machine (Phase 4 sec 1, Phase 10 Task 2).
 *
 * A main lane (the task pipeline) plus three orthogonal lanes (Backoff,
 * OfflineQueue, Paused). Every transition emits an AgentEvent -- the single
 * vocabulary the host/ABI (Phase 12) observes -- so hosts can pause/cancel
 * from any state and always see what the loop is doing. No state in the main
 * lane ever "hangs": network errors fold into the lanes, which are bounded and
 * self-resuming, and the only exits from the machine are done / error /
 * cancelled / a clean idle reset.
 *
 * Header-only: no state, no allocation.
 */
#ifndef OPENCODE_AGENT_STATES_H
#define OPENCODE_AGENT_STATES_H

#include <cstdint>
#include <string>
#include <string_view>

namespace opencode::agent {

/* Main lane: the task pipeline. */
enum class AgentState : uint8_t {
    idle = 0,      /* no task running; accepts the next user turn          */
    preparing = 1, /* intent + context assembly, budget check              */
    connecting = 2,/* transport connect / request send                     */
    streaming = 3, /* response frames flowing through the provider         */
    tool_phase = 4,/* native tool runs (gated writes serialized)           */
    verifying = 5, /* Phase 9 gate over every write proposal               */
    applying = 6,  /* gate-passing edits written to the workspace          */
    done = 7,      /* task finished with a report                          */
    error = 8,     /* non-retryable, user-visible error reported           */
    cancelled = 9, /* host cancelled; rolled back cleanly                  */
};

/* Orthogonal lanes. Only meaningful alongside a main-lane state; each is
 * bounded (backoff/offline retry counts, paused is host-held) and self-
 * resuming: the lane clears itself and the main lane continues. */
enum class AgentLane : uint8_t {
    none = 0,    /* not in any lane                                      */
    backoff = 1, /* retry delay after a transient transport failure      */
    offline = 2, /* queued while unreachable; resumes on reconnect        */
    paused = 3,  /* host-held; resume() clears it                         */
};

constexpr std::string_view to_string(AgentState s) noexcept {
    switch (s) {
        case AgentState::idle: return "idle";
        case AgentState::preparing: return "preparing";
        case AgentState::connecting: return "connecting";
        case AgentState::streaming: return "streaming";
        case AgentState::tool_phase: return "tool_phase";
        case AgentState::verifying: return "verifying";
        case AgentState::applying: return "applying";
        case AgentState::done: return "done";
        case AgentState::error: return "error";
        case AgentState::cancelled: return "cancelled";
    }
    return "?";
}

constexpr std::string_view to_string(AgentLane l) noexcept {
    switch (l) {
        case AgentLane::none: return "none";
        case AgentLane::backoff: return "backoff";
        case AgentLane::offline: return "offline";
        case AgentLane::paused: return "paused";
    }
    return "?";
}

/* True when the state is a terminal exit of a task (the loop reports and
 * returns to idle; it never auto-resumes these). */
constexpr bool is_terminal(AgentState s) noexcept {
    return s == AgentState::done || s == AgentState::error ||
           s == AgentState::cancelled;
}

/* The event emitted on every transition. `meter` is flattened to the two
 * numbers hosts need (tokens_used + status) so the ABI can mirror this struct
 * 1:1; `status` is 0 on success, else a core::error_code value. */
struct AgentEvent {
    AgentState state = AgentState::idle;
    AgentLane lane = AgentLane::none;
    std::string phase;    /* coarse step name, e.g. "streaming"             */
    std::string detail;   /* free-form human-readable detail                */
    std::uint64_t tokens_used = 0;
    std::int32_t status = 0;
};

/* True when `from -> to` is a legal main-lane transition. Exits to the
 * terminal states are allowed from every non-terminal state except idle (a
 * task must at least begin); the machine can be reset from error/cancelled
 * (and, after a completed cycle, from applying) back to idle by the host. */
constexpr bool valid_transition(AgentState from, AgentState to) noexcept {
    if (from == to) return !is_terminal(from); /* idempotent re-emit */
    if (to == AgentState::idle)
        return from == AgentState::applying || from == AgentState::error ||
               from == AgentState::cancelled;
    if (is_terminal(to)) return !is_terminal(from) && from != AgentState::idle;
    switch (from) {
        case AgentState::idle: return to == AgentState::preparing;
        case AgentState::preparing: return to == AgentState::connecting ||
                                          to == AgentState::done;
        case AgentState::connecting:
            return to == AgentState::streaming || to == AgentState::tool_phase;
        case AgentState::streaming:
            return to == AgentState::tool_phase || to == AgentState::verifying ||
                   to == AgentState::done;
        case AgentState::tool_phase:
            return to == AgentState::verifying || to == AgentState::done;
        case AgentState::verifying:
            return to == AgentState::applying || to == AgentState::done ||
                   to == AgentState::tool_phase;
        case AgentState::applying:
            return to == AgentState::done || to == AgentState::verifying ||
                   to == AgentState::idle;
        default: return false;
    }
}

/* True when a lane may be entered/held while in `s`. Lanes attach to the
 * network- and tool-facing states (backoff/offline make no sense in preparing
 * or applying; paused is meaningful anywhere a long await happens). */
constexpr bool lane_allowed(AgentState s, AgentLane l) noexcept {
    if (l == AgentLane::none) return true;
    if (l == AgentLane::paused)
        return s == AgentState::connecting || s == AgentState::streaming ||
               s == AgentState::tool_phase || s == AgentState::verifying;
    return s == AgentState::connecting || s == AgentState::streaming ||
           s == AgentState::tool_phase;
}

/* One-line logfmt-ish rendering of an event for debug logs / the dev CLI. */
inline std::string format_event(const AgentEvent& ev) {
    std::string out = std::string(to_string(ev.state));
    if (ev.lane != AgentLane::none) {
        out += ":" + std::string(to_string(ev.lane));
    }
    if (!ev.phase.empty()) out += " phase=" + ev.phase;
    if (!ev.detail.empty()) out += " detail=" + ev.detail;
    if (ev.tokens_used != 0) out += " tokens=" + std::to_string(ev.tokens_used);
    if (ev.status != 0) out += " status=" + std::to_string(ev.status);
    return out;
}

} /* namespace opencode::agent */

#endif /* OPENCODE_AGENT_STATES_H */

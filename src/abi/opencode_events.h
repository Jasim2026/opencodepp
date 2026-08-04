/*
 * opencode_events.h -- marshalling between engine events and the C ABI.
 *
 * The engine emits agent::AgentEvent (src/agent/states.h); the ABI mirrors it
 * 1:1 as opencode_event_t (include/opencode/opencode.h). These helpers are the
 * only place that translation happens. All functions are noexcept / never
 * throw, matching the ABI contract.
 */
#ifndef OPENCODE_ABI_OPENCODE_EVENTS_H
#define OPENCODE_ABI_OPENCODE_EVENTS_H

#include <cstdio>
#include <cstring>
#include <string>

#include "agent/states.h"
#include "opencode/opencode.h"

namespace opencode::abi {

/* Map an agent main-lane state to an opencode_event_kind_t. The Phase 11
 * lossy-fold event is reported out-of-band via `phase == "fold"`. */
inline opencode_event_kind_t event_kind(const agent::AgentEvent& ev) noexcept {
    if (ev.phase == "fold") return OPENCODE_EVENT_FOLD;
    switch (ev.state) {
        case agent::AgentState::idle: return OPENCODE_EVENT_LOG;
        case agent::AgentState::preparing: return OPENCODE_EVENT_PREPARING;
        case agent::AgentState::connecting: return OPENCODE_EVENT_CONNECTING;
        case agent::AgentState::streaming: return OPENCODE_EVENT_STREAMING;
        case agent::AgentState::tool_phase: return OPENCODE_EVENT_TOOL_PHASE;
        case agent::AgentState::verifying: return OPENCODE_EVENT_VERIFYING;
        case agent::AgentState::applying: return OPENCODE_EVENT_APPLYING;
        case agent::AgentState::done: return OPENCODE_EVENT_DONE;
        case agent::AgentState::error: return OPENCODE_EVENT_FAILED;
        case agent::AgentState::cancelled: return OPENCODE_EVENT_CANCELLED;
    }
    return OPENCODE_EVENT_LOG;
}

inline opencode_lane_t event_lane(agent::AgentLane lane) noexcept {
    return static_cast<opencode_lane_t>(lane); /* enum values match 1:1 */
}

/* Fill a caller-owned opencode_event_t from an AgentEvent. `out->text` must
 * point at a buffer of `out->text_cap` bytes (or be NULL to skip text).
 * Never throws. */
inline void marshal(const agent::AgentEvent& ev, opencode_event_t* out,
                    uint32_t session_id) noexcept {
    std::memset(out, 0, sizeof *out);
    out->version = OPENCODE_EVENT_VERSION;
    out->session_id = session_id;
    out->kind = event_kind(ev);
    out->lane = event_lane(ev.lane);
    out->status = ev.status;
    out->data_i64 = static_cast<int64_t>(ev.tokens_used);
    if (out->text != nullptr && out->text_cap != 0) {
        const std::string line =
            ev.detail.empty()
                ? ev.phase
                : ev.phase + ": " + ev.detail;
        const size_t n =
            std::snprintf(out->text, out->text_cap, "%s", line.c_str());
        out->text_len = n > out->text_cap ? out->text_cap : n;
    }
}

} /* namespace opencode::abi */

#endif /* OPENCODE_ABI_OPENCODE_EVENTS_H */

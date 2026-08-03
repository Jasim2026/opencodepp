/*
 * message.h -- one message in a session: metadata + an ordered body of parts.
 *
 * The primary object the store, provider, and agent move around. Storage uses
 * the binary codec (codec.h); the JSON interop here exists for the wire format
 * and session debug logs only.
 */
#ifndef OPENCODE_MSG_MESSAGE_H
#define OPENCODE_MSG_MESSAGE_H

#include <cstdint>
#include <string>
#include <vector>

#include "core/error.h"
#include "msg/part.h"
#include "msg/role.h"
#include "util/json.h"

namespace opencode::msg {

struct Message {
    std::string id;
    std::string session_id;
    Role role = Role::user;
    std::string model;
    std::vector<Part> parts;
    std::uint64_t created_at = 0; /* wall-clock seconds since the epoch */

    /* Concatenation of all Text parts ("" when there are none). */
    std::string content_text() const;
    /* Tool calls / results in body order (non-owning views). */
    std::vector<const ToolCall*> tool_calls() const;
    std::vector<const ToolResult*> tool_results() const;
    /* True when the message carries a Finish part. */
    bool is_finished() const noexcept;
    /* First Finish part's reason, or FinishReason::unknown. */
    FinishReason finish_reason() const noexcept;
};

/* JSON forms for wire / debug-log use (see header comment). */
util::JVal to_json(const Message& m);
core::error_code from_json(const util::JVal& v, Message& m);

} /* namespace opencode::msg */

#endif /* OPENCODE_MSG_MESSAGE_H */

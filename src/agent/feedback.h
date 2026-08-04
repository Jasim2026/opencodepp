/*
 * feedback.h -- minimal, exact feedback for the self-correction loop (T5).
 *
 * Every gate failure, tool error, and permission denial is converted into a
 * short, structured text the model sees as a tool_result. The feedback is
 * deduped by (stage,file,line) so the loop stops re-prompting when the same
 * feedback recurs (retry doctrine in 02_CODING_PROTOCOL.md). Never throws.
 */
#ifndef OPENCODE_AGENT_FEEDBACK_H
#define OPENCODE_AGENT_FEEDBACK_H

#include <cstdint>
#include <string>
#include <string_view>

#include "core/error.h"
#include "tools/tool.h"
#include "verify/gate.h"

namespace opencode::agent {

/* The structured view of a gate failure the loop reasons about. */
struct VerifyFeedback {
    verify::Stage stage = verify::Stage::syntax;
    std::string file;
    std::string message;
    std::uint32_t line = 0;
    std::uint32_t col = 0;
};

/* Gate failure -> human/model-readable one-liner with file:line. */
std::string gate_feedback_text(const verify::GateResult& r);
/* Same information as structured data (for the loop's dedupe bookkeeping). */
VerifyFeedback to_feedback(const verify::GateResult& r);

/* Dedupe key "stage:file:line"; identical feedback must not re-run. */
std::string feedback_key(const VerifyFeedback& fb);

/* Tool error / denial -> minimal text appended as a tool_result. */
std::string tool_error_text(const tools::ToolResult& r);
std::string permission_text(std::string_view tool_name);

} /* namespace opencode::agent */

#endif /* OPENCODE_AGENT_FEEDBACK_H */

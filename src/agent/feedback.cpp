/*
 * feedback.cpp -- feedback formatters (see feedback.h).
 *
 * The strings are deliberately terse and deterministic so tests can assert on
 * them and the model can parse them: "stage 'file':line: message". Feedback
 * never includes the model's own text (no echo), only what it must fix.
 */
#include "agent/feedback.h"

#include <string>
#include <string_view>

#include "msg/tokens.h"

namespace opencode::agent {

namespace {

std::string stage_name(verify::Stage s) {
    switch (s) {
        case verify::Stage::syntax: return "syntax";
        case verify::Stage::symbols: return "symbols";
        case verify::Stage::impact: return "impact";
        case verify::Stage::diff: return "diff";
        case verify::Stage::testmap: return "testmap";
    }
    return "gate";
}

} /* namespace */

std::string gate_feedback_text(const verify::GateResult& r) {
    std::string out = "verify " + stage_name(r.stage);
    if (!r.file.empty()) out += " '" + r.file + "'";
    if (r.line != 0) out += ":" + std::to_string(r.line);
    if (r.col != 0) out += ":" + std::to_string(r.col);
    if (!r.detail.empty()) out += ": " + r.detail;
    return out;
}

VerifyFeedback to_feedback(const verify::GateResult& r) {
    VerifyFeedback fb;
    fb.stage = r.stage;
    fb.file = r.file;
    fb.message = gate_feedback_text(r);
    fb.line = r.line;
    fb.col = r.col;
    return fb;
}

std::string feedback_key(const VerifyFeedback& fb) {
    return stage_name(fb.stage) + ":" + fb.file + ":" +
           std::to_string(fb.line);
}

std::string tool_error_text(const tools::ToolResult& r) {
    std::string out = "tool " + r.tool_id;
    if (r.status == tools::ToolStatus::canceled) out += " canceled";
    if (!r.content.empty()) out += ": " + r.content;
    return out;
}

std::string permission_text(std::string_view tool_name) {
    return "permission_denied: " + std::string(tool_name);
}

} /* namespace opencode::agent */

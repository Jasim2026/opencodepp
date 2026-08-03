/*
 * context.cpp -- tiered context assembly (see context.h).
 *
 * Deterministic order of work:
 *   1. resolve the per-request token cap (window/max-output guard),
 *   2. compose the system message: system core, then Tier-2 env/repo entries,
 *      budget-gated in fixed order,
 *   3. keep the Tier-1 message window (newest user message + last N assistant
 *      turns and their frames), then add older history newest-first while the
 *      budget holds (Tier 2),
 *   4. record every cut as a ContextEvent and summarize in omitted/truncated.
 */
#include "prompt/context.h"

#include <cstdint>
#include <string>
#include <utility>

#include "msg/message.h"
#include "msg/part.h"
#include "msg/tokens.h"

namespace opencode::prompt {

namespace {

using msg::Message;
using provider::MsgList;
using provider::ToolsSpec;

/* Reserve room for the newest user message; the rest of the cap is what
 * history/environment may consume. */
constexpr std::uint32_t kNewestUserFloor = 800;

inline void push_omit(ContextPlan& out, std::string what, std::string reason) {
    out.events.push_back({ContextEvent::Kind::omitted, std::move(what),
                          std::move(reason)});
    out.omitted.push_back(out.events.back().what);
}
inline void push_truncate(ContextPlan& out, std::string what,
                          std::string reason) {
    out.events.push_back({ContextEvent::Kind::truncated, std::move(what),
                          std::move(reason)});
    out.truncated.push_back(out.events.back().what);
}

/* Substitution for the single {{NAME}} mechanism; unknown names are left as
 * literals. The tools schema is filled only in prompt-mode fallback. */
std::string substitute(std::string_view text, const ContextInput& in,
                       std::string_view schema_text) noexcept {
    std::string s(text);
    auto replace = [&s](const char* name, std::string_view value) {
        const std::string mark = std::string("{{") + name + "}}";
        size_t pos = 0;
        while ((pos = s.find(mark, pos)) != std::string::npos) {
            s.replace(pos, mark.size(), value);
            pos += value.size();
        }
    };
    replace("TOOLS_SCHEMA", schema_text);
    replace("LANG_STYLE", in.lang_style);
    replace("EXAMPLES", in.examples);
    return s;
}

/* Tail-kept truncation for the newest user message. Returns cut=true when the
 * text had to be shortened. */
std::string truncate_text(std::string_view text, std::uint32_t budget,
                          bool& cut) noexcept {
    if (msg::estimate_tokens(text) <= budget) {
        cut = false;
        return std::string(text);
    }
    size_t lo = 0, hi = text.size();
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (msg::estimate_tokens(text.substr(0, mid)) <= budget) lo = mid;
        else hi = mid - 1;
    }
    cut = true;
    constexpr std::string_view kSuffix = " [context truncated]";
    return std::string(text.substr(0, lo)) + std::string(kSuffix);
}

} /* namespace */

std::uint32_t estimate_text_tokens(std::string_view text) noexcept {
    return msg::estimate_tokens(text);
}

core::error_code assemble_context(const ContextInput& in, ContextPlan& out) {
    out = ContextPlan{};

    if (in.messages == nullptr || in.messages->empty())
        return core::make_error_code(core::Err::e_missing_cfg);

    /* the default system id is the compiled stem of SYSTEM_BASE.md so that
     * load_templates("src/prompt/templates", reg) + default assemble work */
    const std::vector<std::string> sys_ids =
        in.system_ids.empty()
            ? std::vector<std::string>{"SYSTEM_BASE"}
            : in.system_ids;

    /* 1. resolve the per-request cap */
    std::uint64_t cap = in.available_tokens ? in.available_tokens
                                            : in.hard_cap_tokens;
    if (in.context_window && in.max_output_tokens &&
        in.context_window > in.max_output_tokens) {
        const std::uint64_t window_room =
            in.context_window - in.max_output_tokens;
        if (cap > window_room) cap = window_room;
    }
    if (cap < kNewestUserFloor) cap = kNewestUserFloor;

    /* 2. tool schema cost projection (native path) */
    out.tools = in.tools ? *in.tools : ToolsSpec{};
    if (!out.tools.empty())
        out.tool_tokens = prompt::tools_schema_tokens(out.tools, in.wire_family);

    /* 3. system message: core templates, then Tier-2 env/repo entries */
    std::string sys_text;
    if (in.registry == nullptr)
        return core::make_error_code(core::Err::e_missing_cfg);
    for (const std::string& id : sys_ids) {
        const prompt::PromptRef* ref = in.registry->find(id);
        if (ref == nullptr)
            return core::make_error_code(core::Err::e_missing_cfg);
        for (const auto& part : ref->parts) {
            if (part.kind == prompt::PromptPartKind::system ||
                part.kind == prompt::PromptPartKind::template_) {
                sys_text += substitute(part.text, in, "");
                sys_text += "\n";
            }
        }
    }

    const std::uint64_t tier2_budget =
        cap - kNewestUserFloor - out.tool_tokens;
    const bool skip_extras = in.edge_mode;
    for (const EnvEntry& e : in.env) {
        if (skip_extras) {
            push_omit(out, "env " + e.key, "edge_mode");
            continue;
        }
        const std::uint32_t t = msg::estimate_tokens(e.text);
        if (sys_text.size() + e.text.size() > 0 &&
            (std::uint64_t)t > tier2_budget) {
            push_omit(out, "env " + e.key,
                      "budget (" + std::to_string(t) + " tokens)");
            continue;
        }
        sys_text += e.text;
        sys_text += "\n";
    }
    for (const FileSnippet& s : in.repo) {
        if (skip_extras) {
            push_omit(out, "repo " + s.path, "edge_mode");
            continue;
        }
        const std::uint32_t t = msg::estimate_tokens(s.text);
        if ((std::uint64_t)t > tier2_budget) {
            push_omit(out, "repo " + s.path,
                      "budget (" + std::to_string(t) + " tokens)");
            continue;
        }
        sys_text += s.text;
        sys_text += "\n";
    }

    /* 4. message window: newest user always kept, and every message from the
     * Nth-newest assistant turn (N = recent_assistant_turns) onward is Tier 1
     * -- the last N assistant turns plus their interleaved tool frames. */
    const MsgList& hist = *in.messages;
    const std::uint32_t sys_tokens = msg::estimate_tokens(sys_text);
    const std::uint32_t keep_assistant =
        in.recent_assistant_turns ? in.recent_assistant_turns : 1;
    std::uint32_t need = keep_assistant;
    std::size_t tier1_from = hist.size();
    for (std::size_t i = hist.size(); i-- > 0;) {
        if (hist[i].role == msg::Role::assistant) {
            if (need > 0) {
                --need;
                tier1_from = i;
            } else {
                break; /* older assistant turn: everything below is Tier 2 */
            }
        }
    }
    /* a history with no assistant turns still keeps the newest user message */
    if (tier1_from == hist.size() && !hist.empty())
        tier1_from = hist.size() - 1;

    std::uint64_t total = 0;
    for (std::size_t i = 0; i < hist.size(); ++i) {
        const Message& m = hist[i];
        const bool tier1 = i >= tier1_from;
        const std::uint32_t est = msg::estimate_message_tokens(m);
        if (!tier1 &&
            total + est + sys_tokens + out.tool_tokens > cap) {
            push_omit(out, "message " + m.id,
                      "budget (" + std::to_string(est) + " tokens)");
            continue;
        }
        if (m.role == msg::Role::user && i + 1 == hist.size()) {
            bool cut = false;
            std::string text = truncate_text(m.content_text(),
                                             kNewestUserFloor, cut);
            if (cut) {
                push_truncate(out, "message " + m.id, "kNewestUserFloor");
                Message copy = m;
                copy.parts.clear();
                copy.parts.push_back(msg::Text{std::move(text)});
                out.messages.push_back(std::move(copy));
                total += msg::estimate_message_tokens(out.messages.back());
                continue;
            }
        }
        out.messages.push_back(m);
        total += est;
    }

    /* 5. assemble the system message in front; optional prompt-mode schema */
    if (in.inject_tool_schema && !out.tools.empty()) {
        std::string schema;
        prompt::tools_schema_json(out.tools, in.wire_family, schema);
        if (!sys_text.empty()) sys_text += "\n";
        sys_text += schema;
    }

    const std::uint32_t sys_tokens2 = msg::estimate_tokens(sys_text);
    out.messages.insert(
        out.messages.begin(),
        Message{"", "", msg::Role::system, "",
                std::vector<msg::Part>{msg::Text{sys_text}}, 0});

    /* 6. totals */
    total += sys_tokens2 + out.tool_tokens;
    out.estimated_tokens = static_cast<std::uint32_t>(total);
    out.bytes = sys_text.size();
    for (const Message& m : out.messages) out.bytes += m.content_text().size();
    out.under_target = out.estimated_tokens <= in.target_tokens;
    return core::ok();
}

} /* namespace opencode::prompt */

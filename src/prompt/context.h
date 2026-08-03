/*
 * context.h -- tiered, token-budgeted context assembler (T1).
 *
 * assemble_context() turns (session history, toolset, env/repo snapshot,
 * model budget) into a ContextPlan: a deterministic message list plus the
 * budget it consumes. Tiering (from the IDEA v2 edge design):
 *   Tier 1 (always): system core + active tool schema cost + newest user
 *                    message + last N assistant turns (with their tool frames).
 *   Tier 2 (budget-gated, fixed order): older history, env snapshot, repo
 *                    snippets, examples/style notes.
 *   Tier 3 (never by default): full-file snapshots (Phase 7 supplies targeted
 *                    snippets instead).
 * Every omission/truncation is recorded in the plan and surfaced as a
 * ContextEvent so hosts always see what was cut. Never throws.
 */
#ifndef OPENCODE_PROMPT_CONTEXT_H
#define OPENCODE_PROMPT_CONTEXT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.h"
#include "msg/message.h"
#include "prompt/registry.h"
namespace opencode::prompt {

/* A repo/environment context entry (Tier 2). Phase 7 feeds graph-derived
 * snippets here; the engine's env snapshot feeds EnvEntry. */
struct EnvEntry {
    std::string key;
    std::string text;
};
struct FileSnippet {
    std::string path;
    std::string text;
};

/* One cut decision, surfaced so hosts can report what was dropped. */
struct ContextEvent {
    enum class Kind : uint8_t { omitted, truncated };
    Kind kind;
    std::string what;   /* e.g. "message <id>", "env <key>", "system_base" */
    std::string reason; /* why (budget / not-in-tier) */
};

struct ContextInput {
    const PromptRegistry* registry = nullptr; /* compiled templates */
    std::vector<std::string> system_ids;      /* default {"SYSTEM_BASE"} */
    const provider::MsgList* messages = nullptr; /* session history, newest last */    const provider::ToolsSpec* tools = nullptr;
    std::string wire_family = "openai";       /* tool-schema cost projection */
    std::vector<EnvEntry> env;                /* Tier 2 */
    std::vector<FileSnippet> repo;            /* Tier 2 */
    std::string lang_style;                   /* CODE_STYLE {{LANG_STYLE}} fill */
    std::string examples;                     /* EXAMPLES fill */

    uint32_t context_window = 0;              /* model cap; 0 = ignore */
    uint32_t max_output_tokens = 0;           /* reserved for the answer */
    uint32_t target_tokens = 3500;            /* T1 edge profile */
    uint32_t hard_cap_tokens = 12000;         /* config max_tokens_per_task */
    uint32_t available_tokens = 0;            /* per-request cap; 0 = hard cap */
    uint32_t recent_assistant_turns = 2;      /* always-kept assistant turns */
    bool edge_mode = false;                   /* skip Tier-2 extras */
    bool inject_tool_schema = false;          /* prompt-mode fallback only */
};

struct ContextPlan {
    std::vector<msg::Message> messages;   /* ready-to-send (chronological) */
    std::uint32_t estimated_tokens = 0;   /* messages + tools + env + repo */
    std::size_t bytes = 0;                /* sum of kept text bytes */
    std::vector<ContextEvent> events;     /* every cut, in order */
    std::vector<std::string> omitted;     /* descriptions of omissions */
    std::vector<std::string> truncated;   /* descriptions of truncations */
    provider::ToolsSpec tools;            /* active toolset (native path) */
    std::uint32_t tool_tokens = 0;        /* provider-native schema cost */
    bool under_target = false;            /* estimated <= target_tokens */
};

/* Assemble the plan. e_missing_cfg when a required system id is absent from
 * the registry or no messages are supplied. Never throws. */
core::error_code assemble_context(const ContextInput& in, ContextPlan& out);

/* Token estimate of one text (shorthand for tests). */
std::uint32_t estimate_text_tokens(std::string_view text) noexcept;

} /* namespace opencode::prompt */

#endif /* OPENCODE_PROMPT_CONTEXT_H */

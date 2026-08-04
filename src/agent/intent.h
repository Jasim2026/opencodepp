/*
 * intent.h -- task intent classification (Phase 10 Task 3, IDEA v2 5-stage).
 *
 * Stage 1 of the pipeline. A cheap deterministic classifier (keywords/regex
 * over the trimmed user turn) with an explicit `unknown` (Uncertain) output.
 * When unknown, the loop asks the model a one-line disambiguation prompt; the
 * host/tests never see a thrown or silently-guessed intent.
 *
 * Output is an IntentPlan that selects the prompt tier (budget_profile) and
 * the toolset for context assembly. Classification cost is intentionally
 * near-zero (no allocation in the hot keyword path beyond the lower-cased
 * scratch string).
 */
#ifndef OPENCODE_AGENT_INTENT_H
#define OPENCODE_AGENT_INTENT_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace opencode::agent {

enum class Intent : uint8_t {
    unknown = 0,  /* Uncertain: ask the model to disambiguate               */
    ask = 1,      /* answer a question from workspace/history              */
    explain = 2,  /* explain a file/function/snippet                       */
    edit = 3,     /* modify existing code                                  */
    refactor = 4, /* restructure without changing behavior                 */
    bugfix = 5,   /* fix a failing test / crash / wrong output             */
    test = 6,     /* write or update tests                                 */
    shell = 7,    /* run a command in the sandbox                          */
    explore = 8,  /* find/search/read without modifying                    */
    meta = 9,     /* memory/rules/instructions (Phase 11)                  */
};

constexpr std::string_view to_string(Intent i) noexcept {
    switch (i) {
        case Intent::unknown: return "unknown";
        case Intent::ask: return "ask";
        case Intent::explain: return "explain";
        case Intent::edit: return "edit";
        case Intent::refactor: return "refactor";
        case Intent::bugfix: return "bugfix";
        case Intent::test: return "test";
        case Intent::shell: return "shell";
        case Intent::explore: return "explore";
        case Intent::meta: return "meta";
    }
    return "?";
}

/* The classifier's output. `budget_profile` maps to the Phase 6 context
 * tiers: "minimal" (ask/explain/meta; no tools), "read" (explore; read-only
 * tools), "edit" (edit/bugfix/test/refactor/shell; full toolset). */
struct IntentPlan {
    Intent intent = Intent::unknown;
    std::vector<std::string> affected_paths; /* file hints from the turn   */
    std::string budget_profile;              /* "minimal" | "read" | "edit"*/
    bool wants_tools = false;                /* offer tools to the model   */
    bool write_allowed = false;              /* this intent may write      */
};

/* Classify a user turn. Deterministic; `unknown` when nothing matches and no
 * file path hints are present (the loop then disambiguates via the model).
 * Never throws. */
IntentPlan classify_intent(std::string_view user_message);

/* File-ish tokens mentioned in the message (quoted or dotted or with a path
 * separator). Read-only; used to bias context snippets for edit/explain. */
std::vector<std::string> extract_path_hints(std::string_view user_message);

} /* namespace opencode::agent */

#endif /* OPENCODE_AGENT_INTENT_H */

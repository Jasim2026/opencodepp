/*
 * intent.cpp -- deterministic intent classifier (see intent.h).
 *
 * Keyword matching is done on the lower-cased turn with word-boundary checks
 * so "test" does not fire on "latest" or "testament". Priority order:
 * specific verbs first (meta/shell/test/bugfix/refactor), then exploration,
 * then explanation/asking, and plain edits last. When no keyword fires but the
 * turn mentions file paths, we default to edit (the paths select the toolset).
 * The classifier never guesses: everything else is `unknown` for the model to
 * disambiguate.
 */
#include "agent/intent.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace opencode::agent {

namespace {

/* Word-boundary-aware substring match on the lower-cased message. */
bool has_word(std::string_view hay, std::string_view word) {
    size_t pos = 0;
    while (true) {
        pos = hay.find(word, pos);
        if (pos == std::string_view::npos) return false;
        const bool left_ok =
            pos == 0 || std::isspace(static_cast<unsigned char>(hay[pos - 1]));
        const bool right_ok = pos + word.size() == hay.size() ||
                              std::isspace(static_cast<unsigned char>(
                                  hay[pos + word.size()])) ||
                              hay[pos + word.size()] == ',' ||
                              hay[pos + word.size()] == '.' ||
                              hay[pos + word.size()] == '?' ||
                              hay[pos + word.size()] == '!';
        if (left_ok && right_ok) return true;
        pos += word.size();
    }
}

/* Array-size-deducing matcher: the count can never be wrong. */
template <size_t N>
bool has_any(std::string_view hay, const std::string_view (&words)[N]) {
    for (size_t i = 0; i < N; ++i)
        if (has_word(hay, words[i])) return true;
    return false;
}

bool is_path_ext(std::string_view token) {
    const size_t dot = token.rfind('.');
    if (dot == std::string_view::npos || dot == 0) return false;
    const std::string_view ext = token.substr(dot + 1);
    constexpr std::string_view kExts[] = {
        "c", "cc", "cpp", "cxx", "h", "hh", "hpp", "hxx", "py", "rs", "go",
        "js", "ts", "json", "toml", "yaml", "yml", "md", "txt", "sh", "cmake",
    };
    for (const std::string_view e : kExts)
        if (ext == e) return true;
    return false;
}

std::string lower_ascii(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (const char c : in)
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    return out;
}

bool looks_pathy(std::string_view token) {
    if (token.find('/') != std::string_view::npos) return true;
    if (token.find('\\') != std::string_view::npos) return true;
    if (is_path_ext(token)) return true;
    return false;
}

} /* namespace */

std::vector<std::string> extract_path_hints(std::string_view user_message) {
    std::vector<std::string> out;
    size_t i = 0;
    const size_t n = user_message.size();
    while (i < n) {
        while (i < n && (std::isspace(static_cast<unsigned char>(
                             user_message[i])) ||
                         user_message[i] == ',' || user_message[i] == ';'))
            ++i;
        size_t j = i;
        while (j < n && !std::isspace(static_cast<unsigned char>(
                            user_message[j])) &&
               user_message[j] != ',' && user_message[j] != ';')
            ++j;
        if (j > i) {
            std::string_view tok = user_message.substr(i, j - i);
            /* strip trailing punctuation */
            while (!tok.empty() &&
                   (tok.back() == '.' || tok.back() == '?' ||
                    tok.back() == '!' || tok.back() == ')'))
                tok.remove_suffix(1);
            /* strip surrounding quotes */
            if (tok.size() >= 2 && tok.front() == '"' && tok.back() == '"')
                tok = tok.substr(1, tok.size() - 2);
            if (looks_pathy(tok)) {
                const std::string s(tok);
                bool dup = false;
                for (const std::string& p : out)
                    if (p == s) {
                        dup = true;
                        break;
                    }
                if (!dup) out.push_back(s);
            }
        }
        i = j + 1;
    }
    return out;
}

IntentPlan classify_intent(std::string_view user_message) {
    const std::string msg = lower_ascii(user_message);
    IntentPlan plan;
    plan.affected_paths = extract_path_hints(user_message);

    static constexpr std::string_view kMeta[] = {
        "remember", "note that", "add a rule", "remember this rule",
    };
    static constexpr std::string_view kShell[] = {
        "run", "execute", "shell", "command", "compile", "build it",
        "run the tests", "make it run",
    };
    static constexpr std::string_view kTest[] = {
        "add tests", "write a test", "write tests", "unit test", "fix the test",
        "add a test", "test suite", "test it",
    };
    static constexpr std::string_view kBugfix[] = {
        "bug", "crash", "segfault", "broken", "fails", "failing",
        "doesn't work", "does not work", "wrong output", "fix",
    };
    static constexpr std::string_view kRefactor[] = {
        "refactor", "restructure", "rename", "clean up", "cleanup",
        "simplify", "extract", "reorganize", "tidy",
    };
    static constexpr std::string_view kExplore[] = {
        "explore", "find", "search", "where is", "show me", "list",
        "discover", "inspect", "look at", "show",
    };
    static constexpr std::string_view kExplain[] = {
        "explain", "describe", "walk me through", "clarify", "what does this",
        "what does the", "why does", "how does",
    };
    static constexpr std::string_view kAsk[] = {
        "what is", "what's", "how do i", "how do you", "can you tell me",
        "question", "why is",
    };
    static constexpr std::string_view kEdit[] = {
        "edit", "change", "modify", "update", "rewrite", "write", "add",
        "remove", "delete", "implement", "create",
    };

    if (has_any(msg, kMeta)) {
        plan.intent = Intent::meta;
        plan.budget_profile = "minimal";
        plan.wants_tools = false;
        plan.write_allowed = false;
        return plan;
    }
    if (has_any(msg, kShell)) {
        plan.intent = Intent::shell;
        plan.budget_profile = "edit";
        plan.wants_tools = true;
        plan.write_allowed = true;
        return plan;
    }
    if (has_any(msg, kTest)) {
        plan.intent = Intent::test;
        plan.budget_profile = "edit";
        plan.wants_tools = true;
        plan.write_allowed = true;
        return plan;
    }
    if (has_any(msg, kBugfix)) {
        plan.intent = Intent::bugfix;
        plan.budget_profile = "edit";
        plan.wants_tools = true;
        plan.write_allowed = true;
        return plan;
    }
    if (has_any(msg, kRefactor)) {
        plan.intent = Intent::refactor;
        plan.budget_profile = "edit";
        plan.wants_tools = true;
        plan.write_allowed = true;
        return plan;
    }
    if (has_any(msg, kExplore)) {
        plan.intent = Intent::explore;
        plan.budget_profile = "read";
        plan.wants_tools = true;
        plan.write_allowed = false;
        return plan;
    }
    if (has_any(msg, kExplain)) {
        plan.intent = Intent::explain;
        plan.budget_profile = "minimal";
        plan.wants_tools = false;
        plan.write_allowed = false;
        return plan;
    }
    if (has_any(msg, kAsk) || msg.find('?') != std::string::npos) {
        plan.intent = Intent::ask;
        plan.budget_profile = "minimal";
        plan.wants_tools = false;
        plan.write_allowed = false;
        return plan;
    }
    if (has_any(msg, kEdit)) {
        plan.intent = Intent::edit;
        plan.budget_profile = "edit";
        plan.wants_tools = true;
        plan.write_allowed = true;
        return plan;
    }
    /* Default toward edit only when real file paths are mentioned. */
    if (!plan.affected_paths.empty()) {
        plan.intent = Intent::edit;
        plan.budget_profile = "edit";
        plan.wants_tools = true;
        plan.write_allowed = true;
        return plan;
    }
    plan.intent = Intent::unknown;
    plan.budget_profile = "minimal";
    plan.wants_tools = false;
    plan.write_allowed = false;
    return plan;
}

} /* namespace opencode::agent */

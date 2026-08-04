/*
 * symbols.cpp -- symbol-diff checker for the verify gate (Phase 9).
 *
 * Compares the before/after symbol sets of an edited file. If a definition
 * was removed and it's still referenced in the workspace, the edit is
 * blocked. This is the main mechanical-error net for T3: it catches the
 * model removing a function/type that other files depend on.
 *
 * The symbol extraction here is the same lightweight regex-fallback used
 * by the gate orchestrator -- top-level definitions at indentation 0.
 * Tree-sitter will improve precision when enabled.
 */
#include "verify/gate.h"

#include <string>
#include <string_view>
#include <vector>

namespace opencode::verify {

namespace {

/* Keyword set for filtering false-positive definitions. */
bool is_keyword(std::string_view s) {
    static const char* kws[] = {
        "if",       "else",     "for",      "while",    "switch",
        "case",     "return",   "break",    "continue", "goto",
        "do",       "struct",   "class",    "enum",     "union",
        "typedef",  "namespace","using",    "import",   "package",
        "func",     "type",     "var",      "const",    "fn",
        "pub",      "mod",      "try",      "catch",    "throw",
        "new",      "delete",   "sizeof",   "typeof",   "nullptr",
        "true",     "false",    "void",     "char",     "int",
        "float",    "double",   "long",     "short",    "unsigned",
        "signed",   "auto",     "register", "volatile", "constexpr",
        "static",   "extern",   "inline",   "virtual",  "override",
        "final",    "template", "typename", "friend",   "operator",
        "public",   "private",  "protected","internal",
    };
    for (const char* k : kws)
        if (s == k) return true;
    return false;
}

/* Extract top-level definitions from source content. Definitions are lines
 * that start at column 0 (no leading whitespace) and have an identifier
 * followed by an opening paren or brace. */
std::vector<std::string> extract_defs(std::string_view content) {
    std::vector<std::string> result;
    std::string_view rest = content;
    while (!rest.empty()) {
        auto nl = rest.find('\n');
        std::string_view line =
            (nl == std::string_view::npos) ? rest : rest.substr(0, nl);

        /* Only consider lines at column 0. */
        if (!line.empty() && line[0] != ' ' && line[0] != '\t' &&
            line[0] != '/' && line[0] != '#' && line[0] != '*' &&
            line[0] != '\\' && line[0] != '@') {
            /* Extract the first identifier. */
            size_t i = 0;
            while (i < line.size() &&
                   (std::isalpha(static_cast<unsigned char>(line[i])) ||
                    line[i] == '_' ||
                    (i > 0 && std::isdigit(static_cast<unsigned char>(line[i])))))
                ++i;
            if (i > 1) {
                std::string name(line.substr(0, i));
                if (!is_keyword(name))
                    result.push_back(std::move(name));
            }
        }

        if (nl == std::string_view::npos) break;
        rest.remove_prefix(nl + 1);
    }
    return result;
}

} /* namespace */

std::vector<SymbolIssue> check_symbols(const EditProposal& proposal,
                                       const GraphIndex& graph) {
    std::vector<SymbolIssue> issues;
    if (!graph.index || !graph.lookup) return issues;

    auto before_defs = extract_defs(proposal.before_content);
    auto after_defs = extract_defs(proposal.after_content);

    /* Find removed definitions. */
    for (const auto& bd : before_defs) {
        bool still_there = false;
        for (const auto& ad : after_defs)
            if (ad == bd) { still_there = true; break; }
        if (still_there) continue;

        /* This definition was removed. Check if it has callers. */
        std::int32_t id = 0;
        std::string nm, fl;
        std::uint32_t ln = 0;
        if (graph.lookup(graph.index, bd, proposal.path, id, nm, fl, ln).ok() &&
            id != 0) {
            std::vector<std::int32_t> callers;
            if (graph.callers_of) {
                graph.callers_of(graph.index, id, callers);
            }
            if (!callers.empty()) {
                issues.push_back({
                    SymbolIssue::Kind::removed_def,
                    bd,
                    proposal.path,
                    0
                });
            }
        }
    }

    return issues;
}

} /* namespace opencode::verify */

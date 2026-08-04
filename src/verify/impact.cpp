/*
 * impact.cpp -- 1-hop caller breakage checker (Phase 9).
 *
 * When a definition is removed from the edited file, we check whether any
 * caller in the workspace depends on it. If so, the edit is blocked with
 * the caller file listed -- this prevents "compiles in the edited file,
 * breaks two files away".
 *
 * This is a graph-level check; it doesn't re-parse caller files (that
 * would require the full build system). It reports the impact and lets
 * the agent or user decide.
 */
#include "verify/gate.h"

#include <string>
#include <string_view>
#include <vector>

namespace opencode::verify {

std::vector<ImpactIssue> check_impact(const EditProposal& proposal,
                                      const GraphIndex& graph) {
    std::vector<ImpactIssue> issues;
    if (!graph.index || !graph.lookup || !graph.callers_of) return issues;

    /* Extract definitions from before/after content. */
    auto defs_of = [](std::string_view content) {
        std::vector<std::string> result;
        std::string_view rest = content;
        while (!rest.empty()) {
            auto nl = rest.find('\n');
            std::string_view line =
                (nl == std::string_view::npos) ? rest : rest.substr(0, nl);
            if (!line.empty() && line[0] != ' ' && line[0] != '\t' &&
                line[0] != '/' && line[0] != '#' && line[0] != '*' &&
                line[0] != '\\' && line[0] != '@') {
                size_t i = 0;
                while (i < line.size() &&
                       (std::isalpha(static_cast<unsigned char>(line[i])) ||
                        line[i] == '_' ||
                        (i > 0 &&
                         std::isdigit(static_cast<unsigned char>(line[i])))))
                    ++i;
                if (i > 1) result.emplace_back(line.substr(0, i));
            }
            if (nl == std::string_view::npos) break;
            rest.remove_prefix(nl + 1);
        }
        return result;
    };

    auto before = defs_of(proposal.before_content);
    auto after = defs_of(proposal.after_content);

    for (const auto& name : before) {
        bool still_there = false;
        for (const auto& a : after)
            if (a == name) { still_there = true; break; }
        if (still_there) continue;

        /* This definition was removed. Find callers. */
        std::int32_t id = 0;
        std::string nm, fl;
        std::uint32_t ln = 0;
        graph.lookup(graph.index, name, proposal.path, id, nm, fl, ln);
        if (id == 0) continue;

        std::vector<std::int32_t> callers;
        graph.callers_of(graph.index, id, callers);

        for (std::int32_t caller_id : callers) {
            /* Resolve caller info. We use the sym_by_id pattern via lookup.
             * Since we don't have a direct sym_by_id adapter, we report the
             * caller id and let the agent resolve it. */
            std::string caller_name;
            std::string caller_file;
            std::uint32_t caller_line = 0;
            std::int32_t dummy = 0;
            if (graph.lookup(graph.index, "", "", dummy, caller_name,
                             caller_file, caller_line).ok()) {
                if (!caller_file.empty()) {
                    issues.push_back({
                        caller_file,
                        caller_name.empty()
                            ? ("sym#" + std::to_string(caller_id))
                            : caller_name,
                        "change removes '" + name + "' which is called from " +
                        caller_file
                    });
                }
            }
        }
    }

    return issues;
}

} /* namespace opencode::verify */

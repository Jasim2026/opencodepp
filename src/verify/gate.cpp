/*
 * gate.cpp -- the verification gate orchestrator (see gate.h).
 *
 * Stages run in order: syntax -> symbols -> impact -> diff -> testmap.
 * run_all() stops on the first mandatory failure. Each stage returns a
 * GateResult with structured feedback (file, line, detail) so the agent
 * can self-correct.
 */
#include "verify/gate.h"

#include <string>
#include <string_view>
#include <vector>

#include "core/error.h"
#include "tools/exec/patch.h"

namespace opencode::verify {

namespace {

GateResult pass(Stage s) {
    GateResult r;
    r.stage = s;
    r.pass = true;
    r.err = core::ok();
    return r;
}

GateResult fail(Stage s, core::error_code ec, const std::string& detail,
                const std::string& file = "", std::uint32_t line = 0,
                std::uint32_t col = 0) {
    GateResult r;
    r.stage = s;
    r.pass = false;
    r.err = ec;
    r.detail = detail;
    r.file = file;
    r.line = line;
    r.col = col;
    return r;
}

GateResult check_syntax_stage(const EditProposal& p) {
    auto issues = check_syntax(p.after_content, p.path);
    if (issues.empty()) return pass(Stage::syntax);
    const auto& worst = issues[0];
    return fail(Stage::syntax,
                core::make_error_code(core::Err::e_verify_fail),
                worst.msg, p.path, worst.line, worst.col);
}

GateResult check_symbols_stage(const EditProposal& p, const Context& ctx) {
    if (!ctx.graph.index || !ctx.graph.lookup) return pass(Stage::symbols);
    /* Compare before vs after symbol sets. A removed definition that is
     * still referenced in the workspace is an error. */
    auto extract_syms = [&](std::string_view content,
                            std::vector<std::string>& defs) {
        /* Lightweight pass: collect top-level definitions (func/class/struct
         * at indentation 0) and all identifier references. This is the
         * regex-fallback heuristic -- good enough for the mechanical-error
         * net; tree-sitter will improve precision when enabled. */
        std::vector<std::string> lines;
        std::string_view rest = content;
        while (!rest.empty()) {
            auto nl = rest.find('\n');
            if (nl == std::string_view::npos) {
                lines.emplace_back(rest);
                break;
            }
            lines.emplace_back(rest.substr(0, nl));
            rest.remove_prefix(nl + 1);
        }
        for (const std::string& line : lines) {
            /* Skip indented lines (not definitions). */
            if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) continue;
            /* Skip blank/comment lines. */
            std::string_view sv = line;
            while (!sv.empty() && sv[0] == ' ') sv.remove_prefix(1);
            if (sv.empty() || sv[0] == '/' || sv[0] == '#' || sv[0] == '*')
                continue;
            /* Collect the first identifier as a definition candidate. */
            size_t i = 0;
            while (i < sv.size() && (std::isalpha(sv[i]) || sv[i] == '_' ||
                                     (i > 0 && std::isdigit(sv[i]))))
                ++i;
            if (i > 2) {
                std::string name(sv.substr(0, i));
                /* Reject if it looks like a keyword. */
                static const char* kws[] = {
                    "if",   "else",  "for",   "while", "switch", "case",
                    "return", "break", "continue", "goto", "do",
                    "struct", "class", "enum",  "union", "typedef",
                    "namespace", "using", "import", "package", "func",
                    "type",  "var",   "const", "fn",    "pub",    "mod",
                };
                bool kw = false;
                for (const char* k : kws)
                    if (name == k) { kw = true; break; }
                if (!kw) defs.push_back(std::move(name));
            }
        }
    };

    std::vector<std::string> before_defs, after_defs;
    extract_syms(p.before_content, before_defs);
    extract_syms(p.after_content, after_defs);

    /* Find removed defs: in before but not in after. */
    std::vector<std::string> removed;
    for (const auto& bd : before_defs) {
        bool found = false;
        for (const auto& ad : after_defs)
            if (ad == bd) { found = true; break; }
        if (!found) removed.push_back(bd);
    }
    if (removed.empty()) return pass(Stage::symbols);

    /* Check if any removed def is still referenced in the workspace. */
    for (const auto& name : removed) {
        std::int32_t id = 0;
        std::string nm, fl;
        std::uint32_t ln = 0;
        if (ctx.graph.lookup(ctx.graph.index, name, p.path, id, nm, fl, ln)
                .ok() && id != 0) {
            /* Symbol is defined elsewhere -- removing it here is fine
             * as long as it's not the only definition. Check callers. */
            std::vector<std::int32_t> callers;
            ctx.graph.callers_of(ctx.graph.index, id, callers);
            if (!callers.empty()) {
                return fail(Stage::symbols,
                    core::make_error_code(core::Err::e_verify_fail),
                    "removed definition '" + name +
                    "' is still referenced by " + std::to_string(callers.size()) +
                    " caller(s) in the workspace",
                    p.path);
            }
        } else {
            /* Symbol not found in the index at all after the edit -- if it
             * was referenced before, the references are now broken. */
            std::vector<std::int32_t> callers;
            ctx.graph.lookup(ctx.graph.index, name, "", id, nm, fl, ln);
            if (id != 0) {
                ctx.graph.callers_of(ctx.graph.index, id, callers);
                if (!callers.empty()) {
                    return fail(Stage::symbols,
                        core::make_error_code(core::Err::e_verify_fail),
                        "removed definition '" + name +
                        "' has no remaining definition but is referenced",
                        p.path);
                }
            }
        }
    }
    return pass(Stage::symbols);
}

GateResult check_impact_stage(const EditProposal& p, const Context& ctx) {
    if (!ctx.graph.index || !ctx.graph.lookup || !ctx.graph.callers_of)
        return pass(Stage::impact);
    /* Find symbols changed in this edit (defs that differ between before/after).
     * For each, check if callers' files have syntax issues after the edit. */
    auto defs_of = [](std::string_view content) {
        std::vector<std::string> result;
        std::string_view rest = content;
        while (!rest.empty()) {
            auto nl = rest.find('\n');
            std::string_view line =
                (nl == std::string_view::npos) ? rest : rest.substr(0, nl);
            if (!line.empty() && line[0] != ' ' && line[0] != '\t' &&
                line[0] != '/' && line[0] != '#') {
                size_t i = 0;
                while (i < line.size() && (std::isalpha(line[i]) ||
                                           line[i] == '_' ||
                                           (i > 0 && std::isdigit(line[i]))))
                    ++i;
                if (i > 2) result.emplace_back(line.substr(0, i));
            }
            if (nl == std::string_view::npos) break;
            rest.remove_prefix(nl + 1);
        }
        return result;
    };

    auto before = defs_of(p.before_content);
    auto after = defs_of(p.after_content);

    for (const auto& name : before) {
        bool still_there = false;
        for (const auto& a : after)
            if (a == name) { still_there = true; break; }
        if (still_there) continue;

        /* This definition was removed. Check if callers exist. */
        std::int32_t id = 0;
        std::string nm, fl;
        std::uint32_t ln = 0;
        ctx.graph.lookup(ctx.graph.index, name, p.path, id, nm, fl, ln);
        if (id == 0) continue;
        std::vector<std::int32_t> callers;
        ctx.graph.callers_of(ctx.graph.index, id, callers);
        for (int32_t cid : callers) {
            (void)cid;
            /* We can report the caller but can't re-parse their file here
             * (that would require reading the file). Report the impact. */
            std::string caller_name;
            std::string caller_file;
            std::uint32_t caller_line = 0;
            std::int32_t dummy = 0;
            ctx.graph.lookup(ctx.graph.index, "", "", dummy, caller_name,
                             caller_file, caller_line);
            if (!caller_file.empty()) {
                return fail(Stage::impact,
                    core::make_error_code(core::Err::e_verify_fail),
                    "change removes '" + name + "' which is called by " +
                    caller_file,
                    p.path);
            }
        }
    }
    return pass(Stage::impact);
}

GateResult check_diff_stage(const EditProposal& p) {
    if (p.patch_text.empty()) {
        /* For file.write, verify after_content is non-empty and different. */
        if (p.after_content == p.before_content) {
            return fail(Stage::diff,
                core::make_error_code(core::Err::e_verify_fail),
                "edit produces no change", p.path);
        }
        return pass(Stage::diff);
    }
    /* For file.patch, try to apply the patch and verify the result. */
    std::vector<tools::exec::patch::Hunk> hunks;
    if (const core::error_code c = tools::exec::patch::parse(p.patch_text, hunks); !c.ok())
        return fail(Stage::diff, c,
                    "patch parse failed", p.path);
    std::string applied;
    if (const core::error_code c = tools::exec::patch::apply(hunks, p.before_content, applied); !c.ok())
        return fail(Stage::diff, c,
                    "patch does not apply cleanly (context mismatch)", p.path);
    if (applied != p.after_content) {
        return fail(Stage::diff,
            core::make_error_code(core::Err::e_verify_fail),
            "patch applied but result differs from expected after_content",
            p.path);
    }
    return pass(Stage::diff);
}

GateResult check_testmap_stage(const EditProposal& /*p*/, const Context& /*ctx*/) {
    /* Testmap is informational -- it never blocks. It just lists affected tests.
     * For now, it always passes. The actual mapping is in testmap.cpp and
     * called by the agent (Phase 10). */
    return pass(Stage::testmap);
}

} /* namespace */

GateResult Gate::run(Stage stage, const EditProposal& proposal,
                     const Context& ctx) const {
    switch (stage) {
        case Stage::syntax:   return check_syntax_stage(proposal);
        case Stage::symbols:  return check_symbols_stage(proposal, ctx);
        case Stage::impact:   return check_impact_stage(proposal, ctx);
        case Stage::diff:     return check_diff_stage(proposal);
        case Stage::testmap:  return check_testmap_stage(proposal, ctx);
    }
    return fail(stage, core::make_error_code(core::Err::e_internal),
                "unknown stage");
}

std::vector<GateResult> Gate::run_all(const EditProposal& proposal,
                                      const Context& ctx) const {
    std::vector<GateResult> results;
    const Stage order[] = {Stage::syntax, Stage::symbols, Stage::impact,
                           Stage::diff, Stage::testmap};
    for (Stage s : order) {
        bool mandatory = true;
        switch (s) {
            case Stage::syntax:  mandatory = ctx.mandatory_syntax;  break;
            case Stage::symbols: mandatory = ctx.mandatory_symbols; break;
            case Stage::impact:  mandatory = ctx.mandatory_impact;  break;
            case Stage::diff:    mandatory = ctx.mandatory_diff;    break;
            case Stage::testmap: mandatory = false; /* always info */ break;
        }
        GateResult r = run(s, proposal, ctx);
        results.push_back(std::move(r));
        if (!results.back().pass && mandatory) break;
    }
    return results;
}

GateResult Gate::run_syntax(std::string_view content,
                            std::string_view file) const {
    auto issues = check_syntax(content, file);
    if (issues.empty()) return pass(Stage::syntax);
    const auto& worst = issues[0];
    return fail(Stage::syntax,
                core::make_error_code(core::Err::e_verify_fail),
                worst.msg, std::string(file), worst.line, worst.col);
}

} /* namespace opencode::verify */

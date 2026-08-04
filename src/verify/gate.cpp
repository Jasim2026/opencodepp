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
    auto issues = check_symbols(p, ctx.graph);
    if (issues.empty()) return pass(Stage::symbols);
    const auto& worst = issues[0];
    std::string kind_str;
    switch (worst.kind) {
        case SymbolIssue::Kind::undefined_ref: kind_str = "undefined reference"; break;
        case SymbolIssue::Kind::removed_def:   kind_str = "removed definition still referenced"; break;
        case SymbolIssue::Kind::wrong_arity:   kind_str = "wrong arity"; break;
    }
    return fail(Stage::symbols,
                core::make_error_code(core::Err::e_verify_fail),
                kind_str + " '" + worst.name + "'",
                worst.file.empty() ? p.path : worst.file, worst.line);
}

GateResult check_impact_stage(const EditProposal& p, const Context& ctx) {
    auto issues = check_impact(p, ctx.graph);
    if (issues.empty()) return pass(Stage::impact);
    const auto& worst = issues[0];
    return fail(Stage::impact,
                core::make_error_code(core::Err::e_verify_fail),
                worst.detail,
                p.path);
}

GateResult check_diff_stage(const EditProposal& p) {
    auto issues = check_diff(p);
    if (issues.empty()) return pass(Stage::diff);
    return fail(Stage::diff,
                core::make_error_code(core::Err::e_verify_fail),
                issues[0].detail, p.path);
}

GateResult check_testmap_stage(const EditProposal& p, const Context& /*ctx*/) {
    auto mappings = map_tests(p);
    (void)mappings; /* informational, never blocks */
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

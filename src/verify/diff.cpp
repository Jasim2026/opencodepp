/*
 * diff.cpp -- patch-apply verification for the verify gate (Phase 9).
 *
 * For file.patch proposals: parse the patch, apply it in memory to
 * before_content, and verify the result matches after_content exactly.
 * Also flags minimal-diff issues (unrelated formatting/whitespace churn
 * unless the tool explicitly reformats).
 *
 * For file.write proposals: verify the after_content is different from
 * before_content (no-op edits are blocked).
 */
#include "verify/gate.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "tools/exec/patch.h"

namespace opencode::verify {

namespace {

/* Count non-whitespace lines that differ between two texts. Used to detect
 * whether the edit is minimal (only changed lines differ, not reindented). */
std::uint32_t count_changed_lines(std::string_view a, std::string_view b) {
    auto split_to_vec = [](std::string_view text) {
        std::vector<std::string> lines;
        std::string_view rest = text;
        while (!rest.empty()) {
            auto nl = rest.find('\n');
            if (nl == std::string_view::npos) {
                lines.emplace_back(rest);
                break;
            }
            lines.emplace_back(rest.substr(0, nl));
            rest.remove_prefix(nl + 1);
        }
        return lines;
    };

    auto la = split_to_vec(a);
    auto lb = split_to_vec(b);
    std::uint32_t diff = 0;
    std::size_t max_lines = la.size() > lb.size() ? la.size() : lb.size();
    for (std::size_t i = 0; i < max_lines; ++i) {
        std::string_view ca = i < la.size() ? std::string_view(la[i]) : "";
        std::string_view cb = i < lb.size() ? std::string_view(lb[i]) : "";
        if (ca != cb) ++diff;
    }
    return diff;
}

} /* namespace */

std::vector<DiffIssue> check_diff(const EditProposal& proposal) {
    std::vector<DiffIssue> issues;

    if (!proposal.patch_text.empty()) {
        /* file.patch: parse and apply, then compare. */
        std::vector<tools::exec::patch::Hunk> hunks;
        if (const core::error_code c =
                tools::exec::patch::parse(proposal.patch_text, hunks);
            !c.ok()) {
            issues.push_back({"patch parse failed: " +
                              std::string(c.message()), 0});
            return issues;
        }

        std::string applied;
        if (const core::error_code c = tools::exec::patch::apply(
                hunks, proposal.before_content, applied);
            !c.ok()) {
            issues.push_back(
                {"patch does not apply cleanly (context mismatch)", 0});
            return issues;
        }

        if (applied != proposal.after_content) {
            issues.push_back(
                {"patch applied but result differs from expected "
                 "after_content",
                 0});
            return issues;
        }
    } else {
        /* file.write: verify the edit is non-trivial. */
        if (proposal.after_content == proposal.before_content) {
            issues.push_back({"edit produces no change", 0});
            return issues;
        }
    }

    /* Minimal-diff check: flag if too many lines changed relative to the
     * size of the edit. This catches the model accidentally reformatting
     * the entire file. Threshold: > 80% of lines changed for a "small"
     * edit (< 20 lines in after_content) is suspicious. */
    std::size_t after_lines = 0;
    {
        std::string_view rest = proposal.after_content;
        while (!rest.empty()) {
            ++after_lines;
            auto nl = rest.find('\n');
            if (nl == std::string_view::npos) break;
            rest.remove_prefix(nl + 1);
        }
    }
    if (after_lines < 20 && !proposal.before_content.empty()) {
        std::uint32_t changed =
            count_changed_lines(proposal.before_content, proposal.after_content);
        if (changed > after_lines * 0.8 && after_lines > 2) {
            issues.push_back({
                "large fraction of lines changed (" +
                std::to_string(changed) + "/" + std::to_string(after_lines) +
                ") -- may be unrelated reformatting",
                0});
        }
    }

    return issues;
}

} /* namespace opencode::verify */

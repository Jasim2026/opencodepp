/*
 * exec/patch.cpp -- unified-diff parser + applier (see patch.h).
 *
 * Application strategy: process hunks bottom-up (last hunk first). Hunks below
 * the current one are already applied, but the region at/after the current
 * hunk's old-start is untouched, so a forward context search from old-start is
 * unambiguous. Each hunk replaces its (context+removed) block with
 * (context+added). Any mismatch aborts with no output -- apply_file never
 * writes on error.
 */
#include "tools/exec/patch.h"

#include <utility>

#include "tools/exec/util.h"
#include "util/string.h"

namespace opencode::tools::exec::patch {

namespace {

core::error_code hunk_err() {
    return core::make_error_code(core::Err::e_tool_reject);
}

bool parse_start(std::string_view tok, std::uint64_t& start) {
    if (tok.empty() || tok[0] != '-') return false;
    /* token like "-12" or "-12,3" or "-12,0" */
    tok.remove_prefix(1);
    const size_t comma = tok.find(',');
    const std::string_view num = tok.substr(0, comma);
    if (!util::parse_u64(num, start)) return false;
    if (start > 0) --start; /* header is 1-based; store 0-based */
    return true;
}

void emit_hunk(const Hunk& h, std::string& out) {
    out += "@@ -";
    out += std::to_string(h.orig_start + 1);
    out += " +";
    out += std::to_string(h.new_start + 1);
    out += " @@\n";
    /* Reconstruct unified diff lines by walking old_body and new_body in
     * parallel. Lines present in both at the same position are context (' ');
     * lines only in old are removed ('-'); lines only in new are added ('+').
     * Since old_body and new_body may have different lengths (more removes
     * than adds or vice versa), we walk them with two cursors. */
    size_t oi = 0, ni = 0;
    while (oi < h.old_body.size() || ni < h.new_body.size()) {
        if (oi < h.old_body.size() && ni < h.new_body.size() &&
            h.old_body[oi] == h.new_body[ni]) {
            out.push_back(' ');
            out += h.old_body[oi];
            out.push_back('\n');
            ++oi; ++ni;
        } else if (oi < h.old_body.size() &&
                   (ni >= h.new_body.size() ||
                    /* Heuristic: if the old line matches the next new line,
                     * it was removed rather than being a context/removed pair
                     * that should be emitted together. */
                    (ni + 1 < h.new_body.size() && h.old_body[oi] == h.new_body[ni + 1]))) {
            /* old line has no corresponding new line at this position - removed */
            out.push_back('-');
            out += h.old_body[oi];
            out.push_back('\n');
            ++oi;
        } else if (ni < h.new_body.size()) {
            /* new line with no corresponding old line - added */
            out.push_back('+');
            out += h.new_body[ni];
            out.push_back('\n');
            ++ni;
        } else {
            break;
        }
    }
}

} /* namespace */

core::error_code parse(std::string_view text, std::vector<Hunk>& out) {
    const std::vector<std::string> lines = split_lines(text);
    Hunk cur;
    bool in_hunk = false;
    std::uint64_t orig = 0, add = 0;
    for (const std::string& l : lines) {
        if (util::starts_with(l, "@@")) {
            if (in_hunk) out.push_back(std::move(cur));
            cur = Hunk{};
            in_hunk = true;
            /* "@@ -o[,n] +a[,m] @@" */
            std::vector<std::string> toks;
            util::split(std::string_view(l).substr(2), " ", [&](std::string_view t) {
                if (!t.empty()) toks.push_back(std::string(t));
            });
            if (toks.size() >= 2 && parse_start(toks[0], orig)) {
                cur.orig_start = orig;
                std::string_view nt = toks[1];
                if (!nt.empty() && nt[0] == '+') {
                    nt.remove_prefix(1);
                    const size_t comma = nt.find(',');
                    std::string_view num = nt.substr(0, comma);
                    if (util::parse_u64(num, add) && add > 0) --add;
                    cur.new_start = add;
                }
            }
            continue;
        }
        if (util::starts_with(l, "---") || util::starts_with(l, "+++"))
            continue; /* file headers; ignored for matching */
        if (l.empty() || l[0] == '\\') continue; /* "\\ No newline..." */
        if (!in_hunk) continue;
        if (l[0] == ' ') {
            cur.ctx.push_back(l.substr(1));
            cur.old_body.push_back(l.substr(1));
            cur.new_body.push_back(l.substr(1));
        } else if (l[0] == '-') {
            cur.rem.push_back(l.substr(1));
            cur.old_body.push_back(l.substr(1));
        } else if (l[0] == '+') {
            cur.add.push_back(l.substr(1));
            cur.new_body.push_back(l.substr(1));
        } else continue;
    }
    if (in_hunk) out.push_back(std::move(cur));
    return core::ok();
}

core::error_code apply(const std::vector<Hunk>& hunks, std::string_view original,
                       std::string& out) {
    std::vector<std::string> lines = split_lines(original);
    for (auto it = hunks.rbegin(); it != hunks.rend(); ++it) {
        const Hunk& h = *it;
        const std::vector<std::string>& old_lines = h.old_body;
        const std::vector<std::string>& new_lines = h.new_body;
        const size_t search_from =
            h.orig_start < lines.size() ? static_cast<size_t>(h.orig_start) : 0;
        ssize_t anchor = -1;
        for (size_t i = search_from;
             i + old_lines.size() <= lines.size(); ++i) {
            bool m = true;
            for (size_t k = 0; k < old_lines.size() && m; ++k)
                if (lines[i + k] != old_lines[k]) m = false;
            if (m) {
                anchor = static_cast<ssize_t>(i);
                break;
            }
        }
        if (anchor < 0) return hunk_err();
        const size_t a = static_cast<size_t>(anchor);
        std::vector<std::string> repl;
        repl.reserve(new_lines.size());
        for (const std::string& nl : new_lines) repl.push_back(nl);
        lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(a),
                    lines.begin() + static_cast<std::ptrdiff_t>(a + old_lines.size()));
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(a),
                     repl.begin(), repl.end());
    }
    out = join_lines(lines);
    return core::ok();
}

core::error_code reverse(std::string_view patch_text, std::string& out) {
    std::vector<Hunk> hunks;
    if (const core::error_code c = parse(patch_text, hunks); !c.ok()) return c;
    std::string text;
    for (const Hunk& h : hunks) {
        Hunk r;
        r.orig_start = h.new_start;
        r.new_start = h.orig_start;
        /* The reverse patch removes what the forward added, and adds what
         * the forward removed. The context lines stay the same. */
        r.ctx = h.ctx;
        r.rem = h.add;
        r.add = h.rem;
        /* Swap the ordered bodies. */
        r.old_body = h.new_body;
        r.new_body = h.old_body;
        emit_hunk(r, text);
    }
    out = std::move(text);
    return core::ok();
}

core::error_code apply_file(const std::string& path,
                            std::string_view patch_text, std::string& report) {
    std::string original;
    if (const core::error_code c = read_whole_file(path, original); !c.ok())
        return c;
    std::vector<Hunk> hunks;
    if (const core::error_code c = parse(patch_text, hunks); !c.ok()) return c;
    std::string next;
    if (const core::error_code c = apply(hunks, original, next); !c.ok()) return c;
    if (const core::error_code c = write_file_atomic(path, next); !c.ok()) return c;
    report = "applied " + std::to_string(hunks.size()) + " hunk";
    if (hunks.size() != 1) report.push_back('s');
    report += " to " + path;
    return core::ok();
}

} /* namespace opencode::tools::exec::patch */

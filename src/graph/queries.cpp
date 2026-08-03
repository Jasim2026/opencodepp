/*
 * queries.cpp -- targeted-context queries over the index (Phase 7).
 *
 * These are the retrieval layer that Phase 6's context assembler and Phase 9's
 * verifier consume: a callee snippet behind a call site, and the set of symbols
 * a change touches plus their 1-hop callers. Everything is capped by
 * IndexLimits (snippet_bytes per item, context_total_bytes per call), so the
 * output stays well inside the T1 token budget.
 */
#include "graph/index.h"

#include <string>
#include <vector>

namespace opencode::graph {

namespace {

void append_unique_id(std::vector<SymId>& v, SymId id) {
    for (const auto x : v)
        if (x == id) return;
    v.push_back(id);
}

bool contains_id(const std::vector<SymId>& v, SymId id) {
    for (const auto x : v)
        if (x == id) return true;
    return false;
}

/* Append sym `id`'s snippet, respecting the remaining byte budget. Returns
 * whether budget remains. */
bool push_snippet(const SymbolIndex& idx, SymId id, std::uint32_t& budget,
                  std::vector<Snippet>& out) {
    Snippet sn;
    if (!idx.snippet(id, 0, sn).ok()) return budget > 0;
    if (sn.bytes > budget) {
        sn.text.resize(budget);
        sn.bytes = budget;
        sn.truncated = true;
        budget = 0;
    } else {
        budget -= static_cast<std::uint32_t>(sn.bytes);
    }
    out.push_back(std::move(sn));
    return budget > 0;
}

} /* namespace */

core::error_code SymbolIndex::snippet_for_call(
    std::string_view call_file, std::string_view call_name,
    std::vector<Snippet>& out) const {
    out.clear();
    Sym callee;
    core::error_code ec = lookup(call_name, call_file, callee);
    if (!ec.ok()) return ec;
    std::uint32_t budget = limits_.context_total_bytes;
    if (!push_snippet(*this, callee.id, budget, out)) return core::ok();
    /* one level of callees, only when the callee is small */
    std::vector<std::string> cals = callees(callee.id);
    if (cals.size() > 8) return core::ok();
    for (const std::string& c : cals) {
        if (budget == 0) break;
        Sym s;
        if (!lookup(c, callee.file, s).ok()) continue;
        if (!push_snippet(*this, s.id, budget, out)) break;
    }
    return core::ok();
}

core::error_code SymbolIndex::context_for_change(
    const std::vector<std::string>& files, std::vector<Snippet>& out) const {
    out.clear();
    std::uint32_t budget = limits_.context_total_bytes;
    std::vector<SymId> seen;
    for (const std::string& f : files) {
        const auto it = files_.find(f);
        if (it == files_.end()) continue; /* not indexed */
        const ParsedFile& pf = it->second;
        const std::uint32_t end = pf.sym_first + pf.sym_count;
        for (std::uint32_t i = pf.sym_first; i < end; ++i) {
            if (budget == 0) return core::ok();
            const Sym& s = syms_[static_cast<size_t>(i)];
            if (s.id == 0 || s.file != f) continue;
            append_unique_id(seen, s.id);
            if (!push_snippet(*this, s.id, budget, out)) return core::ok();
            for (const SymId cid : callers_of(s.id)) {
                if (budget == 0) return core::ok();
                if (contains_id(seen, cid)) continue;
                append_unique_id(seen, cid);
                if (!push_snippet(*this, cid, budget, out)) return core::ok();
            }
        }
    }
    return core::ok();
}

} /* namespace opencode::graph */

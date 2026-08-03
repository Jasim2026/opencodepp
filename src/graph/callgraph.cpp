/*
 * callgraph.cpp -- call-graph queries over the dep store (Phase 7).
 *
 * Deps are stored by value with lazy cross-file resolution (to_sym is filled
 * for same-file callees only; everything else resolves by to_name on query).
 * These are 1-hop queries -- the depth the context assembler and verifier
 * actually need. Deeper traversal is left to the caller (each hop is a lookup).
 */
#include "graph/index.h"

#include <string>
#include <vector>

namespace opencode::graph {

namespace {

void append_unique(std::vector<std::string>& v, std::string_view s) {
    for (const auto& x : v)
        if (x == s) return;
    v.emplace_back(s);
}

void append_unique_id(std::vector<SymId>& v, SymId id) {
    for (const auto x : v)
        if (x == id) return;
    v.push_back(id);
}

} /* namespace */

std::vector<std::string> SymbolIndex::callees(SymId id) const noexcept {
    std::vector<std::string> out;
    for (const Dep& d : deps_) {
        if (d.from_sym == id && d.kind == DepKind::call)
            append_unique(out, d.to_name);
    }
    return out;
}

std::vector<SymId> SymbolIndex::callers_of(SymId id) const noexcept {
    std::vector<SymId> out;
    for (const Dep& d : deps_) {
        if (d.to_sym == id && d.kind == DepKind::call && d.from_sym != 0)
            append_unique_id(out, d.from_sym);
    }
    return out;
}

std::vector<SymId> SymbolIndex::callers_of_name(std::string_view name) const noexcept {
    std::vector<SymId> out;
    for (const Dep& d : deps_) {
        if (d.kind == DepKind::call && d.from_sym != 0 &&
            d.to_name == name)
            append_unique_id(out, d.from_sym);
    }
    return out;
}

} /* namespace opencode::graph */

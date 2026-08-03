/*
 * index.cpp -- SymbolIndex: extraction orchestration, id assignment, lookups,
 * snippets, file removal (Phase 7).
 *
 * SymIds are position-based (id == position in syms_ + 1) and stable within a
 * session: removal tombstones an entry instead of compacting, so ids that other
 * deps reference stay valid. by_name_ is keyed by simple name and rebuilt
 * incrementally on removal (no full rescan).
 */
#include "graph/index.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace opencode::graph {

namespace {

bool read_file_text(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    out.clear();
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

} /* namespace */

Lang SymbolIndex::detect_lang(std::string_view file) const noexcept {
    const size_t dot = file.rfind('.');
    if (dot == std::string_view::npos) return Lang::unknown;
    const std::string_view ext = file.substr(dot);
    if (ext == ".c") return Lang::c;
    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".hpp" ||
        ext == ".hh" || ext == ".hxx" || ext == ".h" || ext == ".ipp")
        return Lang::cpp;
    if (ext == ".go") return Lang::go;
    return Lang::unknown;
}

core::error_code SymbolIndex::extract_file(const std::string& file) {
    const Lang lang = detect_lang(file);
    if (lang == Lang::unknown)
        return core::make_error_code(core::Err::e_missing_cfg);
    std::string text;
    if (!read_file_text(file, text))
        return core::make_error_code(core::Err::e_missing_cfg);
    std::vector<Sym> syms;
    std::vector<Dep> deps;
    core::error_code ec = extract_lang(lang, file, text, syms, deps);
    if (!ec.ok()) return ec;
    remove_file(file);
    return extract_into(*this, file, lang, text, syms, deps);
}

void SymbolIndex::remove_file(const std::string& file) noexcept {
    for (Sym& s : syms_) {
        if (s.file != file) continue;
        auto it = by_name_.find(s.name);
        if (it != by_name_.end()) {
            std::vector<SymId>& vec = it->second;
            vec.erase(std::remove(vec.begin(), vec.end(), s.id), vec.end());
            if (vec.empty()) by_name_.erase(it);
        }
        s.id = 0;
        s.kind = SymKind::unknown;
        s.name.clear();
        s.qual.clear();
        s.file.clear();
        s.line = s.start = s.end = 0;
        s.lang = Lang::unknown;
        s.vis = Visibility::unknown;
    }
    auto dit = std::remove_if(deps_.begin(), deps_.end(),
                              [&file](const Dep& d) { return d.from_file == file; });
    deps_.erase(dit, deps_.end());
    files_.erase(file);
    touch_.erase(file);
    ++version_;
}

/* Assign SymIds, register names, resolve same-file to_sym, append. Called from
 * extract_file (and, in principle, any future backend). */
core::error_code extract_into(SymbolIndex& idx, const std::string& file,
                              Lang lang, const std::string& /*src*/,
                              std::vector<Sym>& syms,
                              std::vector<Dep>& deps) {
    const uint32_t sym_base = static_cast<uint32_t>(idx.syms_.size());
    const uint32_t dep_base = static_cast<uint32_t>(idx.deps_.size());
    std::map<std::string, SymId> name_to_id;

    uint32_t n_sym = 0;
    for (Sym& s : syms) {
        if (n_sym >= idx.limits_.max_syms_per_file) break;
        s.id = static_cast<SymId>(idx.syms_.size()) + 1;
        s.file = file;
        s.lang = lang;
        name_to_id.emplace(s.name, s.id);
        idx.by_name_[s.name].push_back(s.id);
        idx.syms_.push_back(std::move(s));
        ++n_sym;
    }

    uint32_t n_dep = 0;
    for (Dep& d : deps) {
        if (n_dep >= idx.limits_.max_deps_per_file) break;
        d.from_file = file;
        if (d.from_sym != 0) d.from_sym += static_cast<SymId>(sym_base);
        auto it = name_to_id.find(d.to_name);
        if (it != name_to_id.end()) d.to_sym = it->second;
        idx.deps_.push_back(std::move(d));
        ++n_dep;
    }

    ParsedFile pf;
    pf.sym_first = sym_base;
    pf.sym_count = n_sym;
    pf.dep_first = dep_base;
    pf.dep_count = n_dep;
    pf.lang = lang;
    idx.files_[file] = pf;

    ++idx.version_;
    ++idx.extract_count_;
    return core::ok();
}

core::error_code SymbolIndex::lookup(std::string_view qname,
                                     std::string_view file_hint,
                                     Sym& out) const noexcept {
    const std::string q(qname);
    /* fast path: by simple name */
    const auto it = by_name_.find(q);
    if (it != by_name_.end()) {
        const Sym* best = nullptr;
        for (const SymId id : it->second) {
            if (id < 1 || id > static_cast<SymId>(syms_.size())) continue;
            const Sym& s = syms_[static_cast<size_t>(id) - 1];
            if (s.id != id) continue;
            if (best == nullptr ||
                (!file_hint.empty() && s.file == file_hint))
                best = &s;
        }
        if (best != nullptr) {
            out = *best;
            return core::ok();
        }
    }
    /* slow path: qualified name / global scan (rare) */
    const Sym* hit = nullptr;
    for (const Sym& s : syms_) {
        if (s.id == 0) continue;
        if (s.qual == q) {
            hit = &s;
            break;
        }
    }
    if (hit == nullptr)
        for (const Sym& s : syms_) {
            if (s.id == 0) continue;
            if (s.name == q) {
                hit = &s;
                break;
            }
        }
    if (hit == nullptr)
        return core::make_error_code(core::Err::e_missing_cfg);
    out = *hit;
    return core::ok();
}

std::vector<Sym> SymbolIndex::all(SymKind kind, std::string_view prefix,
                                  std::uint32_t limit) const noexcept {
    std::vector<Sym> out;
    for (const Sym& s : syms_) {
        if (s.id == 0) continue;
        if (kind != SymKind::unknown && s.kind != kind) continue;
        if (!prefix.empty() && !s.name.starts_with(prefix)) continue;
        out.push_back(s);
        if (limit != 0 && out.size() >= limit) break;
    }
    return out;
}

core::error_code SymbolIndex::snippet(SymId id, std::uint32_t max_bytes,
                                      Snippet& out) const {
    if (id < 1 || id > static_cast<SymId>(syms_.size()))
        return core::make_error_code(core::Err::e_missing_cfg);
    const Sym& s = syms_[static_cast<size_t>(id) - 1];
    if (s.id == 0)
        return core::make_error_code(core::Err::e_missing_cfg);
    std::string text;
    if (!read_file_text(s.file, text))
        return core::make_error_code(core::Err::e_missing_cfg);
    if (text.empty()) {
        out = Snippet{};
        out.sym = s.qual.empty() ? s.name : s.qual;
        out.file = s.file;
        out.line = s.line;
        return core::ok();
    }
    const char* base = text.data();
    uint32_t b = s.start > text.size() ? static_cast<uint32_t>(text.size())
                                       : s.start;
    uint32_t e = s.end > text.size() ? static_cast<uint32_t>(text.size())
                                     : s.end;
    while (b > 0 && base[b - 1] != '\n') --b;
    while (e < text.size() && base[e] != '\n') ++e;
    const uint32_t cap = max_bytes != 0 ? max_bytes : limits_.snippet_bytes;
    if (e - b > cap) {
        e = b + cap;
        out.truncated = true;
    }
    out.sym = s.qual.empty() ? s.name : s.qual;
    out.file = s.file;
    out.line = s.line;
    out.text.assign(base + b, e - b);
    out.bytes = out.text.size();
    return core::ok();
}

} /* namespace opencode::graph */

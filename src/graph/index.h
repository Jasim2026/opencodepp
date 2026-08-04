/*
 * index.h -- on-demand symbol index + call graph (Phase 7).
 *
 * The engine's "code graph": a lightweight symbol index built by per-language
 * extractors (regex fallback now; tree-sitter backend slots in behind the same
 * interface when OPENCODE_USE_TREE_SITTER is enabled). It exists so context
 * assembly (Phase 6, Tier 2) and verification (Phase 9) get targeted snippets
 * instead of whole files -- the core token-efficiency win of T1.
 *
 * SymIds are stable within one SymbolIndex session; a file change renumbers
 * (version() bumps) but nothing persists ids. Cross-file references are stored
 * by name and resolved lazily by refs()/callers()/callees().
 */
#ifndef OPENCODE_GRAPH_INDEX_H
#define OPENCODE_GRAPH_INDEX_H

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.h"

namespace opencode::graph {

enum class Lang : uint8_t {
    unknown = 0,
    c = 1,
    cpp = 2,
    go = 3,
};

enum class SymKind : uint8_t {
    unknown = 0,
    function = 1,
    method = 2,
    class_ = 3,
    struct_ = 4,
    type = 5,
    global = 6,
    import = 7,
    namespace_ = 8,
    enum_ = 9,
    const_ = 10,
    macro = 11,
    package = 12,
};

enum class DepKind : uint8_t { call = 0, type = 1, include = 2 };

enum class Visibility : uint8_t {
    unknown = 0,
    public_ = 1,
    private_ = 2,
    protected_ = 3,
    internal = 4,
};

using SymId = std::int32_t;

struct Sym {
    SymId id = 0;
    SymKind kind = SymKind::unknown;
    std::string name;  /* simple name */
    std::string qual;  /* qualified name ("ns::foo" / "pkg.Method") */
    std::string file;
    std::uint32_t line = 0;   /* 1-based */
    std::uint32_t start = 0;  /* byte offset in file */
    std::uint32_t end = 0;    /* exclusive byte offset */
    Lang lang = Lang::unknown;
    Visibility vis = Visibility::unknown;
};

struct Dep {
    SymId from_sym = 0;       /* 0 = file-level (includes) */
    SymId to_sym = 0;         /* 0 = unresolved, resolved by name on query */
    DepKind kind = DepKind::call;
    std::string from_file;
    std::string to_file;
    std::string to_name;      /* raw target name (lazy cross-file resolution) */
};

struct Snippet {
    std::string sym;
    std::string file;
    std::string text;
    std::uint32_t line = 0;
    std::size_t bytes = 0;
    bool truncated = false;
};

struct IndexLimits {
    std::uint32_t snippet_bytes = 4096;        /* per-snippet cap */
    std::uint32_t context_total_bytes = 16384; /* context_for_change cap */
    std::uint32_t max_syms_per_file = 4096;
    std::uint32_t max_deps_per_file = 8192;
    std::uint32_t cache_files = 256;           /* lazy parse cache cap */
};

/* Per-file parse bookkeeping for the lazy cache (lazy.cpp). */
struct ParsedFile {
    std::uint64_t mtime = 0;
    std::uint64_t size = 0;
    std::uint32_t sym_first = 0;
    std::uint32_t sym_count = 0;
    std::uint32_t dep_first = 0;
    std::uint32_t dep_count = 0;
    Lang lang = Lang::unknown;
};

class SymbolIndex {
public:
    SymbolIndex() = default;
    explicit SymbolIndex(IndexLimits limits) : limits_(limits) {}

    /* ---- language detection ---- */
    Lang detect_lang(std::string_view file) const noexcept;

    /* ---- indexing ---- */
    /* Parse `file` (never throws on malformed source; e_missing_cfg when the
     * file cannot be read). */
    core::error_code extract_file(const std::string& file);

    /* ---- queries ---- */    core::error_code lookup(std::string_view qname, std::string_view file_hint,
                            Sym& out) const noexcept;
    /* Sym by id (tombstoned ids return e_missing_cfg). */
    core::error_code sym_by_id(SymId id, Sym& out) const noexcept;
    std::vector<Sym> all(SymKind kind, std::string_view prefix,
                         std::uint32_t limit) const noexcept;
    core::error_code snippet(SymId id, std::uint32_t max_bytes,
                             Snippet& out) const;

    /* Call graph (callgraph.cpp): callees of a sym; callers of a sym/name. */
    std::vector<std::string> callees(SymId id) const noexcept;
    std::vector<SymId> callers_of(SymId id) const noexcept;
    std::vector<SymId> callers_of_name(std::string_view name) const noexcept;

    /* ---- lazy / incremental (lazy.cpp) ---- */
    /* Index `file` unless its (mtime,size) is unchanged since last parse.
     * Never re-parses an unchanged file. */
    core::error_code ensure_indexed(const std::string& file,
                                    std::string* changed = nullptr);
    std::uint32_t extract_count() const noexcept { return extract_count_; }
    void set_cache_cap(std::uint32_t n) noexcept { limits_.cache_files = n; }

    /* ---- queries on the whole index (queries.cpp) ---- */
    /* One-level "surgical context": declaration + capped body of the callee
     * behind a call site, plus one-level callees if small. */
    core::error_code snippet_for_call(std::string_view call_file,
                                      std::string_view call_name,
                                      std::vector<Snippet>& out) const;
    /* Symbols touched in `files` + their 1-hop callers, capped by limits. */
    core::error_code context_for_change(
        const std::vector<std::string>& files,
        std::vector<Snippet>& out) const;

    std::uint32_t version() const noexcept { return version_; }
    std::size_t sym_count() const noexcept { return syms_.size(); }
    std::size_t dep_count() const noexcept { return deps_.size(); }
    std::size_t file_count() const noexcept { return files_.size(); }

private:
    friend core::error_code extract_into(SymbolIndex&, const std::string&,
                                         Lang, const std::string&,
                                         std::vector<Sym>&, std::vector<Dep>&);

    SymId next_id() noexcept { return static_cast<SymId>(syms_.size()) + 1; }
    core::error_code index_text(const std::string& file, Lang lang,
                                const std::string& text);
    void remove_file(const std::string& file) noexcept;
    void evict_lru() noexcept;
    void maybe_compact_deps() noexcept;

    IndexLimits limits_;
    std::vector<Sym> syms_;
    std::vector<Dep> deps_;
    std::map<std::string, std::vector<SymId>> by_name_;
    std::map<std::string, ParsedFile> files_;   /* path -> parse state */
    std::map<std::string, std::uint64_t> touch_; /* LRU order */
    std::uint64_t touch_clock_ = 0;
    std::uint32_t version_ = 0;
    std::uint32_t extract_count_ = 0;
};

/* Shared extractor entry (symbols.cpp): parse `src` for `lang`, appending
 * Syms and Deps (ids are assigned by extract_into after counts are known). */
core::error_code extract_lang(Lang lang, const std::string& file,
                              const std::string& src, std::vector<Sym>& syms,
                              std::vector<Dep>& deps) noexcept;

/* Open `path` once: fstat the (mtime,size) fingerprint and read the whole
 * file (index.cpp). Used by ensure_indexed so a re-parse never reads twice. */
bool read_file_stat(const std::string& path, std::string& out,
                    std::uint64_t& mtime, std::uint64_t& size);

} /* namespace opencode::graph */

#endif /* OPENCODE_GRAPH_INDEX_H */

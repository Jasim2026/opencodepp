/*
 * symbols.cpp -- per-language symbol + dependency extraction (Phase 7).
 *
 * The regex-fallback backend is a single-pass line scanner: no std::regex, no
 * allocations beyond the output vectors, deterministic (same input -> same
 * output), and fast enough that indexing a 1k-file synthetic repo stays under
 * the 500 ms acceptance gate. A tree-sitter backend can be added behind the
 * same interface (extract_lang) when OPENCODE_USE_TREE_SITTER is enabled.
 *
 * Strategy: the file is read once and a byte-for-byte "code mask" blanks out
 * comments and string/char literals (same length, newlines preserved), then
 * lines are classified against a small keyword/prefix grammar so offsets map
 * straight back to the source. Imports/includes are pulled from the raw text
 * (quotes would otherwise be masked). Function bodies are brace-matched in the
 * masked text; definitions are detected by their signature line
 * (`name(args) ... {`, with the `{` on the same line) -- a documented
 * limitation of the fallback (multiline signatures are not defs).
 *
 * Dep.from_sym / Dep.to_sym are assigned by extract_into (index.cpp) after
 * SymIds are known. extract_lang sets from_sym = file-local sym index + 1 for
 * call deps whose enclosing def is known, 0 otherwise; extract_into offsets it
 * by the file's sym base. Defs are therefore appended FIRST in each file's
 * sym list, so def index i has file-local sym index syms_base + i.
 */
#include "graph/index.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace opencode::graph {

namespace {

const char* const kCKeywords[] = {
    "if",          "else",      "while",      "for",       "do",
    "switch",      "case",      "return",     "sizeof",    "catch",
    "throw",       "new",       "delete",     "goto",      "try",
    "using",       "typedef",   "friend",     "operator",  "template",
    "typename",    "virtual",   "override",   "final",     "namespace",
    "struct",      "class",     "enum",       "union",     "public",
    "private",     "protected", "const_cast", "static_cast", "dynamic_cast",
    "reinterpret_cast",
    "void",        "char",      "int",        "float",     "double",
    "long",        "short",     "unsigned",   "signed",    "auto",
    "register",    "volatile",  "constexpr",  "static",    "const",
    "extern",      "inline",
};

const char* const kGoKeywords[] = {
    "if",        "for",      "range",    "switch",    "select",
    "func",      "go",       "defer",    "return",    "make",
    "new",       "append",   "len",      "cap",       "copy",
    "delete",    "recover",  "panic",    "goto",      "case",
    "fallthrough", "type",   "var",      "const",     "package",
    "import",    "struct",   "interface", "map",      "chan",
};

bool is_keyword(std::string_view s, const char* const* kw, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (s == kw[i]) return true;
    return false;
}

bool is_c_keyword(std::string_view s) {
    return is_keyword(s, kCKeywords, sizeof kCKeywords / sizeof *kCKeywords);
}
bool is_go_keyword(std::string_view s) {
    return is_keyword(s, kGoKeywords, sizeof kGoKeywords / sizeof *kGoKeywords);
}

bool is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

bool is_space(char c) { return c == ' ' || c == '\t'; }

std::string_view ltrim(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && is_space(s[i])) ++i;
    return s.substr(i);
}

/* Blank comments + string/char literals in-place (same length). Offsets are
 * preserved. Handles line comments, block comments, string and char literals
 * (with escapes). */
void mask_comments_and_strings(std::string& code) {
    const size_t n = code.size();
    bool line_comment = false, block_comment = false;
    for (size_t i = 0; i < n; ++i) {
        const char c = code[i];
        const char nx = i + 1 < n ? code[i + 1] : '\0';
        if (line_comment) {
            if (c == '\n') {
                line_comment = false;
            } else {
                code[i] = ' ';
            }
            continue;
        }
        if (block_comment) {
            if (c == '*' && nx == '/') {
                code[i] = code[i + 1] = ' ';
                ++i;
                block_comment = false;
            } else if (c != '\n') {
                code[i] = ' ';
            }
            continue;
        }
        if (c == '/' && nx == '/') {
            code[i] = code[i + 1] = ' ';
            ++i;
            line_comment = true;
            continue;
        }
        if (c == '/' && nx == '*') {
            code[i] = code[i + 1] = ' ';
            ++i;
            block_comment = true;
            continue;
        }
        if (c == '"') { /* string literal */
            code[i] = ' ';
            for (++i; i < n; ++i) {
                if (code[i] == '\\') {
                    if (i + 1 < n) code[i + 1] = ' ';
                    ++i;
                    continue;
                }
                if (code[i] == '"') {
                    code[i] = ' ';
                    break;
                }
                if (code[i] != '\n') code[i] = ' ';
            }
            continue;
        }
        if (c == '\'') { /* char literal */
            code[i] = ' ';
            for (++i; i < n; ++i) {
                if (code[i] == '\\') {
                    if (i + 1 < n) code[i + 1] = ' ';
                    ++i;
                    continue;
                }
                if (code[i] == '\'') {
                    code[i] = ' ';
                    break;
                }
                if (code[i] != '\n') code[i] = ' ';
            }
            continue;
        }
    }
}

uint32_t line_of(std::string_view src, size_t off) {
    uint32_t line = 1;
    for (size_t i = 0; i < off && i < src.size(); ++i)
        if (src[i] == '\n') ++line;
    return line;
}

/* Find the matching close-brace for `open` at `from` in `code` (masked, so
 * only real braces remain). Returns the offset just past the matching brace. */
size_t match_brace(const std::string& code, size_t from, char open, char close) {
    int depth = 0;
    const size_t n = code.size();
    for (size_t i = from; i < n; ++i) {
        if (code[i] == open) ++depth;
        else if (code[i] == close) {
            --depth;
            if (depth == 0) return i + 1;
        }
    }
    return std::string::npos;
}

struct Line {
    size_t off; /* offset of line start in the buffer */
    size_t end; /* offset just past the newline */
    std::string_view sv;
};

/* Iterate lines without copying. */
template <typename Fn>
void for_each_line(const std::string& s, Fn fn) {
    size_t start = 0;
    const size_t n = s.size();
    for (size_t i = 0; i <= n; ++i) {
        if (i == n || s[i] == '\n') {
            fn(Line{start, i, std::string_view(s).substr(start, i - start)});
            start = i + 1;
        }
    }
}

struct FuncDef {
    size_t name_off = 0;  /* buffer offset of the (unqualified) name */
    size_t start = 0;     /* buffer offset of the signature line start */
    size_t body_open = 0; /* buffer offset of '{' */
    size_t body_end = 0;  /* just past the matching '}' */
    std::string name;
};

/* c/cpp function definition: the first `name(` candidate on the line whose
 * name is not a keyword and whose matching `)` is followed by `{` before `;`.
 * Returns false when the line is not a definition. */
bool try_cpp_def(const std::string& code, const Line& l, FuncDef& out) {
    const std::string_view t = ltrim(l.sv);
    if (t.empty() || t[0] == '#' || t[0] == '}' || t[0] == '{') return false;
    for (size_t i = l.off; i < l.end; ++i) {
        if (code[i] != '(') continue;
        size_t q = i;
        while (q > l.off && is_ident_char(code[q - 1])) --q;
        if (q == i) continue; /* no identifier before '(' */
        const std::string_view tok(code.data() + q, i - q);
        if (is_c_keyword(tok)) continue;
        /* matching ')' on this line */
        int depth = 0;
        size_t j = i;
        for (; j < l.end; ++j) {
            if (code[j] == '(') ++depth;
            else if (code[j] == ')') {
                --depth;
                if (depth == 0) break;
            }
        }
        if (j == l.end) continue; /* unbalanced on this line -> skip */
        /* ')' must be followed by '{' (before any ';') on the same line */
        size_t open = std::string::npos;
        for (size_t k = j + 1; k < l.end; ++k) {
            if (code[k] == '{') {
                open = k;
                break;
            }
            if (code[k] == ';') break;
        }
        if (open == std::string::npos) continue;
        const size_t close = match_brace(code, open, '{', '}');
        if (close == std::string::npos) continue;
        /* extend the name back over :: segments (Foo::bar) */
        size_t ns = q;
        while (ns >= 2 && code[ns - 1] == ':' && code[ns - 2] == ':') {
            ns -= 2;
            while (ns > l.off && is_ident_char(code[ns - 1])) --ns;
        }
        out.name.assign(code.data() + ns, i - ns);
        out.name_off = ns;
        out.start = l.off;
        out.body_open = open;
        out.body_end = close;
        return true;
    }
    return false;
}

} /* namespace */

core::error_code extract_lang(Lang lang, const std::string& file,
                              const std::string& src, std::vector<Sym>& syms,
                              std::vector<Dep>& deps) noexcept {
    if (lang != Lang::c && lang != Lang::cpp && lang != Lang::go)
        return core::make_error_code(core::Err::e_not_impl);

    if (lang == Lang::c || lang == Lang::cpp) {
        /* includes, from raw text (quote form would be masked) */
        for_each_line(src, [&](const Line& l) {
            const std::string_view t = ltrim(l.sv);
            if (!t.starts_with("#include")) return;
            const size_t p = t.find_first_not_of(" \t", 8);
            if (p == std::string_view::npos) return;
            const char open = t[p];
            if (open != '<' && open != '"') return;
            const char close = open == '<' ? '>' : '"';
            const size_t e = t.find(close, p + 1);
            if (e == std::string_view::npos) return;
            Dep d;
            d.kind = DepKind::include;
            d.from_file = file;
            d.to_file = std::string(t.substr(p + 1, e - p - 1));
            d.to_name = d.to_file;
            deps.push_back(std::move(d));
        });

        std::string code = src;
        mask_comments_and_strings(code);

        std::vector<FuncDef> defs;
        std::vector<Sym> extra; /* types, namespaces, macros */
        for_each_line(code, [&](const Line& l) {
            const std::string_view t = ltrim(l.sv);
            if (t.empty()) return;
            const size_t tbase = l.off + (l.sv.size() - t.size());

            if (t[0] == '#') {
                if (t.starts_with("#define")) {
                    const size_t p = t.find_first_not_of(" \t", 7);
                    if (p != std::string_view::npos && is_ident_char(t[p])) {
                        size_t e = p;
                        while (e < t.size() && is_ident_char(t[e])) ++e;
                        Sym s;
                        s.kind = SymKind::macro;
                        s.name = s.qual = std::string(t.substr(p, e - p));
                        s.line = line_of(code, tbase + p);
                        s.start = static_cast<uint32_t>(tbase + p);
                        s.end = s.start + static_cast<uint32_t>(s.name.size());
                        extra.push_back(std::move(s));
                    }
                }
                return;
            }

            /* struct / class / enum / union NAME (optional `typedef `) */
            {
                std::string_view rest = t;
                if (rest.starts_with("typedef")) rest = ltrim(rest.substr(7));
                size_t we = 0;
                while (we < rest.size() && is_ident_char(rest[we])) ++we;
                const std::string_view w = rest.substr(0, we);
                if (w == "struct" || w == "class" || w == "enum" ||
                    w == "union") {
                    std::string_view after = ltrim(rest.substr(we));
                    std::string name;
                    if (!after.empty() && is_ident_char(after[0])) {
                        size_t ne = 0;
                        while (ne < after.size() && is_ident_char(after[ne]))
                            ++ne;
                        name = std::string(after.substr(0, ne));
                    }
                    if (w == "enum" && name == "class") {
                        after = ltrim(after.substr(after.find_first_of(" \t")));
                        if (after.empty() || !is_ident_char(after[0])) return;
                        size_t ne = 0;
                        while (ne < after.size() && is_ident_char(after[ne]))
                            ++ne;
                        name = std::string(after.substr(0, ne));
                    }
                    if (name.empty()) return;
                    Sym s;
                    s.kind = w == "struct" ? SymKind::struct_
                             : w == "class" ? SymKind::class_
                             : w == "enum"  ? SymKind::enum_
                                            : SymKind::type;
                    s.name = s.qual = name;
                    const size_t name_off = tbase + t.size() - after.size();
                    s.line = line_of(code, name_off);
                    s.start = static_cast<uint32_t>(name_off);
                    s.end = s.start + static_cast<uint32_t>(s.name.size());
                    extra.push_back(std::move(s));
                }
                if (w == "struct" || w == "class" || w == "enum" ||
                    w == "union")
                    return;
            }

            /* namespace NAME */
            if (t.starts_with("namespace")) {
                std::string_view rest = ltrim(t.substr(9));
                if (!rest.empty() && is_ident_char(rest[0])) {
                    size_t e = 0;
                    while (e < rest.size() && is_ident_char(rest[e])) ++e;
                    Sym s;
                    s.kind = SymKind::namespace_;
                    s.name = s.qual = std::string(rest.substr(0, e));
                    const size_t name_off = tbase + t.size() - rest.size();
                    s.line = line_of(code, name_off);
                    s.start = static_cast<uint32_t>(name_off);
                    s.end = s.start + static_cast<uint32_t>(s.name.size());
                    extra.push_back(std::move(s));
                }
                return;
            }

            /* function definition */
            FuncDef d;
            if (try_cpp_def(code, l, d)) defs.push_back(std::move(d));
        });

        /* defs first (so file-local index == defs index), then the rest */
        const size_t syms_base = syms.size();
        for (const FuncDef& d : defs) {
            Sym s;
            s.kind = SymKind::function;
            s.name = s.qual = d.name;
            s.line = line_of(code, d.name_off);
            s.start = static_cast<uint32_t>(d.start);
            s.end = static_cast<uint32_t>(d.body_end);
            syms.push_back(std::move(s));
        }
        for (Sym& s : extra) syms.push_back(std::move(s));

        auto inside_def = [&](size_t off) -> int32_t {
            for (size_t i = 0; i < defs.size(); ++i) {
                const FuncDef& d = defs[i];
                if (off > d.body_open && off < d.body_end)
                    return static_cast<int32_t>(i);
            }
            return -1;
        };

        /* globals: [qualifier] TYPE NAME followed by = ; or [ */
        for_each_line(code, [&](const Line& l) {
            const std::string_view t = ltrim(l.sv);
            if (inside_def(l.off) >= 0) return;
            if (t.empty() || t[0] == '#' || t[0] == '}') return;
            size_t p = 0;
            auto skip_ws = [&]() {
                while (p < t.size() && is_space(t[p])) ++p;
            };
            auto skip_word = [&]() {
                skip_ws();
                if (p >= t.size() || !is_ident_char(t[p])) return false;
                while (p < t.size() && is_ident_char(t[p])) ++p;
                return true;
            };
            if (!skip_word()) return;
            const std::string_view w0 = t.substr(0, p);
            if (w0 == "static" || w0 == "const" || w0 == "constexpr" ||
                w0 == "extern" || w0 == "volatile") {
                if (!skip_word()) return; /* TYPE */
            }
            const size_t name_start = p;
            if (!skip_word()) return; /* NAME */
            const size_t name_end = p;
            skip_ws();
            if (p >= t.size() ||
                (t[p] != '=' && t[p] != ';' && t[p] != '['))
                return;
            const std::string name(t.substr(name_start, name_end - name_start));
            const size_t tbase = l.off + (l.sv.size() - t.size());
            Sym s;
            s.kind = SymKind::global;
            s.name = s.qual = name;
            s.line = line_of(code, tbase + name_start);
            s.start = static_cast<uint32_t>(tbase + name_start);
            s.end = s.start + static_cast<uint32_t>(s.name.size());
            syms.push_back(std::move(s));
        });

        /* call sites -> call deps attributed to the enclosing def */
        for_each_line(code, [&](const Line& l) {
            for (size_t i = l.off; i < l.end; ++i) {
                if (code[i] != '(') continue;
                size_t q = i;
                while (q > l.off && is_ident_char(code[q - 1])) --q;
                if (q == i) continue;
                const std::string_view tok(code.data() + q, i - q);
                if (is_c_keyword(tok)) continue;
                bool is_def_name = false;
                for (const FuncDef& d : defs) {
                    if (d.name_off == q) {
                        is_def_name = true;
                        break;
                    }
                }
                if (is_def_name) continue;
                const int32_t enclosing = inside_def(q);
                Dep d;
                d.kind = DepKind::call;
                d.to_name = std::string(tok);
                d.to_file = file;
                d.from_file = file;
                if (enclosing >= 0)
                    d.from_sym = static_cast<int32_t>(syms_base + enclosing + 1);
                deps.push_back(std::move(d));
            }
        });

        return core::ok();
    }

    /* ---- go ---- */
    /* package + imports, from raw text (imports are string literals) */
    for_each_line(src, [&](const Line& l) {
        const std::string_view t = ltrim(l.sv);
        const size_t tbase = l.off + (l.sv.size() - t.size());
        if (t.starts_with("package")) {
            std::string_view rest = ltrim(t.substr(7));
            if (!rest.empty() && is_ident_char(rest[0])) {
                size_t e = 0;
                while (e < rest.size() && is_ident_char(rest[e])) ++e;
                Sym s;
                s.kind = SymKind::package;
                s.name = s.qual = std::string(rest.substr(0, e));
                const size_t name_off = tbase + t.size() - rest.size();
                s.line = line_of(src, name_off);
                s.start = static_cast<uint32_t>(name_off);
                s.end = s.start + static_cast<uint32_t>(s.name.size());
                syms.push_back(std::move(s));
            }
            return;
        }
        if (!t.starts_with("import")) return;
        auto push_imp = [&](std::string_view line) {
            line = ltrim(line);
            const size_t q = line.find('"');
            if (q == std::string_view::npos) return;
            const size_t close = line.find('"', q + 1);
            if (close == std::string_view::npos) return;
            Dep d;
            d.kind = DepKind::include;
            d.from_file = file;
            d.to_file = std::string(line.substr(q + 1, close - q - 1));
            d.to_name = d.to_file;
            deps.push_back(std::move(d));
        };
        std::string_view rest = ltrim(t.substr(6));
        if (rest.starts_with("(")) {
            for (size_t pos = l.end; pos <= src.size();) {
                const size_t nl = src.find('\n', pos);
                const size_t line_end = nl == std::string::npos ? src.size() : nl;
                const std::string_view itv(src.data() + pos, line_end - pos);
                if (ltrim(itv).starts_with(")")) break;
                push_imp(itv);
                if (nl == std::string::npos) break;
                pos = nl + 1;
            }
        } else {
            push_imp(rest);
        }
    });

    std::string code = src;
    mask_comments_and_strings(code);

    std::vector<FuncDef> defs;
    std::vector<Sym> extra; /* types, vars, consts */
    for_each_line(code, [&](const Line& l) {
        const std::string_view t = ltrim(l.sv);
        if (t.empty()) return;
        const size_t tbase = l.off + (l.sv.size() - t.size());

        if (t.starts_with("func") && (t.size() == 4 || is_space(t[4]))) {
            std::string_view rest = ltrim(t.substr(4));
            if (rest.starts_with("(")) {
                int depth = 0;
                size_t e = 0;
                for (; e < rest.size(); ++e) {
                    if (rest[e] == '(') ++depth;
                    else if (rest[e] == ')') {
                        --depth;
                        if (depth == 0) break;
                    }
                }
                if (e == rest.size()) return;
                rest = ltrim(rest.substr(e + 1));
            }
            if (rest.empty() || !is_ident_char(rest[0])) return;
            size_t ne = 0;
            while (ne < rest.size() && is_ident_char(rest[ne])) ++ne;
            const std::string name = std::string(rest.substr(0, ne));
            const size_t name_off = tbase + t.size() - rest.size();
            /* skip spaces and any generic bracket to reach the signature '(' */
            size_t sig = ne;
            while (sig < rest.size()) {
                if (is_space(rest[sig])) {
                    ++sig;
                    continue;
                }
                if (rest[sig] == '[') {
                    int depth = 0;
                    for (; sig < rest.size(); ++sig) {
                        if (rest[sig] == '[') ++depth;
                        else if (rest[sig] == ']') {
                            --depth;
                            if (depth == 0) {
                                ++sig;
                                break;
                            }
                        }
                    }
                    continue;
                }
                break;
            }
            if (sig >= rest.size() || rest[sig] != '(') return;
            /* matching ')' then '{' on the same line */
            int depth = 0;
            size_t j = sig;
            for (; j < rest.size(); ++j) {
                if (rest[j] == '(') ++depth;
                else if (rest[j] == ')') {
                    --depth;
                    if (depth == 0) break;
                }
            }
            if (j == rest.size()) return;
            const size_t open_rel = rest.find('{', j + 1);
            if (open_rel == std::string_view::npos) return;
            const size_t open = tbase + t.size() - rest.size() + open_rel;
            const size_t close = match_brace(code, open, '{', '}');
            if (close == std::string::npos) return;

            defs.push_back(FuncDef{name_off, l.off, open, close, name});
            return;
        }

        if (t.starts_with("type")) {
            std::string_view rest = ltrim(t.substr(4));
            if (!rest.empty() && is_ident_char(rest[0])) {
                size_t e = 0;
                while (e < rest.size() && is_ident_char(rest[e])) ++e;
                Sym s;
                s.kind = SymKind::type;
                const std::string name = std::string(rest.substr(0, e));
                s.name = s.qual = name;
                const size_t name_off = tbase + t.size() - rest.size();
                s.line = line_of(code, name_off);
                s.start = static_cast<uint32_t>(name_off);
                s.end = s.start + static_cast<uint32_t>(s.name.size());
                s.vis = (name[0] >= 'A' && name[0] <= 'Z') ? Visibility::public_
                                                          : Visibility::private_;
                extra.push_back(std::move(s));
            }
            return;
        }

        if (t.starts_with("var") || t.starts_with("const")) {
            std::string_view rest = ltrim(t.substr(t[0] == 'v' ? 3 : 5));
            if (!rest.empty() && is_ident_char(rest[0])) {
                size_t e = 0;
                while (e < rest.size() && is_ident_char(rest[e])) ++e;
                Sym s;
                s.kind = t[0] == 'c' ? SymKind::const_ : SymKind::global;
                const std::string name = std::string(rest.substr(0, e));
                s.name = s.qual = name;
                const size_t name_off = tbase + t.size() - rest.size();
                s.line = line_of(code, name_off);
                s.start = static_cast<uint32_t>(name_off);
                s.end = s.start + static_cast<uint32_t>(s.name.size());
                extra.push_back(std::move(s));
            }
        }
    });

    /* defs first, then the rest */
    const size_t syms_base = syms.size();
    for (const FuncDef& d : defs) {
        Sym s;
        s.kind = SymKind::function;
        s.name = s.qual = d.name;
        s.line = line_of(code, d.name_off);
        s.start = static_cast<uint32_t>(d.start);
        s.end = static_cast<uint32_t>(d.body_end);
        syms.push_back(std::move(s));
    }
    for (Sym& s : extra) syms.push_back(std::move(s));

    /* fix method/function + visibility kinds now that def indices are known */
    for (size_t di = 0; di < defs.size(); ++di) {
        const FuncDef& d = defs[di];
        Sym& s = syms[syms_base + di];
        size_t p = d.name_off;
        while (p > d.start && is_space(code[p - 1])) --p;
        const bool method = p > d.start && code[p - 1] == ')';
        s.kind = method ? SymKind::method : SymKind::function;
        const std::string& nm = d.name;
        s.vis = (!nm.empty() && nm[0] >= 'A' && nm[0] <= 'Z')
                    ? Visibility::public_
                    : Visibility::private_;
    }

    auto inside_def = [&](size_t off) -> int32_t {
        for (size_t i = 0; i < defs.size(); ++i) {
            const FuncDef& d = defs[i];
            if (off > d.body_open && off < d.body_end)
                return static_cast<int32_t>(i);
        }
        return -1;
    };

    /* call sites -> call deps attributed to the enclosing def */
    for_each_line(code, [&](const Line& l) {
        for (size_t i = l.off; i < l.end; ++i) {
            if (code[i] != '(') continue;
            size_t q = i;
            while (q > l.off && is_ident_char(code[q - 1])) --q;
            if (q == i) continue;
            const std::string_view tok(code.data() + q, i - q);
            if (is_go_keyword(tok)) continue;
            bool is_def_name = false;
            for (const FuncDef& d : defs) {
                if (d.name_off == q) {
                    is_def_name = true;
                    break;
                }
            }
            if (is_def_name) continue;
            const int32_t enclosing = inside_def(q);
            Dep d;
            d.kind = DepKind::call;
            d.to_name = std::string(tok);
            d.to_file = file;
            d.from_file = file;
            if (enclosing >= 0)
                d.from_sym = static_cast<int32_t>(syms_base + enclosing + 1);
            deps.push_back(std::move(d));
        }
    });

    return core::ok();
}

} /* namespace opencode::graph */

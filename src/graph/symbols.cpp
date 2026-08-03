/*
 * symbols.cpp -- per-language symbol + dependency extraction (Phase 7).
 *
 * The regex-fallback backend: dependency-free, deterministic, and good enough
 * for the symbols the context assembler and verification actually query
 * (function defs/decls, types, imports, call sites). A tree-sitter backend can
 * be added behind the same interface (extract_lang) when
 * OPENCODE_USE_TREE_SITTER is enabled; selection is compile-time.
 *
 * Strategy: the file is read once and a byte-for-byte "code mask" blanks out
 * comments and string/char literals (same length, newlines preserved), then
 * anchored regexes run on the mask so offsets map straight back to the source.
 * Imports/includes are pulled from the raw text (quotes would otherwise be
 * masked). Keyword false-positives ("if (x) {") are filtered by explicit sets.
 *
 * Dep.from_sym / Dep.to_sym are assigned by extract_into (index.cpp) after
 * SymIds are known. extract_lang sets from_sym = file-local sym index + 1 for
 * call deps whose enclosing def is known, 0 otherwise; extract_into offsets it
 * by the file's sym base.
 */
#include "graph/index.h"

#include <cstdint>
#include <regex>
#include <string>
#include <vector>

namespace opencode::graph {

namespace {

const char* const kCKeywords[] = {
    "if",     "else",   "while",    "for",     "do",    "switch",  "case",
    "return", "sizeof", "catch",    "throw",   "new",   "delete",  "goto",
    "const_cast", "static_cast", "dynamic_cast", "reinterpret_cast",
    "try",    "using",  "typedef",  "friend",  "operator",
};

const char* const kGoKeywords[] = {
    "if",     "for",     "range",  "switch", "select", "func",    "go",
    "defer",  "return",  "make",   "new",    "append", "len",     "cap",
    "copy",   "delete",  "recover", "panic",  "goto",   "case",   "fallthrough",
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

/* Anchored regex: ^ matches at every line start (needed for the per-line
 * "statement" extractors). */
std::regex anchored(const char* pat) {
    return std::regex(pat, std::regex::ECMAScript | std::regex::multiline);
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

struct FuncDef {
    size_t name_off = 0;   /* offset of the captured name */
    size_t body_open = 0;  /* offset of '{' */
    size_t body_end = 0;   /* just past the matching '}' */
    std::string name;
};

/* c/cpp function definitions: an identifier before `(...)` followed by a body
 * brace, with an optional return-type / qualifier prefix. Appends a Sym per
 * match and returns the region list for call attribution. */
std::vector<FuncDef> extract_functions(const std::string& code,
                                       std::vector<Sym>& syms) {
    std::vector<FuncDef> out;
    static const std::regex re(
        "^[ \t]*(?:template[ \t]*<[^>]*>[ \t]*)?"
        "(?:[A-Za-z_][A-Za-z0-9_]*(?:<[^>]*>)?(?:::[A-Za-z_][A-Za-z0-9_]*)*"
        "[ \t]+)*(?:[*/&]+[ \t]*)*"
        "([A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*)"
        "[ \t]*\\([^;{}]*\\)"
        "[ \t\r\n]*(?:(?:const|noexcept|override|final)[ \t\r\n]*)*"
        "(?:->[^{};]*)?"
        "(?::[^{};]*)?"
        "[ \t\r\n]*\\{",
        std::regex::ECMAScript | std::regex::multiline);
    for (std::sregex_iterator it(code.begin(), code.end(), re), end; it != end;
         ++it) {
        const std::smatch& m = *it;
        const std::string name = m[1].str();
        if (is_c_keyword(name)) continue;
        const size_t name_off = static_cast<size_t>(m.position(1));
        const size_t brace = static_cast<size_t>(m.position()) +
                             static_cast<size_t>(m.length());
        /* the match ends exactly at '{', so it IS the body-open brace */
        const size_t open = brace - 1;
        const size_t close = match_brace(code, open, '{', '}');
        if (close == std::string::npos) continue;
        out.push_back(FuncDef{name_off, open, close, name});
        Sym s;
        s.kind = SymKind::function;
        s.name = name;
        s.qual = name;
        s.line = line_of(code, name_off);
        s.start = static_cast<uint32_t>(m.position());
        s.end = static_cast<uint32_t>(close);
        syms.push_back(std::move(s));
    }
    return out;
}

} /* namespace */

core::error_code extract_lang(Lang lang, const std::string& file,
                              const std::string& src, std::vector<Sym>& syms,
                              std::vector<Dep>& deps) noexcept {
    try {
        if (lang == Lang::c || lang == Lang::cpp) {
            /* includes, from raw text (quote form would be masked) */
            static const std::regex re_inc = anchored(
                "^[ \t]*#[ \t]*include[ \t]*[<\"]([^>\"]+)[>\"]");
            for (std::sregex_iterator it(src.begin(), src.end(), re_inc), end;
                 it != end; ++it) {
                Dep d;
                d.kind = DepKind::include;
                d.from_file = file;
                d.to_file = (*it)[1].str();
                d.to_name = (*it)[1].str();
                deps.push_back(std::move(d));
            }

            std::string code = src;
            mask_comments_and_strings(code);

            static const std::regex re_def = anchored(
                "^[ \t]*#[ \t]*define[ \t]+([A-Za-z_][A-Za-z0-9_]*)");
            for (std::sregex_iterator it(code.begin(), code.end(), re_def), end;
                 it != end; ++it) {
                Sym s;
                s.kind = SymKind::macro;
                s.name = s.qual = (*it)[1].str();
                s.line = line_of(code, static_cast<size_t>((*it).position(1)));
                s.start = static_cast<uint32_t>((*it).position(1));
                s.end = s.start + static_cast<uint32_t>(s.name.size());
                syms.push_back(std::move(s));
            }

            static const std::regex re_type = anchored(
                "^[ \t]*(?:typedef[ \t]+)?"
                "(struct|class|enum|union)[ \t]+([A-Za-z_][A-Za-z0-9_]*)");
            for (std::sregex_iterator it(code.begin(), code.end(), re_type),
                 end;
                 it != end; ++it) {
                const std::string kind = (*it)[1].str();
                Sym s;
                s.kind = kind == "struct" ? SymKind::struct_
                         : kind == "class" ? SymKind::class_
                         : kind == "enum"  ? SymKind::enum_
                                           : SymKind::type;
                s.name = s.qual = (*it)[2].str();
                s.line = line_of(code, static_cast<size_t>((*it).position(2)));
                s.start = static_cast<uint32_t>((*it).position(2));
                s.end = s.start + static_cast<uint32_t>(s.name.size());
                syms.push_back(std::move(s));
            }

            static const std::regex re_ns = anchored(
                "^[ \t]*namespace[ \t]+([A-Za-z_][A-Za-z0-9_]*)");
            for (std::sregex_iterator it(code.begin(), code.end(), re_ns), end;
                 it != end; ++it) {
                Sym s;
                s.kind = SymKind::namespace_;
                s.name = s.qual = (*it)[1].str();
                s.line = line_of(code, static_cast<size_t>((*it).position(1)));
                s.start = static_cast<uint32_t>((*it).position(1));
                s.end = s.start + static_cast<uint32_t>(s.name.size());
                syms.push_back(std::move(s));
            }

            /* functions (defs) -- record the base for call attribution */
            const size_t syms_base = syms.size();
            std::vector<FuncDef> defs = extract_functions(code, syms);

            /* top-level globals: a declaration NOT inside any def body */
            static const std::regex re_global = anchored(
                "^[ \t]*(?:static|const|constexpr|extern|volatile)?[ \t]*"
                "([A-Za-z_][A-Za-z0-9_]*)[ \t]+"
                "([A-Za-z_][A-Za-z0-9_]*)[ \t]*(=|;|\\[)");
            for (std::sregex_iterator it(code.begin(), code.end(), re_global),
                 end;
                 it != end; ++it) {
                const size_t off = static_cast<size_t>((*it).position());
                bool inside = false;
                for (const FuncDef& d : defs) {
                    if (off > d.body_open && off < d.body_end) {
                        inside = true;
                        break;
                    }
                }
                if (inside) continue;
                Sym s;
                s.kind = SymKind::global;
                s.name = s.qual = (*it)[2].str();
                s.line = line_of(code, static_cast<size_t>((*it).position(2)));
                s.start = static_cast<uint32_t>((*it).position(2));
                s.end = s.start + static_cast<uint32_t>(s.name.size());
                syms.push_back(std::move(s));
            }

            /* call sites -> call deps attributed to the enclosing def */
            static const std::regex re_call = anchored(
                "\\b([A-Za-z_][A-Za-z0-9_]*)[ \t]*\\(");
            for (std::sregex_iterator it(code.begin(), code.end(), re_call),
                 end;
                 it != end; ++it) {
                const std::string callee = (*it)[1].str();
                if (is_c_keyword(callee)) continue;
                const size_t name_off = static_cast<size_t>((*it).position(1));
                bool is_def = false;
                for (const FuncDef& d : defs) {
                    if (d.name_off == name_off) {
                        is_def = true;
                        break;
                    }
                }
                if (is_def) continue;
                int32_t enclosing = -1;
                for (size_t di = 0; di < defs.size(); ++di) {
                    const FuncDef& d = defs[di];
                    if (name_off > d.body_open && name_off < d.body_end) {
                        enclosing = static_cast<int32_t>(di);
                        break;
                    }
                }
                Dep d;
                d.kind = DepKind::call;
                d.to_name = callee;
                d.to_file = file;
                d.from_file = file;
                if (enclosing >= 0)
                    d.from_sym =
                        static_cast<int32_t>(syms_base + enclosing + 1);
                deps.push_back(std::move(d));
            }
            return core::ok();
        }

        if (lang == Lang::go) {
            /* package + imports, from raw text (imports are string literals) */
            static const std::regex re_pkg = anchored(
                "^package[ \t]+([A-Za-z_][A-Za-z0-9_]*)");
            for (std::sregex_iterator it(src.begin(), src.end(), re_pkg), end;
                 it != end; ++it) {
                Sym s;
                s.kind = SymKind::package;
                s.name = s.qual = (*it)[1].str();
                s.line = line_of(src, static_cast<size_t>((*it).position(1)));
                s.start = static_cast<uint32_t>((*it).position(1));
                s.end = s.start + static_cast<uint32_t>(s.name.size());
                syms.push_back(std::move(s));
            }
            static const std::regex re_imp = anchored(
                "^import[ \t]+(?:[A-Za-z_][A-Za-z0-9_]*[ \t]+)?\"([^\"]+)\"");
            for (std::sregex_iterator it(src.begin(), src.end(), re_imp), end;
                 it != end; ++it) {
                Dep d;
                d.kind = DepKind::include;
                d.from_file = file;
                d.to_file = (*it)[1].str();
                d.to_name = (*it)[1].str();
                deps.push_back(std::move(d));
            }
            static const std::regex re_imp_block = anchored("^import[ \t]*\\(");
            for (std::sregex_iterator it(src.begin(), src.end(), re_imp_block),
                 end;
                 it != end; ++it) {
                const size_t open = static_cast<size_t>((*it).position());
                const size_t close = src.find(')', open);
                if (close == std::string::npos) continue;
                static const std::regex re_q("\"([^\"]+)\"");
                const std::string block = src.substr(open, close - open);
                for (std::sregex_iterator q(block.begin(), block.end(), re_q),
                     qend;
                     q != qend; ++q) {
                    Dep d;
                    d.kind = DepKind::include;
                    d.from_file = file;
                    d.to_file = (*q)[1].str();
                    d.to_name = (*q)[1].str();
                    deps.push_back(std::move(d));
                }
            }

            std::string code = src;
            mask_comments_and_strings(code);

            const size_t syms_base = syms.size();
            std::vector<FuncDef> defs;
            /* funcs and methods (receiver form); generic args optional */
            static const std::regex re_fn = anchored(
                "^func[ \t]+(?:[ \t]*\\([^)]*\\)[ \t]*)?"
                "([A-Za-z_][A-Za-z0-9_]*)[ \t]*(?:\\[[^\\]]*\\][ \t]*)?\\(");
            for (std::sregex_iterator it(code.begin(), code.end(), re_fn), end;
                 it != end; ++it) {
                const std::string name = (*it)[1].str();
                const size_t name_off = static_cast<size_t>((*it).position(1));
                const size_t open = code.find('{', name_off);
                if (open == std::string::npos) continue;
                const size_t close = match_brace(code, open, '{', '}');
                if (close == std::string::npos) continue;
                size_t p = name_off;
                while (p > 0 && code[p - 1] == ' ') --p;
                const bool method = p > 0 && code[p - 1] == ')';
                defs.push_back(FuncDef{name_off, open, close, name});
                Sym s;
                s.kind = method ? SymKind::method : SymKind::function;
                s.name = s.qual = name;
                s.line = line_of(code, name_off);
                s.start = static_cast<uint32_t>(name_off);
                s.end = static_cast<uint32_t>(close);
                s.vis = (name[0] >= 'A' && name[0] <= 'Z')
                            ? Visibility::public_
                            : Visibility::private_;
                syms.push_back(std::move(s));
            }

            static const std::regex re_type = anchored(
                "^type[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]+"
                "(?:struct|interface|func|[A-Za-z_])");
            for (std::sregex_iterator it(code.begin(), code.end(), re_type),
                 end;
                 it != end; ++it) {
                const std::string name = (*it)[1].str();
                Sym s;
                s.kind = SymKind::type;
                s.name = s.qual = name;
                s.line = line_of(code, static_cast<size_t>((*it).position(1)));
                s.start = static_cast<uint32_t>((*it).position(1));
                s.end = s.start + static_cast<uint32_t>(s.name.size());
                s.vis = (name[0] >= 'A' && name[0] <= 'Z') ? Visibility::public_
                                                          : Visibility::private_;
                syms.push_back(std::move(s));
            }

            static const std::regex re_global = anchored(
                "^(var|const)[ \t]+([A-Za-z_][A-Za-z0-9_]*)");
            for (std::sregex_iterator it(code.begin(), code.end(), re_global),
                 end;
                 it != end; ++it) {
                Sym s;
                s.kind = (*it)[1].str() == "const" ? SymKind::const_
                                                   : SymKind::global;
                s.name = s.qual = (*it)[2].str();
                s.line = line_of(code, static_cast<size_t>((*it).position(2)));
                s.start = static_cast<uint32_t>((*it).position(2));
                s.end = s.start + static_cast<uint32_t>(s.name.size());
                syms.push_back(std::move(s));
            }

            static const std::regex re_call = anchored(
                "\\b([A-Za-z_][A-Za-z0-9_]*)[ \t]*\\(");
            for (std::sregex_iterator it(code.begin(), code.end(), re_call),
                 end;
                 it != end; ++it) {
                const std::string callee = (*it)[1].str();
                if (is_go_keyword(callee)) continue;
                const size_t name_off = static_cast<size_t>((*it).position(1));
                bool is_def = false;
                for (const FuncDef& d : defs) {
                    if (d.name_off == name_off) {
                        is_def = true;
                        break;
                    }
                }
                if (is_def) continue;
                int32_t enclosing = -1;
                for (size_t di = 0; di < defs.size(); ++di) {
                    const FuncDef& d = defs[di];
                    if (name_off > d.body_open && name_off < d.body_end) {
                        enclosing = static_cast<int32_t>(di);
                        break;
                    }
                }
                Dep d;
                d.kind = DepKind::call;
                d.to_name = callee;
                d.to_file = file;
                d.from_file = file;
                if (enclosing >= 0)
                    d.from_sym =
                        static_cast<int32_t>(syms_base + enclosing + 1);
                deps.push_back(std::move(d));
            }
            return core::ok();
        }
    } catch (const std::exception&) {
        /* regex / substr can throw on pathological input; never propagate */
        return core::make_error_code(core::Err::e_proto_parse);
    }

    return core::make_error_code(core::Err::e_not_impl);
}

} /* namespace opencode::graph */

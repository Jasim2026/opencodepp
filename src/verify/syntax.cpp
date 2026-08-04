/*
 * syntax.cpp -- the regex-fallback syntax checker (Phase 9).
 *
 * This is the non-tree-sitter backend: a single-pass scanner that checks
 * brace/paren/bracket balance while respecting string literals and line
 * comments. It also catches known-error patterns (double semicolons,
 * unmatched braces, unterminated strings).
 *
 * The target is >= 90% recall on the golden fixtures -- good enough to catch
 * the mechanical errors that the model produces most often (missing closing
 * braces, unbalanced parens in function signatures). Tree-sitter will
 * improve precision when enabled.
 */
#include "verify/gate.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace opencode::verify {

namespace {

enum class CharClass : uint8_t {
    normal = 0,
    line_comment = 1,   /* after '//' to end of line               */
    block_comment = 2,  /* inside block comment                       */
    string_literal = 3, /* inside "..."                            */
    char_literal = 4,   /* inside '...'                            */
};

struct Scanner {
    std::string_view content;
    std::string_view file;
    std::vector<SyntaxIssue> issues;
    std::uint32_t line = 1;
    std::uint32_t col = 1;
    CharClass state = CharClass::normal;

    /* Balanced-delimiter stack: each entry is the char + position. */
    struct Frame {
        char ch;
        std::uint32_t line;
        std::uint32_t col;
    };
    std::vector<Frame> stack;

    void add_issue(std::uint32_t l, std::uint32_t c, const std::string& msg) {
        issues.push_back({l, c, msg});
    }

    bool is_balanced_close(char open, char close) {
        return (open == '(' && close == ')') ||
               (open == '[' && close == ']') ||
               (open == '{' && close == '}');
    }

    std::string escape_char(char c) {
        if (c == '\n') return "\\n";
        if (c == '\t') return "\\t";
        if (c == '\\') return "\\\\";
        return std::string(1, c);
    }

    void scan() {
        std::size_t i = 0;
        bool prev_was_backslash = false;

        while (i < content.size()) {
            char c = content[i];

            if (state == CharClass::line_comment) {
                if (c == '\n') {
                    state = CharClass::normal;
                    ++line; col = 1;
                } else {
                    ++col;
                }
                ++i;
                continue;
            }
            if (state == CharClass::block_comment) {
                if (c == '*' && i + 1 < content.size() && content[i + 1] == '/') {
                    state = CharClass::normal;
                    ++i; ++col; /* skip '*' */
                    ++i; ++col; /* skip '/' */
                    continue;
                }
                if (c == '\n') { ++line; col = 1; }
                else ++col;
                ++i;
                continue;
            }
            if (state == CharClass::string_literal) {
                if (prev_was_backslash) {
                    prev_was_backslash = false;
                    ++i; ++col;
                    continue;
                }
                if (c == '\\') {
                    prev_was_backslash = true;
                    ++i; ++col;
                    continue;
                }
                if (c == '"') {
                    state = CharClass::normal;
                    ++i; ++col;
                    continue;
                }
                if (c == '\n') {
                    add_issue(line, col, "unterminated string literal");
                    state = CharClass::normal;
                    ++line; col = 1;
                } else {
                    ++col;
                }
                ++i;
                continue;
            }
            if (state == CharClass::char_literal) {
                if (prev_was_backslash) {
                    prev_was_backslash = false;
                    ++i; ++col;
                    continue;
                }
                if (c == '\\') {
                    prev_was_backslash = true;
                    ++i; ++col;
                    continue;
                }
                if (c == '\'') {
                    state = CharClass::normal;
                    ++i; ++col;
                    continue;
                }
                if (c == '\n') {
                    add_issue(line, col, "unterminated char literal");
                    state = CharClass::normal;
                    ++line; col = 1;
                } else {
                    ++col;
                }
                ++i;
                continue;
            }

            /* Normal state. */
            if (c == '/' && i + 1 < content.size()) {
                if (content[i + 1] == '/') {
                    state = CharClass::line_comment;
                    ++i; ++col; /* skip first '/' */
                    ++i; ++col; /* skip second '/' */
                    continue;
                }
                if (content[i + 1] == '*') {
                    state = CharClass::block_comment;
                    ++i; ++col; /* skip '/' */
                    ++i; ++col; /* skip '*' */
                    continue;
                }
            }
            if (c == '"') {
                state = CharClass::string_literal;
                ++i; ++col;
                continue;
            }
            if (c == '\'') {
                state = CharClass::char_literal;
                ++i; ++col;
                continue;
            }

            /* Delimiter tracking. */
            if (c == '(' || c == '[' || c == '{') {
                stack.push_back({c, line, col});
                ++i; ++col;
                continue;
            }
            if (c == ')' || c == ']' || c == '}') {
                if (stack.empty()) {
                    add_issue(line, col,
                        std::string("unexpected '") + c + "' without opening");
                } else {
                    auto& top = stack.back();
                    if (!is_balanced_close(top.ch, c)) {
                        add_issue(top.line, top.col,
                            std::string("unmatched '") + top.ch +
                            "' closed by '" + c + "' at " +
                            std::to_string(line) + ":" + std::to_string(col));
                    }
                    stack.pop_back();
                }
                ++i; ++col;
                continue;
            }

            /* Known-error patterns: ;; only when NOT preceded by '('. The
             * pattern for(;;) is valid and common. */
            if (c == ';' && i + 1 < content.size() && content[i + 1] == ';') {
                /* Look back to see if the preceding non-space char is '('. */
                bool in_for = false;
                for (size_t j = stack.size(); j > 0; ) {
                    --j;
                    if (stack[j].ch == '(') { in_for = true; break; }
                    if (stack[j].ch == '{' || stack[j].ch == '[') break;
                }
                /* Also check the character before this ';' in the source. */
                if (!in_for && i > 0 && content[i - 1] != '(') {
                    add_issue(line, col,
                        "double semicolon (;;) outside for-loop header");
                }
                ++i; ++col;
                continue;
            }

            if (c == '\n') { ++line; col = 1; } else { ++col; }
            ++i;
        }

        /* Post-scan checks. */
        if (state == CharClass::block_comment) {
            add_issue(line, col, "unterminated block comment");
        }
        if (state == CharClass::string_literal) {
            add_issue(line, col, "unterminated string literal");
        }
        if (state == CharClass::char_literal) {
            add_issue(line, col, "unterminated char literal");
        }

        for (const auto& f : stack) {
            add_issue(f.line, f.col,
                std::string("unterminated '") + f.ch + "' -- missing closing");
        }
    }
};

} /* namespace */

std::vector<SyntaxIssue> check_syntax(std::string_view content,
                                       std::string_view file) {
    Scanner sc;
    sc.content = content;
    sc.file = file;
    sc.scan();
    return sc.issues;
}

} /* namespace opencode::verify */

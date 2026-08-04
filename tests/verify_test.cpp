/*
 * verify_test.cpp -- verification gate tests (Phase 9, commit 1: syntax).
 *
 * Local compile: g++ -std=c++20 -I src -I include -I . -Wall -Wextra -Werror
 *   tests/verify_test.cpp src/verify/gate.cpp src/verify/syntax.cpp -o /tmp/verify_test && /tmp/verify_test
 */
#include "verify/gate.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.h"

using namespace opencode::verify;
using namespace opencode::core;

static int passed = 0;
static int failed = 0;

#define CHECK(expr) do { \
    if (expr) { ++passed; } \
    else { ++failed; std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); } \
} while(0)

/* ---- syntax stage tests ---- */

void test_balanced_braces() {
    auto issues = check_syntax("int main() { return 0; }", "test.cpp");
    CHECK(issues.empty());
}

void test_unbalanced_open_brace() {
    auto issues = check_syntax("int main() { return 0;", "test.cpp");
    CHECK(!issues.empty());
    CHECK(issues[0].msg.find("unterminated") != std::string::npos);
}

void test_unbalanced_close_brace() {
    auto issues = check_syntax("return 0; }", "test.cpp");
    CHECK(!issues.empty());
    CHECK(issues[0].msg.find("unexpected") != std::string::npos);
}

void test_string_braces_ignored() {
    auto issues = check_syntax(R"(char* s = "{[()]}";)", "test.cpp");
    CHECK(issues.empty());
}

void test_comment_braces_ignored() {
    auto issues = check_syntax("// { [ ( ) ] }", "test.cpp");
    CHECK(issues.empty());
}

void test_block_comment_braces_ignored() {
    auto issues = check_syntax("/* { [ ( ) ] } */", "test.cpp");
    CHECK(issues.empty());
}

void test_unterminated_string() {
    auto issues = check_syntax("char* s = \"hello;", "test.cpp");
    CHECK(!issues.empty());
    CHECK(issues[0].msg.find("unterminated string") != std::string::npos);
}

void test_double_semicolon() {
    auto issues = check_syntax("int x = 5;;\n", "test.cpp");
    CHECK(!issues.empty());
    CHECK(issues[0].msg.find("double semicolon") != std::string::npos);
}

void test_nested_braces() {
    auto issues = check_syntax(R"(
int f() {
    if (x) {
        for (;;) {
        }
    }
}
)", "test.cpp");
    CHECK(issues.empty());
}

void test_mismatched_parens() {
    auto issues = check_syntax("int f(int x] { return 0; }", "test.cpp");
    CHECK(!issues.empty());
}

void test_unterminated_block_comment() {
    auto issues = check_syntax("/* unterminated", "test.cpp");
    CHECK(!issues.empty());
    CHECK(issues[0].msg.find("unterminated block comment") != std::string::npos);
}

/* ---- gate orchestrator tests ---- */

void test_gate_syntax_pass() {
    Gate g;
    Context ctx;
    EditProposal p;
    p.after_content = "int main() { return 0; }";
    auto r = g.run(Stage::syntax, p, ctx);
    CHECK(r.pass);
}

void test_gate_syntax_fail() {
    Gate g;
    Context ctx;
    EditProposal p;
    p.after_content = "int main() { return 0;";
    auto r = g.run(Stage::syntax, p, ctx);
    CHECK(!r.pass);
    CHECK(r.stage == Stage::syntax);
    CHECK(r.err == Err::e_verify_fail);
}

void test_gate_run_all_stops_on_syntax() {
    Gate g;
    Context ctx;
    EditProposal p;
    p.after_content = "int main() {";
    auto results = g.run_all(p, ctx);
    CHECK(results.size() == 1);
    CHECK(!results[0].pass);
    CHECK(results[0].stage == Stage::syntax);
}

void test_gate_run_all_passes_clean() {
    Gate g;
    Context ctx;
    EditProposal p;
    p.before_content = "int main() { return 0; }";
    p.after_content = "int main() { return 1; }";
    auto results = g.run_all(p, ctx);
    /* Should pass syntax, symbols (no graph), impact (no graph), diff (no patch),
     * testmap (always pass) = 5 stages. */
    CHECK(results.size() == 5);
    bool all_pass = true;
    for (const auto& r : results)
        if (!r.pass) all_pass = false;
    CHECK(all_pass);
}

void test_gate_run_syntax_direct() {
    Gate g;
    auto r = g.run_syntax("void f() {", "test.cpp");
    CHECK(!r.pass);
    CHECK(r.file == "test.cpp");
}

void test_gate_diff_no_change() {
    Gate g;
    Context ctx;
    EditProposal p;
    p.tool_name = "file.write";
    p.before_content = "hello";
    p.after_content = "hello";
    auto r = g.run(Stage::diff, p, ctx);
    CHECK(!r.pass);
    CHECK(r.detail.find("no change") != std::string::npos);
}

void test_gate_diff_ok() {
    Gate g;
    Context ctx;
    EditProposal p;
    p.tool_name = "file.write";
    p.before_content = "hello";
    p.after_content = "hello world";
    auto r = g.run(Stage::diff, p, ctx);
    CHECK(r.pass);
}

int main() {
    std::printf("verify_test: running...\n");

    test_balanced_braces();
    test_unbalanced_open_brace();
    test_unbalanced_close_brace();
    test_string_braces_ignored();
    test_comment_braces_ignored();
    test_block_comment_braces_ignored();
    test_unterminated_string();
    test_double_semicolon();
    test_nested_braces();
    test_mismatched_parens();
    test_unterminated_block_comment();

    test_gate_syntax_pass();
    test_gate_syntax_fail();
    test_gate_run_all_stops_on_syntax();
    test_gate_run_all_passes_clean();
    test_gate_run_syntax_direct();
    test_gate_diff_no_change();
    test_gate_diff_ok();

    std::printf("verify_test: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}

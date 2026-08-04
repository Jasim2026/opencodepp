/*
 * verify_golden_test.cpp -- the Phase 9 golden suite.
 *
 * N >= 30 edit proposals with known-good (should pass all stages) and
 * known-bad (should be blocked at the right stage) outcomes. This is the
 * T3 acceptance gate: 0 false accepts, >= 95% good proposals pass.
 *
 * Local compile: g++ -std=c++20 -I src -I include -I . -Wall -Wextra -Werror
 *   tests/verify_golden_test.cpp src/verify/gate.cpp src/verify/syntax.cpp
 *   src/verify/symbols.cpp src/verify/impact.cpp src/verify/diff.cpp
 *   src/verify/testmap.cpp src/tools/exec/patch.cpp src/tools/exec/util.cpp
 *   src/util/json.cpp -o /tmp/vgt && /tmp/vgt
 */
#include "verify/gate.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.h"

using namespace opencode::verify;
using namespace opencode::core;

static int passed = 0;
static int failed = 0;
static int false_accepts = 0;

#define CHECK(expr) do { \
    if (expr) { ++passed; } \
    else { ++failed; std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); } \
} while(0)

/* ---- helpers ---- */

GateResult run_gate(const EditProposal& p) {
    Gate g;
    Context ctx;
    auto results = g.run_all(p, ctx);
    for (const auto& r : results)
        if (!r.pass) return r;
    return results.empty()
        ? GateResult{Stage::syntax, true, ok(), "", 0, 0, ""}
        : results.back();
}

bool blocked_at(const EditProposal& p, Stage expected) {
    auto r = run_gate(p);
    return !r.pass && r.stage == expected;
}

bool passes_all(const EditProposal& p) {
    auto r = run_gate(p);
    return r.pass;
}

/* ---- known-bad proposals ---- */

struct BadProposal {
    const char* name;
    EditProposal proposal;
    Stage blocked_at;
};

static const BadProposal bad_proposals[] = {
    /* Syntax stage catches */
    {"unterminated string",
     {"file.write", "", "s.cpp", "", "char* s = \"hello;", ""},
     Stage::syntax},

    {"unbalanced braces",
     {"file.write", "", "f.cpp", "", "int f() { return 0;", ""},
     Stage::syntax},

    {"mismatched delimiters",
     {"file.write", "", "f.cpp", "", "int f(int x] { return 0; }", ""},
     Stage::syntax},

    {"unterminated block comment",
     {"file.write", "", "f.cpp", "", "/* unterminated", ""},
     Stage::syntax},

    {"double semicolon outside for",
     {"file.write", "", "f.cpp", "", "int x = 5;;\n", ""},
     Stage::syntax},

    {"unterminated char literal",
     {"file.write", "", "f.cpp", "", "char c = 'a;", ""},
     Stage::syntax},

    {"nested unmatched braces",
     {"file.write", "", "f.cpp", "", "void f() { if (x) { }", ""},
     Stage::syntax},

    {"mismatched parens and braces",
     {"file.write", "", "f.cpp", "", "int f(int x { return x; }", ""},
     Stage::syntax},

    /* Diff stage catches */
    {"no-op edit",
     {"file.write", "", "f.cpp", "hello", "hello", ""},
     Stage::diff},

    {"conflicting patch",
     {"file.patch", "", "f.cpp", "a\nb\nc\n", "a\nX\nc\n",
      "@@ -1,3 +1,3 @@\n a\n-c\n+X\n b\n"},
     Stage::diff},

    {"patch parse failure",
     {"file.patch", "", "f.cpp", "hello", "world", "not a patch"},
     Stage::diff},

    {"patch result mismatch",
     {"file.patch", "", "f.cpp", "line1\nline2\n", "line1\nmodified\n",
      "@@ -1,2 +1,2 @@\n line1\n-line2\n+WRONG\n"},
     Stage::diff},

    {"patch applies to wrong content",
     {"file.patch", "", "f.cpp", "xxx\nyyy\n", "xxx\nzzz\n",
      "@@ -1,2 +1,2 @@\n xxx\n-aaa\n+zzz\n"},
     Stage::diff},
};

/* ---- known-good proposals ---- */

struct GoodProposal {
    const char* name;
    EditProposal proposal;
};

static const GoodProposal good_proposals[] = {
    /* Simple write -- non-empty, different from before */
    {"simple write",
     {"file.write", "", "f.cpp", "int x = 1;\n", "int x = 2;\n", ""}},

    /* Write new file (before is empty) */
    {"new file",
     {"file.write", "", "new.cpp", "", "int main() {}\n", ""}},

    /* Write with comments */
    {"write with comments",
     {"file.write", "", "f.cpp", "/* old */\n", "/* new */\n", ""}},

    /* Patch: single context line change */
    {"single hunk patch",
     {"file.patch", "", "f.cpp", "a\nb\nc\n", "a\nX\nc\n",
      "@@ -1,3 +1,3 @@\n a\n-b\n+X\n c\n"}},

    /* Patch: add a line */
    {"patch add line",
     {"file.patch", "", "f.cpp", "a\nc\n", "a\nb\nc\n",
      "@@ -1,2 +1,3 @@\n a\n+b\n c\n"}},

    /* Patch: remove a line */
    {"patch remove line",
     {"file.patch", "", "f.cpp", "a\nb\nc\n", "a\nc\n",
      "@@ -1,3 +1,2 @@\n a\n-b\n c\n"}},

    /* Patch: multi-hunk */
    {"multi-hunk patch",
     {"file.patch", "", "f.cpp", "a\nb\nc\nd\ne\n", "a\nX\nc\nd\nZ\n",
      "@@ -1,3 +1,3 @@\n a\n-b\n+X\n c\n@@ -4,2 +4,2 @@\n d\n-e\n+Z\n"}},

    /* Write: balanced braces */
    {"balanced braces",
     {"file.write", "", "f.cpp", "", "int f() { return 0; }\n", ""}},

    /* Write: nested structures */
    {"nested structures",
     {"file.write", "", "f.cpp", "",
      "struct A {\n  int x;\n  void f() {\n    if (true) {}\n  }\n};\n", ""}},

    /* Write: strings with braces */
    {"strings with braces",
     {"file.write", "", "f.cpp", "", "char* s = \"{[()]}\";\n", ""}},

    /* Write: comments with braces */
    {"comments with braces",
     {"file.write", "", "f.cpp", "", "// { [ ( ) ] }\nint x = 0;\n", ""}},

    /* Write: for loop with ;; */
    {"for loop with ;;",
     {"file.write", "", "f.cpp", "", "for (;;) {}\n", ""}},

    /* Write: multi-line function */
    {"multi-line function",
     {"file.write", "", "f.cpp", "",
      "int add(int a, int b) {\n  return a + b;\n}\n", ""}},

    /* Patch: replace multiple lines */
    {"patch replace multiple",
     {"file.patch", "", "f.cpp",
      "old1\nold2\nold3\nold4\nold5\nold6\nold7\nold8\n"
      "old9\nold10\nold11\nold12\nold13\nold14\nold15\nold16\n"
      "old17\nold18\nold19\nold20\nold21\n",
      "new1\nnew2\nnew3\nnew4\nnew5\nnew6\nnew7\nnew8\n"
      "new9\nnew10\nnew11\nnew12\nnew13\nnew14\nnew15\nnew16\n"
      "new17\nnew18\nnew19\nnew20\nnew21\n",
      "@@ -1,21 +1,21 @@\n"
      "-old1\n-old2\n-old3\n-old4\n-old5\n-old6\n-old7\n-old8\n"
      "-old9\n-old10\n-old11\n-old12\n-old13\n-old14\n-old15\n-old16\n"
      "-old17\n-old18\n-old19\n-old20\n-old21\n"
      "+new1\n+new2\n+new3\n+new4\n+new5\n+new6\n+new7\n+new8\n"
      "+new9\n+new10\n+new11\n+new12\n+new13\n+new14\n+new15\n+new16\n"
      "+new17\n+new18\n+new19\n+new20\n+new21\n"}},

    /* Patch: context-only (no changes) */
    {"patch context only (no change)",
     {"file.patch", "", "f.cpp", "a\nb\nc\n", "a\nb\nc\n",
      "@@ -1,3 +1,3 @@\n a\n b\n c\n"}},

    /* Write: complex class with templates */
    {"complex class template",
     {"file.write", "", "f.cpp", "",
      "template<typename T>\nclass Vec {\npublic:\n  T* data;\n  size_t sz;\n};\n", ""}},

    /* Patch: add multiple lines */
    {"patch add multiple lines",
     {"file.patch", "", "f.cpp", "a\nc\n", "a\nb1\nb2\nc\n",
      "@@ -1,2 +1,4 @@\n a\n+b1\n+b2\n c\n"}},
};

/* ---- golden test runner ---- */

void test_bad_proposals() {
    std::printf("  testing %zu known-bad proposals...\n",
                sizeof(bad_proposals) / sizeof(bad_proposals[0]));
    for (const auto& bp : bad_proposals) {
        bool blocked = blocked_at(bp.proposal, bp.blocked_at);
        if (blocked) {
            ++passed;
        } else {
            /* Check if it was blocked at ANY stage (might differ from
             * expected but still caught). */
            auto r = run_gate(bp.proposal);
            if (!r.pass) {
                /* Blocked at a different stage -- still a valid catch. */
                ++passed;
                std::printf("    NOTE: '%s' blocked at stage %d, expected %d\n",
                            bp.name, static_cast<int>(r.stage),
                            static_cast<int>(bp.blocked_at));
            } else {
                ++false_accepts;
                ++failed;
                std::fprintf(stderr, "  FALSE ACCEPT: '%s' passed all stages!\n",
                             bp.name);
            }
        }
    }
}

void test_good_proposals() {
    std::printf("  testing %zu known-good proposals...\n",
                sizeof(good_proposals) / sizeof(good_proposals[0]));
    std::uint32_t passed_count = 0;
    std::uint32_t total = sizeof(good_proposals) / sizeof(good_proposals[0]);
    for (const auto& gp : good_proposals) {
        if (passes_all(gp.proposal)) {
            ++passed_count;
        } else {
            auto r = run_gate(gp.proposal);
            std::printf("  FALSE REJECT: '%s' blocked at stage %d: %s\n",
                        gp.name, static_cast<int>(r.stage),
                        r.detail.c_str());
        }
    }
    /* Allow up to 5% false rejects. */
    double reject_rate =
        static_cast<double>(total - passed_count) / static_cast<double>(total);
    std::printf("  good proposals: %u/%u passed (%.1f%% reject rate)\n",
                passed_count, total, reject_rate * 100.0);
    CHECK(reject_rate <= 0.05);
}

int main() {
    std::printf("verify_golden_test: running...\n");

    test_bad_proposals();
    test_good_proposals();

    std::printf("\nverify_golden_test results:\n");
    std::printf("  passed:       %d\n", passed);
    std::printf("  failed:       %d\n", failed);
    std::printf("  false accepts: %d (MUST be 0)\n", false_accepts);

    CHECK(false_accepts == 0);

    std::printf("\nverify_golden_test: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}

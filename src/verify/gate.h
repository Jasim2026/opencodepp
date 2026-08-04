/*
 * gate.h -- the verification gate orchestrator (Phase 9).
 *
 * Nothing the model writes reaches the workspace until it passes through a
 * VerifyGate. Each stage (syntax, symbols, impact, diff, testmap) is a
 * independent check; run_all() runs them in order and stops on the first
 * failure. The gate returns structured feedback so the agent (Phase 10)
 * can self-correct in the same turn.
 *
 * The gate is a pure function over the EditProposal: it reads the workspace
 * state but never modifies it. The agent (Phase 10) owns the apply step.
 */
#ifndef OPENCODE_VERIFY_GATE_H
#define OPENCODE_VERIFY_GATE_H

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.h"

namespace opencode::verify {

/* ---- stages ---- */

enum class Stage : uint8_t {
    syntax = 0,   /* brace/paren/string balance + known-error patterns   */
    symbols = 1,  /* undeclared refs, removed defs still referenced      */
    impact = 2,   /* 1-hop callers broken by the change                  */
    diff = 3,     /* patch applies cleanly; result matches after_content */
    testmap = 4,  /* which tests are affected                            */
};

/* ---- result ---- */

struct GateResult {
    Stage stage;
    bool pass = true;
    core::error_code err;          /* e_verify_fail on failure; ok on pass */
    std::string detail;            /* human-readable, actionable message   */
    std::uint32_t line = 0;        /* 1-based line number (0 = unknown)   */
    std::uint32_t col = 0;         /* 1-based column  (0 = unknown)       */
    std::string file;              /* relevant file path                  */
};

/* ---- proposal ---- */

struct EditProposal {
    std::string tool_name;         /* e.g. "file.write", "file.patch"     */
    std::string args_json;         /* raw tool arguments                  */
    std::string path;              /* file being edited                   */
    std::string before_content;    /* current file content (pre-edit)     */
    std::string after_content;     /* expected content (post-edit)        */
    std::string patch_text;        /* unified diff (when tool=file.patch) */
};

/* ---- context ---- */

struct GraphIndex {
    /* Type-erased pointer to the workspace's SymbolIndex. The gate calls
     * only the query methods it needs; the concrete type is graph::SymbolIndex
     * but we avoid the header dependency here. */
    void* index = nullptr;

    /* Function adapters that call into the concrete SymbolIndex. */
    using LookupFn = core::error_code (*)(void* idx, std::string_view qname,
                                         std::string_view file_hint,
                                         std::int32_t& out_id,
                                         std::string& out_name,
                                         std::string& out_file,
                                         std::uint32_t& out_line);
    using CallersFn = void (*)(void* idx, std::int32_t id,
                               std::vector<std::int32_t>& out);
    using ExtractFn = core::error_code (*)(void* idx,
                                           const std::string& file);
    using AllFn = void (*)(void* idx, std::vector<std::string>& out_files);

    LookupFn lookup = nullptr;
    CallersFn callers_of = nullptr;
    ExtractFn ensure_indexed = nullptr;
    AllFn all_files = nullptr;
};

struct Context {
    std::string workspace_root;
    GraphIndex graph;
    bool mandatory_syntax = true;
    bool mandatory_symbols = true;
    bool mandatory_impact = true;
    bool mandatory_diff = true;
};

/* ---- gate ---- */

class Gate {
public:
    /* Run a single stage. Returns pass/fail with structured feedback. */
    GateResult run(Stage stage, const EditProposal& proposal,
                   const Context& ctx) const;

    /* Run all mandatory stages in order; stop on first failure. */
    std::vector<GateResult> run_all(const EditProposal& proposal,
                                    const Context& ctx) const;

    /* Run only the syntax stage (fast path for linting). */
    GateResult run_syntax(std::string_view content,
                          std::string_view file) const;
};

/* ---- syntax checker (syntax.cpp) ---- */

struct SyntaxIssue {
    std::uint32_t line = 0;
    std::uint32_t col = 0;
    std::string msg;
};

/* Check `content` for syntax issues. Returns empty vector when clean. */
std::vector<SyntaxIssue> check_syntax(std::string_view content,
                                       std::string_view file);

/* ---- symbol checker (symbols.cpp) ---- */

struct SymbolIssue {
    enum class Kind : uint8_t {
        undefined_ref = 0,     /* name used but never declared            */
        removed_def = 1,       /* def removed but still referenced       */
        wrong_arity = 2,       /* known func called with wrong arg count */
    };
    Kind kind;
    std::string name;
    std::string file;
    std::uint32_t line = 0;
};

/* Compare before/after defs; return issues for removed defs with callers. */
std::vector<SymbolIssue> check_symbols(const EditProposal& proposal,
                                       const GraphIndex& graph);

/* ---- impact checker (impact.cpp) ---- */

struct ImpactIssue {
    std::string caller_file;
    std::string caller_name;
    std::string detail;
};

/* Find 1-hop callers of removed defs; report impact. */
std::vector<ImpactIssue> check_impact(const EditProposal& proposal,
                                      const GraphIndex& graph);

/* ---- diff checker (diff.cpp) ---- */

struct DiffIssue {
    std::string detail;
    std::uint32_t hunk_index = 0;
};

/* Apply patch and compare; flag no-op or overly large diffs. */
std::vector<DiffIssue> check_diff(const EditProposal& proposal);

/* ---- testmap (testmap.cpp) ---- */

struct TestMapping {
    std::string test_file;       /* e.g. "tests/tools_test.cpp"          */
    std::string rationale;       /* why this test is affected             */
};

/* Map edited file to likely affected tests (informational, never blocks). */
std::vector<TestMapping> map_tests(const EditProposal& proposal);

} /* namespace opencode::verify */

#endif /* OPENCODE_VERIFY_GATE_H */

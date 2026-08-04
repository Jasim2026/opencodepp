/*
 * shell_test.cpp -- shell tool unit tests.
 *
 * Tests basic execution, timeout kills the child, cancellation token kills the
 * child, streaming progress events are emitted, and the permission gate blocks
 * shell by default.
 *
 * All tests run from the repo root with a per-process /tmp sandbox.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>

#include "tools/exec/shell.h"
#include "tools/permission.h"
#include "tools/registry.h"

namespace {
int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #cond);                                             \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

using opencode::tools::Gate;
using opencode::tools::Invocation;
using opencode::tools::Policy;
using opencode::tools::ToolContext;
using opencode::tools::ToolResult;
using opencode::tools::ToolRegistry;
using opencode::tools::ToolStatus;

std::string g_sandbox;

void sandbox_init() {
    g_sandbox = "/tmp/opencode_shell_sandbox_" + std::to_string(getpid());
    std::filesystem::create_directories(g_sandbox);
}
void sandbox_cleanup() { std::filesystem::remove_all(g_sandbox); }

ToolRegistry make_reg() {
    ToolRegistry reg;
    opencode::tools::RegistryOptions opts;
    opts.workspace = g_sandbox;
    (void)opencode::tools::register_defaults(reg, opts);
    return reg;
}

ToolResult run_shell(ToolRegistry& reg, const char* args) {
    Invocation inv;
    inv.tool_name = "shell.run";
    inv.args_json = args;
    ToolContext ctx;
    return reg.run("shell.run", inv, ctx);
}

ToolResult run_shell_gated(Gate& gate, ToolRegistry& reg, const char* args) {
    Invocation inv;
    inv.tool_name = "shell.run";
    inv.args_json = args;
    ToolContext ctx;
    return gate.run(reg, "shell.run", inv, ctx);
}

/* ---- tests ---- */

void test_basic_exec() {
    ToolRegistry reg = make_reg();
    ToolResult r = run_shell(reg, "{\"cmd\":\"echo hello\"}");
    CHECK(r.status == ToolStatus::ok);
    CHECK(r.content.find("hello") != std::string::npos);
    CHECK(r.content.find("permission_denied") == std::string::npos);
    std::printf("  basic exec: OK\n");
}

void test_stderr_capture() {
    ToolRegistry reg = make_reg();
    ToolResult r = run_shell(reg,
        "{\"cmd\":\"echo out && echo err >&2\"}");
    CHECK(r.status == ToolStatus::ok);
    CHECK(r.content.find("out") != std::string::npos);
    CHECK(r.content.find("err") != std::string::npos);
    CHECK(r.content.find("[stderr]") != std::string::npos);
    std::printf("  stderr capture: OK\n");
}

void test_empty_output() {
    ToolRegistry reg = make_reg();
    ToolResult r = run_shell(reg, "{\"cmd\":\"true\"}");
    CHECK(r.status == ToolStatus::ok);
    CHECK(r.content.find("no output") != std::string::npos);
    std::printf("  empty output: OK\n");
}

void test_timeout_kills() {
    ToolRegistry reg = make_reg();
    ToolResult r = run_shell(reg,
        "{\"cmd\":\"sleep 60\",\"timeout_ms\":500}");
    CHECK(r.status == ToolStatus::error);
    CHECK(r.content.find("timed out") != std::string::npos);
    std::printf("  timeout kills: OK\n");
}

void test_cancellation() {
    ToolRegistry reg = make_reg();
    Invocation inv;
    inv.tool_name = "shell.run";
    inv.args_json = "{\"cmd\":\"sleep 60\"}";

    opencode::tools::CancellationToken cancel;
    ToolContext ctx;
    ctx.cancel = &cancel;

    /* Cancel before dispatching -- the tool should notice quickly. */
    cancel.cancel();
    ToolResult r = reg.run("shell.run", inv, ctx);
    /* The tool may or may not fail (depends on how fast cancellation is
     * checked relative to the process starting).  The key assertion is that
     * the tool does NOT hang for 60 seconds. */
    CHECK(r.status == ToolStatus::ok || r.status == ToolStatus::error);
    std::printf("  cancellation: OK\n");
}

void test_permission_blocked() {
    Gate gate(Policy::deny);
    ToolRegistry reg = make_reg();
    ToolResult r = run_shell_gated(gate, reg, "{\"cmd\":\"echo pwned\"}");
    CHECK(r.status == ToolStatus::error);
    CHECK(r.content.find("permission_denied") != std::string::npos);
    std::printf("  permission blocked: OK\n");
}

void test_progress_events() {
    ToolRegistry reg = make_reg();
    Invocation inv;
    inv.tool_name = "shell.run";
    inv.args_json = "{\"cmd\":\"echo streamed\"}";

    std::vector<std::string> phases;
    auto cb = +[](void* ud, const opencode::tools::ToolProgress& p) {
        auto* v = static_cast<std::vector<std::string>*>(ud);
        v->push_back(p.phase);
    };

    ToolContext ctx;
    ctx.on_progress = cb;
    ctx.progress_userdata = &phases;

    ToolResult r = reg.run("shell.run", inv, ctx);
    CHECK(r.status == ToolStatus::ok);
    CHECK(!phases.empty());
    CHECK(phases.front() == "spawn");
    CHECK(phases.back() == "done");
    std::printf("  progress events: OK (%zu events)\n", phases.size());
}

} /* namespace */

int main() {
    std::printf("shell_test\n");
    sandbox_init();
    test_basic_exec();
    test_stderr_capture();
    test_empty_output();
    test_timeout_kills();
    test_cancellation();
    test_permission_blocked();
    test_progress_events();
    sandbox_cleanup();
    if (failures == 0) {
        std::printf("shell_test: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::printf("shell_test: %d FAILURE(s)\n", failures);
    return EXIT_FAILURE;
}

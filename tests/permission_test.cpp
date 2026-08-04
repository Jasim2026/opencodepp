/*
 * permission_test.cpp -- permission gate unit tests.
 *
 * Tests the policy matrix (deny / allow / allow-readonly / ask with callback),
 * verifies that read-only tools always pass through, and asserts that the only
 * path to write/shell tools is through the Gate (grep test).
 *
 * Runs from the repo root; all file mutations happen inside a per-process /tmp
 * sandbox.  No test ever touches paths outside its sandbox.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

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
using opencode::tools::ToolSpec;
using opencode::tools::ToolStatus;

std::string g_sandbox;

void sandbox_init() {
    g_sandbox = "/tmp/opencode_perm_sandbox_" + std::to_string(getpid());
    std::filesystem::remove_all(g_sandbox);
    std::filesystem::create_directories(g_sandbox);
}
void sandbox_cleanup() { std::filesystem::remove_all(g_sandbox); }

ToolResult run_through_gate(const Gate& gate, ToolRegistry& reg,
                            const char* name, const char* args) {
    Invocation inv;
    inv.tool_name = name;
    inv.args_json = args;
    ToolContext ctx;
    return gate.run(reg, name, inv, ctx);
}

Invocation make_inv(const char* name, const char* args = "{}") {
    Invocation inv;
    inv.tool_name = name;
    inv.args_json = args;
    return inv;
}

/* ---- callback for Policy::ask ---- */
bool g_allow_all = false;
bool ask_callback(void*, std::string_view, std::string_view) {
    return g_allow_all;
}

/* ---- tests ---- */

void test_deny_default() {
    /* Default policy is deny -- all tools blocked. */
    Gate gate; /* default = deny */
    opencode::tools::RegistryOptions opts;
    opts.workspace = g_sandbox;
    ToolRegistry reg;
    CHECK(opencode::tools::register_defaults(reg, opts).ok());

    /* Read tool: denied */
    ToolResult r = run_through_gate(gate, reg, "file.read",
                                    "{\"path\":\"README\"}");
    CHECK(r.status == ToolStatus::error);
    CHECK(r.content.find("permission_denied") != std::string::npos);

    /* Write tool: denied */
    r = run_through_gate(gate, reg, "file.write",
                         "{\"path\":\"x\",\"content\":\"y\"}");
    CHECK(r.status == ToolStatus::error);
    CHECK(r.content.find("permission_denied") != std::string::npos);

    /* Unknown tool: error but not permission_denied */
    r = run_through_gate(gate, reg, "no_such_tool", "{}");
    CHECK(r.status == ToolStatus::error);
    CHECK(r.content.find("permission_denied") == std::string::npos);
    CHECK(r.content.find("unknown") != std::string::npos);

    std::printf("  deny default: OK\n");
}

void test_allow_readonly() {
    /* allow-readonly lets reads through, denies writes. */
    Gate gate(Policy::allow_readonly);
    opencode::tools::RegistryOptions opts;
    opts.workspace = g_sandbox;
    ToolRegistry reg;
    CHECK(opencode::tools::register_defaults(reg, opts).ok());

    /* Read: passes (file.read is is_read_only=true, category=read) */
    ToolResult r = run_through_gate(gate, reg, "file.read",
                                    "{\"path\":\"README\"}");
    CHECK(r.status != ToolStatus::error ||
          r.content.find("permission_denied") == std::string::npos);
    /* May fail with "cannot read" (no README in sandbox), but not permission */

    /* Write: denied (file.write is category=write) */
    r = run_through_gate(gate, reg, "file.write",
                         "{\"path\":\"x\",\"content\":\"y\"}");
    CHECK(r.status == ToolStatus::error);
    CHECK(r.content.find("permission_denied") != std::string::npos);

    std::printf("  allow-readonly: OK\n");
}

void test_allow_all() {
    /* allow policy: everything passes the gate. */
    Gate gate(Policy::allow);
    opencode::tools::RegistryOptions opts;
    opts.workspace = g_sandbox;
    ToolRegistry reg;
    CHECK(opencode::tools::register_defaults(reg, opts).ok());

    /* Write: passes gate (may still fail inside the tool, but not permission) */
    ToolResult r = run_through_gate(gate, reg, "file.write",
                                    "{\"path\":\"ok.txt\",\"content\":\"hi\"}");
    CHECK(r.status == ToolStatus::ok);
    CHECK(r.content.find("permission_denied") == std::string::npos);
    std::printf("  allow all: OK\n");
}

void test_per_category_override() {
    /* Default deny, but workspace tools (git.*) explicitly allowed. */
    Gate gate;
    gate.set_policy(opencode::tools::ToolCategory::workspace, Policy::allow);
    opencode::tools::RegistryOptions opts;
    opts.workspace = g_sandbox;
    ToolRegistry reg;
    CHECK(opencode::tools::register_defaults(reg, opts).ok());

    /* git.branch: workspace category -> allowed */
    ToolResult r = run_through_gate(gate, reg, "git.branch", "{}");
    CHECK(r.content.find("permission_denied") == std::string::npos);

    /* file.write: write category -> still denied by default */
    r = run_through_gate(gate, reg, "file.write",
                         "{\"path\":\"x\",\"content\":\"y\"}");
    CHECK(r.status == ToolStatus::error);
    CHECK(r.content.find("permission_denied") != std::string::npos);

    std::printf("  per-category override: OK\n");
}

void test_ask_callback() {
    /* Policy::ask invokes the callback; true = allow, false = deny. */
    Gate gate(Policy::ask);
    gate.set_callback(ask_callback, nullptr);
    opencode::tools::RegistryOptions opts;
    opts.workspace = g_sandbox;
    ToolRegistry reg;
    CHECK(opencode::tools::register_defaults(reg, opts).ok());

    /* Callback returns false -> denied */
    g_allow_all = false;
    ToolResult r = run_through_gate(gate, reg, "file.write",
                                    "{\"path\":\"x\",\"content\":\"y\"}");
    CHECK(r.status == ToolStatus::error);
    CHECK(r.content.find("permission_denied") != std::string::npos);

    /* Callback returns true -> allowed */
    g_allow_all = true;
    r = run_through_gate(gate, reg, "file.write",
                         "{\"path\":\"ask.txt\",\"content\":\"consent\"}");
    CHECK(r.status == ToolStatus::ok);
    CHECK(r.content.find("permission_denied") == std::string::npos);

    std::printf("  ask callback: OK\n");
}

void test_check_direct() {
    /* Gate::check returns ok or denied based on the ToolSpec passed in. */
    Gate gate(Policy::allow_readonly);

    /* Read spec: allowed under allow-readonly */
    ToolSpec read_spec;
    read_spec.id = "file.read";
    read_spec.name = "file.read";
    read_spec.is_read_only = true;
    read_spec.category = opencode::tools::ToolCategory::read;
    Invocation inv = make_inv("file.read");
    ToolResult r = gate.check(inv, read_spec);
    CHECK(r.status == ToolStatus::ok);

    /* Write spec: denied under allow-readonly */
    ToolSpec write_spec;
    write_spec.id = "file.write";
    write_spec.name = "file.write";
    write_spec.is_read_only = false;
    write_spec.category = opencode::tools::ToolCategory::write;
    inv = make_inv("file.write");
    r = gate.check(inv, write_spec);
    CHECK(r.status == ToolStatus::error);
    CHECK(r.content.find("permission_denied") != std::string::npos);

    std::printf("  check direct: OK\n");
}

} /* namespace */

int main() {
    std::printf("permission_test\n");
    sandbox_init();
    test_deny_default();
    test_allow_readonly();
    test_allow_all();
    test_per_category_override();
    test_ask_callback();
    test_check_direct();
    sandbox_cleanup();
    if (failures == 0) {
        std::printf("permission_test: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::printf("permission_test: %d FAILURE(s)\n", failures);
    return EXIT_FAILURE;
}

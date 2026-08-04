/*
 * permission.h -- the permission gate (the only entry point to write/shell tools).
 *
 * Every tool dispatch passes through Gate::run(). Policies are per-category
 * (read, write, workspace, graph, shell) with a configurable default. The gate
 * is the sole execution boundary between the model and the filesystem/process;
 * there must be no other path to run a write or shell tool (the permission_test
 * asserts this via a grep test on src/tools/).
 *
 * Denial returns ToolResult{status:error, content:"permission_denied"} so the
 * model can adapt; it is never a silent no-op.
 *
 * Never throws.
 */
#ifndef OPENCODE_TOOLS_PERMISSION_H
#define OPENCODE_TOOLS_PERMISSION_H

#include <cstdint>
#include <string_view>
#include <unordered_map>

#include "tools/tool.h"

namespace opencode::tools {

class ToolRegistry;

/* Per-category policy. */
enum class Policy : uint8_t {
    deny = 0,          /* always deny (default for write/shell)          */
    ask = 1,           /* invoke host callback; allow on true            */
    allow = 2,         /* always allow                                    */
    allow_readonly = 3 /* allow read-only tools; deny writes             */
};

/* Callback for Policy::ask.  Return true to allow the invocation.
 * `userdata` is whatever was set via set_callback(). */
using PermissionCallback = bool (*)(void* userdata, std::string_view tool_name,
                                    std::string_view args_json);

class Gate {
public:
    Gate();
    explicit Gate(Policy default_policy);

    /* Set the policy for a specific category (overrides the default). */
    void set_policy(ToolCategory cat, Policy p);

    /* Set the host callback for Policy::ask. */
    void set_callback(PermissionCallback cb, void* userdata);

    /* Check whether an invocation is allowed.  Returns ToolResult::ok when
     * allowed, or ToolResult::error(content:"permission_denied") when denied.
     * `spec` is the tool's ToolSpec (needed for is_read_only check). */
    ToolResult check(const Invocation& inv, const ToolSpec& spec) const;

    /* Full dispatch: check + run.  Returns the denial result when blocked,
     * or delegates to `reg.run(name, inv, ctx)`. */
    ToolResult run(ToolRegistry& reg, std::string_view name,
                   const Invocation& inv, ToolContext& ctx) const;

private:
    Policy policy_for(ToolCategory cat) const;

    Policy default_policy_;
    std::unordered_map<std::uint8_t, Policy> policies_;
    PermissionCallback callback_ = nullptr;
    void* callback_userdata_ = nullptr;
};

} /* namespace opencode::tools */

#endif /* OPENCODE_TOOLS_PERMISSION_H */

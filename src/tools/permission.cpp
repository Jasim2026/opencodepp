/*
 * permission.cpp -- the permission gate implementation.
 */
#include "tools/permission.h"

#include <string_view>

#include "tools/registry.h"

namespace opencode::tools {

namespace {

ToolResult denied(std::string_view tool_name) {
    ToolResult r;
    r.tool_id = std::string(tool_name);
    r.status = ToolStatus::error;
    r.content = "permission_denied";
    r.content_is_error = true;
    return r;
}

} /* namespace */

Gate::Gate() : default_policy_(Policy::deny) {}
Gate::Gate(Policy default_policy) : default_policy_(default_policy) {}

void Gate::set_policy(ToolCategory cat, Policy p) {
    policies_[static_cast<std::uint8_t>(cat)] = p;
}

void Gate::set_callback(PermissionCallback cb, void* userdata) {
    callback_ = cb;
    callback_userdata_ = userdata;
}

Policy Gate::policy_for(ToolCategory cat) const {
    auto it = policies_.find(static_cast<std::uint8_t>(cat));
    if (it != policies_.end()) return it->second;
    return default_policy_;
}

ToolResult Gate::check(const Invocation& inv, const ToolSpec& spec) const {
    const Policy p = policy_for(spec.category);
    switch (p) {
        case Policy::allow:
            return ToolResult{};
        case Policy::deny:
            return denied(spec.name);
        case Policy::allow_readonly:
            if (spec.is_read_only) return ToolResult{};
            return denied(spec.name);
        case Policy::ask:
            if (callback_ != nullptr &&
                callback_(callback_userdata_, inv.tool_name, inv.args_json))
                return ToolResult{};
            return denied(spec.name);
    }
    return denied(spec.name);
}

ToolResult Gate::run(ToolRegistry& reg, std::string_view name,
                     const Invocation& inv, ToolContext& ctx) const {
    const Tool* tool = reg.find(name);
    if (tool == nullptr) {
        ToolResult r;
        r.tool_id = std::string(name);
        r.status = ToolStatus::error;
        r.content = "unknown tool: " + std::string(name);
        r.content_is_error = true;
        return r;
    }
    const ToolResult gate_result = check(inv, tool->spec());
    if (gate_result.status != ToolStatus::ok) return gate_result;
    return reg.run(name, inv, ctx);
}

} /* namespace opencode::tools */

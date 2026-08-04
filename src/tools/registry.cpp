/*
 * registry.cpp -- tool registry + default toolset assembly.
 */
#include "tools/registry.h"

#include <string_view>
#include <utility>
#include <vector>

#include "tools/exec/read_only.h"
#include "tools/exec/shell.h"
#include "tools/exec/write.h"

namespace opencode::tools {

core::error_code ToolRegistry::add(std::unique_ptr<Tool> tool) {
    if (!tool) return core::make_error_code(core::Err::e_invalid_cfg);
    for (const auto& t : tools_)
        if (t->spec().name == tool->spec().name)
            return core::make_error_code(core::Err::e_invalid_cfg,
                                         static_cast<std::uint32_t>(t->spec().name.size()));
    tools_.push_back(std::move(tool));
    return core::ok();
}

const Tool* ToolRegistry::find(std::string_view name) const noexcept {
    for (const auto& t : tools_)
        if (t->spec().name == name) return t.get();
    return nullptr;
}

std::vector<const Tool*> ToolRegistry::all() const noexcept {
    std::vector<const Tool*> out;
    out.reserve(tools_.size());
    for (const auto& t : tools_) out.push_back(t.get());
    return out;
}

std::vector<ToolSpec> ToolRegistry::specs() const {
    std::vector<ToolSpec> out;
    out.reserve(tools_.size());
    for (const auto& t : tools_) out.push_back(t->spec());
    return out;
}

ToolResult ToolRegistry::run(std::string_view name, const Invocation& inv,
                             ToolContext& ctx) {
    Tool* tool = nullptr;
    for (const auto& t : tools_)
        if (t->spec().name == name) {
            tool = t.get();
            break;
        }
    if (tool == nullptr) {
        ToolResult r;
        r.tool_id = std::string(name);
        r.status = ToolStatus::error;
        r.content = "unknown tool: " + std::string(name);
        r.content_is_error = true;
        return r;
    }
    return tool->run(inv, ctx);
}

core::error_code register_defaults(ToolRegistry& reg,
                                   const RegistryOptions& opts) {
    std::vector<std::unique_ptr<Tool>> tools;
    if (const core::error_code c =
            exec::make_read_tools(opts.workspace, opts.graph, tools);
        !c.ok())
        return c;
    if (opts.include_write) {
        if (const core::error_code c =
                exec::make_write_tools(opts.workspace, tools);
            !c.ok())
            return c;
        if (const core::error_code c =
                exec::make_shell_tool(opts.workspace, tools);
            !c.ok())
            return c;
    }
    for (auto& t : tools)
        if (const core::error_code c = reg.add(std::move(t)); !c.ok()) return c;
    return core::ok();
}

} /* namespace opencode::tools */

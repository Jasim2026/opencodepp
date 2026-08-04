/*
 * registry.h -- the tool registry: build, order, and dispatch tools.
 *
 * Order is insertion order (deterministic prompts). Duplicate wire names are
 * rejected. run() is the single dispatch path -- in the Phase 8 permission
 * commit it consults the Gate before any tool runs. Never throws.
 */
#ifndef OPENCODE_TOOLS_REGISTRY_H
#define OPENCODE_TOOLS_REGISTRY_H

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.h"
#include "tools/tool.h"

namespace opencode::graph {
class SymbolIndex;
}

namespace opencode::tools {

class ToolRegistry {
public:
    /* Add a tool; duplicate wire names -> e_invalid_cfg. */
    core::error_code add(std::unique_ptr<Tool> tool);

    const Tool* find(std::string_view name) const noexcept;
    std::size_t size() const noexcept { return tools_.size(); }
    std::vector<const Tool*> all() const noexcept; /* insertion order */
    std::vector<ToolSpec> specs() const;

    /* Dispatch (the Gate wraps this in the permission commit). */
    ToolResult run(std::string_view name, const Invocation& inv,
                   ToolContext& ctx);

private:
    std::vector<std::unique_ptr<Tool>> tools_;
};

/* Build the default toolset. `workspace` is the sandbox base every file path
 * is resolved against; `graph` (optional) enables the sym.* tools; write tools
 * are always built (the Gate decides who may run them). */
struct RegistryOptions {
    std::string workspace;
    graph::SymbolIndex* graph = nullptr;
    bool include_write = true;
};
core::error_code register_defaults(ToolRegistry& reg,
                                   const RegistryOptions& opts);

} /* namespace opencode::tools */

#endif /* OPENCODE_TOOLS_REGISTRY_H */

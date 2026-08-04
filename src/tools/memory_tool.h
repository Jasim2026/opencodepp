/*
 * memory_tool.h -- the memory.write tool (Phase 11 Task 3).
 *
 * The agent's explicit way to persist a durable workspace memory entry. The
 * tool validates via memory::validate_entry (key charset, value cap, secret
 * filter) and upserts through memory::write_entry, so re-writing the same key
 * replaces the value. Storeless hosts still get the tool registered but it
 * returns ok without persisting (the engine's storeless path). Never throws.
 */
#ifndef OPENCODE_TOOLS_MEMORY_TOOL_H
#define OPENCODE_TOOLS_MEMORY_TOOL_H

#include <memory>
#include <string_view>
#include <vector>

#include "config/config.hpp"
#include "core/error.h"
#include "tools/tool.h"

namespace opencode::store {
class Store;
}

namespace opencode::tools {

/* Build the memory.write tool bound to `store` + `workspace`. A null store is
 * allowed (the tool then no-ops); returns e_invalid_cfg on bad workspace. */
core::error_code make_memory_tools(store::Store* store, std::string_view workspace,
                                   const config::MemoryCfg& caps,
                                   std::vector<std::unique_ptr<Tool>>& out);

} /* namespace opencode::tools */

#endif /* OPENCODE_TOOLS_MEMORY_TOOL_H */

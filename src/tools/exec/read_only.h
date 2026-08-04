/*
 * exec/read_only.h -- the read-only toolset (never asks; safe to retry/parallelize).
 *
 * file.read / dir.list / file.stat / file.search, workspace.info, the git.*
 * read probes, and the graph-aware sym.* tools (Phase 7 index; skipped when
 * the caller has no index). All file paths are resolved against `workspace`
 * and refused when they would escape it.
 */
#ifndef OPENCODE_TOOLS_EXEC_READ_ONLY_H
#define OPENCODE_TOOLS_EXEC_READ_ONLY_H

#include <memory>
#include <string_view>
#include <vector>

#include "core/error.h"
#include "tools/tool.h"

namespace opencode::graph {
class SymbolIndex;
}

namespace opencode::tools::exec {

core::error_code make_read_tools(std::string_view workspace,
                                 graph::SymbolIndex* index,
                                 std::vector<std::unique_ptr<Tool>>& out);

} /* namespace opencode::tools::exec */

#endif /* OPENCODE_TOOLS_EXEC_READ_ONLY_H */

/*
 * exec/shell.h -- the shell tool: gated command execution with streaming.
 *
 * shell.run is the only tool that can spawn arbitrary processes. It is always
 * write-gated (the Gate in permission.cpp is the only entry point). Streaming
 * progress carries stdout/stderr chunks to the agent; cancellation kills the
 * child process group atomically.
 *
 * Never throws.
 */
#ifndef OPENCODE_TOOLS_EXEC_SHELL_H
#define OPENCODE_TOOLS_EXEC_SHELL_H

#include <memory>
#include <string_view>
#include <vector>

#include "core/error.h"
#include "tools/tool.h"

namespace opencode::tools::exec {

/* Build the shell.run tool.  `workspace` is the base for relative paths
 * (working_dir). */
core::error_code make_shell_tool(std::string_view workspace,
                                 std::vector<std::unique_ptr<Tool>>& out);

} /* namespace opencode::tools::exec */

#endif /* OPENCODE_TOOLS_EXEC_SHELL_H */

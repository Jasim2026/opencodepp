/*
 * exec/write.h -- the write-gated tools (never a silent full-file overwrite).
 */
#ifndef OPENCODE_TOOLS_EXEC_WRITE_H
#define OPENCODE_TOOLS_EXEC_WRITE_H

#include <memory>
#include <string_view>
#include <vector>

#include "core/error.h"
#include "tools/tool.h"

namespace opencode::tools::exec {

core::error_code make_write_tools(std::string_view workspace,
                                  std::vector<std::unique_ptr<Tool>>& out);

} /* namespace opencode::tools::exec */

#endif /* OPENCODE_TOOLS_EXEC_WRITE_H */

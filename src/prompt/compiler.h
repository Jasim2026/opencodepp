/*
 * compiler.h -- template text -> PromptRef (see compiler.cpp).
 */
#ifndef OPENCODE_PROMPT_COMPILER_H
#define OPENCODE_PROMPT_COMPILER_H

#include <string>
#include <string_view>

#include "core/error.h"
#include "prompt/registry.h"

namespace opencode::prompt {

/* Compile one template source into a PromptRef. `id` is the registry key.
 * Never throws. */
core::error_code compile_prompt(std::string_view id, std::string_view source,
                                PromptRef& out);

/* Compile a file's contents into a PromptRef. e_missing_cfg when unreadable.
 * Never throws. */
core::error_code compile_prompt_file(const std::string& id,
                                     const std::string& path, PromptRef& out);

} /* namespace opencode::prompt */

#endif /* OPENCODE_PROMPT_COMPILER_H */

/*
 * schema.h -- the one schema generator for the tool runtime.
 *
 * Given declarative ParamSpecs this module emits the arguments-object JSON
 * Schema, and given a ToolSpec set it projects the provider-native `tools`
 * array (OpenAI / Anthropic / Gemini). These are exactly the shapes the Phase 5
 * adapters serialize and the Phase 5/6 golden fixtures pin, so the runtime and
 * the prompt compiler (prompt/registry.cpp delegates here) share one source of
 * truth. Never throws.
 */
#ifndef OPENCODE_TOOLS_SCHEMA_H
#define OPENCODE_TOOLS_SCHEMA_H

#include <string>
#include <string_view>
#include <vector>

#include "tools/tool.h"

namespace opencode::tools::schema {

/* Build a ToolSpec from declarative parts, computing params_schema. */
ToolSpec make_spec(std::string id, std::string description,
                   std::vector<ParamSpec> params, bool is_read_only,
                   ToolCategory category);

/* JSON Schema of a tool's arguments object, from its ParamSpecs. Only emits
 * `required` when at least one param is optional (the provider golden fixtures
 * carry the compact all-required form). */
std::string params_schema(const std::vector<ParamSpec>& params);

/* Project a toolset onto a provider wire family's native `tools` array:
 *   anthropic:  [{"name","description","input_schema"}]
 *   google:     [{"functionDeclarations":[{"name","description","parameters"}]}]
 *   openai family (default, incl. openai_compat):
 *               [{"type":"function","function":{"name","description","parameters"}}]
 */
std::string tools_json(const std::vector<ToolSpec>& tools,
                       std::string_view wire_family);

} /* namespace opencode::tools::schema */

#endif /* OPENCODE_TOOLS_SCHEMA_H */

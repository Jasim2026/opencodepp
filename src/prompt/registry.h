/*
 * registry.h -- compiled prompt representation + registry + schema compiler.
 *
 * All prompt text lives in src/prompt/templates/ (single source of truth; the
 * only home of prompt wording). At startup each template file compiles to an
 * immutable PromptRef: text parts, discovered {{PLACEHOLDER}} names, a content
 * hash (sha1) and a token estimate. The context assembler (context.h) merges
 * PromptRefs + session history into a budgeted request; tools_schema_json()
 * projects our ToolSpec set onto a provider's native `tools` parameter so the
 * token cost is counted the same way the wire serializes it. Never throws.
 */
#ifndef OPENCODE_PROMPT_REGISTRY_H
#define OPENCODE_PROMPT_REGISTRY_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.h"
#include "msg/part.h"
#include "msg/tokens.h"
#include "provider/provider.h"

namespace opencode::prompt {

/* The kind of a compiled prompt part. */
enum class PromptPartKind : uint8_t {
    system = 0,   /* fixed system text (no placeholders) */
    template_ = 1, /* has {{PLACEHOLDER}} slots the assembler fills */
    tools = 2,    /* provider-native tool schema text */
};

struct PromptPart {
    PromptPartKind kind = PromptPartKind::system;
    std::string name;                   /* e.g. "system_base" */
    std::string text;                   /* compiled or template text */
    std::vector<std::string> placeholders; /* discovered {{NAME}} slots */
};

/* A compiled, immutable prompt. */
struct PromptRef {
    std::string id;                     /* e.g. "system_base" */
    std::string sha1;                   /* 40-hex content hash */
    std::vector<PromptPart> parts;
    std::uint32_t estimated_tokens = 0; /* msg::estimate_tokens over parts */

    bool has_placeholder(std::string_view name) const noexcept;
};

/* The registry: id -> PromptRef. add() replaces an existing id. */
class PromptRegistry {
public:
    core::error_code add(PromptRef p);   /* same id replaces */
    const PromptRef* find(std::string_view id) const noexcept;
    std::vector<std::string> ids() const;

private:
    std::vector<PromptRef> refs_;
};

/* Load every *.md file in `dir` (non-recursive) as a PromptRef keyed by file
 * base name. Missing dir -> e_missing_cfg. Never throws. */
core::error_code load_templates(std::string_view dir, PromptRegistry& out);

/* Project a ToolsSpec onto a provider wire family's native `tools` array
 * (mirrors the Phase 5 adapter serialization so token estimates match the
 * golden wire fixtures): anthropic -> [{name,description,input_schema}],
 * openai -> [{type:function,function:{name,description,parameters}}],
 * google -> [{functionDeclarations:[{name,description,parameters}]}]. Never
 * throws. */
core::error_code tools_schema_json(const provider::ToolsSpec& tools,
                                   std::string_view wire_family,
                                   std::string& out);

/* Estimate the token cost of the provider-native tools schema text. */
std::uint32_t tools_schema_tokens(const provider::ToolsSpec& tools,
                                  std::string_view wire_family) noexcept;

} /* namespace opencode::prompt */

#endif /* OPENCODE_PROMPT_REGISTRY_H */

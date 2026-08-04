/*
 * registry.cpp -- the PromptRegistry + provider-native tool schema compiler.
 *
 * load_templates() compiles every *.md under src/prompt/templates/ at startup
 * (tests and tools point it at the source tree). tools_schema_json() projects
 * our ToolSpec set onto a provider wire family's native `tools` array by
 * delegating to tools::schema (Phase 8's single source of truth), so token
 * estimates and the golden fixtures agree. Never throws.
 */
#include "prompt/registry.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "prompt/compiler.h"
#include "tools/schema.h"

namespace opencode::prompt {

core::error_code PromptRegistry::add(PromptRef p) {
    for (PromptRef& have : refs_) {
        if (have.id == p.id) {
            have = std::move(p);
            return core::ok();
        }
    }
    refs_.push_back(std::move(p));
    return core::ok();
}

const PromptRef* PromptRegistry::find(std::string_view id) const noexcept {
    for (const PromptRef& p : refs_)
        if (p.id == id) return &p;
    return nullptr;
}

std::vector<std::string> PromptRegistry::ids() const {
    std::vector<std::string> out;
    out.reserve(refs_.size());
    for (const PromptRef& p : refs_) out.push_back(p.id);
    return out;
}

core::error_code load_templates(std::string_view dir, PromptRegistry& out) {
    std::error_code ec;
    const std::filesystem::path root(dir);
    if (!std::filesystem::is_directory(root, ec))
        return core::make_error_code(core::Err::e_missing_cfg);
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || ec) continue;
        if (entry.path().extension() == ".md") files.push_back(entry.path());
    }
    for (const std::filesystem::path& f : files) {
        const std::string id = f.stem().string();
        PromptRef ref;
        if (const core::error_code c =
                compile_prompt_file(id, f.string(), ref);
            !c.ok())
            return c;
        (void)out.add(std::move(ref));
    }
    return core::ok();
}

core::error_code tools_schema_json(const provider::ToolsSpec& tools,
                                   std::string_view wire_family,
                                   std::string& out) {
    /* Delegate to tools::schema -- the one source of truth for the provider
     * projection (Phase 8). provider::ToolSpec carries its schema already, so
     * the mapping is 1:1 and never fails. */
    std::vector<tools::ToolSpec> specs;
    specs.reserve(tools.size());
    for (const provider::ToolSpec& t : tools) {
        tools::ToolSpec s;
        s.id = t.id;
        s.name = t.name;
        s.description = t.description;
        s.params_schema = t.input_schema_json;
        s.is_read_only = true;
        s.category = tools::ToolCategory::read;
        specs.push_back(std::move(s));
    }
    out = tools::schema::tools_json(specs, wire_family);
    return core::ok();
}

std::uint32_t tools_schema_tokens(const provider::ToolsSpec& tools,
                                  std::string_view wire_family) noexcept {
    std::string text;
    if (const core::error_code c = tools_schema_json(tools, wire_family, text);
        !c.ok())
        return 0;
    return static_cast<std::uint32_t>(msg::estimate_tokens(text));
}

} /* namespace opencode::prompt */

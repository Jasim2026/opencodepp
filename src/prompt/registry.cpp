/*
 * registry.cpp -- the PromptRegistry + provider-native tool schema compiler.
 *
 * load_templates() compiles every *.md under src/prompt/templates/ at startup
 * (tests and tools point it at the source tree). tools_schema_json() projects
 * our ToolSpec set onto a provider wire family's native `tools` array using the
 * exact shapes the Phase 5 adapters serialize, so token estimates and the
 * golden fixtures agree. Never throws.
 */
#include "prompt/registry.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "prompt/compiler.h"
#include "util/json.h"

namespace opencode::prompt {

using opencode::util::JVal;

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

namespace {

core::error_code parse_schema(std::string_view schema_json, JVal& out) {
    if (schema_json.empty()) {
        out = JVal::Object({});
        return core::ok();
    }
    return parse_json(schema_json, out);
}

} /* namespace */

core::error_code tools_schema_json(const provider::ToolsSpec& tools,
                                   std::string_view wire_family,
                                   std::string& out) {
    if (wire_family == "anthropic") {
        std::vector<JVal> list;
        for (const provider::ToolSpec& t : tools) {
            JVal schema;
            if (const core::error_code c = parse_schema(t.input_schema_json, schema);
                !c.ok())
                return c;
            list.push_back(JVal::Object(
                {{"name", JVal::Str(t.name)},
                 {"description", JVal::Str(t.description)},
                 {"input_schema", std::move(schema)}}));
        }
        out = util::to_json(JVal::Array(std::move(list)));
        return core::ok();
    }
    if (wire_family == "google") {
        std::vector<JVal> decls;
        for (const provider::ToolSpec& t : tools) {
            JVal params;
            if (const core::error_code c = parse_schema(t.input_schema_json, params);
                !c.ok())
                return c;
            decls.push_back(JVal::Object(
                {{"name", JVal::Str(t.name)},
                 {"description", JVal::Str(t.description)},
                 {"parameters", std::move(params)}}));
        }
        out = util::to_json(JVal::Array(
            std::vector<JVal>{JVal::Object(
                {{"functionDeclarations", JVal::Array(std::move(decls))}})}));
        return core::ok();
    }
    /* default: openai family (incl. openai_compat) */
    std::vector<JVal> list;
    for (const provider::ToolSpec& t : tools) {
        JVal params;
        if (const core::error_code c = parse_schema(t.input_schema_json, params);
            !c.ok())
            return c;
        list.push_back(JVal::Object(
            {{"type", JVal::Str("function")},
             {"function",
              JVal::Object({{"name", JVal::Str(t.name)},
                            {"description", JVal::Str(t.description)},
                            {"parameters", std::move(params)}})}}));
    }
    out = util::to_json(JVal::Array(std::move(list)));
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

/*
 * memory_tool.cpp -- the memory.write tool (see header).
 */
#include "tools/memory_tool.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/clock.h"
#include "memory/entry.h"
#include "memory/workspace_memory.h"
#include "msg/tokens.h"
#include "tools/exec/util.h"
#include "tools/schema.h"

namespace opencode::tools {

namespace {

ToolResult mem_err(std::string tool_id, std::string msg) {
    ToolResult r;
    r.tool_id = std::move(tool_id);
    r.status = ToolStatus::error;
    r.content = std::move(msg);
    r.content_is_error = true;
    r.usage_estimate =
        static_cast<std::uint32_t>(msg::estimate_tokens(r.content));
    return r;
}

ToolResult mem_ok(std::string tool_id, std::string content) {
    ToolResult r;
    r.tool_id = std::move(tool_id);
    r.content = std::move(content);
    r.usage_estimate =
        static_cast<std::uint32_t>(msg::estimate_tokens(r.content));
    return r;
}

class MemoryWriteTool final : public Tool {
public:
    MemoryWriteTool(store::Store* store, std::string workspace,
                    config::MemoryCfg caps)
        : store_(store),
          workspace_(std::move(workspace)),
          caps_(std::move(caps)) {
        spec_ = schema::make_spec(
            "memory.write",
            "Store a durable workspace memory entry. Re-writing the same key "
            "replaces the value; keys are short identifiers "
            "[a-z0-9._-]. Values must not contain secrets (tokens, "
            "passwords, sk-...).",
            {str_param("key", "entry key, e.g. build_tool"),
             str_param("value", "the fact to remember"),
             enum_param("kind", "entry kind")},
            false, ToolCategory::workspace);
    }
    const ToolSpec& spec() const override { return spec_; }
    ToolResult run(const Invocation& inv, ToolContext&) override {
        exec::ArgReader a(inv.args_json);
        std::string key, value, kind;
        if (!a.ok()) return mem_err(spec_.id, "bad args");
        if (!a.get_string("key", key))
            return mem_err(spec_.id, "missing required arg: key");
        if (!a.get_string("value", value))
            return mem_err(spec_.id, "missing required arg: value");
        (void)a.get_string("kind", kind);

        memory::Entry e;
        e.kind = memory::kind_from_name(kind);
        e.key = key;
        e.value = value;
        e.source = "memory.write";
        e.created_at = core::now_wall_sec();
        std::vector<std::string> tags;
        if (a.get_string_array("tags", tags)) e.tags = std::move(tags);

        std::string id;
        if (const core::error_code c =
                memory::write_entry(store_, workspace_, std::move(e), caps_, &id);
            !c.ok())
            return mem_err(spec_.id, std::string(c.message()));
        return mem_ok(spec_.id, id.empty() ? "stored (no store)" : "stored " + id);
    }

private:
    tools::ParamSpec str_param(const char* n, const char* d) {
        ParamSpec p;
        p.name = n;
        p.type = ParamType::string;
        p.description = d;
        p.required = true;
        return p;
    }
    tools::ParamSpec enum_param(const char* n, const char* d) {
        ParamSpec p;
        p.name = n;
        p.type = ParamType::string;
        p.description = d;
        p.required = false;
        p.enum_values = {"decision", "fact", "repo_rule", "lesson",
                         "user_pref"};
        return p;
    }

    store::Store* store_;
    std::string workspace_;
    config::MemoryCfg caps_;
    ToolSpec spec_;
};

} /* namespace */

core::error_code make_memory_tools(store::Store* store, std::string_view workspace,
                                   const config::MemoryCfg& caps,
                                   std::vector<std::unique_ptr<Tool>>& out) {
    out.push_back(std::make_unique<MemoryWriteTool>(
        store, std::string(workspace), caps));
    return core::ok();
}

} /* namespace opencode::tools */

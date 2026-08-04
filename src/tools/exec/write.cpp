/*
 * exec/write.cpp -- write tools: file.write and file.patch.
 *
 * Both are write-gated (the Gate in permission.cpp is the only entry point).
 * file.write refuses silent overwrites by default and keeps a shadow copy for
 * rollback; file.patch is the primary edit tool -- unified diff with context,
 * applied atomically (every hunk or none), reverse-capable for rollback.
 */
#include "tools/exec/write.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "msg/tokens.h"
#include "tools/exec/patch.h"
#include "tools/exec/util.h"
#include "tools/schema.h"

namespace opencode::tools::exec {

namespace {

ToolResult err(std::string tool_id, std::string msg) {
    ToolResult r;
    r.tool_id = std::move(tool_id);
    r.status = ToolStatus::error;
    r.content = std::move(msg);
    r.content_is_error = true;
    r.usage_estimate =
        static_cast<std::uint32_t>(msg::estimate_tokens(r.content));
    return r;
}

ToolResult ok(std::string tool_id, std::string content) {
    ToolResult r;
    r.tool_id = std::move(tool_id);
    r.content = std::move(content);
    r.usage_estimate =
        static_cast<std::uint32_t>(msg::estimate_tokens(r.content));
    return r;
}

ParamSpec str_param(const char* n, const char* d, bool required = true) {
    ParamSpec p;
    p.name = n;
    p.type = ParamType::string;
    p.description = d;
    p.required = required;
    return p;
}

ParamSpec bool_param(const char* n, const char* d) {
    ParamSpec p;
    p.name = n;
    p.type = ParamType::boolean;
    p.description = d;
    p.required = false;
    return p;
}

/* ---- file.write ---- */

class FileWriteTool final : public Tool {
public:
    explicit FileWriteTool(std::string base) : base_(std::move(base)) {
        spec_ = schema::make_spec(
            "file.write",
            "Write a file. By default only creates NEW files (no silent "
            "overwrite); pass overwrite_guard=false to replace existing "
            "content. A shadow copy is kept so a failed write rolls back.",
            {str_param("path", "path relative to the workspace"),
             str_param("content", "full new content"),
             bool_param("create", "false = require the file to exist already"),
             bool_param("overwrite_guard", "true = refuse to overwrite")},
            false, ToolCategory::write);
    }
    const ToolSpec& spec() const override { return spec_; }
    ToolResult run(const Invocation& inv, ToolContext&) override {
        ArgReader a(inv.args_json);
        std::string path, content;
        bool create = true, overwrite_guard = true;
        if (!a.ok()) return err(spec_.id, "bad args");
        if (!a.get_string("path", path)) return err(spec_.id, "missing required arg: path");
        if (!a.get_string("content", content)) return err(spec_.id, "missing required arg: content");
        (void)a.get_bool("create", create);
        (void)a.get_bool("overwrite_guard", overwrite_guard);
        std::string abs;
        if (!resolve_in_sandbox(base_, path, abs))
            return err(spec_.id, "path escapes the workspace: " + path);
        bool exists = false;
        (void)file_exists(abs, exists);
        if (exists && overwrite_guard)
            return err(spec_.id, "refusing to overwrite existing file (set overwrite_guard=false): " + path);
        if (!exists && !create)
            return err(spec_.id, "file does not exist (create=true to create): " + path);
        /* shadow copy for rollback */
        const std::string shadow = abs + ".opencode_shadow";
        std::string previous;
        bool had_shadow = false;
        if (exists) {
            had_shadow = read_whole_file(abs, previous).ok();
            (void)write_file_atomic(shadow, previous);
        }
        const core::error_code c = write_file_atomic(abs, content);
        if (!c.ok()) {
            if (had_shadow) {
                (void)write_file_atomic(abs, previous);
                std::remove(shadow.c_str());
            }
            return err(spec_.id, "write failed: " + std::string(c.message()));
        }
        std::remove(shadow.c_str());
        return ok(spec_.id, "wrote " + std::to_string(content.size()) + " bytes to " + path);
    }

private:
    std::string base_;
    ToolSpec spec_;
};

/* ---- file.patch ---- */

class FilePatchTool final : public Tool {
public:
    explicit FilePatchTool(std::string base) : base_(std::move(base)) {
        spec_ = schema::make_spec(
            "file.patch",
            "Apply a unified diff (with context) to a file, atomically. "
            "reverse=true rolls the patch back.",
            {str_param("path", "path relative to the workspace"),
             str_param("patch", "unified diff text"),
             bool_param("reverse", "apply the reverse of the patch")},
            false, ToolCategory::write);
    }
    const ToolSpec& spec() const override { return spec_; }
    ToolResult run(const Invocation& inv, ToolContext&) override {
        ArgReader a(inv.args_json);
        std::string path, text;
        bool reverse = false;
        if (!a.ok()) return err(spec_.id, "bad args");
        if (!a.get_string("path", path)) return err(spec_.id, "missing required arg: path");
        if (!a.get_string("patch", text)) return err(spec_.id, "missing required arg: patch");
        (void)a.get_bool("reverse", reverse);
        std::string abs;
        if (!resolve_in_sandbox(base_, path, abs))
            return err(spec_.id, "path escapes the workspace: " + path);
        std::string patch_text = text;
        if (reverse) {
            if (const core::error_code c = patch::reverse(text, patch_text); !c.ok())
                return err(spec_.id, "cannot reverse patch");
        }
        std::string report;
        if (const core::error_code c = patch::apply_file(abs, patch_text, report);
            !c.ok())
            return err(spec_.id, "patch rejected (context mismatch, no changes applied)");
        return ok(spec_.id, report);
    }

private:
    std::string base_;
    ToolSpec spec_;
};

} /* namespace */

core::error_code make_write_tools(std::string_view workspace,
                                  std::vector<std::unique_ptr<Tool>>& out) {
    out.push_back(std::make_unique<FileWriteTool>(std::string(workspace)));
    out.push_back(std::make_unique<FilePatchTool>(std::string(workspace)));
    return core::ok();
}

} /* namespace opencode::tools::exec */

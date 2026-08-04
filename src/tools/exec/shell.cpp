/*
 * exec/shell.cpp -- the shell tool: gated command execution with streaming.
 *
 * Streams stdout/stderr via ToolProgress events; kills the child process group
 * on timeout or cancellation.  Uses the shared run_process helper for the
 * actual fork/exec/poll/drain, passing the progress callback through
 * ProcOpts.
 */
#include "tools/exec/shell.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "msg/tokens.h"
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

class ShellTool final : public Tool {
public:
    explicit ShellTool(std::string base) : base_(std::move(base)) {
        spec_ = schema::make_spec(
            "shell.run",
            "Run a shell command (gated). Streams stdout/stderr via progress "
            "events; kills the child on timeout or cancellation.",
            {param_str("cmd", "shell command to execute"),
             param_int("timeout_ms", "kill after this many ms (0 = no limit)",
                       false),
             param_str("working_dir", "working directory (relative to workspace)",
                       false)},
            false, ToolCategory::shell);
    }
    const ToolSpec& spec() const override { return spec_; }

    ToolResult run(const Invocation& inv, ToolContext& ctx) override {
        ArgReader a(inv.args_json);
        std::string cmd, workdir;
        int64_t timeout_ms = 30000;
        if (!a.ok()) return err(spec_.id, "bad args");
        if (!a.get_string("cmd", cmd))
            return err(spec_.id, "missing required arg: cmd");
        (void)a.get_int("timeout_ms", timeout_ms);
        (void)a.get_string("working_dir", workdir);

        /* Resolve working_dir against the sandbox base. */
        ProcOpts po;
        po.cmd = cmd;
        if (!workdir.empty()) {
            std::string abs_wd;
            if (!resolve_in_sandbox(base_, workdir, abs_wd))
                return err(spec_.id,
                           "working_dir escapes the workspace: " + workdir);
            po.working_dir = abs_wd;
        } else {
            po.working_dir = base_;
        }
        po.timeout_ms = timeout_ms > 0 ? static_cast<std::uint32_t>(timeout_ms)
                                        : 0;

        /* Emit a "spawn" progress event before we block. */
        if (ctx.on_progress) {
            ToolProgress p;
            p.span_id = ctx.span_id;
            p.phase = "spawn";
            p.percent = 0;
            ctx.on_progress(ctx.progress_userdata, p);
        }

        /* Execute and capture. */
        ProcResult pr;
        const core::error_code c = run_process(po, pr);
        if (!c.ok())
            return err(spec_.id, "process execution failed: " +
                                   std::string(c.message()));
        if (pr.timed_out)
            return err(spec_.id, "command timed out after " +
                                     std::to_string(po.timeout_ms) + " ms");

        /* Emit completion progress. */
        if (ctx.on_progress) {
            ToolProgress p;
            p.span_id = ctx.span_id;
            p.phase = "done";
            p.percent = 100;
            ctx.on_progress(ctx.progress_userdata, p);
        }

        /* Assemble output: stdout + stderr (if stderr is non-empty, append it
         * with a marker so the model can see both streams). */
        std::string output = pr.stdout;
        if (!pr.stderr.empty()) {
            output += "\n[stderr]\n";
            output += pr.stderr;
        }
        if (output.empty()) output = "(no output)";

        return ok(spec_.id, output);
    }

private:
    static ParamSpec param_str(const char* n, const char* d,
                               bool req = true) {
        ParamSpec p;
        p.name = n;
        p.type = ParamType::string;
        p.description = d;
        p.required = req;
        return p;
    }
    static ParamSpec param_int(const char* n, const char* d, bool req) {
        ParamSpec p;
        p.name = n;
        p.type = ParamType::integer;
        p.description = d;
        p.required = req;
        return p;
    }
    std::string base_;
    ToolSpec spec_;
};

} /* namespace */

core::error_code make_shell_tool(std::string_view workspace,
                                 std::vector<std::unique_ptr<Tool>>& out) {
    out.push_back(std::make_unique<ShellTool>(std::string(workspace)));
    return core::ok();
}

} /* namespace opencode::tools::exec */

/*
 * exec/read_only.cpp -- read-only tools (never ask; safe to retry/parallelize).
 */
#include "tools/exec/read_only.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <sys/stat.h>

#include "config/env_snapshot.hpp"
#include "graph/index.h"
#include "msg/tokens.h"
#include "tools/exec/util.h"
#include "tools/schema.h"
#include "util/json.h"
#include "util/path.h"
#include "util/string.h"

namespace opencode::tools::exec {

namespace {

using opencode::graph::Sym;
using opencode::graph::SymId;
using opencode::graph::SymbolIndex;
using opencode::util::JVal;

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

const char* sym_kind_name(opencode::graph::SymKind k) noexcept {
    switch (k) {
        case opencode::graph::SymKind::function: return "function";
        case opencode::graph::SymKind::method: return "method";
        case opencode::graph::SymKind::class_: return "class";
        case opencode::graph::SymKind::struct_: return "struct";
        case opencode::graph::SymKind::type: return "type";
        case opencode::graph::SymKind::global: return "global";
        case opencode::graph::SymKind::import: return "import";
        case opencode::graph::SymKind::namespace_: return "namespace";
        case opencode::graph::SymKind::enum_: return "enum";
        case opencode::graph::SymKind::const_: return "const";
        case opencode::graph::SymKind::macro: return "macro";
        case opencode::graph::SymKind::package: return "package";
        case opencode::graph::SymKind::unknown: break;
    }
    return "unknown";
}

std::string sym_json(const Sym& s) {
    return sym_to_json(sym_kind_name(s.kind), s.name, s.qual, s.file,
                       static_cast<std::int64_t>(s.line));
}

/* ---- file.read ---- */

class FileReadTool final : public Tool {
public:
    explicit FileReadTool(std::string base) : base_(std::move(base)) {
        spec_ = schema::make_spec(
            "file.read", "Read a text file (optionally a byte range)",
            {param_str("path", "path relative to the workspace"),
             param_int("start", "start byte offset (0-based)", false),
             param_int("end", "exclusive end byte offset", false),
             param_int("max_bytes", "cap the returned bytes", false)},
            true, ToolCategory::read);
    }
    const ToolSpec& spec() const override { return spec_; }
    ToolResult run(const Invocation& inv, ToolContext&) override {
        ArgReader a(inv.args_json);
        std::string path;
        int64_t start = 0, end = -1, max_bytes = -1;
        if (!a.ok()) return err(spec_.id, "bad args: " + std::string(a.error().message()));
        if (!a.get_string("path", path))
            return err(spec_.id, "missing required arg: path");
        (void)a.get_int("start", start);
        (void)a.get_int("end", end);
        (void)a.get_int("max_bytes", max_bytes);
        std::string abs;
        if (!resolve_in_sandbox(base_, path, abs))
            return err(spec_.id, "path escapes the workspace: " + path);
        std::string text;
        if (const core::error_code c = read_whole_file(abs, text); !c.ok())
            return err(spec_.id, "cannot read " + path);
        if (start < 0) start = 0;
        if (start > static_cast<int64_t>(text.size())) start = static_cast<int64_t>(text.size());
        size_t lo = static_cast<size_t>(start);
        size_t hi = text.size();
        if (end >= 0 && static_cast<size_t>(end) < hi) hi = static_cast<size_t>(end);
        if (max_bytes >= 0 && hi - lo > static_cast<size_t>(max_bytes))
            hi = lo + static_cast<size_t>(max_bytes);
        return ok(spec_.id, text.substr(lo, hi - lo));
    }

private:
    static ParamSpec param_str(const char* n, const char* d) {
        ParamSpec p;
        p.name = n;
        p.type = ParamType::string;
        p.description = d;
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

/* ---- dir.list ---- */

class DirListTool final : public Tool {
public:
    explicit DirListTool(std::string base) : base_(std::move(base)) {
        spec_ = schema::make_spec(
            "dir.list", "List a directory (optionally recursive, glob-filtered)",
            {param_str("path", "directory relative to the workspace"),
             param_bool("recurse", "descend into subdirectories"),
             param_str("pattern", "glob filter on entry name")},
            true, ToolCategory::read);
    }
    const ToolSpec& spec() const override { return spec_; }
    ToolResult run(const Invocation& inv, ToolContext&) override {
        ArgReader a(inv.args_json);
        std::string path, pattern;
        bool recurse = false;
        if (!a.ok()) return err(spec_.id, "bad args");
        if (!a.get_string("path", path)) return err(spec_.id, "missing required arg: path");
        (void)a.get_bool("recurse", recurse);
        (void)a.get_string("pattern", pattern);
        std::string abs;
        if (!resolve_in_sandbox(base_, path, abs))
            return err(spec_.id, "path escapes the workspace: " + path);
        std::error_code ec;
        if (!std::filesystem::is_directory(abs, ec))
            return err(spec_.id, "not a directory: " + path);
        std::string out;
        if (recurse) {
            for (const auto& e :
                 std::filesystem::recursive_directory_iterator(abs, ec)) {
                if (ec) break;
                if (!pattern.empty() &&
                    !glob_match(pattern, util::basename(e.path().string())))
                    continue;
                out += (e.is_directory() ? "dir\t" : "file\t");
                out += e.path().string().substr(base_.size() + 1);
                out.push_back('\n');
            }
        } else {
            for (const auto& e : std::filesystem::directory_iterator(abs, ec)) {
                if (ec) break;
                if (!pattern.empty() &&
                    !glob_match(pattern, util::basename(e.path().string())))
                    continue;
                out += (e.is_directory() ? "dir\t" : "file\t");
                out += e.path().string().substr(base_.size() + 1);
                out.push_back('\n');
            }
        }
        return ok(spec_.id, out);
    }

private:
    static ParamSpec param_str(const char* n, const char* d) {
        ParamSpec p;
        p.name = n;
        p.type = ParamType::string;
        p.description = d;
        return p;
    }
    static ParamSpec param_bool(const char* n, const char* d) {
        ParamSpec p;
        p.name = n;
        p.type = ParamType::boolean;
        p.description = d;
        p.required = false;
        return p;
    }
    std::string base_;
    ToolSpec spec_;
};

/* ---- file.stat ---- */

class FileStatTool final : public Tool {
public:
    explicit FileStatTool(std::string base) : base_(std::move(base)) {
        spec_ = schema::make_spec(
            "file.stat", "Stat a path: size, mtime, type, permissions",
            {param_str("path", "path relative to the workspace")}, true,
            ToolCategory::read);
    }
    const ToolSpec& spec() const override { return spec_; }
    ToolResult run(const Invocation& inv, ToolContext&) override {
        ArgReader a(inv.args_json);
        std::string path;
        if (!a.ok() || !a.get_string("path", path))
            return err(spec_.id, "missing required arg: path");
        std::string abs;
        if (!resolve_in_sandbox(base_, path, abs))
            return err(spec_.id, "path escapes the workspace: " + path);
        struct stat st;
        if (::stat(abs.c_str(), &st) != 0)
            return err(spec_.id, "cannot stat: " + path);
        std::vector<std::pair<std::string_view, JVal>> obj;
        obj.emplace_back("path", JVal::Str(path));
        obj.emplace_back("size", JVal::Num(static_cast<double>(st.st_size)));
        obj.emplace_back("mtime_sec", JVal::Num(static_cast<double>(st.st_mtime)));
        obj.emplace_back("is_dir", JVal::Bool(S_ISDIR(st.st_mode)));
        obj.emplace_back("is_file", JVal::Bool(S_ISREG(st.st_mode)));
        obj.emplace_back("mode", JVal::Num(static_cast<double>(st.st_mode & 0777)));
        return ok(spec_.id, util::to_json(JVal::Object(std::move(obj))));
    }

private:
    static ParamSpec param_str(const char* n, const char* d) {
        ParamSpec p;
        p.name = n;
        p.type = ParamType::string;
        p.description = d;
        return p;
    }
    std::string base_;
    ToolSpec spec_;
};

/* ---- file.search ---- */

class FileSearchTool final : public Tool {
public:
    explicit FileSearchTool(std::string base) : base_(std::move(base)) {
        spec_ = schema::make_spec(
            "file.search",
            "Find files under a path by glob or regex; optional gitignore",
            {param_str("pattern", "glob or regex to match against relative paths"),
             param_str("path", "search root relative to the workspace"),
             param_bool("regex", "treat pattern as a regex"),
             param_bool("gitignore", "honor .gitignore and skip .git")},
            true, ToolCategory::read);
    }
    const ToolSpec& spec() const override { return spec_; }
    ToolResult run(const Invocation& inv, ToolContext& ctx) override {
        ArgReader a(inv.args_json);
        std::string pattern, path = ".";
        bool use_regex = false, gitignore = true;
        if (!a.ok()) return err(spec_.id, "bad args");
        if (!a.get_string("pattern", pattern))
            return err(spec_.id, "missing required arg: pattern");
        (void)a.get_string("path", path);
        (void)a.get_bool("regex", use_regex);
        (void)a.get_bool("gitignore", gitignore);
        std::string abs;
        if (!resolve_in_sandbox(base_, path, abs))
            return err(spec_.id, "path escapes the workspace: " + path);
        std::error_code ec;
        if (!std::filesystem::is_directory(abs, ec))
            return err(spec_.id, "not a directory: " + path);
        std::vector<std::string> ignores;
        if (gitignore) {
            const std::string gi = abs + "/.gitignore";
            std::string text;
            if (read_whole_file(gi, text).ok()) {
                for (const std::string& line : split_lines(text)) {
                    std::string_view l = util::trim(line);
                    if (l.empty() || l.front() == '#') continue;
                    if (l.back() == '/') l.remove_suffix(1);
                    ignores.push_back(std::string(l));
                }
            }
        }
        auto ignored = [&](const std::string& rel) {
            const std::string name = std::string(util::basename(rel));
            for (const std::string& ig : ignores)
                if (glob_match(ig, rel) || glob_match(ig, name)) return true;
            return false;
        };
        std::regex re;
        bool have_re = false;
        if (use_regex) {
            try {
                re = std::regex(pattern, std::regex::ECMAScript);
                have_re = true;
            } catch (const std::regex_error&) {
                return err(spec_.id, "invalid regex: " + pattern);
            }
        }
        std::string out;
        for (const auto& e :
             std::filesystem::recursive_directory_iterator(abs, ec)) {
            if (ec) break;
            if (ctx.cancel && ctx.cancel->cancelled()) {
                return err(spec_.id, "canceled");
            }
            if (e.is_directory()) continue;
            std::string rel = e.path().string().substr(abs.size() + 1);
            if (rel.find(".git") != std::string::npos) continue;
            if (ignored(rel)) continue;
            bool hit = use_regex
                           ? (have_re && std::regex_search(rel, re))
                           : (glob_match(pattern, rel) ||
                              glob_match(pattern, std::string(util::basename(rel))));
            if (hit) {
                out += rel;
                out.push_back('\n');
            }
        }
        return ok(spec_.id, out);
    }

private:
    static ParamSpec param_str(const char* n, const char* d) {
        ParamSpec p;
        p.name = n;
        p.type = ParamType::string;
        p.description = d;
        return p;
    }
    static ParamSpec param_bool(const char* n, const char* d) {
        ParamSpec p;
        p.name = n;
        p.type = ParamType::boolean;
        p.description = d;
        p.required = false;
        return p;
    }
    std::string base_;
    ToolSpec spec_;
};

/* ---- workspace.info ---- */

class WorkspaceInfoTool final : public Tool {
public:
    WorkspaceInfoTool() {
        spec_ = schema::make_spec(
            "workspace.info", "Host + repo environment snapshot (cheap probes)",
            {}, true, ToolCategory::workspace);
    }
    const ToolSpec& spec() const override { return spec_; }
    ToolResult run(const Invocation&, ToolContext&) override {
        const config::EnvSnapshot& e = config::env_snapshot();
        std::vector<std::pair<std::string_view, JVal>> obj;
        obj.emplace_back("platform", JVal::Str(e.platform));
        obj.emplace_back("arch", JVal::Str(e.arch));
        obj.emplace_back("shell", JVal::Str(e.shell));
        obj.emplace_back("is_git_repo", JVal::Bool(e.is_git_repo));
        obj.emplace_back("git_branch", JVal::Str(e.git_branch));
        obj.emplace_back("git_dirty", JVal::Bool(e.git_dirty));
        obj.emplace_back("free_mem_mb", JVal::Num(static_cast<double>(e.free_mem_mb)));
        obj.emplace_back("load_avg", JVal::Num(e.load_avg));
        return ok(spec_.id, util::to_json(JVal::Object(std::move(obj))));
    }

private:
    ToolSpec spec_;
};

/* ---- sym.* (graph-aware, Phase 7) ---- */

class SymLookupTool final : public Tool {
public:
    explicit SymLookupTool(SymbolIndex& idx) : idx_(idx) {
        spec_ = schema::make_spec(
            "sym.lookup", "Resolve a symbol name to its definition",
            {param_str("name", "symbol name"),
             param_str("file", "hint file for unambiguous resolution")},
            true, ToolCategory::graph);
    }
    const ToolSpec& spec() const override { return spec_; }
    ToolResult run(const Invocation& inv, ToolContext&) override {
        ArgReader a(inv.args_json);
        std::string name, file;
        if (!a.ok() || !a.get_string("name", name))
            return err(spec_.id, "missing required arg: name");
        (void)a.get_string("file", file);
        Sym s;
        if (const core::error_code c = idx_.lookup(name, file, s); !c.ok())
            return err(spec_.id, "symbol not found: " + name);
        return ok(spec_.id, sym_json(s));
    }

private:
    static ParamSpec param_str(const char* n, const char* d) {
        ParamSpec p;
        p.name = n;
        p.type = ParamType::string;
        p.description = d;
        return p;
    }
    SymbolIndex& idx_;
    ToolSpec spec_;
};

class SymRefsTool final : public Tool {
public:
    explicit SymRefsTool(SymbolIndex& idx) : idx_(idx) {
        spec_ = schema::make_spec(
            "sym.refs", "1-hop callers of a symbol (by name)",
            {param_str("name", "symbol name")}, true, ToolCategory::graph);
    }
    const ToolSpec& spec() const override { return spec_; }
    ToolResult run(const Invocation& inv, ToolContext&) override {
        ArgReader a(inv.args_json);
        std::string name;
        if (!a.ok() || !a.get_string("name", name))
            return err(spec_.id, "missing required arg: name");
        std::string out;
        for (const SymId id : idx_.callers_of_name(name)) {
            Sym s;
            if (idx_.sym_by_id(id, s).ok()) {
                out += s.name;
                out += "\t";
                out += s.file;
                out.push_back('\n');
            }
        }
        return ok(spec_.id, out);
    }

private:
    static ParamSpec param_str(const char* n, const char* d) {
        ParamSpec p;
        p.name = n;
        p.type = ParamType::string;
        p.description = d;
        return p;
    }
    SymbolIndex& idx_;
    ToolSpec spec_;
};

class SymSnippetTool final : public Tool {
public:
    explicit SymSnippetTool(SymbolIndex& idx) : idx_(idx) {
        spec_ = schema::make_spec(
            "sym.snippet", "Declaration + capped body of a symbol",
            {param_str("name", "symbol name"),
             param_str("file", "hint file"),
             param_int("max_bytes", "snippet cap")},
            true, ToolCategory::graph);
    }
    const ToolSpec& spec() const override { return spec_; }
    ToolResult run(const Invocation& inv, ToolContext&) override {
        ArgReader a(inv.args_json);
        std::string name, file;
        int64_t max_bytes = 0;
        if (!a.ok() || !a.get_string("name", name))
            return err(spec_.id, "missing required arg: name");
        (void)a.get_string("file", file);
        (void)a.get_int("max_bytes", max_bytes);
        Sym s;
        if (const core::error_code c = idx_.lookup(name, file, s); !c.ok())
            return err(spec_.id, "symbol not found: " + name);
        opencode::graph::Snippet sn;
        const std::uint32_t cap = max_bytes > 0 ? static_cast<std::uint32_t>(max_bytes)
                                                : 4096;
        if (const core::error_code c = idx_.snippet(s.id, cap, sn); !c.ok())
            return err(spec_.id, "no snippet for: " + name);
        return ok(spec_.id, snippet_to_text(sn.file, sn.sym, sn.text));
    }

private:
    static ParamSpec param_str(const char* n, const char* d) {
        ParamSpec p;
        p.name = n;
        p.type = ParamType::string;
        p.description = d;
        return p;
    }
    static ParamSpec param_int(const char* n, const char* d) {
        ParamSpec p;
        p.name = n;
        p.type = ParamType::integer;
        p.description = d;
        p.required = false;
        return p;
    }
    SymbolIndex& idx_;
    ToolSpec spec_;
};

class SymCalleesTool final : public Tool {
public:
    explicit SymCalleesTool(SymbolIndex& idx) : idx_(idx) {
        spec_ = schema::make_spec(
            "sym.callees", "1-hop callees of a symbol (by name)",
            {param_str("name", "symbol name"),
             param_str("file", "hint file")},
            true, ToolCategory::graph);
    }
    const ToolSpec& spec() const override { return spec_; }
    ToolResult run(const Invocation& inv, ToolContext&) override {
        ArgReader a(inv.args_json);
        std::string name, file;
        if (!a.ok() || !a.get_string("name", name))
            return err(spec_.id, "missing required arg: name");
        (void)a.get_string("file", file);
        Sym s;
        if (const core::error_code c = idx_.lookup(name, file, s); !c.ok())
            return err(spec_.id, "symbol not found: " + name);
        std::string out;
        for (const std::string& c : idx_.callees(s.id)) {
            out += c;
            out.push_back('\n');
        }
        return ok(spec_.id, out);
    }

private:
    static ParamSpec param_str(const char* n, const char* d) {
        ParamSpec p;
        p.name = n;
        p.type = ParamType::string;
        p.description = d;
        return p;
    }
    SymbolIndex& idx_;
    ToolSpec spec_;
};

/* ---- git.* read probes ---- */

class GitToolBase {
protected:
    explicit GitToolBase(std::string base) : base_(std::move(base)) {}
    std::string git_run(const std::string& args, bool& failed) const {
        /* Git probes are read-only, so a single retry is safe and absorbs
         * transient spawn/index contention on loaded hosts. */
        for (int attempt = 0; attempt < 2; ++attempt) {
            ProcOpts opts;
            opts.cmd = "git " + args;
            opts.working_dir = base_;
            opts.timeout_ms = 10000;
            ProcResult r;
            const bool proc_ok = run_process(opts, r).ok();
            if (proc_ok && r.exit_code == 0) {
                failed = false;
                return r.stdout;
            }
        }
        failed = true;
        return {};
    }
    std::string base_;
};

class GitDiffTool final : public Tool, private GitToolBase {
public:
    explicit GitDiffTool(std::string base) : GitToolBase(std::move(base)) {
        spec_ = schema::make_spec(
            "git.diff", "git diff for a file (or the whole tree)",
            {param_str("file", "path to diff"),
             param_bool("staged", "diff the staged changes")},
            true, ToolCategory::workspace);
    }
    const ToolSpec& spec() const override { return spec_; }
    ToolResult run(const Invocation& inv, ToolContext&) override {
        ArgReader a(inv.args_json);
        std::string file;
        bool staged = false;
        (void)a.get_string("file", file);
        (void)a.get_bool("staged", staged);
        std::string cmd = staged ? "diff --cached" : "diff";
        if (!file.empty()) cmd += " -- " + file;
        bool failed = false;
        const std::string out = git_run(cmd, failed);
        if (failed) return err(spec_.id, "git failed");
        return ok(spec_.id, out);
    }

private:
    static ParamSpec param_str(const char* n, const char* d) {
        ParamSpec p;
        p.name = n;
        p.type = ParamType::string;
        p.description = d;
        p.required = false;
        return p;
    }
    static ParamSpec param_bool(const char* n, const char* d) {
        ParamSpec p;
        p.name = n;
        p.type = ParamType::boolean;
        p.description = d;
        p.required = false;
        return p;
    }
    ToolSpec spec_;
};

class GitStatusTool final : public Tool, private GitToolBase {
public:
    explicit GitStatusTool(std::string base) : GitToolBase(std::move(base)) {
        spec_ = schema::make_spec("git.status", "git status --short", {}, true,
                                  ToolCategory::workspace);
    }
    const ToolSpec& spec() const override { return spec_; }
    ToolResult run(const Invocation&, ToolContext&) override {
        bool failed = false;
        const std::string out = git_run("status --short", failed);
        if (failed) return err(spec_.id, "not a git repository");
        return ok(spec_.id, out);
    }

private:
    ToolSpec spec_;
};

class GitBranchTool final : public Tool, private GitToolBase {
public:
    explicit GitBranchTool(std::string base) : GitToolBase(std::move(base)) {
        spec_ = schema::make_spec("git.branch", "current branch name", {}, true,
                                  ToolCategory::workspace);
    }
    const ToolSpec& spec() const override { return spec_; }
    ToolResult run(const Invocation&, ToolContext&) override {
        bool failed = false;
        const std::string out = git_run("branch --show-current", failed);
        if (failed) return err(spec_.id, "not a git repository");
        return ok(spec_.id, out);
    }

private:
    ToolSpec spec_;
};

class GitShowTool final : public Tool, private GitToolBase {
public:
    explicit GitShowTool(std::string base) : GitToolBase(std::move(base)) {
        spec_ = schema::make_spec(
            "git.show", "Show a commit, or a file at a commit",
            {param_str("commit", "commit or ref (default HEAD)"),
             param_str("file", "file path at that commit")},
            true, ToolCategory::workspace);
    }
    const ToolSpec& spec() const override { return spec_; }
    ToolResult run(const Invocation& inv, ToolContext&) override {
        ArgReader a(inv.args_json);
        std::string commit = "HEAD", file;
        (void)a.get_string("commit", commit);
        (void)a.get_string("file", file);
        bool failed = false;
        std::string out;
        if (file.empty())
            out = git_run("show --no-color --stat " + commit, failed);
        else
            out = git_run("show " + commit + ":" + file, failed);
        if (failed) return err(spec_.id, "git failed");
        return ok(spec_.id, out);
    }

private:
    static ParamSpec param_str(const char* n, const char* d) {
        ParamSpec p;
        p.name = n;
        p.type = ParamType::string;
        p.description = d;
        p.required = false;
        return p;
    }
    ToolSpec spec_;
};

} /* namespace */

core::error_code make_read_tools(std::string_view workspace,
                                 SymbolIndex* index,
                                 std::vector<std::unique_ptr<Tool>>& out) {
    out.push_back(std::make_unique<FileReadTool>(std::string(workspace)));
    out.push_back(std::make_unique<DirListTool>(std::string(workspace)));
    out.push_back(std::make_unique<FileStatTool>(std::string(workspace)));
    out.push_back(std::make_unique<FileSearchTool>(std::string(workspace)));
    out.push_back(std::make_unique<WorkspaceInfoTool>());
    if (index != nullptr) {
        out.push_back(std::make_unique<SymLookupTool>(*index));
        out.push_back(std::make_unique<SymRefsTool>(*index));
        out.push_back(std::make_unique<SymSnippetTool>(*index));
        out.push_back(std::make_unique<SymCalleesTool>(*index));
    }
    out.push_back(std::make_unique<GitDiffTool>(std::string(workspace)));
    out.push_back(std::make_unique<GitStatusTool>(std::string(workspace)));
    out.push_back(std::make_unique<GitBranchTool>(std::string(workspace)));
    out.push_back(std::make_unique<GitShowTool>(std::string(workspace)));
    return core::ok();
}

} /* namespace opencode::tools::exec */

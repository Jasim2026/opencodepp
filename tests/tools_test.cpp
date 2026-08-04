// tools_test.cpp -- Phase 8: tool runtime.
// Suites: schema generation (provider-native, golden cross-validation),
// path safety, read tools, patch/write tools (apply/reverse round-trip),
// git + workspace + sym tools, shell (timeout/streaming/cancel). Runs from the
// repo root; every file mutation happens inside a per-process /tmp sandbox.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#include "graph/index.h"
#include "tools/exec/patch.h"
#include "tools/exec/util.h"
#include "tools/registry.h"
#include "tools/schema.h"
#include "util/json.h"
#include "util/path.h"

namespace {
int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #cond);                                             \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

using opencode::tools::ParamSpec;
using opencode::tools::ParamType;
using opencode::tools::ToolCategory;
using opencode::tools::ToolContext;
using opencode::tools::ToolResult;
using opencode::tools::ToolSpec;
using opencode::tools::ToolStatus;
using opencode::tools::Invocation;
using opencode::tools::schema::make_spec;
using opencode::tools::schema::params_schema;
using opencode::tools::schema::tools_json;
using opencode::util::JVal;
using opencode::util::join;
using opencode::util::parse_json;
using opencode::util::to_json;
namespace patch = opencode::tools::exec::patch;

std::string read_file(const std::string& path) {
    std::string out;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
            out.append(buf, n);
        std::fclose(f);
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

std::string read_file_raw(const std::string& path) {
    std::string out;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
            out.append(buf, n);
        std::fclose(f);
    }
    return out;
}

void write_file(const std::string& path, const std::string& content) {
    const char* slash = std::strrchr(path.c_str(), '/');
    if (slash != nullptr && slash != path.c_str()) {
        const std::string dir(path.c_str(),
                              static_cast<size_t>(slash - path.c_str()));
        const int rc = ::mkdir(dir.c_str(), 0755);
        (void)rc; /* EEXIST is fine */
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr);
    if (!f) return;
    std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
}

std::string g_sandbox;

std::string json_escape(const char* text) {
    std::string out;
    for (size_t i = 0; text[i]; ++i) {
        if (text[i] == '\n' || text[i] == '"' || text[i] == '\\') {
            out.push_back('\\');
            out.push_back(text[i] == '\n' ? 'n' : text[i]);
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

void sandbox_init() {
    g_sandbox = "/tmp/opencode_tools_sandbox_" + std::to_string(getpid());
    std::filesystem::remove_all(g_sandbox);
    std::filesystem::create_directories(g_sandbox);
    write_file(g_sandbox + "/README", "sandbox root\n");
}
void sandbox_cleanup() { std::filesystem::remove_all(g_sandbox); }
std::string sb(const std::string& rel) {
    return opencode::util::join(g_sandbox, rel);
}

ParamSpec iparam(const std::string& name) {
    ParamSpec p;
    p.name = name;
    p.type = ParamType::integer;
    return p;
}

/* The `add(a:int, b:int)` tool exactly as the golden fixtures pin it. */
ToolSpec golden_add_spec() {
    return make_spec("add", "Add two numbers",
                     {iparam("a"), iparam("b")}, true, ToolCategory::read);
}

std::string fixture_tools_json(const std::string& fixture_path) {
    const std::string text = read_file(fixture_path);
    JVal doc;
    if (!parse_json(text, doc).ok() || !doc.find("tools")) return "";
    const JVal* tools = doc.find("tools");
    return to_json(*tools);
}

ToolResult run_tool(opencode::tools::ToolRegistry& reg, const char* name,
                    const char* args, ToolContext* ctx = nullptr) {
    Invocation inv;
    inv.tool_name = name;
    inv.args_json = args;
    ToolContext local;
    ToolContext& c = ctx ? *ctx : local;
    return reg.run(name, inv, c);
}

bool result_ok(const ToolResult& r) {
    return r.status == ToolStatus::ok;
}
bool result_err(const ToolResult& r) {
    return r.status == ToolStatus::error && r.content_is_error;
}

/* ---- schema ---- */

void test_params_schema() {
    /* all-required compact form -- byte-for-byte the golden fixture */
    const std::string want =
        "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"integer\"},"
        "\"b\":{\"type\":\"integer\"}}}";
    CHECK(params_schema({iparam("a"), iparam("b")}) == want);

    /* optional param emits `required` */
    ParamSpec opt = iparam("end");
    opt.required = false;
    const std::string s2 = params_schema({iparam("start"), opt});
    CHECK(s2.find("\"required\":[\"start\"]") != std::string::npos);
    CHECK(s2.find("\"end\":{\"type\":\"integer\"}") != std::string::npos);

    /* string_array / enum / default / description */
    ParamSpec pat = iparam("pattern");
    pat.type = ParamType::string;
    pat.enum_values = {"*.c", "*.cpp"};
    pat.default_json = "\"*.c\"";
    pat.description = "glob to match";
    const std::string s3 = params_schema({pat});
    CHECK(s3.find("\"enum\":[\"*.c\",\"*.cpp\"]") != std::string::npos);
    CHECK(s3.find("\"default\":\"*.c\"") != std::string::npos);
    CHECK(s3.find("\"description\":\"glob to match\"") != std::string::npos);

    ParamSpec arr = iparam("paths");
    arr.type = ParamType::string_array;
    const std::string s4 = params_schema({arr});
    CHECK(s4.find("\"type\":\"array\"") != std::string::npos);
    CHECK(s4.find("\"items\":{\"type\":\"string\"}") != std::string::npos);

    /* empty params -> object with just type */
    CHECK(params_schema({}) == "{\"type\":\"object\"}");
}

void test_schema_cross_provider() {
    const ToolSpec add = golden_add_spec();
    const std::vector<ToolSpec> tools{add};

    /* golden fixtures pin the exact provider shapes */
    CHECK(tools_json(tools, "anthropic") ==
          fixture_tools_json("tests/fixtures/responses/anthropic_request.json"));
    CHECK(tools_json(tools, "openai") ==
          fixture_tools_json("tests/fixtures/responses/openai_request.json"));
    CHECK(tools_json(tools, "openai_compat") ==
          fixture_tools_json("tests/fixtures/responses/openai_compat_request.json"));
    CHECK(tools_json(tools, "google") ==
          fixture_tools_json("tests/fixtures/responses/google_request.json"));

    const std::string params =
        "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"integer\"},"
        "\"b\":{\"type\":\"integer\"}}}";
    CHECK(add.params_schema == params);

    std::printf("  schema bytes: openai=%zu anthropic=%zu google=%zu\n",
                tools_json(tools, "openai").size(),
                tools_json(tools, "anthropic").size(),
                tools_json(tools, "google").size());
}

/* ---- path safety ---- */

void test_path_safety() {
    using opencode::tools::exec::resolve_in_sandbox;
    std::string out;
    CHECK(resolve_in_sandbox("/ws", "a/b.txt", out) && out == "/ws/a/b.txt");
    CHECK(resolve_in_sandbox("/ws", "a/./b/../c.txt", out) &&
          out == "/ws/a/c.txt");
    CHECK(!resolve_in_sandbox("/ws", "", out));
    CHECK(!resolve_in_sandbox("/ws", "/etc/passwd", out));
    CHECK(!resolve_in_sandbox("/ws", "../etc/passwd", out));
    CHECK(!resolve_in_sandbox("/ws", "a/../../etc/passwd", out));
    CHECK(resolve_in_sandbox("/ws", "a/b/../../x", out) && out == "/ws/x");
    std::printf("  path safety: OK\n");
}

/* ---- read tools ---- */

void test_read_tools() {
    write_file(sb("a.txt"), "alpha\nbeta\ngamma\n");
    write_file(sb("sub/b.go"), "package b\n");
    write_file(sb("sub/c.txt"), "charlie\n");

    opencode::tools::ToolRegistry reg;
    opencode::tools::RegistryOptions opts;
    opts.workspace = g_sandbox;
    CHECK(opencode::tools::register_defaults(reg, opts).ok());

    /* file.read: whole, range, max_bytes */
    ToolResult r = run_tool(reg, "file.read", "{\"path\":\"a.txt\"}");
    CHECK(result_ok(r) && r.content == "alpha\nbeta\ngamma\n");
    r = run_tool(reg, "file.read",
                 "{\"path\":\"a.txt\",\"start\":6,\"end\":10}");
    CHECK(result_ok(r) && r.content == "beta");
    r = run_tool(reg, "file.read",
                 "{\"path\":\"a.txt\",\"max_bytes\":5}");
    CHECK(result_ok(r) && r.content == "alpha");

    /* dir.list: flat + recursive + pattern */
    r = run_tool(reg, "dir.list", "{\"path\":\".\"}");
    CHECK(result_ok(r));
    CHECK(r.content.find("file\tREADME") != std::string::npos);
    CHECK(r.content.find("a.txt") != std::string::npos);
    CHECK(r.content.find("sub") != std::string::npos);
    r = run_tool(reg, "dir.list",
                 "{\"path\":\".\",\"recurse\":true,\"pattern\":\"*.txt\"}");
    CHECK(result_ok(r));
    CHECK(r.content.find("c.txt") != std::string::npos);
    CHECK(r.content.find("b.go") == std::string::npos);

    /* file.stat */
    r = run_tool(reg, "file.stat", "{\"path\":\"a.txt\"}");
    CHECK(result_ok(r) && r.content.find("\"size\":17") != std::string::npos);
    CHECK(r.content.find("\"is_file\":true") != std::string::npos);

    /* file.search: glob, then regex */
    r = run_tool(reg, "file.search",
                 "{\"pattern\":\"*.txt\",\"path\":\".\"}");
    CHECK(result_ok(r));
    CHECK(r.content.find("a.txt") != std::string::npos);
    CHECK(r.content.find("c.txt") != std::string::npos);
    CHECK(r.content.find("b.go") == std::string::npos);
    r = run_tool(reg, "file.search",
                 "{\"pattern\":\"^sub/.*\\\\.go$\",\"path\":\".\",\"regex\":true}");
    CHECK(result_ok(r) && r.content.find("sub/b.go") != std::string::npos);

    /* file.search honors .gitignore and .git */
    write_file(sb(".gitignore"), "*.o\nbuild/\n");
    write_file(sb("obj.o"), "x");
    write_file(sb("build/x.o"), "y");
    write_file(sb("src/keep.txt"), "z");
    r = run_tool(reg, "file.search",
                 "{\"pattern\":\"*\",\"path\":\".\",\"gitignore\":true}");
    CHECK(result_ok(r));
    CHECK(r.content.find("src/keep.txt") != std::string::npos);
    CHECK(r.content.find("obj.o") == std::string::npos);
    CHECK(r.content.find("build/x.o") == std::string::npos);

    /* path escapes are refused by every file tool */
    r = run_tool(reg, "file.read", "{\"path\":\"../etc/passwd\"}");
    CHECK(result_err(r));
    CHECK(r.content.find("escapes") != std::string::npos);
    r = run_tool(reg, "file.write",
                 "{\"path\":\"../../root/pwn\",\"content\":\"x\"}");
    CHECK(result_err(r));
    std::printf("  read tools: OK\n");
}

/* ---- patch + write ---- */

const char* kDoc =
    "line1\nline2\nline3\nline4\nline5\nline6\nline7\nline8\nline9\nline10\n";

const char* kPatch =
    "--- a/doc.txt\n+++ b/doc.txt\n"
    "@@ -1,4 +1,4 @@\n"
    " line1\n"
    "-line2\n"
    "+line2 modified\n"
    " line3\n"
    " line4\n"
    "@@ -8,3 +8,3 @@\n"
    " line8\n"
    "-line9\n"
    "+line9 modified\n"
    " line10\n";

const char* kPatchApplied =
    "line1\nline2 modified\nline3\nline4\nline5\nline6\nline7\nline8\n"
    "line9 modified\nline10\n";

void test_patch_round_trip() {
    std::vector<patch::Hunk> hunks;
    CHECK(patch::parse(kPatch, hunks).ok());
    CHECK(hunks.size() == 2);
    std::string applied;
    CHECK(patch::apply(hunks, kDoc, applied).ok());
    CHECK(applied == kPatchApplied);

    /* apply reverse hunks to restore the original */
    std::string rev_patch;
    CHECK(patch::reverse(kPatch, rev_patch).ok());
    std::vector<patch::Hunk> rev_hunks;
    CHECK(patch::parse(rev_patch, rev_hunks).ok());
    std::string restored;
    CHECK(patch::apply(rev_hunks, applied, restored).ok());
    CHECK(restored == kDoc);

    /* context mismatch -> clean error, no partial apply */
    const std::string bad =
        "@@ -1,2 +1,2 @@\n no such line\n-line2\n+line2 modified\n";
    std::vector<patch::Hunk> bh;
    CHECK(patch::parse(bad, bh).ok());
    std::string out;
    CHECK(!patch::apply(bh, kDoc, out).ok());

    /* bench: 1000 applied hunks over a 10k-line buffer */
    const std::string big = [] {
        std::string s;
        for (int i = 0; i < 10000; ++i) s += "x" + std::to_string(i) + "\n";
        return s;
    }();
    std::string patch1000;
    for (int i = 0; i < 1000; ++i) {
        patch1000 += "@@ -" + std::to_string(i * 10 + 1) + " +" +
                     std::to_string(i * 10 + 1) + " @@\n";
        patch1000 += " x" + std::to_string(i * 10) + "\n";
        patch1000 += "-x" + std::to_string(i * 10 + 1) + "\n";
        patch1000 += "+XX\n";
        patch1000 += " x" + std::to_string(i * 10 + 2) + "\n";
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<patch::Hunk> hh;
    CHECK(patch::parse(patch1000, hh).ok());
    std::string big_out;
    CHECK(patch::apply(hh, big, big_out).ok());
    const auto t1 = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    CHECK(big_out.find("XX") != std::string::npos);
    std::printf("  patch bench: 1000 hunks / 10k lines = %lld ms\n",
                static_cast<long long>(ms));
    std::printf("  patch round-trip: OK\n");
}

void test_write_tools() {
    opencode::tools::ToolRegistry reg;
    opencode::tools::RegistryOptions opts;
    opts.workspace = g_sandbox;
    CHECK(opencode::tools::register_defaults(reg, opts).ok());

    /* file.write to a new file */
    ToolResult r = run_tool(reg, "file.write",
                            "{\"path\":\"new.txt\",\"content\":\"hello\"}");
    CHECK(result_ok(r));
    CHECK(read_file(sb("new.txt")) == "hello");

    /* overwrite_guard refuses to clobber */
    r = run_tool(reg, "file.write",
                 "{\"path\":\"new.txt\",\"content\":\"over\"}");
    CHECK(result_err(r) && r.content.find("overwrite") != std::string::npos);
    CHECK(read_file(sb("new.txt")) == "hello");

    /* explicit overwrite succeeds */
    r = run_tool(reg, "file.write",
                 "{\"path\":\"new.txt\",\"content\":\"over\","
                 "\"overwrite_guard\":false}");
    CHECK(result_ok(r));
    CHECK(read_file(sb("new.txt")) == "over");

    /* create=false requires an existing file */
    r = run_tool(reg, "file.write",
                 "{\"path\":\"ghost.txt\",\"content\":\"x\",\"create\":false}");
    CHECK(result_err(r));

    /* failed write leaves the target untouched */
    write_file(sb("guard"), "keep");
    r = run_tool(reg, "file.write",
                 "{\"path\":\"guard/sub\",\"content\":\"boom\"}");
    CHECK(result_err(r));
    CHECK(read_file(sb("guard")) == "keep");

    /* file.patch: apply then reverse via the tool */
    write_file(sb("doc.txt"), kDoc);
    const std::string patch_arg = json_escape(kPatch);
    r = run_tool(reg, "file.patch",
                 (std::string("{\"path\":\"doc.txt\",\"patch\":\"") +
                  patch_arg + "\"}").c_str());
    CHECK(result_ok(r));
    CHECK(read_file_raw(sb("doc.txt")) == kPatchApplied);

    r = run_tool(reg, "file.patch",
                 (std::string("{\"path\":\"doc.txt\",\"patch\":\"") +
                  patch_arg + "\",\"reverse\":true}").c_str());
    CHECK(result_ok(r));
    CHECK(read_file_raw(sb("doc.txt")) == kDoc);

    /* context mismatch -> clean error, file unchanged */
    const char* bad =
        "@@ -1,2 +1,2 @@\n no such line\n-line2\n+line2 modified\n";
    const std::string bad_arg = json_escape(bad);
    r = run_tool(reg, "file.patch",
                 (std::string("{\"path\":\"doc.txt\",\"patch\":\"") +
                  bad_arg + "\"}").c_str());
    CHECK(result_err(r));
    CHECK(read_file_raw(sb("doc.txt")) == kDoc);
    std::printf("  write tools: OK\n");
}

/* ---- workspace + git + sym ---- */

void test_workspace_info() {
    opencode::tools::ToolRegistry reg;
    opencode::tools::RegistryOptions opts;
    opts.workspace = g_sandbox;
    CHECK(opencode::tools::register_defaults(reg, opts).ok());
    ToolResult r = run_tool(reg, "workspace.info", "{}");
    CHECK(result_ok(r));
    JVal doc;
    CHECK(parse_json(r.content, doc).ok());
    const JVal* platform = doc.find("platform");
    CHECK(platform != nullptr && !platform->str.empty());
    CHECK(doc.find("is_git_repo") != nullptr);
    std::printf("  workspace.info: OK\n");
}

void test_git_tools() {
    const std::string repo = sb("gitrepo");
    std::filesystem::create_directories(repo);
    using opencode::tools::exec::ProcOpts;
    using opencode::tools::exec::ProcResult;
    auto sh = [&](const std::string& cmd) {
        ProcOpts o;
        o.cmd = cmd;
        o.working_dir = repo;
        o.timeout_ms = 10000;
        ProcResult r;
        CHECK(opencode::tools::exec::run_process(o, r).ok());
        CHECK(r.exit_code == 0);
    };
    sh("git init -q");
    sh("git -c user.email=t@t -c user.name=t commit --allow-empty -qm seed");
    write_file(repo + "/f.txt", "v1\n");
    sh("git add f.txt");
    sh("git -c user.email=t@t -c user.name=t commit -qm init");

    opencode::tools::ToolRegistry reg;
    opencode::tools::RegistryOptions opts;
    opts.workspace = repo;
    CHECK(opencode::tools::register_defaults(reg, opts).ok());

    /* clean status, branch, show */
    ToolResult r = run_tool(reg, "git.status", "{}");
    CHECK(result_ok(r));
    CHECK(r.content.find("f.txt") == std::string::npos);
    r = run_tool(reg, "git.branch", "{}");
    CHECK(result_ok(r) && !r.content.empty());
    r = run_tool(reg, "git.show", "{\"file\":\"f.txt\"}");
    CHECK(result_ok(r) && r.content.find("v1") != std::string::npos);

    /* dirty status + diff */
    write_file(repo + "/f.txt", "v2\n");
    r = run_tool(reg, "git.status", "{}");
    CHECK(result_ok(r) && r.content.find("f.txt") != std::string::npos);
    r = run_tool(reg, "git.diff", "{\"file\":\"f.txt\"}");
    CHECK(result_ok(r));
    CHECK(r.content.find("-v1") != std::string::npos);
    CHECK(r.content.find("+v2") != std::string::npos);
    std::printf("  git tools: OK\n");
}

void test_sym_tools() {
    opencode::graph::SymbolIndex idx;
    CHECK(idx.ensure_indexed("tests/fixtures/workspace/math.c").ok());
    CHECK(idx.ensure_indexed("tests/fixtures/workspace/util.cpp").ok());
    CHECK(idx.ensure_indexed("tests/fixtures/workspace/main.go").ok());

    opencode::tools::ToolRegistry reg;
    opencode::tools::RegistryOptions opts;
    opts.workspace = "tests/fixtures/workspace";
    opts.graph = &idx;
    CHECK(opencode::tools::register_defaults(reg, opts).ok());

    ToolResult r = run_tool(reg, "sym.lookup", "{\"name\":\"add\"}");
    CHECK(result_ok(r));
    CHECK(r.content.find("\"kind\":\"function\"") != std::string::npos);

    r = run_tool(reg, "sym.callees", "{\"name\":\"apply\"}");
    CHECK(result_ok(r) && r.content.find("twice") != std::string::npos);

    r = run_tool(reg, "sym.refs", "{\"name\":\"add\"}");
    CHECK(result_ok(r) && r.content.find("twice") != std::string::npos);

    r = run_tool(reg, "sym.snippet", "{\"name\":\"twice\"}");
    CHECK(result_ok(r) && r.content.find("twice") != std::string::npos);

    r = run_tool(reg, "sym.lookup", "{\"name\":\"does_not_exist_zz\"}");
    CHECK(result_err(r));
    std::printf("  sym tools: OK\n");
}

} /* namespace */

int main() {
    std::printf("tools_test\n");
    sandbox_init();
    test_params_schema();
    test_schema_cross_provider();
    test_path_safety();
    test_read_tools();
    test_patch_round_trip();
    test_write_tools();
    test_workspace_info();
    test_git_tools();
    test_sym_tools();
    sandbox_cleanup();
    if (failures == 0) {
        std::printf("tools_test: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::printf("tools_test: %d FAILURE(s)\n", failures);
    return EXIT_FAILURE;
}

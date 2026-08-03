// bench_prompt.cpp -- Phase 6: print context sizes for representative sessions
// (feeds T1: context per task <= 3,500 tokens; request bytes vs Go baseline).
//
// Loads src/prompt/templates and assembles a few edge-profile contexts,
// printing per-case estimated tokens, bytes, tiers, and cut events. Run from
// the repo root (or pass the repo root as argv[1]):
//   ./bench_prompt            # uses "."
//   ./bench_prompt /path/repo
#define _POSIX_C_SOURCE 200809L

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.h"
#include "msg/message.h"
#include "prompt/context.h"
#include "prompt/registry.h"
#include "util/json.h"

namespace {

using namespace opencode;
using namespace opencode::core;
using namespace opencode::msg;
using namespace opencode::provider;
using namespace opencode::prompt;
using opencode::util::JVal;

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

/* The fixture-driven contexts (same corpus as prompt_test). */
void bench_fixtures(const std::string& root, const PromptRegistry& reg) {
    const char* files[] = {
        "tests/fixtures/prompts/edge_fix.json",
        "tests/fixtures/prompts/code_refactor.json",
        "tests/fixtures/prompts/big_history_trim.json",
    };
    for (const char* f : files) {
        const std::string path = root + "/" + f;
        JVal doc;
        if (!parse_json(read_file(path), doc).ok()) continue;
        const std::string name =
            doc.find("name") ? std::string(doc.find("name")->str) : path;

        MsgList hist;
        for (const JVal& m : doc.find("messages")->arr) {
            Message msg;
            msg.id = m.find("id") ? std::string(m.find("id")->str) : "";
            msg.role = m.find("role")->str == "assistant" ? Role::assistant
                                                          : Role::user;
            msg.parts.push_back(Text{std::string(m.find("text")->str)});
            hist.push_back(std::move(msg));
        }
        ToolsSpec tools;
        if (const JVal* t = doc.find("tool")) {
            ToolSpec ts;
            ts.id = std::string(t->find("id")->str);
            ts.name = std::string(t->find("name")->str);
            ts.description = std::string(t->find("description")->str);
            ts.input_schema_json = std::string(t->find("input_schema")->str);
            tools.push_back(std::move(ts));
        }

        ContextInput in;
        in.registry = &reg;
        in.messages = &hist;
        if (!tools.empty()) in.tools = &tools;
        if (const JVal* v = doc.find("available_tokens"))
            in.available_tokens = (uint32_t)v->num;
        ContextPlan p;
        if (!assemble_context(in, p).ok()) {
            std::printf("%-22s assemble FAILED\n", name.c_str());
            continue;
        }
        std::printf("%-22s tokens=%u target=%u bytes=%zu msgs=%zu "
                    "tool_tokens=%u cuts=%zu under=%d\n",
                    name.c_str(), p.estimated_tokens,
                    in.target_tokens, p.bytes, p.messages.size(),
                    p.tool_tokens, p.events.size(),
                    p.under_target ? 1 : 0);
    }
}

} /* namespace */

int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    PromptRegistry reg;
    if (const error_code c = load_templates(root + "/src/prompt/templates", reg);
        !c.ok()) {
        std::fprintf(stderr,
                     "bench_prompt: cannot load templates (run from the repo "
                     "root, or pass it as argv[1]): %s\n",
                     std::string(c.message()).c_str());
        return EXIT_FAILURE;
    }
    std::printf("# bench_prompt -- template tokens\n");
    std::printf("templates loaded: %zu\n", reg.ids().size());
    for (const std::string& id : reg.ids()) {
        const PromptRef* ref = reg.find(id);
        std::printf("  template %-20s tokens=%-5u sha1=%.12s\n",
                    id.c_str(), ref->estimated_tokens, ref->sha1.c_str());
    }
    std::printf("# context assemblies\n");
    bench_fixtures(root, reg);
    return EXIT_SUCCESS;
}

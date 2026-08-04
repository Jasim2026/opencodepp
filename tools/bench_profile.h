// bench_profile.h -- the Phase 13 T2 profile micro-benchmarks (shared).
//
// One implementation, two front ends: `tools/bench_engine --profile` prints
// the table; `tools/measure` runs the same measurements and asserts the
// budgets. Header-only inline so both TUs stay single-file. All functions
// never throw; each returns the best-of-trials wall time in MICROSECONDS per
// operation, or -1 when a fixture could not be built (e.g. prompt templates
// missing because the tool was not run from the repo root).
#ifndef OPENCODE_TOOLS_BENCH_PROFILE_H
#define OPENCODE_TOOLS_BENCH_PROFILE_H

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agent/intent.h"
#include "app.hpp"
#include "core/channel.h"
#include "core/error.h"
#include "measure_common.h"
#include "msg/message.h"
#include "msg/part.h"
#include "prompt/context.h"
#include "prompt/registry.h"
#include "provider/provider.h"
#include "tools/registry.h"
#include "tools/tool.h"
#include "verify/gate.h"

namespace opencode::profile {

using opencode::core::error_code;

/* ---- intent classification ---- */

inline double bench_intent(int trials) {
    const char* turns[] = {
        "Fix the failing test in src/core/loop.cpp, it crashes on startup.",
        "What does the channel do in this codebase?",
        "Refactor tools/registry.cpp to use unique_ptr everywhere.",
        "Run the tests and report.",
    };
    return measure::min_us(
        [&]() {
            volatile int sink = 0;
            for (int i = 0; i < 2000; ++i)
                sink += static_cast<int>(
                    agent::classify_intent(turns[i % 4]).intent);
            (void)sink;
        },
        trials) / 2000.0;
}

/* ---- context assembly ---- */

struct ContextFixture {
    prompt::PromptRegistry reg;
    provider::MsgList history;
    bool ready = false;

    ContextFixture() {
        for (const char* dir :
             {"src/prompt/templates", "../src/prompt/templates",
              "opencodepp/src/prompt/templates"}) {
            if (prompt::load_templates(dir, reg).ok()) {
                ready = true;
                break;
            }
        }
        if (!ready) return;
        msg::Message u1, a1, u2;
        u1.role = msg::Role::user;
        u1.parts.push_back(msg::Text{"Hello, help me understand the codebase."});
        a1.role = msg::Role::assistant;
        a1.parts.push_back(msg::Text{"The codebase is C++20. Ask away."});
        u2.role = msg::Role::user;
        u2.parts.push_back(msg::Text{"Now fix the bug in core/loop.cpp."});
        history.push_back(std::move(u1));
        history.push_back(std::move(a1));
        history.push_back(std::move(u2));
    }
};

inline double bench_context(const ContextFixture& fx, int trials) {
    if (!fx.ready) return -1.0;
    prompt::ContextInput in;
    in.registry = &fx.reg;
    in.messages = &fx.history;
    in.context_window = 128'000;
    return measure::min_us(
        [&]() {
            prompt::ContextPlan plan;
            for (int i = 0; i < 20; ++i) {
                const error_code ec = prompt::assemble_context(in, plan);
                if (!ec.ok()) std::abort();
            }
        },
        trials) / 20.0;
}

/* ---- tool dispatch (pure overhead path: lookup + arg parse + dispatch) ---- */

struct NoopTool final : public tools::Tool {
    tools::ToolSpec spec_;
    NoopTool() {
        spec_.id = "noop";
        spec_.name = "noop";
        spec_.description = "measurement no-op tool";
        spec_.params_schema = "{\"type\":\"object\",\"properties\":{}}";
    }
    const tools::ToolSpec& spec() const override { return spec_; }
    tools::ToolResult run(const tools::Invocation&,
                          tools::ToolContext&) override {
        tools::ToolResult r;
        r.tool_id = spec_.id;
        r.content = "noop";
        return r;
    }
};

inline double bench_dispatch(int trials) {
    tools::ToolRegistry reg;
    if (!reg.add(std::make_unique<NoopTool>()).ok()) return -1.0;
    tools::Invocation inv;
    inv.tool_name = "noop";
    inv.args_json = "{}";
    tools::ToolContext tc;
    return measure::min_us(
        [&]() {
            for (int i = 0; i < 5000; ++i) {
                const tools::ToolResult r = reg.run("noop", inv, tc);
                if (r.status != tools::ToolStatus::ok) std::abort();
            }
        },
        trials) / 5000.0;
}

/* ---- verify gate ---- */

inline double bench_gate(int trials) {
    verify::Gate gate;
    verify::Context ctx;
    const verify::EditProposal good{
        "file.write", "", "f.cpp", "", "int f() { return 0; }\n", ""};
    return measure::min_us(
        [&]() {
            for (int i = 0; i < 20; ++i) {
                const std::vector<verify::GateResult> rs = gate.run_all(good, ctx);
                if (rs.empty() || !rs.back().pass) std::abort();
            }
        },
        trials) / 20.0;
}

/* ---- event emit (channel push+pop) ---- */

inline double bench_event_emit(int trials) {
    core::Channel ch(256);
    static const char kPayload[64] = {
        "phase=fuzz detail=frame1234 payload=xxxxxxxxxxxxxxx"};
    return measure::min_us(
        [&]() {
            core::Channel::Message out;
            for (int i = 0; i < 5000; ++i) {
                if (!ch.try_push(7u, kPayload,
                                 static_cast<uint32_t>(sizeof kPayload)))
                    std::abort();
                if (ch.try_pop(out) != core::Channel::kOk) std::abort();
            }
        },
        trials) / 5000.0;
}

/* ---- engine init + memory ---- */

struct InitResult {
    double min_init_ms = -1.0;
    long rss_before_kb = -1;
    long rss_after_kb = -1;
};

inline InitResult bench_init(int trials, const std::string& workspace) {
    InitResult r;
    const std::string rm = "rm -rf " + workspace + " && mkdir -p " + workspace;
    if (::system(rm.c_str()) != 0) {
        /* best-effort; a fresh workspace is a nice-to-have, not a gate */
    }
    config::Config cfg;
    measure::trim_allocator();
    r.rss_before_kb = measure::rss_kb_min();
    r.min_init_ms = measure::min_us(
        [&]() {
            app::Engine e;
            const error_code ec =
                e.configure(cfg, workspace, "", tools::Policy::allow);
            if (!ec.ok()) std::abort();
        },
        trials) / 1000.0;
    measure::trim_allocator();
    r.rss_after_kb = measure::rss_kb_min();
    return r;
}

} /* namespace opencode::profile */

#endif /* OPENCODE_TOOLS_BENCH_PROFILE_H */

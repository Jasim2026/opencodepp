// bench_main.cpp -- benchmark runner skeleton (Phase 0).
// Benchmarks register here; the T1/T2 measurement harness grows over phases.
// Usage: bench_engine [--quick] [--profile]
//   --quick    only the fast subset (no soak/network)
//   --profile  run the Phase 13 T2 profile micro-benchmarks (init, RSS, the
//              five latency budgets) and print the table, then exit
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "bench_profile.h"
#include "core/arena.h"
#include "measure_common.h"
#include "msg/codec.h"
#include "msg/message.h"
#include "msg/part.h"
#include "msg/tokens.h"
#include "util/json.h"

namespace {

using Clock = std::chrono::steady_clock;

/* Timing helper: elapsed microseconds of a callable. */
template <typename F>
double measure_us(F&& f) {
    const auto t0 = Clock::now();
    f();
    const auto t1 = Clock::now();
    return std::chrono::duration<double, std::micro>(t1 - t0).count();
}

int num_benchmarks = 0; /* incremented by REGISTER_BENCH below (per TU) */

struct Bench {
    const char* name;
    void (*fn)();
};
[[maybe_unused]] Bench benches[32]; /* registry; filled as benchmarks register */

#define REGISTER_BENCH(nm, body)                                   \
    static void bench_##nm();                                      \
    static struct Register_##nm {                                  \
        Register_##nm() {                                          \
            benches[num_benchmarks++] = {#nm, bench_##nm};         \
        }                                                          \
    } reg_##nm;                                                    \
    static void bench_##nm() body

using namespace opencode::core;
using namespace opencode::util;
namespace core = opencode::core;   /* qualified alias for the msg codec APIs */
namespace measure = opencode::measure; /* Phase 13 measurement helpers        */
namespace profile = opencode::profile; /* Phase 13 T2 profile benchs          */

/* ---- Phase 1: arena vs malloc (acceptance gate: >= 10x on warm reuse) ---- */

REGISTER_BENCH(arena_alloc_100k, {
    Arena a;
    for (int r = 0; r < 20; ++r) { /* 20 reuse cycles: 2M allocs total */
        for (int i = 0; i < 100000; ++i) {
            void* p = a.alloc(64);
            if (p == nullptr) std::abort();
        }
        a.reset();
    }
})

REGISTER_BENCH(malloc_alloc_100k, {
    for (int r = 0; r < 20; ++r) {
        void* ptrs[1024];
        for (int i = 0; i < 100000; ++i) {
            void* p = std::malloc(64);
            if (p == nullptr) std::abort();
            ptrs[i % 1024] = p;
        }
        for (void* p : ptrs) std::free(p);
    }
})

/* ---- Phase 1: JSON parse of a typical small agent message ---- */

REGISTER_BENCH(json_parse_small, {
    const char* doc =
        "{\"type\":\"text\",\"role\":\"assistant\",\"content\":\"hello "
        "world\",\"id\":\"msg_123\",\"meta\":{\"tokens\":42,\"cost\":0.01}}";
    for (int i = 0; i < 10000; ++i) {
        JVal v;
        size_t pos = 0;
        const error_code ec = parse_json(std::string_view(doc), v, &pos);
        if (!ec.ok()) std::abort();
    }
})

/* ---- Phase 2: binary codec vs equivalent JSON on a 500-part message ---- */

struct CodecBenchData {
    opencode::msg::Message msg;
    std::string json;              /* equivalent JSON document */
    std::vector<std::uint8_t> bin; /* binary encoding of msg */
    core::Arena arena;

    CodecBenchData() {
        msg.id = "m-big";
        msg.session_id = "s-1";
        msg.role = opencode::msg::Role::assistant;
        msg.model = "gpt-4o";
        msg.created_at = 1700000000ull;
        msg.parts.reserve(500);
        for (int i = 0; i < 500; ++i) {
            switch (i % 7) {
                case 0:
                    msg.parts.push_back(opencode::msg::Text{"repeat content"});
                    break;
                case 1:
                    msg.parts.push_back(opencode::msg::Reasoning{"trace"});
                    break;
                case 2:
                    msg.parts.push_back(opencode::msg::ImageUrl{"https://e.com/i.png"});
                    break;
                case 3:
                    msg.parts.push_back(opencode::msg::Binary{
                        "image/png", std::vector<std::uint8_t>(64, 0xAB)});
                    break;
                case 4:
                    msg.parts.push_back(opencode::msg::ToolCall{
                        "c1", "bash", "{\"cmd\":\"ls\"}", i % 2 == 0});
                    break;
                case 5:
                    msg.parts.push_back(opencode::msg::ToolResult{"c1", "ok", false});
                    break;
                case 6:
                    msg.parts.push_back(opencode::msg::Finish{
                        opencode::msg::FinishReason::end_turn});
                    break;
            }
        }
        const std::span<std::byte> sp = opencode::msg::encode_message(msg, arena);
        bin.assign(reinterpret_cast<const std::uint8_t*>(sp.data()),
                   reinterpret_cast<const std::uint8_t*>(sp.data()) + sp.size());
        json = opencode::util::to_json(opencode::msg::to_json(msg));
    }
};
static const CodecBenchData kCodec; /* built once before main() */

static std::span<const std::byte> bytes_of(const std::vector<std::uint8_t>& v) {
    return {reinterpret_cast<const std::byte*>(v.data()), v.size()};
}

REGISTER_BENCH(codec_encode_500, {
    for (int i = 0; i < 200; ++i) {
        core::Arena a;
        const std::span<std::byte> sp = opencode::msg::encode_message(kCodec.msg, a);
        if (sp.empty()) std::abort();
    }
})

REGISTER_BENCH(codec_decode_500, {
    for (int i = 0; i < 200; ++i) {
        opencode::msg::Message m;
        const core::error_code ec =
            opencode::msg::decode_message(bytes_of(kCodec.bin), m);
        if (!ec.ok()) std::abort();
    }
})

REGISTER_BENCH(json_parse_500eq, {
    for (int i = 0; i < 200; ++i) {
        JVal v;
        size_t pos = 0;
        const core::error_code ec = parse_json(kCodec.json, v, &pos);
        if (!ec.ok()) std::abort();
    }
})

/* ---- Phase 2: token estimator ---- */

REGISTER_BENCH(tokens_prose, {
    const char* prose =
        "The committee reviewed the proposal and approved the revised budget "
        "allocation before the midday recess.";
    for (int i = 0; i < 100000; ++i) {
        volatile std::size_t t = opencode::msg::estimate_tokens(prose);
        (void)t;
    }
})

REGISTER_BENCH(tokens_code, {
    const char* code =
        "const x = foo(a, b) + bar(c, d) * 42;\n"
        "for (i = 0; i < n; i++) total += items[i];\n";
    for (int i = 0; i < 100000; ++i) {
        volatile std::size_t t = opencode::msg::estimate_tokens(code);
        (void)t;
    }
})

REGISTER_BENCH(tokens_message_500, {
    for (int i = 0; i < 200; ++i) {
        volatile std::size_t t = opencode::msg::estimate_message_tokens(kCodec.msg);
        (void)t;
    }
})

/* ---- zero benchmarks registered in Phase 0 (harness shape only) ---- */

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s [--quick] [--profile]\n"
                 "  --quick    run only the fast subset (no soak/network)\n"
                 "  --profile  run the T2 profile micro-benchmarks, then exit\n",
                 argv0);
}

/* ---- Phase 13: T2 profile (init, RSS, latency budgets) ---- */

int run_profile(int trials) {
    const std::string ws = "/tmp/opencode_bench_ws";
    measure::PerfRow rows[8];
    int n = 0;

    const profile::InitResult init = profile::bench_init(trials, ws);
    const long rss_delta = init.rss_after_kb - init.rss_before_kb;
    rows[n++] = {"engine_init", init.min_init_ms * 1000.0, 100.0, 400.0, true};
    rows[n++] = {"rss_idle_delta_kb", static_cast<double>(rss_delta),
                 10240.0, 10240.0, true};

    const profile::ContextFixture fx;
    const double intent = profile::bench_intent(trials);
    const double context = profile::bench_context(fx, trials);
    const double dispatch = profile::bench_dispatch(trials);
    const double gate = profile::bench_gate(trials);
    const double event = profile::bench_event_emit(trials);
    rows[n++] = {"intent_classify", intent, 1.0, 5.0, intent >= 0};
    rows[n++] = {"context_assembly", context, 10.0, 50.0, context >= 0};
    rows[n++] = {"tool_dispatch", dispatch, 5.0, 25.0, dispatch >= 0};
    rows[n++] = {"verify_gate", gate, 50.0, 250.0, gate >= 0};
    rows[n++] = {"event_emit", event, 1.0, 5.0, event >= 0};

    std::printf("opencodepp bench_engine --profile (trials=%d)\n", trials);
    std::printf("engine init: %.3f ms; rss before=%ld kB after=%ld kB "
                "delta=%ld kB\n",
                init.min_init_ms, init.rss_before_kb, init.rss_after_kb,
                rss_delta);
    std::printf("\nT2 latency budgets:\n");
    measure::print_rows(rows, n);
    return 0;
}

} /* namespace */

int main(int argc, char** argv) {
    bool quick = false;
    bool profile = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quick") == 0) {
            quick = true;
        } else if (std::strcmp(argv[i], "--profile") == 0) {
            profile = true;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (profile) return run_profile(5);

    const double ns_per_op = measure_us([] {});

    std::printf("bench_engine 0.1.0 (%s) -- %d benchmark(s) registered\n",
                quick ? "quick" : "full", num_benchmarks);
    std::printf("empty-loop overhead: %.1f ns/op\n\n", ns_per_op);
    for (int i = 0; i < num_benchmarks; ++i) {
        /* warmup, then time the whole bench body */
        benches[i].fn();
        const double us = measure_us(benches[i].fn);
        std::printf("bench %-22s %10.1f us/run\n", benches[i].name, us);
    }
    return 0;
}

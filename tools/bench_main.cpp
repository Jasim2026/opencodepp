// bench_main.cpp -- benchmark runner skeleton (Phase 0).
// Benchmarks register here; the T1/T2 measurement harness grows over phases.
// Usage: bench_engine [--quick]
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "core/arena.h"
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

/* ---- zero benchmarks registered in Phase 0 (harness shape only) ---- */

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s [--quick]\n"
                 "  --quick   run only the fast subset (no soak/network)\n",
                 argv0);
}

} /* namespace */

int main(int argc, char** argv) {
    bool quick = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quick") == 0) {
            quick = true;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

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

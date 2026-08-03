// bench_main.cpp -- benchmark runner skeleton (Phase 0).
// Benchmarks register here; the T1/T2 measurement harness grows over phases.
// Usage: bench_engine [--quick]
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdint>

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
    std::printf("empty-loop overhead: %.1f ns/op\n", ns_per_op);
    std::printf("Phase 0: harness ready; T1/T2 benches land with their phases.\n");
    return 0;
}

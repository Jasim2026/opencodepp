// measure.cpp -- the Phase 13 T2 measurement + hardening gate.
//
// Measures the Phase 13 latency/memory/size budgets in-process against the
// static lib and asserts them (with slack) so it doubles as the `hardening`
// acceptance gate:
//
//   budget                    target    assert <   slack factor
//   engine init (create+cfg)  100 ms     500 ms      5x
//   idle RSS delta             10 MB      20 MB      2x
//   active RSS delta           30 MB      40 MB      1.3x
//   intent classify             1 ms       5 ms      5x
//   context assembly           10 ms      50 ms      5x
//   tool dispatch               5 ms      25 ms      5x
//   verify gate                50 ms     250 ms      5x
//   event emit                  1 ms       5 ms      5x
//   opencodepp_cli binary      15 MB      15 MB      n/a (hard target)
//
// Emits a JSON report (default reports/13_measure.json) and exits non-zero on
// the first budget violation. Run from the repo root (prompt templates load
// from the repo layout). Never throws.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "bench_profile.h"
#include "measure_common.h"
#include "opencode/opencode.h"

namespace {

using namespace opencode;

struct Args {
    std::string bin = "";
    std::string out = "reports/13_measure.json";
    int trials = 5;
    double size_limit_mb = 15.0; /* 0 = informational (no size assertion) */
};

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s [--bin PATH] [--out PATH] [--trials N] [--size-limit MB]\n"
                 "  --bin PATH       path to the opencodepp_cli binary (size check)\n"
                 "  --out PATH       JSON report path (default reports/13_measure.json)\n"
                 "  --trials N       trials per measurement (default 5)\n"
                 "  --size-limit MB  hard binary-size budget; 0 = skip (informational)\n"
                 "  Run from the repo root. Exits non-zero on a budget violation.\n",
                 argv0);
}

/* ---- report ---- */

void emit_json(const char* path, const measure::PerfRow* rows, int n,
               const profile::InitResult& init, long bin_bytes, bool ok) {
    FILE* f = std::fopen(path, "w");
    if (f == nullptr) return;
    std::fprintf(f,
                 "{\n"
                 "  \"tool\": \"measure\",\n"
                 "  \"version\": \"%u.%u.%u\",\n"
                 "  \"abi\": %u,\n"
                 "  \"ok\": %s,\n"
                 "  \"engine_init_ms\": %.3f,\n"
                 "  \"rss_before_kb\": %ld,\n"
                 "  \"rss_after_kb\": %ld,\n"
                 "  \"cli_binary_bytes\": %ld,\n"
                 "  \"budgets\": [\n",
                 OPENCODE_VERSION_MAJOR, OPENCODE_VERSION_MINOR,
                 OPENCODE_VERSION_PATCH, OPENCODE_ABI_VERSION, ok ? "true" : "false",
                 init.min_init_ms, init.rss_before_kb, init.rss_after_kb,
                 bin_bytes);
    for (int i = 0; i < n; ++i) {
        std::fprintf(f,
                     "    {\"name\":\"%s\",\"min_us\":%.1f,\"target_ms\":%.1f,"
                     "\"pass\":%s}%s\n",
                     rows[i].name, rows[i].min_us, rows[i].limit_ms,
                     rows[i].pass ? "true" : "false", i + 1 < n ? "," : "");
    }
    std::fprintf(f, "  ]\n}\n");
    std::fclose(f);
}

} /* namespace */

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "measure: %s needs a value\n", what);
                usage(argv[0]);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--bin") {
            a.bin = need("--bin");
        } else if (arg == "--out") {
            a.out = need("--out");
        } else if (arg == "--trials") {
            a.trials = std::atoi(need("--trials"));
            if (a.trials < 1) a.trials = 1;
        } else if (arg == "--size-limit") {
            a.size_limit_mb = std::atof(need("--size-limit"));
            if (a.size_limit_mb < 0) a.size_limit_mb = 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    const std::string ws = "/tmp/opencode_measure_ws";

    measure::PerfRow rows[9];
    int n = 0;

    /* T2 init + memory. Sampled FIRST so rss_before is a true process floor. */
    const profile::InitResult init = profile::bench_init(a.trials, ws);
    const long rss_delta = init.rss_after_kb - init.rss_before_kb;
    rows[n++] = {"engine_init", init.min_init_ms * 1000.0, 100.0, 400.0,
                 init.min_init_ms >= 0 && init.min_init_ms <= 500.0};
    rows[n++] = {"rss_idle_delta_kb", static_cast<double>(rss_delta), 10240.0,
                 10240.0, rss_delta >= 0 && rss_delta <= 20480};

    /* T2 latency budgets (target ms, assert = target*slack). The fixture and
     * the benchs allocate, so active RSS is sampled after them. */
    const profile::ContextFixture fx;
    const double intent = profile::bench_intent(a.trials);
    const double context = profile::bench_context(fx, a.trials);
    const double dispatch = profile::bench_dispatch(a.trials);
    const double gate = profile::bench_gate(a.trials);
    const double event = profile::bench_event_emit(a.trials);

    rows[n++] = {"intent_classify", intent, 1.0, 5.0,
                 intent >= 0 && intent <= 5000.0};
    rows[n++] = {"context_assembly", context, 10.0, 50.0,
                 context >= 0 && context <= 50000.0};
    rows[n++] = {"tool_dispatch", dispatch, 5.0, 25.0,
                 dispatch >= 0 && dispatch <= 25000.0};
    rows[n++] = {"verify_gate", gate, 50.0, 250.0,
                 gate >= 0 && gate <= 250000.0};
    rows[n++] = {"event_emit", event, 1.0, 5.0,
                 event >= 0 && event <= 5000.0};

    /* Active RSS: peak over the latency benchs (which allocate plans, tools,
     * gate results, etc.). */
    const long active_delta = measure::hwm_kb() - init.rss_before_kb;
    rows[n++] = {"rss_active_delta_kb", static_cast<double>(active_delta),
                 30720.0, 10240.0,
                 active_delta >= 0 && active_delta <= 40960};

    /* T2 binary size. Enforced only when --size-limit > 0 and --bin given;
     * otherwise informational (the stripped size build is the gate). */
    const long bin_bytes = a.bin.empty() ? -1 : measure::file_size(a.bin);
    const bool size_enforced = a.size_limit_mb > 0 && !a.bin.empty();
    const long size_limit_bytes =
        static_cast<long>(a.size_limit_mb * 1048576.0);
    rows[n++] = {"cli_binary_bytes", static_cast<double>(bin_bytes),
                 static_cast<double>(size_limit_bytes), 0.0,
                 !size_enforced || (bin_bytes >= 0 &&
                                    bin_bytes <= size_limit_bytes)};

    const bool ok = measure::rows_ok(rows, n) && fx.ready && rss_delta >= 0 &&
                    active_delta >= 0 &&
                    (bin_bytes >= 0 || !size_enforced);

    std::printf("opencodepp measure %u.%u.%u (abi %u)\n", OPENCODE_VERSION_MAJOR,
                OPENCODE_VERSION_MINOR, OPENCODE_VERSION_PATCH, OPENCODE_ABI_VERSION);
    std::printf("engine init: %.3f ms (target < 100 ms)\n", init.min_init_ms);
    std::printf("rss: before=%ld kB idle-after=%ld kB active-delta=%ld kB\n",
                init.rss_before_kb, init.rss_after_kb, active_delta);
    if (bin_bytes < 0) {
        std::printf("cli binary: not found (%s) -- size informational\n",
                    a.bin.c_str());
    } else {
        std::printf("cli binary: %ld bytes (%.2f MB%s)\n", bin_bytes,
                    static_cast<double>(bin_bytes) / 1048576.0,
                    size_enforced ? "" : " -- informational");
    }
    std::printf("\nbudget table (assert = target + slack):\n");
    measure::print_rows(rows, n);

    emit_json(a.out.c_str(), rows, n, init, bin_bytes, ok);
    std::printf("\nreport: %s\n", a.out.c_str());
    std::printf("%s\n", ok ? "hardening: PASS" : "hardening: FAIL");
    return ok ? 0 : 1;
}

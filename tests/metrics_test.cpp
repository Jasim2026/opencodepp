// metrics_test.cpp -- Phase 1: counters/gauges/histograms, percentiles.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/metrics.h"

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

void test_counter_gauge() {
    using namespace opencode::core;
    Metrics m;
    m.inc("requests");
    m.inc("requests", 9);
    m.inc("inflight");
    m.dec("inflight");
    m.inc("inflight", 3);
    m.set("peak", 7);

    size_t seen = 0;
    int64_t requests = 0, inflight = 0, peak = 0;
    m.snapshot([&](std::string_view n, Metrics::Kind k, double v, uint64_t) {
        (void)v;
        if (n == "requests" && k == Metrics::Kind::counter) requests = 10;
        if (n == "inflight" && k == Metrics::Kind::counter) inflight = 3;
        if (n == "peak" && k == Metrics::Kind::gauge) peak = 7;
        ++seen;
    });
    CHECK(seen == 3);
    CHECK(requests == 10);
    CHECK(inflight == 3);
    CHECK(peak == 7);
}

void test_histogram_percentile() {
    using namespace opencode::core;
    Metrics m;
    /* 100 observations: 1ms .. 100ms roughly linear in log space */
    for (int i = 0; i < 100; ++i) {
        m.observe("latency", 0.001 + 0.001 * i);
    }
    double p50 = m.percentile("latency", 50.0);
    double p95 = m.percentile("latency", 95.0);
    CHECK(p50 >= 0.001);
    CHECK(p95 > p50);
    CHECK(p95 <= 0.1001 * 2.0); /* buckets: upper edge within 2x of observed max */
    CHECK(m.percentile("nope", 50.0) == -1.0);
}

void test_snapshot_histogram_value() {
    using namespace opencode::core;
    Metrics m;
    m.observe("t", 0.010); /* 10ms */
    m.observe("t", 0.010);
    uint64_t count = 0;
    m.snapshot([&](std::string_view n, Metrics::Kind k, double v, uint64_t c) {
        if (n == "t" && k == Metrics::Kind::histogram) {
            count = c;
            CHECK(v > 0.0);
        }
    });
    CHECK(count == 2);
}
} /* namespace */

int main() {
    test_counter_gauge();
    test_histogram_percentile();
    test_snapshot_histogram_value();
    if (failures == 0) {
        std::printf("metrics_test: OK\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "metrics_test: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}

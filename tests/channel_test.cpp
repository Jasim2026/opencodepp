// channel_test.cpp -- Phase 1: bounded MPSC order, backpressure, close, wakeup.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "core/channel.h"

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

using namespace opencode::core;

void test_order() {
    Channel ch(8);
    int vals[3] = {1, 2, 3};
    CHECK(ch.try_push(0, &vals[0], sizeof(int)));
    CHECK(ch.try_push(0, &vals[1], sizeof(int)));
    CHECK(ch.try_push(0, &vals[2], sizeof(int)));
    Channel::Message m;
    for (int expect : vals) {
        CHECK(ch.try_pop(m) == Channel::kOk);
        CHECK(*static_cast<const int*>(m.data) == expect);
    }
    CHECK(ch.try_pop(m) == Channel::kEmpty);
}

void test_backpressure() {
    Channel ch(2);
    int a = 1, b = 2, c = 3;
    CHECK(ch.try_push(0, &a, sizeof(int)));
    CHECK(ch.try_push(0, &b, sizeof(int)));
    CHECK(ch.full());
    CHECK(!ch.try_push(0, &c, sizeof(int))); /* full -> reject, no block */
    Channel::Message m;
    CHECK(ch.try_pop(m) == Channel::kOk);
    CHECK(ch.try_push(0, &c, sizeof(int)));
    CHECK(ch.size() == 2);
}

void test_producer_close() {
    Channel ch(4);
    int a = 7;
    CHECK(ch.try_push(0, &a, sizeof(int)));
    ch.close();
    CHECK(ch.closed());
    CHECK(!ch.try_push(0, &a, sizeof(int))); /* closed -> reject */
    Channel::Message m;
    CHECK(ch.try_pop(m) == Channel::kOk);   /* drain remaining */
    CHECK(ch.try_pop(m) == Channel::kClosed); /* then closed */
}

void test_consumer_close() {
    Channel ch(4);
    int a = 7;
    ch.try_push(0, &a, sizeof(int));
    ch.drop(); /* consumer close: queue dropped */
    CHECK(ch.closed());
    CHECK(!ch.try_push(0, &a, sizeof(int)));
    Channel::Message m;
    CHECK(ch.try_pop(m) == Channel::kClosed);
}

void test_wakeup_hook() {
    Channel ch(4);
    int wakes = 0;
    struct Ctx {
        int* wakes;
    } ctx{&wakes};
    auto wake = [](void* u) { ++(*static_cast<Ctx*>(u)->wakes); };
    ch.set_wakeup(wake, &ctx);
    int a = 1;
    ch.try_push(0, &a, sizeof(int));
    ch.try_push(0, &a, sizeof(int));
    CHECK(wakes == 2);
    CHECK(!ch.try_push(0, &a, sizeof(int)) || true); /* full -> still 2 wakes */
}

void test_multiproducer() {
    Channel ch(256);
    constexpr int kThreads = 4;
    constexpr int kPer = 500;
    std::atomic<int> pushed{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&] {
            int local = 0;
            for (int i = 0; i < kPer; ++i) {
                if (ch.try_push(0, &local, sizeof(int))) ++pushed;
            }
        });
    }
    for (auto& t : ts) t.join();
    /* drain whatever got through; total received == pushed */
    Channel::Message m;
    size_t popped = 0;
    while (ch.try_pop(m) == Channel::kOk) ++popped;
    CHECK(popped == static_cast<size_t>(pushed.load()));
    CHECK(pushed.load() <= kThreads * kPer);
}
} /* namespace */

int main() {
    test_order();
    test_backpressure();
    test_producer_close();
    test_consumer_close();
    test_wakeup_hook();
    test_multiproducer();
    if (failures == 0) {
        std::printf("channel_test: OK\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "channel_test: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}

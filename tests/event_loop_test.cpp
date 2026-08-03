// event_loop_test.cpp -- Phase 1: fd events, timers, timeout, wakeup, cancel.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <unistd.h>
#include <sys/socket.h>

#include "core/event_loop.h"

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

struct FdHook {
    int fd;
    uint32_t got = 0;
    int fires = 0;
    static void cb(void* u, uint32_t events) {
        auto* h = static_cast<FdHook*>(u);
        h->got = events;
        ++h->fires;
        h->loop->del(h->fd);
    }
    opencode::core::EventLoop* loop;
};

struct TimerHook {
    int fires = 0;
    uint64_t last_id = 0;
    static void cb(void* u, uint64_t id) {
        auto* h = static_cast<TimerHook*>(u);
        ++h->fires;
        h->last_id = id;
    }
};

using namespace opencode::core;

void test_socket_readable() {
    int sv[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    EventLoop loop;
    FdHook hook;
    hook.fd = sv[0];
    hook.loop = &loop;
    CHECK(loop.add(sv[0], EventLoop::kRead, &FdHook::cb, &hook));
    CHECK(loop.has_fd(sv[0]));
    const char msg[] = "x";
    CHECK(::write(sv[1], msg, 1) == 1);
    int n = loop.run_once(-1);
    CHECK(n == 1);
    CHECK(hook.fires == 1);
    CHECK((hook.got & EventLoop::kRead) != 0);
    CHECK(!loop.has_fd(sv[0])); /* auto-removed by the handler */
    CHECK(!loop.add(-1, EventLoop::kRead, nullptr, nullptr));
    /* duplicate registration rejected while still registered */
    EventLoop loop2;
    FdHook h2;
    h2.fd = sv[1];
    h2.loop = &loop2;
    CHECK(loop2.add(sv[1], EventLoop::kRead, &FdHook::cb, &h2));
    CHECK(!loop2.add(sv[1], EventLoop::kRead, nullptr, nullptr));
    ::close(sv[0]);
    ::close(sv[1]);
}

void test_timer_fires() {
    EventLoop loop;
    TimerHook hook;
    uint64_t id = loop.add_timer(15, &TimerHook::cb, &hook, true);
    CHECK(id != 0);
    int n = loop.run_once(-1);
    CHECK(n == 1);
    CHECK(hook.fires == 1);
    CHECK(hook.last_id == id);
}

void test_timeout_returns() {
    EventLoop loop;
    auto t0 = std::chrono::steady_clock::now();
    int n = loop.run_once(25);
    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    CHECK(n == 0);
    CHECK(dt >= 15 && dt < 300);
}

void test_wakeup_unblocks() {
    EventLoop loop;
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        loop.wakeup();
    });
    int n = loop.run_once(-1);
    t.join();
    CHECK(n == 0); /* wake pipe is internal; not counted */
}

void test_cancel_timer() {
    EventLoop loop;
    TimerHook hook;
    uint64_t id = loop.add_timer(20, &TimerHook::cb, &hook, true);
    CHECK(loop.cancel_timer(id));
    int n = loop.run_once(60);
    CHECK(n == 0);
    CHECK(hook.fires == 0);
}

void test_repeating_timer() {
    EventLoop loop;
    TimerHook hook;
    loop.add_timer(1, &TimerHook::cb, &hook, false);
    for (int i = 0; i < 5; ++i) loop.run_once(-1);
    CHECK(hook.fires >= 5);
}

void test_mod() {
    int sv[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    EventLoop loop;
    FdHook hook;
    hook.fd = sv[0];
    hook.loop = &loop;
    CHECK(loop.add(sv[0], EventLoop::kWrite, &FdHook::cb, &hook));
    CHECK(loop.mod(sv[0], EventLoop::kRead));
    const char msg[] = "x";
    CHECK(::write(sv[1], msg, 1) == 1);
    int n = loop.run_once(-1);
    CHECK(n == 1);
    CHECK((hook.got & EventLoop::kRead) != 0);
    ::close(sv[0]);
    ::close(sv[1]);
}
} /* namespace */

int main() {
    test_socket_readable();
    test_timer_fires();
    test_timeout_returns();
    test_wakeup_unblocks();
    test_cancel_timer();
    test_repeating_timer();
    test_mod();
    if (failures == 0) {
        std::printf("event_loop_test: OK\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "event_loop_test: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}

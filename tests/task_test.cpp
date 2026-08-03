// task_test.cpp -- Phase 1: stackless coroutines over the event loop:
// timer task, two-coroutine ping/pong over a socketpair, cancel.
#include <cstdio>
#include <cstdlib>

#include <unistd.h>
#include <sys/socket.h>

#include "core/task.h"

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

void test_timer_task() {
    EventLoop loop;
    TaskScheduler sched(loop);
    int fired = 0;
    auto task = [&]() -> co_task {
        co_await io::await_timer(15);
        ++fired;
    };
    uint64_t id = sched.spawn(task());
    CHECK(id != 0);
    int guard = 0;
    while (sched.pending() > 0 && ++guard < 1000) {
        loop.run_once(25);
    }
    CHECK(fired == 1);
    CHECK(sched.pending() == 0);
}

void test_ping_pong() {
    int sv[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    EventLoop loop;
    TaskScheduler sched(loop);
    int acount = 0, bcount = 0;

    auto a = [&]() -> co_task {
        for (int i = 0; i < 3; ++i) {
            co_await io::await_readable(sv[0]);
            char c;
            const ssize_t nr = ::read(sv[0], &c, 1);
            (void)nr;
            ++acount;
            const ssize_t nw = ::write(sv[0], "a", 1);
            (void)nw;
        }
    };
    auto b = [&]() -> co_task {
        for (int i = 0; i < 3; ++i) {
            co_await io::await_readable(sv[1]);
            char c;
            const ssize_t nr = ::read(sv[1], &c, 1);
            (void)nr;
            ++bcount;
            const ssize_t nw = ::write(sv[1], "b", 1);
            (void)nw;
        }
    };
    const ssize_t seed = ::write(sv[1], "s", 1); /* seed makes sv[0] readable */
    (void)seed;
    sched.spawn(a());
    sched.spawn(b());
    int guard = 0;
    while (sched.pending() > 0 && ++guard < 2000) {
        loop.run_once(50);
        sched.run_ready();
    }
    CHECK(acount == 3);
    CHECK(bcount == 3);
    CHECK(sched.pending() == 0);
    ::close(sv[0]);
    ::close(sv[1]);
}

void test_cancel_fd_await() {
    int sv[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    EventLoop loop;
    TaskScheduler sched(loop);
    int fired = 0;
    auto task = [&]() -> co_task {
        co_await io::await_readable(sv[0]);
        ++fired;
    };
    uint64_t id = sched.spawn(task());
    CHECK(sched.pending() == 1);
    CHECK(loop.has_fd(sv[0]));
    CHECK(sched.cancel(id));
    CHECK(sched.pending() == 0);
    CHECK(!loop.has_fd(sv[0])); /* fd registration unwound */
    int n = loop.run_once(40);
    CHECK(n == 0);
    CHECK(fired == 0);
    ::close(sv[0]);
    ::close(sv[1]);
}

void test_cancel_timer_await() {
    EventLoop loop;
    TaskScheduler sched(loop);
    int fired = 0;
    auto task = [&]() -> co_task {
        co_await io::await_timer(100000);
        ++fired;
    };
    uint64_t id = sched.spawn(task());
    CHECK(sched.pending() == 1);
    CHECK(sched.cancel(id));
    CHECK(sched.pending() == 0);
    int n = loop.run_once(50); /* should not block on the long timer */
    CHECK(n == 0);
    CHECK(fired == 0);
}

void test_synchronous_task() {
    EventLoop loop;
    TaskScheduler sched(loop);
    int done = 0;
    auto task = [&]() -> co_task { ++done; co_return; };
    uint64_t id = sched.spawn(task());
    CHECK(id != 0);
    CHECK(done == 1);           /* eager start */
    CHECK(sched.pending() == 0); /* completed and reaped */
}
} /* namespace */

int main() {
    test_timer_task();
    test_ping_pong();
    test_cancel_fd_await();
    test_cancel_timer_await();
    test_synchronous_task();
    if (failures == 0) {
        std::printf("task_test: OK\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "task_test: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}

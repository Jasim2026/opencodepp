/*
 * net_pool_test.cpp -- RetryPolicy determinism/classification and Pool
 * reuse / idle-cap / in-flight-cap mechanics against an in-process fake
 * accept server. Local verification: plain g++ build-and-run.
 */
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/error.h"
#include "core/event_loop.h"
#include "net/policy.h"
#include "net/pool.h"
#include "net/socket.h"
#include "net/transport.h"

namespace {

using namespace opencode;
using namespace opencode::core;
using namespace opencode::net;

int failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,   \
                         #cond);                                           \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

void test_policy_decide() {
    RetryPolicy p;
    RetryBudget b = p.budget();
    CHECK(b.max_retries == 8);
    CHECK(b.base_delay_ms == 300);
    CHECK(b.max_delay_ms == 30000);

    CHECK(!p.decide(core::ok(), 1).retry);
    CHECK(p.decide(core::ok(), 1).reason == "ok");

    /* Non-retryable: auth. */
    RetryDecision da = p.decide(make_error_code(Err::e_auth), 1);
    CHECK(!da.retry);
    CHECK(da.exhausted);
    CHECK(da.reason == "non-retryable");

    /* Retryable, within budget. */
    RetryDecision dc = p.decide(make_error_code(Err::e_net_connect), 1);
    CHECK(dc.retry);
    CHECK(!dc.exhausted);
    CHECK(dc.reason == "backoff");
    CHECK(dc.delay_ms >= 210 && dc.delay_ms <= 390);

    /* Exhausted at the budget boundary. */
    RetryDecision de = p.decide(make_error_code(Err::e_net_connect), 8);
    CHECK(!de.retry);
    CHECK(de.exhausted);
    CHECK(de.reason == "exhausted");

    std::printf("  policy decide: OK\n");
}

void test_policy_determinism() {
    RetryPolicy a;
    RetryPolicy b;
    a.set_seed(0xABCD);
    b.set_seed(0xABCD);
    for (uint32_t attempt = 1; attempt <= 8; ++attempt) {
        CHECK(a.next_delay_ms(attempt) == b.next_delay_ms(attempt));
    }

    /* Different seeds may differ (not asserted: only determinism + bounds). */
    RetryPolicy c;
    c.set_seed(0x1234);
    const uint64_t d1 = a.next_delay_ms(3);
    const uint64_t d2 = c.next_delay_ms(3);
    CHECK(d1 <= 30000 && d2 <= 30000);
    CHECK(d1 > 0 && d2 > 0);

    /* Growth: attempt 2 is ~2x attempt 1 (jitter keeps an overlap). */
    RetryPolicy p;
    p.set_seed(42);
    const uint64_t first = p.next_delay_ms(1);
    const uint64_t second = p.next_delay_ms(2);
    CHECK(first <= second * 2);

    /* Long-run cap: high attempts clamp at max_delay_ms (before jitter). */
    RetryPolicy q;
    q.set_seed(7);
    const uint64_t big = q.next_delay_ms(30);
    CHECK(big >= 21000 && big <= 30000);
    std::printf("  policy determinism: OK\n");
}

void test_policy_retry_after() {
    RetryPolicy p;
    p.set_seed(99);
    RetryDecision d = p.decide_retry_after(5, 1);
    CHECK(d.retry);
    CHECK(d.reason == "rate-limit");
    CHECK(d.delay_ms >= 5000);

    RetryDecision d2 = p.decide_retry_after(5, 8);
    CHECK(!d2.retry);
    CHECK(d2.exhausted);

    CHECK(RetryPolicy::retryable(make_error_code(Err::e_net_timeout)));
    CHECK(RetryPolicy::retryable(make_error_code(Err::e_net_offline)));
    CHECK(RetryPolicy::retryable(make_error_code(Err::e_rate_limit)));
    CHECK(!RetryPolicy::retryable(make_error_code(Err::e_auth)));
    CHECK(!RetryPolicy::retryable(make_error_code(Err::e_proto_parse)));
    std::printf("  policy retry-after: OK\n");
}

/* Accept server that counts connections and reads each to EOF on its own
 * thread (so idle keep-alive conns never block the accept loop). */
struct FakeAcceptor {
    struct State {
        std::atomic<bool> stop{false};
        std::atomic<int> accepted{0};
    };
    std::shared_ptr<State> st = std::make_shared<State>();
    std::thread th;
    int port = 0;

    FakeAcceptor() = default;
    FakeAcceptor(FakeAcceptor&& o) noexcept
        : st(std::move(o.st)), th(std::move(o.th)), port(o.port) {}
    FakeAcceptor& operator=(FakeAcceptor&&) noexcept = default;
    FakeAcceptor(const FakeAcceptor&) = delete;
    FakeAcceptor& operator=(const FakeAcceptor&) = delete;

    ~FakeAcceptor() {
        if (st) st->stop.store(true);
        if (th.joinable()) th.join();
    }
};

FakeAcceptor start_acceptor() {
    const int ls = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    ::bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof a);
    sockaddr_in g{};
    socklen_t gl = sizeof g;
    ::getsockname(ls, reinterpret_cast<sockaddr*>(&g), &gl);
    const int port = ntohs(g.sin_port);
    ::listen(ls, 16);

    FakeAcceptor fa;
    fa.port = port;
    fa.th = std::thread([ls, st = fa.st]() {
        while (!st->stop.load()) {
            struct pollfd pfd{};
            pfd.fd = ls;
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, 200) <= 0) continue;
            const int c = ::accept(ls, nullptr, nullptr);
            if (c < 0) continue;
            st->accepted.fetch_add(1);
            std::thread([c]() {
                char buf[64];
                while (::read(c, buf, sizeof buf) > 0) {
                }
                ::close(c);
            }).detach();
        }
        ::close(ls);
    });
    return fa;
}

/* Wait (bounded) for the accept thread to observe `n` connections; the
 * client's connect() completes before the server side increments `accepted`,
 * so the check must not race the accept loop. */
void wait_accepted(FakeAcceptor& fa, int n) {
    const uint64_t t0 = now_mono_ms();
    while (fa.st->accepted.load() < n && now_mono_ms() - t0 < 3000)
        std::this_thread::yield();
}

EventLoop g_loop;

Pool::Key key_for(int port, bool tls = false) {
    Pool::Key k;
    k.host = "127.0.0.1";
    k.port = static_cast<uint16_t>(port);
    k.tls = tls;
    return k;
}

void test_pool_reuse() {
    FakeAcceptor fa = start_acceptor();
    Pool pool;
    const Pool::Key key = key_for(fa.port);

    Transport t1;
    CHECK(pool.acquire(g_loop, key, nullptr, now_mono_ms() + 3000, t1).ok());
    CHECK(t1.connected());
    CHECK(pool.in_flight(key) == 1);
    wait_accepted(fa, 1);
    CHECK(fa.st->accepted.load() == 1);
    const int fd1 = t1.fd();

    pool.release(std::move(t1), key, true);
    CHECK(pool.idle_count() == 1);
    CHECK(pool.in_flight(key) == 0);

    /* Second acquire reuses the parked connection: no new server accept. */
    Transport t2;
    CHECK(pool.acquire(g_loop, key, nullptr, now_mono_ms() + 3000, t2).ok());
    CHECK(t2.fd() == fd1);
    CHECK(fa.st->accepted.load() == 1);
    CHECK(pool.in_flight(key) == 1);

    /* Unhealthy release closes the transport (never reused). */
    pool.release(std::move(t2), key, false);
    CHECK(pool.idle_count() == 0);
    CHECK(pool.in_flight(key) == 0);
    std::printf("  pool reuse: OK\n");
}

void test_pool_idle_cap() {
    FakeAcceptor fa = start_acceptor();
    Pool::Config cfg;
    cfg.max_idle_per_host = 2;
    cfg.max_in_flight_per_host = 8;
    Pool pool(cfg);
    const Pool::Key key = key_for(fa.port);
    const uint64_t dl = now_mono_ms() + 3000;

    Transport ts[3];
    for (int i = 0; i < 3; ++i)
        CHECK(pool.acquire(g_loop, key, nullptr, dl, ts[i]).ok());
    for (auto& t : ts) pool.release(std::move(t), key, true);

    wait_accepted(fa, 3);
    CHECK(pool.idle_count() == 2); /* one closed by the per-host cap */
    CHECK(fa.st->accepted.load() == 3);
    pool.drop_idle();
    CHECK(pool.idle_count() == 0);
    std::printf("  pool idle cap: OK\n");
}

void test_pool_inflight_cap() {
    FakeAcceptor fa = start_acceptor();
    Pool::Config cfg;
    cfg.max_in_flight_per_host = 1;
    cfg.acquire_wait_ms = 50;
    Pool pool(cfg);
    const Pool::Key key = key_for(fa.port);

    Transport t1;
    CHECK(pool.acquire(g_loop, key, nullptr, now_mono_ms() + 3000, t1).ok());
    Transport t2;
    const uint64_t t0 = now_mono_ms();
    const error_code ec =
        pool.acquire(g_loop, key, nullptr, now_mono_ms() + 3000, t2);
    CHECK(ec.code() == Err::e_net_timeout);
    CHECK(now_mono_ms() - t0 >= 40);
    CHECK(!t2.connected());
    pool.release(std::move(t1), key, true);
    std::printf("  pool in-flight cap: OK\n");
}

void test_pool_connect_refused() {
    /* Bound-but-not-listening socket: deterministic RST. */
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    sockaddr_in g{};
    socklen_t gl = sizeof g;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&g), &gl);
    const int port = ntohs(g.sin_port);

    Pool pool;
    Transport t;
    const error_code ec =
        pool.acquire(g_loop, key_for(port), nullptr, now_mono_ms() + 2000, t);
    CHECK(ec.code() == Err::e_net_connect);
    CHECK(!t.connected());
    ::close(fd);
    std::printf("  pool connect refused: OK\n");
}

void test_pool_injected_opener() {
    FakeAcceptor fa = start_acceptor();
    Pool pool;
    const Pool::Key key = key_for(fa.port);
    Transport t;
    /* A failing opener is propagated and nothing is left in-flight. */
    const auto bad = [](EventLoop&, const Pool::Key&, const TlsConfig*,
                        uint64_t, Transport&) -> core::error_code {
        return make_error_code(Err::e_not_impl, 7);
    };
    const error_code ec = pool.acquire(g_loop, key, nullptr,
                                       now_mono_ms() + 2000, t, bad);
    CHECK(ec.code() == Err::e_not_impl);
    CHECK(ec.detail() == 7);
    CHECK(pool.in_flight(key) == 0);
    std::printf("  pool injected opener: OK\n");
}

} /* namespace */

int main() {
    test_policy_decide();
    test_policy_determinism();
    test_policy_retry_after();
    test_pool_reuse();
    test_pool_idle_cap();
    test_pool_inflight_cap();
    test_pool_connect_refused();
    test_pool_injected_opener();
    if (failures == 0) {
        std::printf("net_pool_test: OK\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "net_pool_test: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}

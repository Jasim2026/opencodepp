/*
 * net_meter_offline_test.cpp -- Meter counters/percentiles and the OfflineQueue
 * state machine (probe -> offline -> recovering -> drain -> online) against an
 * in-process probe server that can be stopped and restarted on one port.
 */
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
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
#include "net/meter.h"
#include "net/offline.h"

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

EventLoop g_loop;

void test_meter_counters() {
    Meter m;
    m.add_bytes_out(100);
    m.add_bytes_in(50);
    m.add_tokens_in(2000);
    m.add_tokens_out(1500);
    m.inc_round_trip();
    m.inc_retry();
    m.inc_reconnect();
    m.inc_timeout();
    m.add_offline_ms(1234);

    CHECK(m.bytes_out() == 100);
    CHECK(m.bytes_in() == 50);
    CHECK(m.tokens_in() == 2000);
    CHECK(m.tokens_out() == 1500);
    CHECK(m.round_trips() == 1);
    CHECK(m.retries() == 1);
    CHECK(m.reconnects() == 1);
    CHECK(m.timeouts() == 1);
    CHECK(m.offline_ms() == 1234);

    Meter::Snapshot s = m.snapshot();
    CHECK(s.bytes_out == 100 && s.retries == 1 && s.offline_ms == 1234);
    std::printf("  meter counters: OK\n");
}

void test_meter_rtt() {
    Meter m;
    CHECK(m.rtt_p50() == 0);
    const uint64_t rtts[] = {50, 10, 30, 40, 20};
    for (const uint64_t v : rtts) m.record_rtt(v);
    CHECK(m.rtt_samples() == 5);
    CHECK(m.rtt_p50() == 30); /* sorted: 10 20 30 40 50 */
    CHECK(m.rtt_p95() == 50);

    /* Ring cap: past kMaxRttSamples the window is bounded. */
    for (size_t i = 0; i < Meter::kMaxRttSamples + 100; ++i)
        m.record_rtt(1);
    CHECK(m.rtt_samples() == Meter::kMaxRttSamples);
    CHECK(m.rtt_p50() == 1);

    m.reset();
    CHECK(m.rtt_samples() == 0);
    CHECK(m.bytes_out() == 0);
    std::printf("  meter rtt percentiles: OK\n");
}

/* A probe target on a fixed port; stop() closes the listener so the port
 * becomes unreachable, start() reopens it (SO_REUSEADDR). */
struct PortServer {
    std::shared_ptr<std::atomic<bool>> stop;
    std::thread th;
    int ls = -1;
    int port = 0;
};

PortServer start_on(int port) {
    PortServer s;
    s.stop = std::make_shared<std::atomic<bool>>(false);
    s.ls = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(s.ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<uint16_t>(port));
    ::bind(s.ls, reinterpret_cast<sockaddr*>(&a), sizeof a);
    ::listen(s.ls, 16);
    sockaddr_in g{};
    socklen_t gl = sizeof g;
    ::getsockname(s.ls, reinterpret_cast<sockaddr*>(&g), &gl);
    s.port = ntohs(g.sin_port);
    auto stop = s.stop;
    const int ls = s.ls;
    s.th = std::thread([stop, ls]() {
        while (!stop->load()) {
            struct pollfd pfd{};
            pfd.fd = ls;
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, 100) <= 0) continue;
            const int c = ::accept(ls, nullptr, nullptr);
            if (c < 0) continue;
            std::thread([c]() {
                char b[64];
                while (::read(c, b, sizeof b) > 0) {
                }
                ::close(c);
            }).detach();
        }
        ::close(ls);
    });
    return s;
}

void stop_server(PortServer& s) {
    s.stop->store(true);
    ::shutdown(s.ls, SHUT_RDWR);
    if (s.th.joinable()) s.th.join();
}

OfflineQueue::Config probe_cfg(int port) {
    OfflineQueue::Config cfg;
    cfg.probe_host = "127.0.0.1";
    cfg.probe_port = static_cast<uint16_t>(port);
    cfg.probe_timeout_ms = 500;
    return cfg;
}

void test_offline_transitions() {
    PortServer srv = start_on(0);
    const int port = srv.port;

    OfflineQueue q(probe_cfg(port));
    std::vector<std::string> states;
    q.set_state_cb([&](Connectivity c) {
        states.push_back(c == Connectivity::online   ? "online"
                         : c == Connectivity::offline ? "offline"
                                                       : "recovering");
    });

    /* Initially online; probe to a live server succeeds. */
    CHECK(q.check(g_loop).ok());
    CHECK(q.state() == Connectivity::online);

    /* Server goes away: probe fails -> offline. */
    stop_server(srv);
    CHECK(!q.check(g_loop).ok());
    CHECK(q.state() == Connectivity::offline);
    CHECK(states.size() == 1 && states[0] == "offline");

    /* Requests queue up while offline. */
    QueueEntry e1, e2, e3;
    e1.request_id = "a";
    e2.request_id = "b";
    e3.request_id = "c";
    CHECK(q.enqueue(std::move(e1)).ok());
    CHECK(q.enqueue(std::move(e2)).ok());
    CHECK(q.enqueue(std::move(e3)).ok());
    CHECK(q.pending() == 3);

    /* submit() while offline queues (never drops). */
    QueueEntry s1;
    s1.request_id = "d";
    const auto drain = [](EventLoop&, const QueueEntry&,
                          uint64_t) -> core::error_code { return core::ok(); };
    CHECK(q.submit(g_loop, std::move(s1), drain).ok());
    CHECK(q.pending() == 4);

    /* Server returns: recovering -> drain FIFO -> online. */
    PortServer srv2 = start_on(port);
    CHECK(q.check(g_loop).ok());
    CHECK(q.state() == Connectivity::recovering);

    std::vector<std::string> drained;
    const auto drain2 = [&drained](EventLoop&, const QueueEntry& e,
                                   uint64_t) -> core::error_code {
        drained.push_back(e.request_id);
        return core::ok();
    };
    CHECK(q.drain(g_loop, now_mono_ms() + 5000, drain2).ok());
    CHECK(drained.size() == 4);
    CHECK(drained[0] == "a" && drained[1] == "b" && drained[2] == "c" &&
          drained[3] == "d");
    CHECK(q.pending() == 0);
    CHECK(q.state() == Connectivity::online);
    CHECK(states.size() >= 3 && states.back() == "online");
    stop_server(srv2);
    std::printf("  offline transitions: OK\n");
}

void test_offline_requeue_on_failure() {
    PortServer srv = start_on(0);
    const int port = srv.port;
    OfflineQueue q(probe_cfg(port));

    QueueEntry a, b, c;
    a.request_id = "a";
    b.request_id = "b";
    c.request_id = "c";
    CHECK(q.enqueue(std::move(a)).ok());
    CHECK(q.enqueue(std::move(b)).ok());
    CHECK(q.enqueue(std::move(c)).ok());

    /* "b" fails retryably: requeued at the head, queue keeps [b, c]. */
    const auto flaky = [](EventLoop&, const QueueEntry& e,
                          uint64_t) -> core::error_code {
        return e.request_id == "b"
                   ? make_error_code(Err::e_net_connect, 9)
                   : core::ok();
    };
    const error_code ec = q.drain(g_loop, now_mono_ms() + 5000, flaky);
    CHECK(ec.code() == Err::e_net_connect);
    CHECK(q.pending() == 2);
    CHECK(q.state() == Connectivity::offline);

    /* Retry after recovery: the same "b" now succeeds, "c" follows. */
    const auto ok2 = [](EventLoop&, const QueueEntry&,
                        uint64_t) -> core::error_code { return core::ok(); };
    CHECK(q.drain(g_loop, now_mono_ms() + 5000, ok2).ok());
    CHECK(q.pending() == 0);
    CHECK(q.state() == Connectivity::online);
    stop_server(srv);
    std::printf("  offline requeue on failure: OK\n");
}

void test_offline_capacity() {
    PortServer srv = start_on(0);
    OfflineQueue::Config cfg = probe_cfg(srv.port);
    cfg.max_queue = 2;
    OfflineQueue q(cfg);
    QueueEntry e;
    CHECK(q.enqueue(QueueEntry{}).ok());
    CHECK(q.enqueue(QueueEntry{}).ok());
    CHECK(q.enqueue(QueueEntry{}).code() == Err::e_net_overflow);
    CHECK(q.pending() == 2);
    CHECK(q.capacity() == 2);
    stop_server(srv);
    std::printf("  offline capacity: OK\n");
}

void test_submit_online_direct() {
    PortServer srv = start_on(0);
    OfflineQueue q(probe_cfg(srv.port));
    int calls = 0;
    const auto drain = [&calls](EventLoop&, const QueueEntry&,
                                uint64_t) -> core::error_code {
        ++calls;
        return core::ok();
    };
    QueueEntry e;
    e.request_id = "direct";
    CHECK(q.submit(g_loop, std::move(e), drain).ok());
    CHECK(calls == 1); /* sent immediately, not queued */
    CHECK(q.pending() == 0);
    stop_server(srv);
    std::printf("  submit online direct: OK\n");
}

} /* namespace */

int main() {
    test_meter_counters();
    test_meter_rtt();
    test_offline_transitions();
    test_offline_requeue_on_failure();
    test_offline_capacity();
    test_submit_online_direct();
    if (failures == 0) {
        std::printf("net_meter_offline_test: OK\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "net_meter_offline_test: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}

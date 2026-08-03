// drill.cpp -- Phase 4 fault-injection battery (05_NETWORK_RESILIENCE.md 7-8).
//
// An in-process mock provider serves a programmable per-request behavior
// script; the drill drives the real net layers (Pool, http1, RetryPolicy,
// OfflineQueue, Meter) against it and asserts the Phase-4 acceptance outcomes:
//   - full SSE response streams over raw sockets
//   - 429 / 5xx injection -> backoff + eventual success; meter records retries
//   - mid-stream kill (truncation) and garbage bytes -> retry -> complete
//   - offline -> queue fills -> restore -> drains in FIFO order
//   - zero aborts (every scenario must exit cleanly)
// Exit code 0 iff every scenario passes.
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <thread>
#include <vector>

#include "core/clock.h"
#include "core/error.h"
#include "core/event_loop.h"
#include "net/http1.h"
#include "net/meter.h"
#include "net/offline.h"
#include "net/policy.h"
#include "net/pool.h"
#include "net/socket.h"
#include "net/transport.h"

namespace {

using namespace opencode;
using namespace opencode::core;
using namespace opencode::net;

int failures = 0;
#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__,         \
                         __LINE__, #cond);                               \
            ++failures;                                                  \
        }                                                                \
    } while (0)

EventLoop g_loop;

constexpr const char* kSseScript =
    "data: {\"type\":\"text\",\"text\":\"Hello\"}\r\n"
    "\r\n"
    "data: {\"type\":\"text\",\"text\":\" world\"}\r\n"
    "\r\n"
    "data: {\"type\":\"done\",\"finish_reason\":\"stop\"}\r\n"
    "\r\n";

enum class Behavior { ok, rate_limit, server_error, truncate, garbage };

struct Mock {
    std::shared_ptr<std::atomic<bool>> stop =
        std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<size_t>> served =
        std::make_shared<std::atomic<size_t>>(0);
    std::shared_ptr<std::vector<Behavior>> script =
        std::make_shared<std::vector<Behavior>>();
    std::shared_ptr<std::mutex> mu = std::make_shared<std::mutex>();
    std::thread th;
    int ls = -1;
    uint16_t port = 0;
};

std::string read_request(int fd) {
    std::string raw;
    char buf[1024];
    while (true) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0) break;
        raw.append(buf, static_cast<size_t>(n));
        if (raw.find("\r\n\r\n") != std::string::npos) break;
        if (raw.size() > 1u << 20) break;
    }
    return raw.substr(0, raw.find("\r\n"));
}

void send_all(int fd, const char* data, size_t n) {
    size_t off = 0;
    while (off < n) {
        const ssize_t w = ::write(fd, data + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        off += static_cast<size_t>(w);
    }
}

void serve_conn(int fd, const std::shared_ptr<Mock>& m) {
    (void)read_request(fd);
    const size_t idx = m->served->fetch_add(1);
    Behavior b = Behavior::ok;
    {
        std::lock_guard<std::mutex> lk(*m->mu);
        if (!m->script->empty()) b = (*m->script)[idx % m->script->size()];
    }

    const size_t len = std::strlen(kSseScript);
    char head[512];
    int n = 0;
    switch (b) {
        case Behavior::ok:
            n = std::snprintf(
                head, sizeof head,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: close\r\n"
                "Content-Length: %zu\r\n"
                "\r\n",
                len);
            send_all(fd, head, static_cast<size_t>(n));
            send_all(fd, kSseScript, len);
            break;
        case Behavior::rate_limit:
            n = std::snprintf(head, sizeof head,
                              "HTTP/1.1 429 Too Many Requests\r\n"
                              "Retry-After: 1\r\n"
                              "Content-Length: 0\r\n"
                              "Connection: close\r\n"
                              "\r\n");
            send_all(fd, head, static_cast<size_t>(n));
            break;
        case Behavior::server_error:
            n = std::snprintf(head, sizeof head,
                              "HTTP/1.1 500 Internal Server Error\r\n"
                              "Content-Length: 0\r\n"
                              "Connection: close\r\n"
                              "\r\n");
            send_all(fd, head, static_cast<size_t>(n));
            break;
        case Behavior::truncate:
            n = std::snprintf(
                head, sizeof head,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Connection: close\r\n"
                "Content-Length: %zu\r\n"
                "\r\n",
                len);
            send_all(fd, head, static_cast<size_t>(n));
            send_all(fd, kSseScript, len / 2); /* kill the stream mid-frame */
            break;
        case Behavior::garbage:
            send_all(fd, "NOT HTTP\r\n\r\n%^&*garbage\r\n", 24);
            break;
    }
    ::close(fd);
}

Mock start_mock(int port) {
    auto self = std::make_shared<Mock>(); /* stop/served/script/mu, shared */
    Mock m;
    m.stop = self->stop;
    m.served = self->served;
    m.script = self->script;
    m.mu = self->mu;
    m.ls = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(m.ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(m.ls, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) {
        std::fprintf(stderr, "drill: bind: %s\n", std::strerror(errno));
        std::exit(2);
    }
    ::listen(m.ls, 16);
    sockaddr_in g{};
    socklen_t gl = sizeof g;
    ::getsockname(m.ls, reinterpret_cast<sockaddr*>(&g), &gl);
    m.port = ntohs(g.sin_port);

    const int ls = m.ls;
    m.th = std::thread([self, ls]() {
        while (!self->stop->load()) {
            struct pollfd pfd{};
            pfd.fd = ls;
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, 100) <= 0) continue;
            const int c = ::accept(ls, nullptr, nullptr);
            if (c < 0) continue;
            std::thread([self, c]() { serve_conn(c, self); }).detach();
        }
        ::close(ls);
    });
    return m;
}

void kill_mock(Mock& m) {
    m.stop->store(true);
    ::shutdown(m.ls, SHUT_RDWR);
    if (m.th.joinable()) m.th.join();
    m.ls = -1;
}

void set_script(Mock& m, std::vector<Behavior> s) {
    std::lock_guard<std::mutex> lk(*m.mu);
    *m.script = std::move(s);
}

Pool::Key key(uint16_t port) {
    return Pool::Key{"127.0.0.1", port, false};
}

/* One exchange on a pooled transport; records rtt + bytes on `meter`.
 * Returns the wire/parse error, or ok once the full exchange completed (the
 * HTTP status is left for http_status_error()). */
error_code run_exchange(uint16_t port, const HttpRequest& req, Pool& pool,
                        Meter& meter, HttpResponse& resp) {
    const uint64_t t0 = now_mono_ms();
    Transport t;
    error_code ec =
        pool.acquire(g_loop, key(port), nullptr, now_mono_ms() + 5'000, t);
    if (!ec.ok()) return ec;
    ec = http_exchange(g_loop, t, req, now_mono_ms() + 10'000, resp);
    const uint64_t rtt = now_mono_ms() - t0;
    meter.record_rtt(rtt);
    meter.inc_round_trip();
    meter.add_bytes_in(resp.body.size());
    const bool healthy = ec.ok() && resp.keep_alive;
    pool.release(std::move(t), key(port), healthy);
    return ec;
}

/* Retry loop per 05_NETWORK_RESILIENCE.md Section 3. Returns true once a full
 * 2xx body arrived; asserts retries were recorded on the meter. When the
 * terminal (non-retryable) error is reached, it is returned via `terminal`. */
bool run_with_retries(uint16_t port, HttpRequest& req, Pool& pool,
                      Meter& meter, std::string& body,
                      error_code* terminal = nullptr) {
    RetryPolicy policy;
    uint32_t attempt = 1;
    while (true) {
        HttpResponse resp;
        const error_code wire = run_exchange(port, req, pool, meter, resp);
        error_code ec = wire;
        if (ec.ok()) ec = http_status_error(resp);
        if (ec.ok()) {
            body = resp.body;
            if (terminal) *terminal = core::ok();
            return true;
        }
        RetryDecision d;
        if (ec.code() == Err::e_rate_limit) {
            uint32_t ra = 1;
            const std::string_view v =
                http_header(resp.headers, "Retry-After");
            if (!v.empty()) ra = static_cast<uint32_t>(std::atoi(v.data()));
            d = policy.decide_retry_after(ra, attempt);
        } else {
            d = policy.decide(ec, attempt);
        }
        if (!d.retry) {
            if (terminal) *terminal = ec;
            return false;
        }
        meter.inc_retry();
        std::this_thread::sleep_for(
            std::chrono::milliseconds(d.delay_ms));
        ++attempt;
    }
}

HttpRequest chat_req(const char* id) {
    HttpRequest r;
    r.method = "POST";
    r.path = "/v1/chat";
    r.request_id = id;
    r.body = "{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
    r.headers.push_back(HttpHeader{"Content-Type", "application/json"});
    return r;
}

void sc_stream_full() {
    Mock m = start_mock(0);
    set_script(m, {Behavior::ok});
    Pool pool;
    Meter meter;
    HttpRequest req = chat_req("drill-stream");
    std::string body;
    CHECK(run_with_retries(m.port, req, pool, meter, body));
    CHECK(body.find("\"type\":\"text\"") != std::string::npos);
    CHECK(body.find("\"finish_reason\":\"stop\"") != std::string::npos);
    CHECK(meter.retries() == 0);
    CHECK(meter.round_trips() >= 1);
    CHECK(meter.bytes_in() == body.size());
    CHECK(meter.rtt_samples() >= 1); /* rtt recording happened */
    kill_mock(m);
    std::printf("  stream_full: OK\n");
}

void sc_rate_limit() {
    Mock m = start_mock(0);
    set_script(m, {Behavior::rate_limit, Behavior::ok});
    Pool pool;
    Meter meter;
    HttpRequest req = chat_req("drill-429");
    std::string body;
    const uint64_t t0 = now_mono_ms();
    CHECK(run_with_retries(m.port, req, pool, meter, body));
    const uint64_t elapsed = now_mono_ms() - t0;
    CHECK(meter.retries() == 1); /* one 429 -> one backoff */
    CHECK(elapsed >= 900);       /* Retry-After: 1s honored */
    CHECK(body.find("Hello") != std::string::npos);
    kill_mock(m);
    std::printf("  inject_429 backoff: OK\n");
}

void sc_server_error() {
    Mock m = start_mock(0);
    set_script(m, {Behavior::server_error, Behavior::server_error,
                   Behavior::ok});
    Pool pool;
    Meter meter;
    HttpRequest req = chat_req("drill-500");
    std::string body;
    CHECK(run_with_retries(m.port, req, pool, meter, body));
    CHECK(meter.retries() == 2); /* two 500s -> two retries */
    CHECK(body.find("Hello") != std::string::npos);
    kill_mock(m);
    std::printf("  inject_500 backoff: OK\n");
}

void sc_truncate() {
    Mock m = start_mock(0);
    set_script(m, {Behavior::truncate, Behavior::ok});
    Pool pool;
    Meter meter;
    HttpRequest req = chat_req("drill-trunc");
    std::string body;
    CHECK(run_with_retries(m.port, req, pool, meter, body));
    CHECK(meter.retries() >= 1); /* premature EOF is retryable */
    CHECK(body.find("finish_reason") != std::string::npos);
    kill_mock(m);
    std::printf("  truncate_mid_stream: OK\n");
}

void sc_garbage() {
    Mock m = start_mock(0);
    set_script(m, {Behavior::garbage, Behavior::ok, Behavior::ok});
    Pool pool;
    Meter meter;
    HttpRequest req = chat_req("drill-garbage");
    std::string body;
    error_code term;
    CHECK(!run_with_retries(m.port, req, pool, meter, body, &term));
    CHECK(term.code() == Err::e_proto_parse); /* clean, structured error */
    CHECK(meter.retries() == 0);              /* non-retryable: never retried */

    /* Session survives: the same loop/pool completes a healthy request. */
    HttpRequest ok_req = chat_req("drill-garbage-survive");
    std::string body2;
    CHECK(run_with_retries(m.port, ok_req, pool, meter, body2));
    CHECK(body2.find("Hello") != std::string::npos);
    kill_mock(m);
    std::printf("  garbage_bytes: OK\n");
}

/* Kill the provider, queue offline, restore -> FIFO drain -> online. */
void sc_offline_restore() {
    Mock m = start_mock(0);
    const uint16_t port = m.port;
    set_script(m, {Behavior::ok, Behavior::ok, Behavior::ok, Behavior::ok});

    OfflineQueue::Config cfg;
    cfg.probe_host = "127.0.0.1";
    cfg.probe_port = port;
    cfg.probe_timeout_ms = 500;
    OfflineQueue q(cfg);
    std::vector<std::string> states;
    q.set_state_cb([&](Connectivity c) {
        states.push_back(c == Connectivity::online   ? "online"
                         : c == Connectivity::offline ? "offline"
                                                       : "recovering");
    });

    CHECK(q.check(g_loop).ok());
    CHECK(q.state() == Connectivity::online);

    /* Kill the provider: probe fails -> offline, requests queue up. */
    kill_mock(m);
    CHECK(!q.check(g_loop).ok());
    CHECK(q.state() == Connectivity::offline);
    QueueEntry a, b, c;
    a.request_id = "a";
    b.request_id = "b";
    c.request_id = "c";
    CHECK(q.enqueue(std::move(a)).ok());
    CHECK(q.enqueue(std::move(b)).ok());
    CHECK(q.enqueue(std::move(c)).ok());
    CHECK(q.pending() == 3);

    /* Restore on the same port: recovering -> FIFO drain -> online. */
    Mock m2 = start_mock(port);
    CHECK(q.check(g_loop).ok());
    CHECK(q.state() == Connectivity::recovering);

    Pool pool;
    Meter meter;
    std::vector<std::string> drained;
    const auto drain = [&](EventLoop&, const QueueEntry& e,
                           uint64_t) -> error_code {
        HttpRequest req = chat_req(e.request_id.c_str());
        req.path = e.path;
        req.method = e.method;
        req.headers = e.headers;
        req.body = e.body;
        HttpResponse resp;
        const error_code ec = run_exchange(port, req, pool, meter, resp);
        if (!ec.ok()) return ec;
        const error_code cls = http_status_error(resp);
        if (!cls.ok()) return cls;
        drained.push_back(e.request_id);
        return core::ok();
    };
    CHECK(q.drain(g_loop, now_mono_ms() + 10'000, drain).ok());
    CHECK(drained.size() == 3);
    CHECK(drained[0] == "a" && drained[1] == "b" && drained[2] == "c");
    CHECK(q.pending() == 0);
    CHECK(q.state() == Connectivity::online);
    CHECK(meter.round_trips() == 3);
    kill_mock(m2);
    std::printf("  offline_restore: OK\n");
}

/* The loop and pool must survive the whole battery (zero aborts). */
void sc_survives() {
    Mock m = start_mock(0);
    set_script(m, {Behavior::ok});
    Pool pool;
    Meter meter;
    HttpRequest req = chat_req("drill-last");
    std::string body;
    CHECK(run_with_retries(m.port, req, pool, meter, body));
    CHECK(body.find("Hello") != std::string::npos);
    kill_mock(m);
    std::printf("  loop_survives: OK\n");
}

} /* namespace */

int main() {
    /* The mock writes to sockets whose peer may already be gone (probes, fault
     * scenarios); turn SIGPIPE off so a write never kills the battery. */
    std::signal(SIGPIPE, SIG_IGN);
    std::printf("drill: running Phase 4 fault-injection battery\n");
    sc_stream_full();
    sc_rate_limit();
    sc_server_error();
    sc_truncate();
    sc_garbage();
    sc_offline_restore();
    sc_survives();
    if (failures == 0) {
        std::printf("drill: ALL SCENARIOS PASS\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "drill: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}

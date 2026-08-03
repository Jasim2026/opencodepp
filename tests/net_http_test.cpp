/*
 * net_http_test.cpp -- HTTP/1.1 request builder, incremental response parser,
 * one-shot exchange, and SSE/JSONL streaming against in-process fake servers.
 * Local verification: plain g++ build-and-run (see protocol note in NOTES.md).
 */
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/error.h"
#include "core/event_loop.h"
#include "net/http1.h"
#include "net/sse.h"
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

int ephemeral_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) {
        ::close(fd);
        return 0;
    }
    sockaddr_in got{};
    socklen_t gl = sizeof got;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&got), &gl);
    const int port = ntohs(got.sin_port);
    ::listen(fd, 4);
    ::close(fd);
    return port;
}

std::string read_head(int fd) {
    std::string out;
    char buf[1024];
    while (true) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
        if (out.find("\r\n\r\n") != std::string::npos) break;
    }
    return out;
}

std::string chunk_enc(const std::string& body) {
    std::string out;
    auto hex = [](size_t n) {
        char b[32];
        std::snprintf(b, sizeof b, "%zx", n);
        return std::string(b);
    };
    out += "5\r\nhello\r\n";
    if (body.size() > 0) {
        out += hex(body.size());
        out += "\r\n";
        out += body;
        out += "\r\n";
    }
    out += "0\r\n\r\n";
    return out;
}

std::string make_response(int code, const char* reason,
                          const std::string& body, bool chunked,
                          const char* extra_headers = "") {
    char head[1024];
    std::string out;
    if (chunked) {
        std::snprintf(head, sizeof head,
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: text/event-stream\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "Connection: close\r\n"
                      "%s"
                      "\r\n",
                      code, reason, extra_headers);
        out = head;
        out += chunk_enc(body);
        return out;
    }
    std::snprintf(head, sizeof head,
                  "HTTP/1.1 %d %s\r\n"
                  "Content-Type: text/plain\r\n"
                  "Content-Length: %zu\r\n"
                  "%s"
                  "\r\n",
                  code, reason, body.size(), extra_headers);
    out = head;
    out += body;
    return out;
}

/* Multi-connection fake HTTP server on an ephemeral loopback port. The handler
 * maps the raw request head to the raw response bytes. */
struct FakeServer {
    struct State {
        std::atomic<bool> ready{false};
        std::atomic<bool> stop{false};
    };
    std::shared_ptr<State> st = std::make_shared<State>();
    std::thread th;
    int port = 0;

    FakeServer() = default;
    FakeServer(FakeServer&& o) noexcept
        : st(std::move(o.st)), th(std::move(o.th)), port(o.port) {}
    FakeServer(const FakeServer&) = delete;
    FakeServer& operator=(const FakeServer&) = delete;

    ~FakeServer() {
        if (st) st->stop.store(true);
        if (th.joinable()) th.join();
    }
};

FakeServer start_server(const std::function<std::string(std::string_view)>& h) {
    const int port = ephemeral_port();
    CHECK(port != 0);
    const int ls = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(ls >= 0);
    int one = 1;
    ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<uint16_t>(port));
    CHECK(::bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
    CHECK(::listen(ls, 8) == 0);

    FakeServer s;
    s.port = port;
    s.th = std::thread([ls, st = s.st, h]() {
        st->ready.store(true);
        while (!st->stop.load()) {
            struct pollfd pfd{};
            pfd.fd = ls;
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, 200) <= 0) continue;
            const int c = ::accept(ls, nullptr, nullptr);
            if (c < 0) continue;
            const std::string req = read_head(c);
            const std::string resp = h(req);
            size_t off = 0;
            while (off < resp.size()) {
                const ssize_t w = ::write(c, resp.data() + off, resp.size() - off);
                if (w <= 0) break;
                off += static_cast<size_t>(w);
            }
            ::close(c);
        }
        ::close(ls);
    });
    while (!s.st->ready.load()) std::this_thread::yield();
    return s;
}

Transport connect_fake(EventLoop& loop, int port) {
    Transport t;
    Socket s;
    const error_code ec =
        s.connect(loop, Addr{"127.0.0.1", static_cast<uint16_t>(port)}, 2000);
    CHECK(ec.ok());
    CHECK(t.attach(std::move(s), nullptr).ok());
    return t;
}

void test_build_request() {
    HttpRequest req;
    req.method = "POST";
    req.path = "/v1/chat";
    req.headers = {{"Host", "api.example.com"},
                   {"Content-Type", "application/json"},
                   {"X-Request-Id", "rid-1"}};
    req.body = "{\"x\":1}";
    req.request_id = "rid-1";
    std::string wire;
    CHECK(http_build_request(req, wire).ok());
    CHECK(wire.rfind("POST /v1/chat HTTP/1.1", 0) == 0);
    CHECK(wire.find("Content-Length: 7") != std::string::npos);
    CHECK(wire.find("X-Request-Id: rid-1") != std::string::npos);
    CHECK(wire.find("\r\n\r\n{\"x\":1}") != std::string::npos);

    HttpRequest get;
    std::string w2;
    CHECK(http_build_request(get, w2).ok());
    CHECK(w2.rfind("GET / HTTP/1.1", 0) == 0);
    CHECK(w2.find("Content-Length:") == std::string::npos);

    HttpRequest big;
    for (int i = 0; i < 2000; ++i)
        big.headers.push_back({std::string("H") + std::to_string(i),
                               std::string(64, 'x')});
    std::string w3;
    CHECK(http_build_request(big, w3).code() == Err::e_net_overflow);
    std::printf("  build request: OK\n");
}

void test_parse_content_length() {
    const std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 11\r\n"
        "\r\n"
        "hello world";
    HttpParser p;
    for (size_t i = 0; i < resp.size(); i += 3) {
        CHECK(p.feed(std::string_view(resp).substr(i, 3)).ok());
    }
    CHECK(p.head_done());
    CHECK(p.head().code == 200);
    CHECK(p.expected_body() == 11);
    CHECK(p.body_done());
    CHECK(p.body_received() == 11);
    char buf[32];
    const size_t n = p.take(buf, sizeof buf);
    CHECK(n == 11);
    CHECK(std::memcmp(buf, "hello world", 11) == 0);
    CHECK(p.buffered() == 0);
    std::printf("  parse content-length: OK\n");
}

void test_parse_chunked() {
    const std::string body = "hello world, chunked body!";
    const std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n" +
        chunk_enc(body);
    HttpParser p;
    /* Awkward one-byte feeds exercise partial-size / partial-data paths. */
    for (const char c : resp) CHECK(p.feed(std::string_view(&c, 1)).ok());
    CHECK(p.head_done());
    CHECK(p.chunked());
    CHECK(p.body_done());
    CHECK(p.body_received() == 5 + body.size()); /* "hello" + body */
    std::string got;
    char buf[32];
    while (p.buffered() > 0) {
        const size_t n = p.take(buf, sizeof buf);
        got.append(buf, n);
    }
    CHECK(got == "hello" + body);
    std::printf("  parse chunked: OK\n");
}

void test_parse_eof_framed() {
    const std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "\r\n"
        "streaming bytes, no length";
    HttpParser p;
    CHECK(p.feed(resp).ok());
    CHECK(p.head_done());
    CHECK(p.eof_framed());
    CHECK(!p.body_done());
    char buf[64];
    const size_t n = p.take(buf, sizeof buf);
    CHECK(std::string(buf, n) == "streaming bytes, no length");
    CHECK(!p.keep_alive());
    std::printf("  parse eof-framed: OK\n");
}

void test_parse_errors() {
    HttpParser p;
    CHECK(p.feed("HTTP/1.1 NOT_A_CODE\r\n\r\n").code() == Err::e_proto_parse);

    HttpParser q;
    CHECK(q.feed("HTTP/1.1 200 OK\r\nNo-Colon\r\n\r\n").code() ==
          Err::e_proto_parse);

    HttpParser r;
    std::string huge;
    huge.reserve(70 * 1024);
    while (huge.size() < 70 * 1024) huge += "X: y\r\n";
    CHECK(r.feed(huge).code() == Err::e_net_overflow);
    std::printf("  parse errors: OK\n");
}

void test_status_error() {
    HttpResponse ok_;
    ok_.code = 200;
    CHECK(http_status_error(ok_).ok());
    HttpResponse rate;
    rate.code = 429;
    CHECK(http_status_error(rate).code() == Err::e_rate_limit);
    HttpResponse auth;
    auth.code = 401;
    CHECK(http_status_error(auth).code() == Err::e_auth);
    HttpResponse nf;
    nf.code = 404;
    CHECK(http_status_error(nf).code() == Err::e_provider_err);
    HttpResponse srv;
    srv.code = 500;
    CHECK(http_status_error(srv).code() == Err::e_provider_err);
    HttpResponse redir;
    redir.code = 302;
    CHECK(http_status_error(redir).code() == Err::e_net_http);
    std::printf("  status error mapping: OK\n");
}

void test_exchange() {
    EventLoop loop;
    auto srv = start_server([](std::string_view req) -> std::string {
        if (req.find("GET /health ") != std::string::npos)
            return make_response(200, "OK", "ok\n", false);
        if (req.find("POST /echo ") != std::string::npos) {
            const size_t p = req.find("\r\n\r\n");
            const std::string body =
                p == std::string::npos ? "" : std::string(req.substr(p + 4));
            return make_response(200, "OK", body, false);
        }
        if (req.find("GET /boom ") != std::string::npos)
            return make_response(429, "Too Many Requests", "slow down\n", false);
        return make_response(404, "Not Found", "nope\n", false);
    });

    const uint64_t dl = now_mono_ms() + 5000;

    Transport t = connect_fake(loop, srv.port);
    HttpResponse r;
    HttpRequest get;
    get.method = "GET";
    get.path = "/health";
    get.headers = {{"Host", "127.0.0.1"}};
    CHECK(http_exchange(loop, t, get, dl, r).ok());
    CHECK(r.code == 200);
    CHECK(r.body == "ok\n");
    CHECK(http_status_error(r).ok());
    CHECK(r.keep_alive);

    Transport t2 = connect_fake(loop, srv.port);
    HttpRequest post;
    post.method = "POST";
    post.path = "/echo";
    post.headers = {{"Host", "127.0.0.1"}, {"Content-Type", "text/plain"}};
    post.body = "echo me";
    HttpResponse r2;
    CHECK(http_exchange(loop, t2, post, dl, r2).ok());
    CHECK(r2.code == 200);
    CHECK(r2.body == "echo me");

    Transport t3 = connect_fake(loop, srv.port);
    HttpRequest boom;
    boom.method = "GET";
    boom.path = "/boom";
    boom.headers = {{"Host", "127.0.0.1"}};
    HttpResponse r3;
    CHECK(http_exchange(loop, t3, boom, dl, r3).ok());
    CHECK(r3.code == 429);
    CHECK(http_status_error(r3).code() == Err::e_rate_limit);
    CHECK(r3.body == "slow down\n");

    /* Truncation guard: a cap smaller than the body leaves truncated=true and
     * a bounded body. */
    Transport t4 = connect_fake(loop, srv.port);
    HttpRequest ok2;
    ok2.method = "GET";
    ok2.path = "/health";
    ok2.headers = {{"Host", "127.0.0.1"}};
    HttpResponse r4;
    CHECK(http_exchange(loop, t4, ok2, dl, r4, 2).ok());
    CHECK(r4.truncated);
    CHECK(r4.body.size() == 2);

    std::printf("  http exchange: OK\n");
}

void test_sse_parse() {
    const std::string script =
        "id: 1\r\n"
        "event: message\r\n"
        "data: {\"t\":\"a\"}\r\n"
        "\r\n"
        ": comment line\r\n"
        "data: {\"t\":\"b\"}\r\n"
        "data: {\"t\":\"c\"}\r\n"
        "\r\n"
        "event: done\r\n"
        "data: {\"t\":\"d\"}\r\n"
        "\r\n";
    SseParser sp;
    int n = 0;
    const auto sink = [&](const SseEvent& ev) {
        ++n;
        if (n == 1) {
            CHECK(ev.id == "1");
            CHECK(ev.event == "message");
            CHECK(ev.data == "{\"t\":\"a\"}");
        }
        if (n == 2) {
            CHECK(ev.data == "{\"t\":\"b\"}\n{\"t\":\"c\"}");
            CHECK(ev.event == "");
        }
        if (n == 3) {
            CHECK(ev.event == "done");
            CHECK(ev.data == "{\"t\":\"d\"}");
        }
    };
    CHECK(sp.feed(script, sink).ok());
    CHECK(n == 3);
    CHECK(sp.events() == 3);

    /* Byte-by-byte (partial frame buffering). */
    SseParser sp2;
    int n2 = 0;
    for (const char c : script)
        CHECK(sp2.feed(std::string_view(&c, 1), [&](const SseEvent&) { ++n2; })
                  .ok());
    CHECK(n2 == 3);

    /* LF-only framing. */
    const std::string lf = "data: one\n\ndata: two\n\n";
    SseParser sp3;
    int n3 = 0;
    CHECK(sp3.feed(lf, [&](const SseEvent&) { ++n3; }).ok());
    CHECK(n3 == 2);

    /* Unterminated trailing frame is buffered, not emitted. */
    SseParser sp4;
    int n4 = 0;
    CHECK(sp4.feed("data: partial", [&](const SseEvent&) { ++n4; }).ok());
    CHECK(n4 == 0);

    /* Per-frame cap breach. */
    SseParser sp5;
    sp5.set_max_frame_bytes(32);
    std::string big = "data: ";
    big.append(100, 'x');
    CHECK(sp5.feed(big, [&](const SseEvent&) {}).code() == Err::e_net_overflow);

    /* Event-count limit. */
    SseParser sp6;
    sp6.set_max_events(1);
    CHECK(sp6.feed("data: a\n\ndata: b\n\n", [&](const SseEvent&) {})
              .code() == Err::e_net_overflow);

    /* Abort between frames. */
    std::atomic<bool> abort_flag{false};
    SseParser sp7;
    sp7.set_abort_flag(&abort_flag);
    CHECK(sp7.feed("data: a\n\n", [&](const SseEvent&) {}).ok());
    abort_flag.store(true);
    CHECK(sp7.feed("data: b\n\n", [&](const SseEvent&) {})
              .code() == Err::e_cancelled);
    std::printf("  sse parse: OK\n");
}

void test_sse_jsonl() {
    const std::string lines = "{\"i\":1}\n{\"i\":2}\n";
    SseParser sp(StreamKind::jsonl);
    int n = 0;
    std::string d1, d2;
    CHECK(sp.feed(lines, [&](const SseEvent& ev) {
        ++n;
        if (n == 1) d1 = ev.data;
        if (n == 2) d2 = ev.data;
    }).ok());
    CHECK(n == 2);
    CHECK(d1 == "{\"i\":1}");
    CHECK(d2 == "{\"i\":2}");
    std::printf("  sse jsonl: OK\n");
}

void test_sse_stream() {
    const std::string body =
        "data: {\"t\":\"s1\"}\r\n"
        "\r\n"
        "data: {\"t\":\"s2\"}\r\n"
        "\r\n";
    EventLoop loop;
    /* Serve the SSE body chunked, splitting mid-stream (between the two
     * frames) to exercise partial-frame delivery over a real connection. */
    const size_t split = body.find("\r\n\r\n") + 4;
    auto srv = start_server([&](std::string_view) -> std::string {
        std::string raw;
        raw += "HTTP/1.1 200 OK\r\n"
               "Content-Type: text/event-stream\r\n"
               "Transfer-Encoding: chunked\r\n"
               "Connection: close\r\n"
               "\r\n";
        char hx[32];
        std::snprintf(hx, sizeof hx, "%zx\r\n", split);
        raw += hx;
        raw += body.substr(0, split);
        raw += "\r\n";
        std::snprintf(hx, sizeof hx, "%zx\r\n", body.size() - split);
        raw += hx;
        raw += body.substr(split);
        raw += "\r\n0\r\n\r\n";
        return raw;
    });

    const uint64_t dl = now_mono_ms() + 5000;

    /* Stream the response body into the SSE parser over a real connection. */
    Transport t = connect_fake(loop, srv.port);
    HttpRequest req;
    req.method = "GET";
    req.path = "/stream";
    req.headers = {{"Host", "127.0.0.1"}};

    std::string wire;
    CHECK(http_build_request(req, wire).ok());
    ssize_t sent = 0;
    size_t off = 0;
    while (off < wire.size()) {
        const error_code ec =
            t.write(loop, reinterpret_cast<const uint8_t*>(wire.data() + off),
                    wire.size() - off, dl, sent);
        CHECK(ec.ok());
        off += static_cast<size_t>(sent);
    }

    HttpParser p;
    SseParser sp;
    int n = 0;
    std::string collected;
    const error_code ec = sse_stream(
        loop, t, p, dl, sp, [&](const SseEvent& ev) {
            ++n;
            collected += ev.data;
        });
    CHECK(ec.ok());
    CHECK(n == 2);
    CHECK(collected == "{\"t\":\"s1\"}{\"t\":\"s2\"}");

    /* Short-circuited chunked body with a Connection: close still parses. */
    Transport t2 = connect_fake(loop, srv.port);
    HttpParser p2;
    SseParser sp2;
    int n2 = 0;
    std::string collected2;
    ssize_t sent2 = 0;
    size_t off2 = 0;
    while (off2 < wire.size()) {
        const error_code ec2w =
            t2.write(loop, reinterpret_cast<const uint8_t*>(wire.data() + off2),
                     wire.size() - off2, dl, sent2);
        CHECK(ec2w.ok());
        off2 += static_cast<size_t>(sent2);
    }
    const error_code ec2 = sse_stream(
        loop, t2, p2, dl, sp2, [&](const SseEvent& ev) {
            ++n2;
            collected2 += ev.data;
        });
    CHECK(ec2.ok());
    CHECK(n2 == 2);
    CHECK(collected2 == "{\"t\":\"s1\"}{\"t\":\"s2\"}");
    std::printf("  sse stream: OK\n");
}

} /* namespace */

int main() {
    test_build_request();
    test_parse_content_length();
    test_parse_chunked();
    test_parse_eof_framed();
    test_parse_errors();
    test_status_error();
    test_exchange();
    test_sse_parse();
    test_sse_jsonl();
    test_sse_stream();
    if (failures == 0) {
        std::printf("net_http_test: OK\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "net_http_test: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}

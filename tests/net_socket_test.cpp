// net_socket_test.cpp -- Phase 4: non-blocking socket on the event loop.
//
// Exercises connect (echo round trip, connection-refused), recv with an
// absolute deadline (timeout on a silent peer), graceful shutdown (EOF), and
// IPv6 when loopback v6 is available. Every check is deterministic (loopback
// only; no real DNS, no public network).
#include <arpa/inet.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "core/clock.h"
#include "core/error.h"
#include "core/event_loop.h"
#include "net/socket.h"

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

using opencode::core::Err;
using opencode::core::error_code;
using opencode::core::EventLoop;
using opencode::core::now_mono_ms;
using opencode::net::Addr;
using opencode::net::Socket;

/* Bind an ephemeral loopback TCP port and return it (0 on failure). */
int ephemeral_port(bool v6) {
    const int fd = ::socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    if (v6) {
        sockaddr_in6 a{};
        a.sin6_family = AF_INET6;
        a.sin6_addr = in6addr_loopback;
        a.sin6_port = 0;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) {
            ::close(fd);
            return 0;
        }
        sockaddr_in6 got{};
        socklen_t gl = sizeof got;
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&got), &gl);
        ::listen(fd, 4);
        ::close(fd);
        return ntohs(got.sin6_port);
    }
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
    ::listen(fd, 4);
    ::close(fd);
    return ntohs(got.sin_port);
}

/* A bound loopback listener owned by a helper thread. `ready` flips once the
 * listener is up so the client never connects too early (or hangs forever). */
struct Server {
    std::atomic<bool> ready{false};
    int port = 0;
    std::thread th;

    ~Server() {
        if (th.joinable()) th.join();
    }
};

int listener_for(int port, bool v6, std::atomic<bool>* ready) {
    const int ls = ::socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return -1;
    int one = 1;
    ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (v6) {
        sockaddr_in6 a{};
        a.sin6_family = AF_INET6;
        a.sin6_addr = in6addr_loopback;
        a.sin6_port = htons(static_cast<uint16_t>(port));
        if (::bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0 ||
            ::listen(ls, 4) < 0) {
            ::close(ls);
            return -1;
        }
    } else {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(static_cast<uint16_t>(port));
        if (::bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0 ||
            ::listen(ls, 4) < 0) {
            ::close(ls);
            return -1;
        }
    }
    if (ready != nullptr) ready->store(true);
    return ls;
}

/* Accept with a bounded wait so a failed test never hangs the suite. */
int accept_bounded(int ls) {
    struct pollfd pfd{};
    pfd.fd = ls;
    pfd.events = POLLIN;
    if (::poll(&pfd, 1, 10000) <= 0) return -1;
    return ::accept(ls, nullptr, nullptr);
}

/* Serve one connection: read `n` bytes, echo them, then close. */
void echo_server(int port, bool v6, std::atomic<bool>* ready, size_t n,
                 int delay_ms) {
    const int ls = listener_for(port, v6, ready);
    if (ls < 0) return;
    const int c = accept_bounded(ls);
    if (c >= 0) {
        std::string buf(n, '\0');
        size_t got = 0;
        while (got < n) {
            const ssize_t r = ::read(c, &buf[got], n - got);
            if (r <= 0) break;
            got += static_cast<size_t>(r);
        }
        if (delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        if (got > 0) {
            size_t off = 0;
            while (off < got) {
                const ssize_t w = ::write(c, buf.data() + off, got - off);
                if (w <= 0) break;
                off += static_cast<size_t>(w);
            }
        }
        ::close(c);
    }
    ::close(ls);
}

/* Read until EOF, then close (used to observe the client's FIN). */
void drain_server(int port, bool v6, std::atomic<bool>* ready) {
    const int ls = listener_for(port, v6, ready);
    if (ls < 0) return;
    const int c = accept_bounded(ls);
    if (c >= 0) {
        char buf[64];
        while (::read(c, buf, sizeof buf) > 0) {
        }
        ::close(c);
    }
    ::close(ls);
}

void test_echo(bool v6) {
    const int port = ephemeral_port(v6);
    if (port == 0) {
        std::printf("  (IPv6 loopback unavailable; skipped)\n");
        return;
    }
    const std::string host = v6 ? "::1" : "127.0.0.1";
    Server srv;
    srv.port = port;
    srv.th = std::thread(echo_server, port, v6, &srv.ready, 4, 0);
    while (!srv.ready.load()) std::this_thread::yield();

    EventLoop loop;
    Socket s;
    const error_code ce = s.connect(loop, Addr{host, static_cast<uint16_t>(port)}, 2000);
    if (!ce.ok()) {
        std::fprintf(stderr, "  [echo%s] connect failed: msg=%s detail=%u host=%s port=%d\n",
                     v6 ? " (v6)" : "", ce.message().data(), ce.detail(), host.c_str(), port);
    }
    CHECK(ce.ok());
    CHECK(s.connected());
    CHECK(s.fd() >= 0);

    if (ce.ok()) {
        const char* msg = "ping";
        ssize_t sent = 0;
        const error_code we = s.send(loop, reinterpret_cast<const uint8_t*>(msg), 4, now_mono_ms() + 2000, sent);
        CHECK(we.ok() && sent == 4);

        uint8_t buf[4] = {0};
        ssize_t got = 0;
        const error_code re = s.recv(loop, buf, sizeof buf, now_mono_ms() + 2000, got);
        CHECK(re.ok() && got == 4 && std::memcmp(buf, "ping", 4) == 0);
    }
    s.close();
    std::printf("  echo%s round trip: OK\n", v6 ? " (v6)" : "");
}

void test_recv_timeout() {
    const int port = ephemeral_port(false);
    CHECK(port != 0);
    Server srv;
    srv.port = port;
    srv.th = std::thread(echo_server, port, false, &srv.ready, 0, 5000); /* accept, never send */
    while (!srv.ready.load()) std::this_thread::yield();

    EventLoop loop;
    Socket s;
    CHECK(s.connect(loop, Addr{"127.0.0.1", static_cast<uint16_t>(port)}, 2000).ok());
    uint8_t buf[8];
    ssize_t got = 0;
    const error_code re = s.recv(loop, buf, sizeof buf, now_mono_ms() + 200, got);
    CHECK(re.code() == Err::e_net_timeout);
    CHECK(got == 0);
    s.close();
    std::printf("  recv timeout: OK\n");
}

void test_connect_refused() {
    /* Bind but do NOT listen: the handshake gets an RST deterministically and
     * no other process can steal the port while we hold it. */
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    CHECK(::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
    sockaddr_in got{};
    socklen_t gl = sizeof got;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&got), &gl);
    const int port = ntohs(got.sin_port);

    EventLoop loop;
    Socket s;
    const error_code ce = s.connect(loop, Addr{"127.0.0.1", static_cast<uint16_t>(port)}, 2000);
    CHECK(ce.code() == Err::e_net_connect);
    CHECK(!s.connected());
    s.close();
    ::close(fd);
    std::printf("  connect refused: OK\n");
}

void test_shutdown_eof() {
    const int port = ephemeral_port(false);
    CHECK(port != 0);
    Server srv;
    srv.port = port;
    srv.th = std::thread(drain_server, port, false, &srv.ready);
    while (!srv.ready.load()) std::this_thread::yield();

    EventLoop loop;
    Socket s;
    CHECK(s.connect(loop, Addr{"127.0.0.1", static_cast<uint16_t>(port)}, 2000).ok());
    const char* msg = "data";
    ssize_t sent = 0;
    CHECK(s.send(loop, reinterpret_cast<const uint8_t*>(msg), 4, now_mono_ms() + 2000, sent).ok());
    CHECK(s.shutdown_write().ok());

    /* The drain server sees our FIN, closes; we observe EOF. */
    uint8_t buf[8];
    ssize_t got = 0;
    const error_code re = s.recv(loop, buf, sizeof buf, now_mono_ms() + 2000, got);
    CHECK(re.ok() && got == 0 && s.eof());
    s.close();
    std::printf("  shutdown -> peer EOF: OK\n");
}

} /* namespace */

int main() {
    test_echo(false);
    test_echo(true);
    test_recv_timeout();
    test_connect_refused();
    test_shutdown_eof();
    if (failures == 0) {
        std::printf("net_socket_test: OK\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "net_socket_test: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}

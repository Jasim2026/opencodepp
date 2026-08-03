/*
 * socket.h -- non-blocking TCP socket on the caller's event loop.
 *
 * The engine's I/O doctrine (05_NETWORK_RESILIENCE.md Section 1): all I/O is
 * asynchronous on the caller-owned EventLoop, no thread-per-request. A net op
 * that would block registers fd interest (plus timers for deadlines) on the
 * caller's loop and waits on it -- the "blocking-over-loop" pattern. The loop
 * thread pumps run_once(); the net layer itself never blocks the OS thread.
 *
 * Never throws, never aborts: every path returns a core::error_code with a
 * network category (05_NETWORK_RESILIENCE.md Section 1.2).
 */
#ifndef OPENCODE_NET_SOCKET_H
#define OPENCODE_NET_SOCKET_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "core/clock.h"
#include "core/error.h"
#include "core/event_loop.h"

namespace opencode::net {

struct Addr {
    std::string host;
    uint16_t port = 0;
};

/* Drive the caller's loop until `done()` is true or `deadline_ms` (monotonic)
 * passes. deadline_ms == 0 waits forever (polled in slices so timers and other
 * fds still run). Returns true iff `done()` became true. */
bool pump_until(core::EventLoop& loop, uint64_t deadline_ms,
                const std::function<bool()>& done);

/* Wait until `fd` is readable (or `deadline_ms`, 0 = forever). Registers the
 * fd on the loop, pumps, deregisters. Returns ok or e_net_timeout. */
core::error_code wait_fd_readable(core::EventLoop& loop, int fd,
                                  uint64_t deadline_ms);

/* Wait until `fd` is writable (or `deadline_ms`, 0 = forever). */
core::error_code wait_fd_writable(core::EventLoop& loop, int fd,
                                  uint64_t deadline_ms);

class Socket {
public:
    Socket() = default;
    ~Socket() { close(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& o) noexcept
        : fd_(o.fd_), connected_(o.connected_), eof_(o.eof_) {
        o.fd_ = -1;
        o.connected_ = false;
        o.eof_ = false;
    }
    Socket& operator=(Socket&& o) noexcept {
        if (this != &o) {
            close();
            fd_ = o.fd_;
            connected_ = o.connected_;
            eof_ = o.eof_;
            o.fd_ = -1;
            o.connected_ = false;
            o.eof_ = false;
        }
        return *this;
    }

    /* Create a fresh non-blocking TCP fd for `family` (AF_INET/AF_INET6). */
    core::error_code open(int family);

    /* Connect to `addr`, trying every getaddrinfo result until one succeeds.
     * `timeout_ms` bounds the whole attempt (0 = wait indefinitely, polled in
     * slices). On success: connected, TCP_NODELAY + keepalive set, NOT
     * registered with the loop. */
    core::error_code connect(core::EventLoop& loop, const Addr& addr,
                             uint32_t timeout_ms);

    bool connected() const noexcept { return connected_; }
    bool eof() const noexcept { return eof_; }
    int fd() const noexcept { return fd_; }

    /* Read up to n bytes: got > 0 on data; got == 0 + ok() on EOF (eof());
     * e_net_timeout when the absolute deadline passes before data.
     * deadline_ms == 0 waits indefinitely (polled in slices).
     * Blocking-over-loop. */
    core::error_code recv(core::EventLoop& loop, uint8_t* buf, size_t n,
                          uint64_t deadline_ms, ssize_t& got);

    /* Write up to n bytes; sent < n when the peer buffer filled (caller retries
     * with the remainder). Blocking-over-loop. */
    core::error_code send(core::EventLoop& loop, const uint8_t* data, size_t n,
                          uint64_t deadline_ms, ssize_t& sent);

    /* Send FIN so the peer observes EOF; subsequent recv drains to eof(). */
    core::error_code shutdown_write();

    /* Close the fd, idempotent. Always succeeds (best-effort per doctrine). */
    void close() noexcept;

    /* Internal event-loop waiter; exposed for wait_fd_readable/writable. */
    struct Waiter {
        bool ready = false;
    };
    static void on_fd(void* userdata, uint32_t events) noexcept;

private:
    core::error_code wait_readable(core::EventLoop& loop, uint64_t deadline_ms);
    core::error_code wait_writable(core::EventLoop& loop, uint64_t deadline_ms);

    int fd_ = -1;
    bool connected_ = false;
    bool eof_ = false;
};

} /* namespace opencode::net */

#endif /* OPENCODE_NET_SOCKET_H */

#include "net/socket.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace opencode::net {

namespace {

constexpr uint64_t kWaitSliceMs = 60'000; /* poll slice for indefinite waits */

/* Map a syscall-level failure to the retryable net vocabulary. */
core::error_code from_errno(int e) {
    return core::make_error_code(core::Err::e_net_connect,
                                 static_cast<uint32_t>(e));
}

} /* namespace */

bool pump_until(core::EventLoop& loop, uint64_t deadline_ms,
                const std::function<bool()>& done) {
    while (!done()) {
        const uint64_t now = core::now_mono_ms();
        if (deadline_ms != 0 && now >= deadline_ms) return done();
        int64_t remain = deadline_ms != 0
                             ? static_cast<int64_t>(deadline_ms - now)
                             : static_cast<int64_t>(kWaitSliceMs);
        if (remain > static_cast<int64_t>(kWaitSliceMs)) remain = kWaitSliceMs;
        loop.run_once(static_cast<int>(remain));
    }
    return true;
}

void Socket::on_fd(void* userdata, uint32_t) noexcept {
    static_cast<Waiter*>(userdata)->ready = true;
}

core::error_code wait_fd_readable(core::EventLoop& loop, int fd,
                                  uint64_t deadline_ms) {
    Socket::Waiter w;
    if (!loop.add(fd, core::EventLoop::kRead, Socket::on_fd, &w)) {
        return core::make_error_code(core::Err::e_internal, 1);
    }
    const bool ok_ = pump_until(loop, deadline_ms, [&w] { return w.ready; });
    loop.del(fd);
    return ok_ ? core::ok()
               : core::make_error_code(core::Err::e_net_timeout);
}

core::error_code wait_fd_writable(core::EventLoop& loop, int fd,
                                  uint64_t deadline_ms) {
    Socket::Waiter w;
    if (!loop.add(fd, core::EventLoop::kWrite | core::EventLoop::kErr |
                          core::EventLoop::kHup,
                  Socket::on_fd, &w)) {
        return core::make_error_code(core::Err::e_internal, 1);
    }
    const bool ok_ = pump_until(loop, deadline_ms, [&w] { return w.ready; });
    loop.del(fd);
    return ok_ ? core::ok()
               : core::make_error_code(core::Err::e_net_timeout);
}

core::error_code Socket::open(int family) {
    close();
    const int fd = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                            0);
    if (fd < 0) return from_errno(errno);
    fd_ = fd;
    return core::ok();
}

core::error_code Socket::connect(core::EventLoop& loop, const Addr& addr,
                                 uint32_t timeout_ms) {
    close();
    if (addr.host.empty()) return core::make_error_code(core::Err::e_net_resolve);

    const uint64_t deadline =
        timeout_ms ? core::now_mono_ms() + timeout_ms : 0;
    const std::string port_str = std::to_string(addr.port);

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const int grc = ::getaddrinfo(addr.host.c_str(), port_str.c_str(), &hints,
                                  &res);
    if (grc != 0 || res == nullptr) {
        return core::make_error_code(core::Err::e_net_resolve);
    }

    core::error_code last = core::make_error_code(core::Err::e_net_connect);
    bool done = false;
    for (addrinfo* ai = res; ai != nullptr && !done; ai = ai->ai_next) {
        if (deadline != 0 && core::now_mono_ms() >= deadline) {
            last = core::make_error_code(core::Err::e_net_timeout);
            break;
        }
        const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        const int fl = ::fcntl(fd, F_GETFL, 0);
        if (fl >= 0) ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);

        int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            fd_ = fd;
            connected_ = true;
            done = true;
            break;
        }
        if (rc < 0 && errno != EINPROGRESS) {
            last = from_errno(errno);
            ::close(fd);
            continue;
        }

        /* EINPROGRESS: wait for writability + SO_ERROR, bounded by deadline. */
        Waiter w;
        if (!loop.add(fd, core::EventLoop::kWrite | core::EventLoop::kErr |
                              core::EventLoop::kHup,
                      on_fd, &w)) {
            ::close(fd);
            last = core::make_error_code(core::Err::e_internal, 2);
            break;
        }
        pump_until(loop, deadline, [&w] { return w.ready; });
        loop.del(fd);

        int soerr = 0;
        socklen_t slen = sizeof soerr;
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) == 0 &&
            soerr == 0 && w.ready) {
            fd_ = fd;
            connected_ = true;
            done = true;
            break;
        }
        if (!w.ready && deadline != 0 && core::now_mono_ms() >= deadline) {
            last = core::make_error_code(core::Err::e_net_timeout);
        } else {
            last = from_errno(soerr != 0 ? soerr : ECONNREFUSED);
        }
        ::close(fd);
    }
    ::freeaddrinfo(res);

    if (!done) return last;
    /* Post-connect tuning: NODELAY + keepalive. */
    const int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    ::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    return core::ok();
}

core::error_code Socket::recv(core::EventLoop& loop, uint8_t* buf, size_t n,
                              uint64_t deadline_ms, ssize_t& got) {
    got = 0;
    if (fd_ < 0) return core::make_error_code(core::Err::e_net_connect);
    const uint64_t deadline = deadline_ms;
    for (;;) {
        if (deadline != 0 && core::now_mono_ms() >= deadline) {
            return core::make_error_code(core::Err::e_net_timeout);
        }
        const ssize_t r = ::read(fd_, buf, n);
        if (r > 0) {
            got = r;
            return core::ok();
        }
        if (r == 0) {
            eof_ = true;
            return core::ok();
        }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            const core::error_code ew = wait_readable(loop, deadline);
            if (!ew.ok()) return ew;
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        /* ECONNRESET / EPIPE mid-stream: retryable EOF-class error. */
        eof_ = true;
        return from_errno(errno);
    }
}

core::error_code Socket::send(core::EventLoop& loop, const uint8_t* data,
                              size_t n, uint64_t deadline_ms, ssize_t& sent) {
    sent = 0;
    if (fd_ < 0) return core::make_error_code(core::Err::e_net_connect);
    const uint64_t deadline = deadline_ms;
    for (;;) {
        if (deadline != 0 && core::now_mono_ms() >= deadline) {
            return core::make_error_code(core::Err::e_net_timeout);
        }
        const ssize_t w = ::write(fd_, data, n);
        if (w > 0) {
            sent = w;
            return core::ok();
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            const core::error_code ew = wait_writable(loop, deadline);
            if (!ew.ok()) return ew;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        return from_errno(errno);
    }
}

core::error_code Socket::wait_readable(core::EventLoop& loop,
                                       uint64_t deadline_ms) {
    return wait_fd_readable(loop, fd_, deadline_ms);
}

core::error_code Socket::wait_writable(core::EventLoop& loop,
                                       uint64_t deadline_ms) {
    return wait_fd_writable(loop, fd_, deadline_ms);
}

core::error_code Socket::shutdown_write() {
    if (fd_ < 0) return core::make_error_code(core::Err::e_net_connect);
    ::shutdown(fd_, SHUT_WR);
    return core::ok();
}

void Socket::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
        connected_ = false;
        eof_ = false;
    }
}

} /* namespace opencode::net */

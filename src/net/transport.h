/*
 * transport.h -- a connected (optionally TLS) byte stream.
 *
 * Owns a non-blocking Socket plus an optional TlsBackend. Raw or TLS reads and
 * writes share one blocking-over-loop interface with an absolute deadline
 * (0 = wait indefinitely). Connection pooling (net/pool) moves Transports
 * between idle and in-flight states.
 */
#ifndef OPENCODE_NET_TRANSPORT_H
#define OPENCODE_NET_TRANSPORT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "core/clock.h"
#include "core/error.h"
#include "core/event_loop.h"
#include "net/socket.h"
#include "net/tls.h"

namespace opencode::net {

class Transport {
public:
    Transport() = default;
    ~Transport() = default;
    Transport(Transport&&) noexcept = default;
    Transport& operator=(Transport&&) noexcept = default;
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    /* Take ownership of a connected socket, optionally wrapping it in TLS.
     * Call tls_handshake() before I/O when tls != nullptr. */
    core::error_code attach(Socket&& sock, std::unique_ptr<TlsBackend> tls);

    /* Complete the TLS handshake on the attached socket (raw: no-op ok). */
    core::error_code tls_handshake(core::EventLoop& loop, uint64_t deadline_ms);

    core::error_code read(core::EventLoop& loop, uint8_t* buf, size_t n,
                          uint64_t deadline_ms, ssize_t& got);
    core::error_code write(core::EventLoop& loop, const uint8_t* data, size_t n,
                           uint64_t deadline_ms, ssize_t& sent);

    core::error_code shutdown_write();
    void close() noexcept;
    /* Release the socket fd without closing (nothing in Phase 4 takes it). */
    Socket detach_socket();

    bool connected() const noexcept { return sock_.connected(); }
    bool tls() const noexcept { return tls_ != nullptr; }
    int fd() const noexcept { return sock_.fd(); }
    std::string_view backend_name() const noexcept {
        return tls_ != nullptr ? tls_->name() : "raw";
    }

    /* Last-use timestamp for idle expiry (pool). */
    uint64_t last_use_ms() const noexcept { return last_use_ms_; }
    void touch() noexcept { last_use_ms_ = core::now_mono_ms(); }

    /* TLS session resumption hooks; raw transports report e_not_impl. */
    core::error_code capture_session(TlsSession& out);
    core::error_code replay_session(const TlsSession& in);

    TlsBackend* tls_backend() noexcept { return tls_.get(); }
    Socket& socket() noexcept { return sock_; }

private:
    Socket sock_;
    std::unique_ptr<TlsBackend> tls_;
    uint64_t last_use_ms_ = 0;
};

} /* namespace opencode::net */

#endif /* OPENCODE_NET_TRANSPORT_H */

/*
 * tls.h -- TLS pluggability interface (03_ARCHITECTURE.md Section 4).
 *
 * TlsBackend is implemented by optional backend TUs:
 *   - tls_mbedtls.cpp : mbedTLS 2.28/3.x, behind OPENCODE_USE_MBEDTLS.
 *   - tls_host.cpp    : zero-dep host-callback backend (ABI hook, Phase 12).
 * Default selection: mbedTLS when built, else host. The interface header never
 * mentions a concrete backend (04_DEPENDENCY_POLICY.md Section 3).
 *
 * Backend methods are blocking-over-loop: they wait on the caller's
 * EventLoop when the underlying transport would block, honoring the absolute
 * deadline_ms (0 = wait indefinitely). Never throw.
 */
#ifndef OPENCODE_NET_TLS_H
#define OPENCODE_NET_TLS_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "core/error.h"
#include "core/event_loop.h"

namespace opencode::net {

/* Opaque resumable-session blob. Storage owned by the backend that created it;
 * this object only carries the pointer + the release function (defined in
 * tls.cpp, always compiled, so the type is usable with any backend). */
class TlsSession {
public:
    using ReleaseFn = void (*)(void*) noexcept;

    TlsSession() noexcept = default;
    ~TlsSession();
    TlsSession(TlsSession&& o) noexcept;
    TlsSession& operator=(TlsSession&& o) noexcept;
    TlsSession(const TlsSession&) = delete;
    TlsSession& operator=(const TlsSession&) = delete;

    bool valid() const noexcept { return impl_ != nullptr; }

private:
    friend class TlsBackend;
    void* impl_ = nullptr;
    ReleaseFn release_ = nullptr;
};

struct TlsConfig {
    std::string sni;        /* server name for SNI + cert verification */
    std::string alpn;       /* e.g. "http/1.1"; empty = no ALPN */
    std::string ca_cert;    /* PEM trust anchor; empty = system (none for now) */
    std::string client_cert; /* PEM client cert; empty = none */
    std::string client_key;  /* PEM client key; empty = none */
};

class TlsBackend {
public:
    virtual ~TlsBackend() = default;

protected:
    /* Give a TlsSession ownership of a backend-allocated session blob. */
    static void adopt(TlsSession& s, void* impl, TlsSession::ReleaseFn rel) noexcept {
        s.impl_ = impl;
        s.release_ = rel;
    }
    /* Backend-side read access to a captured session blob. */
    static void* session_impl(const TlsSession& s) noexcept { return s.impl_; }

public:
    /* Perform the handshake on an already-connected, non-blocking `fd`.
     * Returns ok on success; e_net_tls on failure; e_net_timeout on deadline. */
    virtual core::error_code handshake(core::EventLoop& loop, int fd,
                                       uint64_t deadline_ms) = 0;

    /* Read/write plaintext through the TLS record layer. got==0/sent==0 with
     * ok() means EOF (peer closed). Never returns "would block": waits on the
     * loop internally. */
    virtual core::error_code read(core::EventLoop& loop, void* buf, size_t n,
                                  uint64_t deadline_ms, ssize_t& got) = 0;
    virtual core::error_code write(core::EventLoop& loop, const void* buf,
                                   size_t n, uint64_t deadline_ms,
                                   ssize_t& sent) = 0;

    /* Release backend resources. Idempotent. */
    virtual void close() noexcept = 0;

    virtual std::string_view name() const noexcept = 0;

    /* Session resumption (04/05: avoid a handshake per turn). The pool calls
     * capture_session() after a successful handshake and replays it on a fresh
     * connection before its handshake. Non-resumable backends report
     * e_not_impl (callers treat that as "no resumption"). */
    virtual core::error_code capture_session(TlsSession& out) {
        (void)out;
        return core::make_error_code(core::Err::e_not_impl);
    }
    virtual core::error_code replay_session(const TlsSession& in) {
        (void)in;
        return core::make_error_code(core::Err::e_not_impl);
    }
};

enum class TlsMode : uint8_t { kHost = 0, kMbedtls = 1 };

/* Preferred backend for this build: mbedTLS when OPENCODE_USE_MBEDTLS, else
 * the host-callback backend. */
TlsMode default_tls_mode() noexcept;

/* Create a backend. kMbedtls without OPENCODE_USE_MBEDTLS -> e_not_impl. */
core::error_code create_tls_backend(TlsMode mode, const TlsConfig& cfg,
                                    std::unique_ptr<TlsBackend>& out);

} /* namespace opencode::net */

#endif /* OPENCODE_NET_TLS_H */

#include "net/tls_mbedtls.h"

/*
 * tls_mbedtls.cpp -- mbedTLS TlsBackend (Phase 4; 05_NETWORK_RESILIENCE.md).
 *
 * Compiled into the core OBJECT library unconditionally; the mbedTLS include
 * surface and all backend code sit behind OPENCODE_USE_MBEDTLS, so this TU
 * adds no dependency when the option is OFF. Targets the mbedTLS 2.28 API
 * (what CI's libmbedtls-dev ships); 3.x keeps these calls (deprecated-only
 * warnings require MBEDTLS_DEPRECATED_WARNING, which we never define).
 *
 * Session resumption: the backend captures the mbedTLS session object (deep
 * copy) into an opaque TlsSession after a successful handshake; the pool keeps
 * one per endpoint and replays it on a fresh connection before its handshake
 * (mbedtls_ssl_set_session). Cuts a full handshake round trip per turn.
 */

#if OPENCODE_USE_MBEDTLS

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <unistd.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#if MBEDTLS_VERSION_NUMBER < 0x03000000
#include <mbedtls/net_sockets.h>
#endif

#include "core/error.h"
#include "net/socket.h"
#include "net/tls.h"

namespace opencode::net {

namespace {

constexpr const char* kPers = "opencodepp";

/* Low-level callback error codes changed between mbedTLS 2.x and 3.x (the net
 * module errors were dropped in 3.0). */
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
constexpr int kRecvFailed = MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
constexpr int kSendFailed = MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
#else
constexpr int kRecvFailed = MBEDTLS_ERR_NET_RECV_FAILED;
constexpr int kSendFailed = MBEDTLS_ERR_NET_SEND_FAILED;
#endif

core::error_code tls_err(int rc) {
    /* mbedTLS result codes are negative; detail carries -rc (positive). */
    return core::make_error_code(core::Err::e_net_tls,
                                 static_cast<uint32_t>(-rc));
}

class MbedtlsBackend final : public TlsBackend {
public:
    explicit MbedtlsBackend(const TlsConfig& cfg) : cfg_(cfg) {
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&drbg_);
        mbedtls_ssl_init(&ssl_);
        mbedtls_ssl_config_init(&conf_);
        mbedtls_x509_crt_init(&ca_);
        mbedtls_x509_crt_init(&clicert_);
        mbedtls_pk_init(&pkey_);
        mbedtls_ssl_session_init(&replay_);
    }

    ~MbedtlsBackend() override {
        close();
        mbedtls_ssl_session_free(&replay_);
        mbedtls_ssl_free(&ssl_);
        mbedtls_ssl_config_free(&conf_);
        mbedtls_x509_crt_free(&ca_);
        mbedtls_x509_crt_free(&clicert_);
        mbedtls_pk_free(&pkey_);
        mbedtls_ctr_drbg_free(&drbg_);
        mbedtls_entropy_free(&entropy_);
    }

    core::error_code handshake(core::EventLoop& loop, int fd,
                               uint64_t deadline_ms) override {
        fd_ = fd;
        core::error_code setup = configure();
        if (!setup.ok()) return setup;

        int rc = mbedtls_ssl_handshake(&ssl_);
        while (rc != 0) {
            if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
                const core::error_code ew = wait(loop, deadline_ms, true);
                if (!ew.ok()) return ew;
            } else if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                const core::error_code ew = wait(loop, deadline_ms, false);
                if (!ew.ok()) return ew;
            } else {
                return tls_err(rc);
            }
            rc = mbedtls_ssl_handshake(&ssl_);
        }
        handshaken_ = true;
        return core::ok();
    }

    core::error_code read(core::EventLoop& loop, void* buf, size_t n,
                          uint64_t deadline_ms, ssize_t& got) override {
        got = 0;
        for (;;) {
            const int rc = mbedtls_ssl_read(
                &ssl_, static_cast<unsigned char*>(buf), n);
            if (rc > 0) {
                got = rc;
                return core::ok();
            }
            if (rc == 0) {
                got = 0; /* TLS close_notify / EOF */
                return core::ok();
            }
            if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
                const core::error_code ew = wait(loop, deadline_ms, true);
                if (!ew.ok()) return ew;
                continue;
            }
            if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                const core::error_code ew = wait(loop, deadline_ms, false);
                if (!ew.ok()) return ew;
                continue;
            }
            return tls_err(rc);
        }
    }

    core::error_code write(core::EventLoop& loop, const void* buf, size_t n,
                           uint64_t deadline_ms, ssize_t& sent) override {
        sent = 0;
        for (;;) {
            const int rc = mbedtls_ssl_write(
                &ssl_, static_cast<const unsigned char*>(buf), n);
            if (rc > 0) {
                sent = rc;
                return core::ok();
            }
            if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
                const core::error_code ew = wait(loop, deadline_ms, true);
                if (!ew.ok()) return ew;
                continue;
            }
            if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                const core::error_code ew = wait(loop, deadline_ms, false);
                if (!ew.ok()) return ew;
                continue;
            }
            return tls_err(rc);
        }
    }

    void close() noexcept override {
        if (closed_) return;
        closed_ = true;
        if (handshaken_) {
            /* Best-effort close_notify; ignore the result. */
            mbedtls_ssl_close_notify(&ssl_);
        }
        fd_ = -1;
    }

    std::string_view name() const noexcept override { return "mbedtls"; }

    core::error_code capture_session(TlsSession& out) override {
        auto* copy = new mbedtls_ssl_session;
        mbedtls_ssl_session_init(copy);
        /* Same signature in mbedTLS 2.x and 3.x: deep-copies the resumption
         * session into `copy`. */
        const int rc = mbedtls_ssl_get_session(&ssl_, copy);
        if (rc != 0) {
            mbedtls_ssl_session_free(copy);
            delete copy;
            return tls_err(rc);
        }
        adopt(out, copy, [](void* p) noexcept {
            auto* ss = static_cast<mbedtls_ssl_session*>(p);
            mbedtls_ssl_session_free(ss);
            delete ss;
        });
        return core::ok();
    }

    core::error_code replay_session(const TlsSession& in) override {
        void* impl = TlsBackend::session_impl(in);
        if (impl == nullptr) return core::make_error_code(core::Err::e_net_tls, 1);
        const auto* s = static_cast<const mbedtls_ssl_session*>(impl);
        const int rc = mbedtls_ssl_set_session(&ssl_, s);
        if (rc != 0) return tls_err(rc);
        return core::ok();
    }

private:
    static int net_recv(void* ctx, unsigned char* buf, size_t n) {
        const int fd = static_cast<MbedtlsBackend*>(ctx)->fd_;
        if (fd < 0) return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
        const ssize_t r = ::read(fd, buf, n);
        if (r > 0) return static_cast<int>(r);
        if (r == 0) return MBEDTLS_ERR_SSL_CONN_EOF;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        }
        return kRecvFailed;
    }

    static int net_send(void* ctx, const unsigned char* buf, size_t n) {
        const int fd = static_cast<MbedtlsBackend*>(ctx)->fd_;
        if (fd < 0) return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
        const ssize_t w = ::write(fd, buf, n);
        if (w > 0) return static_cast<int>(w);
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        return kSendFailed;
    }

    core::error_code wait(core::EventLoop& loop, uint64_t deadline_ms,
                          bool for_read) {
        return for_read ? wait_fd_readable(loop, fd_, deadline_ms)
                        : wait_fd_writable(loop, fd_, deadline_ms);
    }

    core::error_code configure() {
        if (configured_) return core::ok();
        configured_ = true;

        int rc = mbedtls_ctr_drbg_seed(&drbg_, mbedtls_entropy_func, &entropy_,
                                       reinterpret_cast<const unsigned char*>(kPers),
                                       std::strlen(kPers));
        if (rc != 0) return tls_err(rc);

        rc = mbedtls_ssl_config_defaults(&conf_, MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
        if (rc != 0) return tls_err(rc);

        mbedtls_ssl_conf_rng(&conf_, mbedtls_ctr_drbg_random, &drbg_);
        mbedtls_ssl_conf_authmode(&conf_,
                                  cfg_.ca_cert.empty() ? MBEDTLS_SSL_VERIFY_NONE
                                                       : MBEDTLS_SSL_VERIFY_REQUIRED);

        if (!cfg_.ca_cert.empty()) {
            rc = mbedtls_x509_crt_parse(
                &ca_, reinterpret_cast<const unsigned char*>(cfg_.ca_cert.c_str()),
                cfg_.ca_cert.size() + 1);
            if (rc != 0) return tls_err(rc);
            mbedtls_ssl_conf_ca_chain(&conf_, &ca_, nullptr);
        }

        if (!cfg_.client_cert.empty() && !cfg_.client_key.empty()) {
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
            rc = mbedtls_pk_parse_key(
                &pkey_, reinterpret_cast<const unsigned char*>(cfg_.client_key.c_str()),
                cfg_.client_key.size() + 1, nullptr, 0, nullptr, nullptr);
#else
            rc = mbedtls_pk_parse_key(
                &pkey_, reinterpret_cast<const unsigned char*>(cfg_.client_key.c_str()),
                cfg_.client_key.size() + 1, nullptr, 0);
#endif
            if (rc != 0) return tls_err(rc);
            rc = mbedtls_x509_crt_parse(
                &clicert_,
                reinterpret_cast<const unsigned char*>(cfg_.client_cert.c_str()),
                cfg_.client_cert.size() + 1);
            if (rc != 0) return tls_err(rc);
            rc = mbedtls_ssl_conf_own_cert(&conf_, &clicert_, &pkey_);
            if (rc != 0) return tls_err(rc);
        }

        rc = mbedtls_ssl_setup(&ssl_, &conf_);
        if (rc != 0) return tls_err(rc);

        if (!cfg_.alpn.empty()) {
            char alpn[64];
            const size_t len =
                cfg_.alpn.size() < sizeof alpn - 1 ? cfg_.alpn.size() : sizeof alpn - 1;
            std::memcpy(alpn, cfg_.alpn.data(), len);
            alpn[len] = '\0';
            const char* protos[] = {alpn, nullptr};
            mbedtls_ssl_conf_alpn_protocols(&conf_, protos);
        }

        if (!cfg_.sni.empty()) {
            rc = mbedtls_ssl_set_hostname(&ssl_, cfg_.sni.c_str());
            if (rc != 0) return tls_err(rc);
        }

        if (replay_valid_) {
            rc = mbedtls_ssl_set_session(&ssl_, &replay_);
            if (rc != 0) return tls_err(rc);
            replay_valid_ = false;
        }

        mbedtls_ssl_set_bio(&ssl_, this, net_send, net_recv, nullptr);
        return core::ok();
    }

    TlsConfig cfg_;
    mbedtls_entropy_context entropy_;
    mbedtls_ctr_drbg_context drbg_;
    mbedtls_ssl_context ssl_;
    mbedtls_ssl_config conf_;
    mbedtls_x509_crt ca_;
    mbedtls_x509_crt clicert_;
    mbedtls_pk_context pkey_;
    mbedtls_ssl_session replay_;
    int fd_ = -1;
    bool configured_ = false;
    bool closed_ = false;
    bool handshaken_ = false;
    bool replay_valid_ = false;
};

} /* namespace */

core::error_code create_mbedtls_backend(const TlsConfig& cfg,
                                        std::unique_ptr<TlsBackend>& out) {
    out = std::unique_ptr<TlsBackend>(new MbedtlsBackend(cfg));
    return core::ok();
}

} /* namespace opencode::net */

#else /* OPENCODE_USE_MBEDTLS */

namespace opencode::net {

core::error_code create_mbedtls_backend(const TlsConfig& cfg,
                                        std::unique_ptr<TlsBackend>& out) {
    (void)cfg;
    (void)out;
    return core::make_error_code(core::Err::e_not_impl);
}

} /* namespace opencode::net */

#endif /* OPENCODE_USE_MBEDTLS */

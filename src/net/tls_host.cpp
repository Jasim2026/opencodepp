#include "net/tls_mbedtls.h"

/*
 * tls_host.cpp -- zero-dependency TLS backend for constrained/Android hosts.
 *
 * The transport is provided by the host through a callback registered at
 * engine assembly time (the ABI hook opencode_tls_fn lands in Phase 12, along
 * with the engine assembly in src/abi/). Until that callback exists there is
 * nothing to drive, so handshake/receive/send report e_not_impl: a build
 * without mbedTLS simply has no TLS path and every request must run over
 * plaintext (the dev/mock path logs a loud warning; 05_NETWORK_RESILIENCE.md
 * Section 2 allows plaintext in dev only).
 */

#include <string_view>

#include "core/error.h"
#include "net/tls.h"

namespace opencode::net {

namespace {

class HostTlsBackend final : public TlsBackend {
public:
    explicit HostTlsBackend(const TlsConfig& cfg) : cfg_(cfg) {}

    core::error_code handshake(core::EventLoop&, int, uint64_t) override {
        return core::make_error_code(core::Err::e_not_impl);
    }
    core::error_code read(core::EventLoop&, void*, size_t, uint64_t,
                          ssize_t&) override {
        return core::make_error_code(core::Err::e_not_impl);
    }
    core::error_code write(core::EventLoop&, const void*, size_t, uint64_t,
                           ssize_t&) override {
        return core::make_error_code(core::Err::e_not_impl);
    }
    void close() noexcept override {}
    std::string_view name() const noexcept override { return "host"; }

private:
    TlsConfig cfg_; /* held for Phase 12 wiring */
};

} /* namespace */

core::error_code create_host_tls_backend(const TlsConfig& cfg,
                                         std::unique_ptr<TlsBackend>& out) {
    out = std::unique_ptr<TlsBackend>(new HostTlsBackend(cfg));
    return core::ok();
}

} /* namespace opencode::net */

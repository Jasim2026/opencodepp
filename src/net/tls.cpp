#include "net/tls.h"

#include "net/tls_mbedtls.h"

namespace opencode::net {

TlsSession::~TlsSession() {
    if (release_ != nullptr) release_(impl_);
}

TlsSession::TlsSession(TlsSession&& o) noexcept
    : impl_(o.impl_), release_(o.release_) {
    o.impl_ = nullptr;
    o.release_ = nullptr;
}

TlsSession& TlsSession::operator=(TlsSession&& o) noexcept {
    if (this != &o) {
        if (release_ != nullptr) release_(impl_);
        impl_ = o.impl_;
        release_ = o.release_;
        o.impl_ = nullptr;
        o.release_ = nullptr;
    }
    return *this;
}

TlsMode default_tls_mode() noexcept {
#if OPENCODE_USE_MBEDTLS
    return TlsMode::kMbedtls;
#else
    return TlsMode::kHost;
#endif
}

core::error_code create_tls_backend(TlsMode mode, const TlsConfig& cfg,
                                    std::unique_ptr<TlsBackend>& out) {
    switch (mode) {
#if OPENCODE_USE_MBEDTLS
        case TlsMode::kMbedtls:
            return create_mbedtls_backend(cfg, out);
#endif
        case TlsMode::kHost:
            return create_host_tls_backend(cfg, out);
        default:
            break;
    }
    return core::make_error_code(core::Err::e_not_impl);
}

} /* namespace opencode::net */

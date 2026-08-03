/*
 * tls_mbedtls.h -- private: mbedTLS TlsBackend factory (backend TU contract).
 * Only included by tls.cpp (selection) and the mbedTLS TU itself.
 */
#ifndef OPENCODE_NET_TLS_MBEDTLS_H
#define OPENCODE_NET_TLS_MBEDTLS_H

#include <memory>

#include "core/error.h"
#include "net/tls.h"

namespace opencode::net {

/* Create the mbedTLS backend. Requires OPENCODE_USE_MBEDTLS; otherwise this
 * TU is compiled with the backend #if'd out and returns e_not_impl. */
core::error_code create_mbedtls_backend(const TlsConfig& cfg,
                                        std::unique_ptr<TlsBackend>& out);

/* Create the zero-dependency host-callback backend. */
core::error_code create_host_tls_backend(const TlsConfig& cfg,
                                         std::unique_ptr<TlsBackend>& out);

} /* namespace opencode::net */

#endif /* OPENCODE_NET_TLS_MBEDTLS_H */

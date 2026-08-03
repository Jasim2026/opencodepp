#include "net/transport.h"

namespace opencode::net {

core::error_code Transport::attach(Socket&& sock,
                                   std::unique_ptr<TlsBackend> tls) {
    sock_ = std::move(sock);
    tls_ = std::move(tls);
    touch();
    return core::ok();
}

core::error_code Transport::tls_handshake(core::EventLoop& loop,
                                          uint64_t deadline_ms) {
    if (tls_ == nullptr) return core::ok();
    if (sock_.fd() < 0) return core::make_error_code(core::Err::e_net_connect);
    return tls_->handshake(loop, sock_.fd(), deadline_ms);
}

core::error_code Transport::read(core::EventLoop& loop, uint8_t* buf, size_t n,
                                 uint64_t deadline_ms, ssize_t& got) {
    touch();
    if (tls_ != nullptr) return tls_->read(loop, buf, n, deadline_ms, got);
    return sock_.recv(loop, buf, n, deadline_ms, got);
}

core::error_code Transport::write(core::EventLoop& loop, const uint8_t* data,
                                  size_t n, uint64_t deadline_ms,
                                  ssize_t& sent) {
    touch();
    if (tls_ != nullptr) return tls_->write(loop, data, n, deadline_ms, sent);
    return sock_.send(loop, data, n, deadline_ms, sent);
}

core::error_code Transport::shutdown_write() {
    if (tls_ != nullptr) {
        /* close_notify; best-effort. */
        tls_->close();
    }
    return sock_.shutdown_write();
}

void Transport::close() noexcept {
    if (tls_ != nullptr) tls_->close();
    sock_.close();
}

Socket Transport::detach_socket() {
    Socket out = std::move(sock_);
    tls_.reset();
    return out;
}

core::error_code Transport::capture_session(TlsSession& out) {
    if (tls_ == nullptr) return core::make_error_code(core::Err::e_not_impl);
    return tls_->capture_session(out);
}

core::error_code Transport::replay_session(const TlsSession& in) {
    if (tls_ == nullptr) return core::make_error_code(core::Err::e_not_impl);
    return tls_->replay_session(in);
}

} /* namespace opencode::net */

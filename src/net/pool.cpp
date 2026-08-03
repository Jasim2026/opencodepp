#include "net/pool.h"

#include <algorithm>

namespace opencode::net {

Pool::~Pool() { shutdown(); }

core::error_code Pool::open_transport(core::EventLoop& loop, const Key& key,
                                      const TlsConfig* tls,
                                      uint64_t deadline_ms, Transport& out) {
    Socket s;
    uint32_t timeout_ms = 0;
    if (deadline_ms != 0) {
        const uint64_t now = core::now_mono_ms();
        timeout_ms = deadline_ms > now
                         ? static_cast<uint32_t>(deadline_ms - now)
                         : 1;
    }
    core::error_code ec =
        s.connect(loop, Addr{key.host, key.port}, timeout_ms);
    if (!ec.ok()) return ec;

    std::unique_ptr<TlsBackend> backend;
    if (key.tls) {
        ec = create_tls_backend(default_tls_mode(), tls != nullptr ? *tls : TlsConfig{},
                                backend);
        if (!ec.ok()) {
            s.close();
            return ec;
        }
    }
    ec = out.attach(std::move(s), std::move(backend));
    if (!ec.ok()) return ec;
    if (key.tls) {
        ec = out.tls_handshake(loop, deadline_ms);
        if (!ec.ok()) {
            out.close();
            return ec;
        }
    }
    out.touch();
    return core::ok();
}

int Pool::inflight_for(const Key& key) const noexcept {
    for (const auto& kv : inflight_) {
        if (kv.first == key) return kv.second;
    }
    return 0;
}

void Pool::add_inflight(const Key& key, int delta) noexcept {
    for (auto& kv : inflight_) {
        if (kv.first == key) {
            kv.second += delta;
            return;
        }
    }
    if (delta > 0) inflight_.emplace_back(key, delta);
}

void Pool::reap_idle() noexcept {
    if (idle_.empty()) return;
    const uint64_t now = core::now_mono_ms();
    std::deque<Entry> kept;
    for (auto& e : idle_) {
        if (now - e.parked_ms > cfg_.idle_timeout_ms) {
            e.t.close();
        } else {
            kept.push_back(std::move(e));
        }
    }
    idle_ = std::move(kept);
}

core::error_code Pool::acquire(core::EventLoop& loop, const Key& key,
                               const TlsConfig* tls, uint64_t deadline_ms,
                               Transport& out, const Opener& opener) {
    reap_idle();

    /* Reuse the freshest idle transport for this host. */
    for (auto it = idle_.rbegin(); it != idle_.rend(); ++it) {
        if (it->key == key) {
            out = std::move(it->t);
            const size_t idx =
                static_cast<size_t>(std::distance(idle_.rbegin(), it));
            idle_.erase(idle_.begin() + (idle_.size() - 1 - idx));
            add_inflight(key, +1);
            out.touch();
            return core::ok();
        }
    }

    /* In-flight capacity wait (blocking-over-loop). */
    const size_t cap = cfg_.max_in_flight_per_host;
    const uint64_t wait_until =
        cfg_.acquire_wait_ms != 0 ? core::now_mono_ms() + cfg_.acquire_wait_ms : 0;
    while (inflight_for(key) >= static_cast<int>(cap)) {
        if (wait_until != 0 && core::now_mono_ms() >= wait_until)
            return core::make_error_code(core::Err::e_net_timeout, 1);
        pump_until(loop, wait_until,
                   [&] { return inflight_for(key) < static_cast<int>(cap); });
    }

    const Opener& use = opener ? opener : open_transport;
    core::error_code ec = use(loop, key, tls, deadline_ms, out);
    if (ec.ok()) add_inflight(key, +1);
    return ec;
}

void Pool::push_idle(Transport&& t, const Key& key) {
    t.touch();
    idle_.push_back({std::move(t), key, core::now_mono_ms()});
    size_t cnt = 0;
    for (const auto& e : idle_) {
        if (e.key == key) ++cnt;
    }
    while (cnt > cfg_.max_idle_per_host) {
        for (size_t i = 0; i < idle_.size(); ++i) {
            if (idle_[i].key == key) {
                idle_[i].t.close();
                idle_.erase(idle_.begin() + static_cast<std::ptrdiff_t>(i));
                --cnt;
                break;
            }
        }
    }
}

void Pool::release(Transport&& t, const Key& key, bool healthy) {
    add_inflight(key, -1);
    if (!healthy || !t.connected()) {
        t.close();
        return;
    }
    push_idle(std::move(t), key);
}

void Pool::drop_idle() noexcept {
    for (auto& e : idle_) e.t.close();
    idle_.clear();
}

void Pool::shutdown() noexcept {
    drop_idle();
    inflight_.clear();
}

size_t Pool::in_flight(const Key& key) const noexcept {
    const int n = inflight_for(key);
    return n > 0 ? static_cast<size_t>(n) : 0;
}

size_t Pool::in_flight_total() const noexcept {
    size_t n = 0;
    for (const auto& kv : inflight_) n += static_cast<size_t>(kv.second);
    return n;
}

} /* namespace opencode::net */

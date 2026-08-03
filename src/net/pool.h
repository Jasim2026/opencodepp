/*
 * pool.h -- per-host keep-alive connection pool.
 *
 * Idle Transports are parked per (host, port, tls) and reaped by age;
 * in-flight capacity per host is bounded (doctrine: max 4 in-flight). A
 * corrupt stream is never reused: callers release with healthy=false and the
 * pool closes it. The connection opener is injectable so tests can hand the
 * pool pre-made transports. Never throws.
 */
#ifndef OPENCODE_NET_POOL_H
#define OPENCODE_NET_POOL_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/clock.h"
#include "core/error.h"
#include "core/event_loop.h"
#include "net/socket.h"
#include "net/tls.h"
#include "net/transport.h"

namespace opencode::net {

class Pool {
public:
    struct Config {
        size_t max_idle_per_host = 2;
        size_t max_in_flight_per_host = 4;
        uint64_t idle_timeout_ms = 60'000;
        uint32_t acquire_wait_ms = 5'000; /* wait for an in-flight slot */
    };
    Pool() noexcept : cfg_(Config()) {}
    explicit Pool(Config cfg) noexcept : cfg_(cfg) {}
    ~Pool();

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    struct Key {
        std::string host;
        uint16_t port = 0;
        bool tls = false;

        bool operator==(const Key& o) const noexcept {
            return host == o.host && port == o.port && tls == o.tls;
        }
    };

    /* Real opener: TCP connect, attach, optional TLS handshake (uses
     * default_tls_mode()). Also the injectable hook for tests. */
    using Opener = std::function<core::error_code(
        core::EventLoop&, const Key&, const TlsConfig*, uint64_t, Transport&)>;
    static core::error_code open_transport(core::EventLoop& loop,
                                           const Key& key, const TlsConfig* tls,
                                           uint64_t deadline_ms, Transport& out);

    /* Acquire: reuse an idle transport, else open one via `opener` (defaults
     * to open_transport). Blocks-on-loop up to cfg_.acquire_wait_ms when the
     * host is at in-flight capacity; then e_net_timeout. The returned
     * transport belongs to the caller until release(). */
    core::error_code acquire(core::EventLoop& loop, const Key& key,
                             const TlsConfig* tls, uint64_t deadline_ms,
                             Transport& out, const Opener& opener = {});

    /* Return a transport. healthy=false closes it (corrupt stream). Healthy
     * transports are parked idle, bounded by max_idle_per_host. */
    void release(Transport&& t, const Key& key, bool healthy);

    void drop_idle() noexcept;
    void shutdown() noexcept;

    size_t idle_count() const noexcept { return idle_.size(); }
    size_t in_flight(const Key& key) const noexcept;
    size_t in_flight_total() const noexcept;

private:
    void reap_idle() noexcept;
    void push_idle(Transport&& t, const Key& key);
    int inflight_for(const Key& key) const noexcept;
    void add_inflight(const Key& key, int delta) noexcept;

    struct Entry {
        Transport t;
        Key key;
        uint64_t parked_ms = 0;
    };
    std::deque<Entry> idle_;
    std::vector<std::pair<Key, int>> inflight_;

    Config cfg_;
};

} /* namespace opencode::net */

#endif /* OPENCODE_NET_POOL_H */

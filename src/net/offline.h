/*
 * offline.h -- connectivity probe + bounded FIFO queue with auto-drain.
 *
 * State machine (05_NETWORK_RESILIENCE.md Section 5): ONLINE -> OFFLINE when a
 * probe fails; RECOVERING -> ONLINE when a probe succeeds and the queue drains
 * in order. Requests are never dropped: when the engine can't reach the API it
 * enqueues (bounded by capacity), and drain() replays them FIFO on recovery.
 * State transitions are reported through a callback. Never throws.
 */
#ifndef OPENCODE_NET_OFFLINE_H
#define OPENCODE_NET_OFFLINE_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>

#include "core/clock.h"
#include "core/error.h"
#include "core/event_loop.h"
#include "net/http1.h"

namespace opencode::net {

enum class Connectivity : uint8_t { online = 0, offline = 1, recovering = 2 };

/* A queued request the engine wants to send (replayed verbatim on drain). */
struct QueueEntry {
    std::string request_id;
    std::string method = "POST";
    std::string path = "/v1/chat";
    HttpHeaders headers;
    std::string body;
};

class OfflineQueue {
public:
    struct Config {
        std::string probe_host = "127.0.0.1";
        uint16_t probe_port = 80;
        uint32_t probe_timeout_ms = 1000;
        size_t max_queue = 100;
    };
    OfflineQueue() noexcept : cfg_(Config{}) {}
    explicit OfflineQueue(Config cfg) noexcept : cfg_(cfg) {}

    using StateCb = std::function<void(Connectivity)>;
    /* Returns ok once `entry` was fully processed; a retryable error stops the
     * drain (the entry is requeued at the head). */
    using DrainCb = std::function<core::error_code(
        core::EventLoop&, const QueueEntry&, uint64_t deadline_ms)>;

    void set_state_cb(StateCb cb) noexcept { state_cb_ = std::move(cb); }

    Connectivity state() const noexcept { return state_; }

    /* Cheap TCP probe against cfg_.probe_host:port. */
    core::error_code probe(core::EventLoop& loop) const;

    /* Check connectivity; on transitions notify state_cb_. Returns the probe
     * outcome (ok = reachable). */
    core::error_code check(core::EventLoop& loop);

    /* Queue a request. Returns e_net_overflow when at capacity. */
    core::error_code enqueue(QueueEntry e);

    /* Send `e` now when online, else queue it. Requests are never dropped. */
    core::error_code submit(core::EventLoop& loop, QueueEntry e,
                            const DrainCb& drain);

    /* If recovering/online after a probe success, drain pending requests in
     * FIFO order through `drain`. A retryable failure requeues at the head and
     * returns to OFFLINE. Returns the failing error, or ok. */
    core::error_code drain(core::EventLoop& loop, uint64_t deadline_ms,
                           const DrainCb& drain);

    size_t pending() const noexcept { return queue_.size(); }
    size_t capacity() const noexcept { return cfg_.max_queue; }
    void clear() noexcept { queue_.clear(); }

private:
    Config cfg_;
    Connectivity state_ = Connectivity::online;
    StateCb state_cb_;
    std::deque<QueueEntry> queue_;
};

} /* namespace opencode::net */

#endif /* OPENCODE_NET_OFFLINE_H */

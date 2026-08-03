#include "net/offline.h"

#include "net/socket.h"

namespace opencode::net {

core::error_code OfflineQueue::probe(core::EventLoop& loop) const {
    Socket s;
    core::error_code ec =
        s.connect(loop, Addr{cfg_.probe_host, cfg_.probe_port},
                  cfg_.probe_timeout_ms);
    s.close();
    return ec;
}

core::error_code OfflineQueue::check(core::EventLoop& loop) {
    const core::error_code ec = probe(loop);
    if (ec.ok()) {
        if (state_ == Connectivity::offline) {
            state_ = Connectivity::recovering;
            if (state_cb_) state_cb_(state_);
        } else if (state_ == Connectivity::recovering && queue_.empty()) {
            state_ = Connectivity::online;
            if (state_cb_) state_cb_(state_);
        } else if (state_ == Connectivity::recovering) {
            /* still draining */
        }
        return core::ok();
    }
    if (state_ != Connectivity::offline) {
        state_ = Connectivity::offline;
        if (state_cb_) state_cb_(state_);
    }
    return ec;
}

core::error_code OfflineQueue::enqueue(QueueEntry e) {
    if (queue_.size() >= cfg_.max_queue)
        return core::make_error_code(core::Err::e_net_overflow);
    queue_.push_back(std::move(e));
    return core::ok();
}

core::error_code OfflineQueue::submit(core::EventLoop& loop, QueueEntry e,
                                      const DrainCb& drain) {
    if (state_ == Connectivity::online) {
        const uint64_t dl = core::now_mono_ms() + 60'000;
        return drain(loop, e, dl);
    }
    return enqueue(std::move(e));
}

core::error_code OfflineQueue::drain(core::EventLoop& loop,
                                     uint64_t deadline_ms,
                                     const DrainCb& drain) {
    while (!queue_.empty()) {
        QueueEntry e = std::move(queue_.front());
        queue_.pop_front();
        const core::error_code ec = drain(loop, e, deadline_ms);
        if (!ec.ok()) {
            /* Never lose the request: requeue at the head, drop back offline. */
            queue_.push_front(std::move(e));
            if (state_ != Connectivity::offline) {
                state_ = Connectivity::offline;
                if (state_cb_) state_cb_(state_);
            }
            return ec;
        }
    }
    if (state_ != Connectivity::online) {
        state_ = Connectivity::online;
        if (state_cb_) state_cb_(state_);
    }
    return core::ok();
}

} /* namespace opencode::net */

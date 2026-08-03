/*
 * channel.h — bounded multi-producer/single-consumer message queue.
 *
 * Fixed-capacity ring; every op is allocation-free after construction. Used for
 * event fan-out and cross-thread handoff (host / worker → loop thread).
 *
 * Semantics:
 *   - producers (any thread) call try_push(); a successful push fires the
 *     registered wakeup hook (normally EventLoop::wakeup) so a blocked loop
 *     re-runs and drains.
 *   - the single consumer (loop thread) calls try_pop().
 *   - close(): producer-side "no more pushes"; consumers keep draining until
 *     empty, then try_pop returns kClosed. Consumer-side close() drops the
 *     queue and rejects further pushes.
 *   - try_push never blocks: returns false when full or closed (backpressure).
 */
#ifndef OPENCODE_CORE_CHANNEL_H
#define OPENCODE_CORE_CHANNEL_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace opencode::core {

class Channel {
public:
    using WakeFn = void (*)(void* ctx);

    enum PopResult : uint8_t { kEmpty = 0, kOk = 1, kClosed = 2 };

    struct Message {
        uint32_t tag = 0;
        uint32_t len = 0;
        const void* data = nullptr; /* borrowed; lifetime owned by producer */
    };

    explicit Channel(size_t capacity = 64) noexcept;
    ~Channel();
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    /* Producer. False when full or closed. Fires wakeup on success. */
    bool try_push(uint32_t tag, const void* data, uint32_t len);
    bool try_push(const Message& m) {
        return try_push(m.tag, m.data, m.len);
    }

    /* Consumer. Sets out on kOk. */
    PopResult try_pop(Message& out);

    size_t size() const noexcept;
    size_t capacity() const noexcept { return cap_; }
    bool empty() const noexcept { return count_ == 0; }
    bool full() const noexcept { return count_ == cap_; }

    void set_wakeup(WakeFn fn, void* ctx) noexcept {
        std::lock_guard<std::mutex> l(m_);
        wake_ = fn;
        wake_ctx_ = ctx;
    }

    /* Producer close: no more pushes; consumers drain until empty, then
     * try_pop returns kClosed. */
    void close();
    /* Consumer close: drop the queue and reject further pushes. */
    void drop();
    bool closed() const noexcept { return closed_; }

private:
    size_t cap_;
    std::vector<Message> ring_;
    mutable std::mutex m_;
    size_t head_ = 0; /* next read  (consumer) */
    size_t tail_ = 0; /* next write (producer) */
    size_t count_ = 0;
    bool closed_ = false;
    WakeFn wake_ = nullptr;
    void* wake_ctx_ = nullptr;
};

} /* namespace opencode::core */

#endif /* OPENCODE_CORE_CHANNEL_H */

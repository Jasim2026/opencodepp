/*
 * event_loop.h — single-threaded I/O event loop.
 *
 * Abstraction over epoll (Linux) with a poll() fallback. Owns: an fd registry
 * (read/write/error interest per fd), a timer heap, a cross-thread wakeup pipe.
 * The host pumps the loop from its own thread (opencode_engine_drive); the
 * engine never spawns threads by default.
 *
 * Thread-safety: all methods except wakeup() are single-threaded (loop thread).
 * wakeup() is safe from any thread.
 */
#ifndef OPENCODE_CORE_EVENT_LOOP_H
#define OPENCODE_CORE_EVENT_LOOP_H

#include <cstdint>
#include <vector>

namespace opencode::core {

class EventLoop {
public:
    enum Event : uint32_t {
        kRead = 1u << 0,
        kWrite = 1u << 1,
        kErr = 1u << 2,
        kHup = 1u << 3,
    };

    using Handler = void (*)(void* userdata, uint32_t events);
    using TimerHandler = void (*)(void* userdata, uint64_t timer_id);

    EventLoop();
    ~EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    /* Register/modify/remove fd interest. Returns false on failure. */
    bool add(int fd, uint32_t interest, Handler h, void* userdata);
    bool mod(int fd, uint32_t interest);
    bool del(int fd);
    bool has_fd(int fd) const;

    /* One-shot or repeating timer. Returns timer id (0 = failure). */
    uint64_t add_timer(uint64_t delay_ms, TimerHandler h, void* userdata,
                       bool oneshot = true);
    bool cancel_timer(uint64_t timer_id);

    /* Run the loop for up to timeout_ms (-1 = forever). Returns events handled. */
    int run_once(int timeout_ms);

    /* Wake a blocked run_once() from any thread. */
    bool wakeup() noexcept;

    uint64_t now_ms() const noexcept;

    static constexpr int kMaxEvents = 1024;

private:
    struct FdSlot {
        int fd = -1;
        uint32_t interest = 0;
        Handler h = nullptr;
        void* u = nullptr;
    };
    struct Timer {
        uint64_t id = 0;
        uint64_t deadline_ms = 0;
        uint64_t period_ms = 0;
        TimerHandler h = nullptr;
        void* u = nullptr;
        bool oneshot = true;
        bool cancelled = false;
    };

    struct Impl;
    Impl* impl_;

    void drain_wakeup();
    size_t fire_due_timers();
};

} /* namespace opencode::core */

#endif /* OPENCODE_CORE_EVENT_LOOP_H */

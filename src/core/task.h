/*
 * task.h — stackless coroutines over the event loop (the async abstraction the
 * agent loop drives). C++20 coroutines; no exceptions, no RTTI.
 *
 *   co_task tick(TaskScheduler&) {
 *       for (;;) {
 *           co_await io::await_timer(1000);
 *           co_await io::await_readable(fd);
 *       }
 *   }
 *
 * All tasks run on the loop thread: awaiters register callbacks with the loop
 * and resume synchronously from it, so there is no data race between resume
 * paths. A task is eagerly started by spawn() and resumes from where it left
 * off when its awaited event fires. cancel() destroys a suspended task (its
 * awaiters unwind: fd registration removed, timer cancelled).
 */
#ifndef OPENCODE_CORE_TASK_H
#define OPENCODE_CORE_TASK_H

#include <coroutine>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>

#include "core/event_loop.h"

namespace opencode::core {

class TaskScheduler;

/* Return type of a task coroutine. Owns its frame; the scheduler takes over
 * via release(). */
class co_task {
public:
    struct promise_type {
        TaskScheduler* sched = nullptr;
        uint64_t id = 0;

        co_task get_return_object() noexcept {
            return co_task{std::coroutine_handle<promise_type>::
                               from_promise(*this)};
        }
        /* Tasks start suspended; the scheduler's spawn() is what starts them
         * (so the scheduler context and g_loop are bound before the body
         * runs). Resuming a completed frame is UB, so never run eagerly. */
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        [[noreturn]] void unhandled_exception() noexcept { std::abort(); }
    };

    explicit co_task(std::coroutine_handle<promise_type> h) noexcept : h_(h) {}
    co_task(co_task&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    co_task& operator=(co_task&& o) noexcept {
        if (this != &o) {
            if (h_) h_.destroy();
            h_ = o.h_;
            o.h_ = nullptr;
        }
        return *this;
    }
    co_task(const co_task&) = delete;
    co_task& operator=(const co_task&) = delete;
    ~co_task() {
        if (h_) h_.destroy();
    }

    std::coroutine_handle<promise_type> release() noexcept {
        std::coroutine_handle<promise_type> h = h_;
        h_ = nullptr;
        return h;
    }

private:
    std::coroutine_handle<promise_type> h_;
};

/* Owns the task registry and drives resumption. One per event loop. */
class TaskScheduler {
public:
    explicit TaskScheduler(EventLoop& loop) noexcept : loop_(loop) {}
    ~TaskScheduler();

    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;

    /* Start a task eagerly on the loop thread. Returns its id (0 = none). */
    uint64_t spawn(co_task&& task);
    /* Destroy a task (safe only outside an in-progress resume of it). */
    bool cancel(uint64_t id);
    /* Resume all tasks woken by the last loop pass. */
    void run_ready();

    EventLoop& loop() const noexcept { return loop_; }
    size_t pending() const noexcept { return reg_.size(); }

    /* Internal: resume a handle, destroying its frame on completion. Used by
     * the event-loop callbacks (through the promise's sched pointer). */
    void resume_one(std::coroutine_handle<co_task::promise_type> h) noexcept;

private:
    friend struct co_task::promise_type;
    EventLoop& loop_;
    std::unordered_map<uint64_t, std::coroutine_handle<co_task::promise_type>> reg_;
    std::vector<std::coroutine_handle<co_task::promise_type>> ready_;
    uint64_t next_id_ = 1;

    void on_complete(uint64_t id) noexcept { reg_.erase(id); }
};

namespace io {

/* Current loop for the executing task (set by TaskScheduler, thread-local). */
EventLoop* current_loop() noexcept;

/* One-shot fd wait. await_resume() == the raw event flags (EventLoop::Event). */
class await_readable {
public:
    explicit await_readable(int fd,
                            uint32_t interest = EventLoop::kRead) noexcept
        : fd_(fd), interest_(interest) {}

    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<co_task::promise_type> h) noexcept;
    uint32_t await_resume() const noexcept { return events_; }
    ~await_readable() {
        /* cancellation unwinds: drop the fd registration if still live */
        if (loop_ != nullptr && !cleaned_) loop_->del(fd_);
    }

private:
    friend class TaskScheduler;
    int fd_;
    uint32_t interest_;
    EventLoop* loop_ = nullptr;
    std::coroutine_handle<co_task::promise_type> h_;
    uint32_t events_ = 0;
    bool cleaned_ = false;

    static void on_event(void* userdata, uint32_t events) noexcept;
};

/* One-shot timer wait (ms). await_resume() == true if it actually fired. */
class await_timer {
public:
    explicit await_timer(uint64_t ms) noexcept : ms_(ms) {}

    bool await_ready() const noexcept { return ms_ == 0; }
    bool await_suspend(std::coroutine_handle<co_task::promise_type> h) noexcept;
    bool await_resume() const noexcept { return ok_ && !resolved_immediately_; }
    ~await_timer() {
        /* cancellation unwinds: cancel the timer if still pending */
        if (loop_ != nullptr && !cleaned_ && timer_id_ != 0)
            loop_->cancel_timer(timer_id_);
    }

private:
    friend class TaskScheduler;
    uint64_t ms_;
    EventLoop* loop_ = nullptr;
    std::coroutine_handle<co_task::promise_type> h_;
    uint64_t timer_id_ = 0;
    bool ok_ = false;
    bool resolved_immediately_ = false;
    bool cleaned_ = false;

    static void on_timer(void* userdata, uint64_t timer_id) noexcept;
};

} /* namespace io */

} /* namespace opencode::core */

#endif /* OPENCODE_CORE_TASK_H */

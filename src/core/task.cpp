#include "core/task.h"

#include <utility>

namespace opencode::core {
namespace io {

namespace {
thread_local EventLoop* g_loop = nullptr;
}

EventLoop* current_loop() noexcept { return g_loop; }

/* RAII: bind g_loop for the duration of a resume/suspend cycle. */
class LoopScope {
public:
    explicit LoopScope(EventLoop* loop) noexcept : prev_(g_loop) {
        g_loop = loop;
    }
    ~LoopScope() { g_loop = prev_; }
    LoopScope(const LoopScope&) = delete;
    LoopScope& operator=(const LoopScope&) = delete;

private:
    EventLoop* prev_;
};

bool await_readable::await_suspend(
    std::coroutine_handle<co_task::promise_type> h) noexcept {
    h_ = h;
    loop_ = g_loop;
    if (loop_ == nullptr) return false; /* not running under a scheduler */
    return loop_->add(fd_, interest_, &await_readable::on_event, this);
}

void await_readable::on_event(void* userdata, uint32_t events) noexcept {
    auto* a = static_cast<await_readable*>(userdata);
    a->events_ = events;
    a->cleaned_ = true;
    a->loop_->del(a->fd_);
    a->loop_ = nullptr;
    std::coroutine_handle<co_task::promise_type> h = a->h_;
    a->h_ = nullptr;
    co_task::promise_type& p = h.promise();
    p.sched->resume_one(h);
}

bool await_timer::await_suspend(
    std::coroutine_handle<co_task::promise_type> h) noexcept {
    h_ = h;
    loop_ = g_loop;
    if (loop_ == nullptr) {
        resolved_immediately_ = true;
        return false;
    }
    timer_id_ = loop_->add_timer(ms_, &await_timer::on_timer, this, true);
    ok_ = timer_id_ != 0;
    return ok_; /* false → resume immediately (timer add failed) */
}

void await_timer::on_timer(void* userdata, uint64_t /*timer_id*/) noexcept {
    auto* a = static_cast<await_timer*>(userdata);
    a->cleaned_ = true;
    a->loop_ = nullptr;
    std::coroutine_handle<co_task::promise_type> h = a->h_;
    a->h_ = nullptr;
    co_task::promise_type& p = h.promise();
    p.sched->resume_one(h);
}

} /* namespace io */

TaskScheduler::~TaskScheduler() {
    for (auto& [id, h] : reg_) {
        (void)id;
        h.destroy();
    }
}

uint64_t TaskScheduler::spawn(co_task&& task) {
    std::coroutine_handle<co_task::promise_type> h = task.release();
    if (!h) return 0;
    auto& p = h.promise();
    p.sched = this;
    p.id = next_id_++;
    reg_.emplace(p.id, h);
    const uint64_t id = p.id;
    resume_one(h);
    return id;
}

bool TaskScheduler::cancel(uint64_t id) {
    auto it = reg_.find(id);
    if (it == reg_.end()) return false;
    std::coroutine_handle<co_task::promise_type> h = it->second;
    reg_.erase(it);
    h.destroy(); /* unwinds awaiters: fd removed / timer cancelled */
    return true;
}

void TaskScheduler::run_ready() {
    std::vector<std::coroutine_handle<co_task::promise_type>> v;
    v.swap(ready_);
    for (auto h : v) resume_one(h);
}

void TaskScheduler::resume_one(
    std::coroutine_handle<co_task::promise_type> h) noexcept {
    /* Every resume (spawn, run_ready, fd/timer callbacks) runs the coroutine
     * with the loop bound so its next co_await can register with it. */
    io::LoopScope scope(&loop_);
    h.resume();
    if (!h.done()) return;
    /* Completed: hand the frame back and destroy it. */
    co_task::promise_type& p = h.promise();
    if (p.sched != nullptr && p.id != 0) {
        p.sched->on_complete(p.id);
        p.id = 0;
    }
    h.destroy();
}

} /* namespace opencode::core */

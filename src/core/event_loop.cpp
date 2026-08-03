#include "core/event_loop.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <unordered_map>
#include <unistd.h>

#include "core/clock.h"

#ifdef __linux__
#include <sys/epoll.h>
#else
#include <poll.h>
#endif

namespace opencode::core {

namespace {

#ifdef __linux__
uint32_t to_epoll(uint32_t interest) noexcept {
    uint32_t e = EPOLLERR | EPOLLHUP;
    if (interest & EventLoop::kRead) e |= EPOLLIN;
    if (interest & EventLoop::kWrite) e |= EPOLLOUT;
    return e;
}
uint32_t from_epoll(uint32_t e) noexcept {
    uint32_t o = 0;
    if (e & EPOLLIN) o |= EventLoop::kRead;
    if (e & EPOLLOUT) o |= EventLoop::kWrite;
    if (e & EPOLLERR) o |= EventLoop::kErr;
    if (e & EPOLLHUP) o |= EventLoop::kHup;
    return o;
}
#else
uint32_t to_poll(uint32_t interest) noexcept {
    uint32_t e = POLLERR | POLLHUP;
    if (interest & EventLoop::kRead) e |= POLLIN;
    if (interest & EventLoop::kWrite) e |= POLLOUT;
    return e;
}
uint32_t from_poll(int16_t e) noexcept {
    uint32_t o = 0;
    if (e & POLLIN) o |= EventLoop::kRead;
    if (e & POLLOUT) o |= EventLoop::kWrite;
    if (e & POLLERR) o |= EventLoop::kErr;
    if (e & POLLHUP) o |= EventLoop::kHup;
    return o;
}
#endif

void set_nonblock(int fd) noexcept {
    int fl = ::fcntl(fd, F_GETFL, 0);
    if (fl >= 0) ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

} /* namespace */

struct EventLoop::Impl {
    int wake_rd = -1;
    int wake_wr = -1;
#ifdef __linux__
    int epfd = -1;
#endif
    FdSlot* wake_slot = nullptr;
    std::vector<FdSlot*> slots;      /* index = slot id; null = free */
    std::vector<size_t> free_slots;
    std::unordered_map<int, size_t> fd_slot;
    std::vector<Timer> heap;
    uint64_t next_timer_id = 1;
};

EventLoop::EventLoop() : impl_(new Impl()) {
    int p[2];
    if (::pipe(p) == 0) {
        impl_->wake_rd = p[0];
        impl_->wake_wr = p[1];
        set_nonblock(impl_->wake_rd);
        set_nonblock(impl_->wake_wr);
    }
#ifdef __linux__
    impl_->epfd = ::epoll_create1(EPOLL_CLOEXEC);
#endif
    /* Reserve slot 0 for the wakeup pipe. */
    auto* ws = new FdSlot();
    ws->fd = impl_->wake_rd;
    ws->interest = kRead;
    impl_->wake_slot = ws;
    impl_->slots.push_back(ws);
#ifdef __linux__
    struct epoll_event ev{};
    ev.events = to_epoll(kRead);
    ev.data.ptr = ws;
    ::epoll_ctl(impl_->epfd, EPOLL_CTL_ADD, impl_->wake_rd, &ev);
#endif
}

EventLoop::~EventLoop() {
#ifdef __linux__
    if (impl_->epfd >= 0) ::close(impl_->epfd);
#endif
    for (FdSlot* s : impl_->slots) delete s;
    if (impl_->wake_rd >= 0) ::close(impl_->wake_rd);
    if (impl_->wake_wr >= 0) ::close(impl_->wake_wr);
    delete impl_;
}

uint64_t EventLoop::now_ms() const noexcept { return now_mono_ms(); }

bool EventLoop::add(int fd, uint32_t interest, Handler h, void* userdata) {
    if (fd < 0 || impl_->fd_slot.count(fd)) return false;
    size_t slot;
    if (!impl_->free_slots.empty()) {
        slot = impl_->free_slots.back();
        impl_->free_slots.pop_back();
    } else {
        slot = impl_->slots.size();
        impl_->slots.push_back(nullptr);
    }
    auto* s = new FdSlot();
    s->fd = fd;
    s->interest = interest;
    s->h = h;
    s->u = userdata;
    impl_->slots[slot] = s;
    impl_->fd_slot.emplace(fd, slot);
#ifdef __linux__
    struct epoll_event ev{};
    ev.events = to_epoll(interest);
    ev.data.ptr = s;
    if (::epoll_ctl(impl_->epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        delete s;
        impl_->slots[slot] = nullptr;
        impl_->fd_slot.erase(fd);
        return false;
    }
#else
    (void)0;
#endif
    return true;
}

bool EventLoop::mod(int fd, uint32_t interest) {
    auto it = impl_->fd_slot.find(fd);
    if (it == impl_->fd_slot.end()) return false;
    FdSlot* s = impl_->slots[it->second];
    s->interest = interest;
#ifdef __linux__
    struct epoll_event ev{};
    ev.events = to_epoll(interest);
    ev.data.ptr = s;
    if (::epoll_ctl(impl_->epfd, EPOLL_CTL_MOD, fd, &ev) < 0) return false;
#endif
    return true;
}

bool EventLoop::del(int fd) {
    auto it = impl_->fd_slot.find(fd);
    if (it == impl_->fd_slot.end()) return false;
    const size_t slot = it->second;
#ifdef __linux__
    struct epoll_event ev{};
    ::epoll_ctl(impl_->epfd, EPOLL_CTL_DEL, fd, &ev);
#endif
    delete impl_->slots[slot];
    impl_->slots[slot] = nullptr;
    impl_->fd_slot.erase(it);
    impl_->free_slots.push_back(slot);
    return true;
}

bool EventLoop::has_fd(int fd) const {
    return impl_->fd_slot.count(fd) != 0;
}

uint64_t EventLoop::add_timer(uint64_t delay_ms, TimerHandler h, void* userdata,
                              bool oneshot) {
    Timer t;
    t.id = impl_->next_timer_id++;
    t.deadline_ms = now_ms() + delay_ms;
    t.period_ms = delay_ms;
    t.h = h;
    t.u = userdata;
    t.oneshot = oneshot;
    impl_->heap.push_back(t);
    std::push_heap(impl_->heap.begin(), impl_->heap.end(),
                   [](const Timer& a, const Timer& b) {
                       return a.deadline_ms > b.deadline_ms;
                   });
    return t.id;
}

bool EventLoop::cancel_timer(uint64_t timer_id) {
    for (Timer& t : impl_->heap) {
        if (t.id == timer_id) {
            t.cancelled = true;
            return true;
        }
    }
    return false;
}

void EventLoop::drain_wakeup() {
    char buf[64];
    while (::read(impl_->wake_rd, buf, sizeof buf) > 0) {
    }
}

size_t EventLoop::fire_due_timers() {
    size_t handled = 0;
    const uint64_t now = now_ms();
    while (!impl_->heap.empty() && impl_->heap.front().deadline_ms <= now) {
        Timer t = impl_->heap.front();
        std::pop_heap(impl_->heap.begin(), impl_->heap.end(),
                      [](const Timer& a, const Timer& b) {
                          return a.deadline_ms > b.deadline_ms;
                      });
        impl_->heap.pop_back();
        if (t.cancelled) continue;
        if (!t.oneshot) {
            t.deadline_ms = now_ms() + t.period_ms;
            impl_->heap.push_back(t);
            std::push_heap(impl_->heap.begin(), impl_->heap.end(),
                           [](const Timer& a, const Timer& b) {
                               return a.deadline_ms > b.deadline_ms;
                           });
        }
        TimerHandler h = t.h;
        void* u = t.u;
        h(u, t.id);
        ++handled;
    }
    return handled;
}

int EventLoop::run_once(int timeout_ms) {
    int handled = 0;
    /* Absolute deadline for the caller's timeout; -1 means "wait forever". */
    int64_t deadline = timeout_ms < 0 ? -1 : static_cast<int64_t>(now_ms()) + timeout_ms;
    for (;;) {
        handled += static_cast<int>(fire_due_timers());
        if (handled > 0) return handled;

        int64_t now = static_cast<int64_t>(now_ms());
        int wait;
        if (deadline < 0) {
            wait = -1;
        } else {
            int64_t rem = deadline - now;
            if (rem <= 0) return handled; /* caller timeout elapsed */
            wait = rem > INT_MAX ? INT_MAX : static_cast<int>(rem);
        }
        if (!impl_->heap.empty()) {
            int64_t d = static_cast<int64_t>(impl_->heap.front().deadline_ms) - now;
            if (d < 0) d = 0;
            if (wait < 0 || d < wait) wait = d > INT_MAX ? INT_MAX : static_cast<int>(d);
        }
        if (wait == 0) continue; /* timer due this instant → fire it */

#ifdef __linux__
        struct epoll_event evs[kMaxEvents];
        int n = ::epoll_wait(impl_->epfd, evs, kMaxEvents, wait);
        for (int i = 0; i < n; ++i) {
            auto* s = static_cast<FdSlot*>(evs[i].data.ptr);
            if (s == impl_->wake_slot) {
                drain_wakeup();
                continue;
            }
            const uint32_t ocev = from_epoll(evs[i].events);
            Handler h = s->h;
            void* u = s->u;
            if (h != nullptr) {
                h(u, ocev);
                ++handled;
            }
        }
        if (n > 0) return handled; /* an event (fd or wakeup) arrived */
#else
        (void)0;
        std::vector<struct pollfd> pfds;
        pfds.reserve(impl_->slots.size());
        for (FdSlot* s : impl_->slots) {
            if (s == nullptr) continue;
            struct pollfd p{};
            p.fd = s->fd;
            p.events = static_cast<int16_t>(to_poll(s->interest));
            pfds.push_back(p);
        }
        const int n = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), wait);
        for (size_t i = 0; i < pfds.size(); ++i) {
            if (pfds[i].revents == 0) continue;
            FdSlot* s = impl_->slots[i];
            if (s == impl_->wake_slot) {
                drain_wakeup();
                continue;
            }
            const uint32_t ocev = from_poll(pfds[i].revents);
            Handler h = s->h;
            void* u = s->u;
            if (h != nullptr) {
                h(u, ocev);
                ++handled;
            }
        }
        if (n > 0) return handled;
#endif
        /* n == 0: the wait expired with no fd event → re-check timers. */
    }
}

bool EventLoop::wakeup() noexcept {
    if (impl_->wake_wr < 0) return false;
    const char c = 1;
    ssize_t w = ::write(impl_->wake_wr, &c, 1);
    return w == 1 || errno == EAGAIN;
}

} /* namespace opencode::core */

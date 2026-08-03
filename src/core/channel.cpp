#include "core/channel.h"

#include <utility>

namespace opencode::core {

Channel::Channel(size_t capacity) noexcept
    : cap_(capacity == 0 ? 1 : capacity), ring_(cap_) {}

Channel::~Channel() = default;

bool Channel::try_push(uint32_t tag, const void* data, uint32_t len) {
    {
        std::lock_guard<std::mutex> l(m_);
        if (closed_ || count_ == cap_) return false;
        ring_[tail_] = Message{tag, len, data};
        tail_ = (tail_ + 1) % cap_;
        ++count_;
    }
    if (wake_ != nullptr) wake_(wake_ctx_);
    return true;
}

Channel::PopResult Channel::try_pop(Message& out) {
    std::lock_guard<std::mutex> l(m_);
    if (count_ == 0) return closed_ ? kClosed : kEmpty;
    out = ring_[head_];
    head_ = (head_ + 1) % cap_;
    --count_;
    return kOk;
}

size_t Channel::size() const noexcept {
    std::lock_guard<std::mutex> l(m_);
    return count_;
}

void Channel::close() {
    std::lock_guard<std::mutex> l(m_);
    closed_ = true;
    if (count_ == 0) {
        head_ = 0;
        tail_ = 0;
    }
}

void Channel::drop() {
    std::lock_guard<std::mutex> l(m_);
    count_ = 0;
    head_ = 0;
    tail_ = 0;
    closed_ = true;
}

} /* namespace opencode::core */

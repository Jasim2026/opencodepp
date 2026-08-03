#include "core/arena.h"

#include <cstdlib>
#include <limits>

namespace opencode::core {

Arena::Arena(size_t first_chunk) noexcept : next_cap_(first_chunk) {}

Arena::Arena(Arena&& o) noexcept
    : head_(o.head_),
      tail_(o.tail_),
      next_cap_(o.next_cap_),
      used_(o.used_),
      total_(o.total_),
      nblocks_(o.nblocks_) {
    o.head_ = nullptr;
    o.tail_ = nullptr;
    o.next_cap_ = Arena::kDefaultChunk;
    o.used_ = o.total_ = o.nblocks_ = 0;
}

Arena& Arena::operator=(Arena&& o) noexcept {
    if (this != &o) {
        clear();
        head_ = o.head_;
        tail_ = o.tail_;
        next_cap_ = o.next_cap_;
        used_ = o.used_;
        total_ = o.total_;
        nblocks_ = o.nblocks_;
        o.head_ = nullptr;
        o.tail_ = nullptr;
        o.used_ = o.total_ = o.nblocks_ = 0;
    }
    return *this;
}

Arena::~Arena() { clear(); }

Arena::Block* Arena::grow(size_t need) noexcept {
    /* Grow geometrically up to kMaxChunk, then keep linear. */
    size_t cap = next_cap_;
    if (cap < need) cap = need;
    next_cap_ = (next_cap_ >= kMaxChunk) ? next_cap_ + kMaxChunk
                                         : next_cap_ * 2;

    const size_t header = sizeof(Block);
    if (cap > (std::numeric_limits<size_t>::max)() - header - Arena::kAlign)
        return nullptr;

    void* raw = std::malloc(header + cap + Arena::kAlign - 1);
    if (raw == nullptr) return nullptr;

    auto* b = static_cast<Block*>(raw);
    b->cap = cap;
    b->off = 0;
    b->older = head_;
    b->newer = nullptr;
    if (head_ != nullptr) head_->newer = b;
    head_ = b;
    if (tail_ == nullptr) tail_ = b;
    ++nblocks_;
    total_ += header + cap + Arena::kAlign - 1;
    return b;
}

void* Arena::alloc(size_t n, size_t align) noexcept {
    if (align < alignof(max_align_t)) align = alignof(max_align_t);
    if (align > kAlign) align = kAlign;
    if (n == 0) n = 1;

    /* Scan oldest → newest so a reset workload reuses existing blocks before
     * growing (steady-state zero-churn after warmup). */
    for (Block* b = tail_; b != nullptr; b = b->newer) {
        const size_t off = align_up(b->off, align);
        if (off + n <= b->cap) {
            const size_t consumed = off + n - b->off;
            b->off = off + n;
            used_ += consumed;
            return b->data() + off;
        }
    }

    Block* b = grow(n);
    if (b == nullptr) return nullptr;
    const size_t off = align_up(0, align);
    b->off = off + n;
    used_ += off + n;
    return b->data() + off;
}

void Arena::reset() noexcept {
    for (Block* b = head_; b != nullptr; b = b->older) b->off = 0;
    used_ = 0;
}

void Arena::clear() noexcept {
    Block* b = head_;
    while (b != nullptr) {
        Block* older = b->older;
        std::free(b);
        b = older;
    }
    head_ = nullptr;
    tail_ = nullptr;
    used_ = total_ = nblocks_ = 0;
    next_cap_ = kDefaultChunk;
}

} /* namespace opencode::core */

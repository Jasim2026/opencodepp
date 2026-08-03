/*
 * arena.h -- chunked bump allocator (the memory foundation).
 *
 * Deterministic, no per-allocation bookkeeping, O(1) reset, geometric growth.
 * Chunks are malloc'd (16 KiB initial) and reused across resets, so steady-state
 * churn is zero after warmup -- a key T2 ingredient (per-turn allocation ~0).
 *
 * Thread-safety: an Arena is NOT thread-safe; one Arena per owning context.
 */
#ifndef OPENCODE_CORE_ARENA_H
#define OPENCODE_CORE_ARENA_H

#include <cstddef>
#include <cstdint>

namespace opencode::core {

class Arena {
public:
    static constexpr size_t kDefaultChunk = 16u << 10; /* 16 KiB */
    static constexpr size_t kMaxChunk = 1u << 20;      /* growth cap: 1 MiB */
    /* Data regions are aligned to kAlign, so any power-of-two alignment
     * request <= kAlign is satisfied by bump-aligning the offset. */
    static constexpr size_t kAlign = 64;

    explicit Arena(size_t first_chunk = kDefaultChunk) noexcept;
    Arena(Arena&& o) noexcept;
    Arena& operator=(Arena&& o) noexcept;
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    ~Arena();

    /* Aligned bump allocation. `align` must be a power of two <= kAlign.
     * Returns nullptr on OOM (never throws). */
    void* alloc(size_t n, size_t align = alignof(max_align_t)) noexcept;

    template <typename T>
    T* alloc_array(size_t count) noexcept {
        if (count > (SIZE_MAX / sizeof(T))) return nullptr;
        return static_cast<T*>(alloc(count * sizeof(T), alignof(T)));
    }

    /* O(#blocks): discard all allocations, KEEP the memory for reuse. */
    void reset() noexcept;

    /* Free every chunk; back to a pristine state. */
    void clear() noexcept;

    size_t bytes_used() const noexcept { return used_; }
    size_t bytes_cap() const noexcept { return total_; }
    size_t block_count() const noexcept { return nblocks_; }

private:
    struct Block {
        size_t cap; /* usable bytes from data() */
        size_t off; /* next free offset in data() */
        Block* older; /* toward the oldest block */
        Block* newer; /* toward the most recently grown block */
        /* Data region: aligned right after the header. */
        char* data() noexcept {
            const uintptr_t v =
                reinterpret_cast<uintptr_t>(this) + sizeof(Block);
            return reinterpret_cast<char*>((v + kAlign - 1) & ~(kAlign - 1));
        }
    };
    Block* head_ = nullptr; /* most recently grown block */
    Block* tail_ = nullptr; /* oldest block (first in list) */
    Block* cur_ = nullptr;  /* block being bumped into (fill cursor) */
    size_t next_cap_;
    size_t used_ = 0;
    size_t total_ = 0; /* bytes actually malloc'd (headers + data) */
    size_t nblocks_ = 0;

    static size_t align_up(size_t v, size_t a) noexcept {
        return (v + a - 1) & ~(a - 1);
    }
    Block* grow(size_t need) noexcept;
};

/* RAII guard: resets the arena at scope end (frees all its allocations). */
class ScopeArena {
public:
    explicit ScopeArena(Arena& a) noexcept : a_(a) {}
    ~ScopeArena() { a_.reset(); }
    ScopeArena(const ScopeArena&) = delete;
    ScopeArena& operator=(const ScopeArena&) = delete;

private:
    Arena& a_;
};

} /* namespace opencode::core */

#endif /* OPENCODE_CORE_ARENA_H */

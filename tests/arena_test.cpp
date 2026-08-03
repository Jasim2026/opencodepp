// arena_test.cpp -- Phase 1: bump allocator correctness and reset semantics.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>

#include "core/arena.h"

namespace {
int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #cond);                                             \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

void test_basic_alloc() {
    using namespace opencode::core;
    Arena a;
    void* p = a.alloc(64);
    CHECK(p != nullptr);
    void* q = a.alloc(4096);
    CHECK(q != nullptr);
    CHECK(p != q);
    CHECK(a.bytes_used() == 64 + 4096);
    CHECK(a.block_count() >= 1);
}

void test_alignment() {
    using namespace opencode::core;
    Arena a;
    for (int i = 0; i < 100; ++i) {
        void* p = a.alloc(1 + static_cast<size_t>(i) * 3, 64);
        CHECK(p != nullptr);
        CHECK(reinterpret_cast<uintptr_t>(p) % 64 == 0);
    }
}

void test_reset_reuses_memory() {
    using namespace opencode::core;
    Arena a;
    for (int i = 0; i < 100; ++i) a.alloc(1000);
    const size_t cap_after = a.bytes_cap();
    const size_t blocks_after = a.block_count();
    a.reset();
    CHECK(a.bytes_used() == 0);
    CHECK(a.bytes_cap() == cap_after);    /* memory kept */
    CHECK(a.block_count() == blocks_after);
    for (int i = 0; i < 100; ++i) a.alloc(1000); /* no new blocks needed */
    CHECK(a.block_count() == blocks_after);
}

void test_growth_caps() {
    using namespace opencode::core;
    Arena a;
    /* small requests never exceed the max chunk cap */
    for (int i = 0; i < 200; ++i) a.alloc(64);
    CHECK(a.bytes_cap() <= Arena::kMaxChunk + Arena::kMaxChunk * 2);
    /* a big request allocates what it needs */
    void* big = a.alloc(Arena::kMaxChunk * 2);
    CHECK(big != nullptr);
}

void test_clear_and_oom() {
    using namespace opencode::core;
    Arena a;
    a.alloc(100);
    a.clear();
    CHECK(a.bytes_used() == 0);
    CHECK(a.block_count() == 0);
    /* giant allocation returns nullptr (not a crash) */
    CHECK(a.alloc(SIZE_MAX - 1) == nullptr);
}

void test_alloc_array_and_scope() {
    using namespace opencode::core;
    Arena a;
    {
        ScopeArena scope(a);
        uint32_t* xs = a.alloc_array<uint32_t>(1000);
        CHECK(xs != nullptr);
        xs[999] = 42;
        CHECK(a.bytes_used() > 0);
    }
    CHECK(a.bytes_used() == 0); /* scope reset */
}

void test_move() {
    using namespace opencode::core;
    Arena a;
    a.alloc(16);
    size_t used = a.bytes_used();
    Arena b(std::move(a));
    CHECK(b.bytes_used() == used);
    CHECK(a.block_count() == 0); /* moved-from is empty */
}
} /* namespace */

int main() {
    test_basic_alloc();
    test_alignment();
    test_reset_reuses_memory();
    test_growth_caps();
    test_clear_and_oom();
    test_alloc_array_and_scope();
    test_move();
    if (failures == 0) {
        std::printf("arena_test: OK\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "arena_test: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}

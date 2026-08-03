/*
 * hash.h — stable, allocation-free hashes for cache/session keys and quick
 * structural fingerprinting. None of these are cryptographic.
 */
#ifndef OPENCODE_UTIL_HASH_H
#define OPENCODE_UTIL_HASH_H

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace opencode::util {

/* FNV-1a 64 — good dispersion, trivially portable (stable across runs/ABIs). */
inline uint64_t fnv1a64(std::string_view s, uint64_t seed = 1469598103934665603ull) {
    uint64_t h = seed;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

/* FNV-1a 32 (for table sizes / bloom width). */
inline uint32_t fnv1a32(std::string_view s, uint32_t seed = 2166136261u) {
    uint32_t h = seed;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

/* Mix hash of two values (for composite keys: (ns, name)). */
inline uint64_t mix64(uint64_t a, uint64_t b) noexcept {
    uint64_t h = a ^ (b + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2));
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return h;
}

} /* namespace opencode::util */

#endif /* OPENCODE_UTIL_HASH_H */

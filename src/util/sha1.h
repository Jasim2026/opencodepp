/*
 * sha1.h -- SHA-1 message digest (content fingerprinting).
 *
 * Used by the prompt compiler to pin a compiled PromptRef to its source text
 * (plan 12_PHASE_06.md Task 1): the hash travels in the binary so prompt drift
 * is visible in tests. Pure stdlib, no dependencies. Not for security use.
 */
#ifndef OPENCODE_UTIL_SHA1_H
#define OPENCODE_UTIL_SHA1_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace opencode::util {

/* 20-byte SHA-1 digest in host order (network byte order applied by sha1_hex). */
struct Sha1Digest {
    std::uint32_t h[5] = {0};
};

/* Hash `data`; returns the 20-byte digest. Never throws. */
Sha1Digest sha1(std::string_view data) noexcept;

/* Hex (40 lowercase chars) form of the digest. */
std::string sha1_hex(std::string_view data);

} /* namespace opencode::util */

#endif /* OPENCODE_UTIL_SHA1_H */

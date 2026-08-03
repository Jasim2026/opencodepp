/*
 * sha1.cpp -- SHA-1 per RFC 3174. Never throws; purely additive hashing.
 * Handles arbitrarily long input via streaming 64-byte blocks + final padding.
 */
#include "util/sha1.h"

#include <cstdio>
#include <cstring>

namespace opencode::util {
namespace {

inline std::uint32_t rotl(std::uint32_t v, unsigned n) noexcept {
    return (v << n) | (v >> (32 - n));
}

struct Ctx {
    std::uint32_t h[5];
    std::uint64_t bits = 0;
    unsigned char buf[64];
    size_t len = 0;
};

void process(Ctx& c, const unsigned char* block) noexcept {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<std::uint32_t>(block[i * 4 + 3]);
    for (int i = 16; i < 80; ++i)
        w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    std::uint32_t a = c.h[0], b = c.h[1], d = c.h[2], e = c.h[3], f = c.h[4];
    for (int i = 0; i < 80; ++i) {
        std::uint32_t k, fn;
        if (i < 20) {
            fn = (b & d) | ((~b) & e);
            k = 0x5A827999u;
        } else if (i < 40) {
            fn = b ^ d ^ e;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            fn = (b & d) | (b & e) | (d & e);
            k = 0x8F1BBCDCu;
        } else {
            fn = b ^ d ^ e;
            k = 0xCA62C1D6u;
        }
        const std::uint32_t tmp = rotl(a, 5) + fn + f + k + w[i];
        f = e;
        e = d;
        d = rotl(b, 30);
        b = a;
        a = tmp;
    }
    c.h[0] += a;
    c.h[1] += b;
    c.h[2] += d;
    c.h[3] += e;
    c.h[4] += f;
}

void update(Ctx& c, std::string_view data) noexcept {
    c.bits += static_cast<std::uint64_t>(data.size()) * 8;
    size_t off = 0;
    if (c.len > 0) {
        const size_t need = 64 - c.len;
        const size_t take = (data.size() < need) ? data.size() : need;
        std::memcpy(c.buf + c.len, data.data(), take);
        c.len += take;
        off += take;
        if (c.len == 64) {
            process(c, c.buf);
            c.len = 0;
        }
    }
    while (off + 64 <= data.size()) {
        process(c, reinterpret_cast<const unsigned char*>(data.data() + off));
        off += 64;
    }
    if (off < data.size()) {
        std::memcpy(c.buf, data.data() + off, data.size() - off);
        c.len = data.size() - off;
    }
}

Sha1Digest finish(Ctx& c) noexcept {
    /* Pad: 0x80, then zeros until len == 56 (mod 64), then 64-bit bit length. */
    const uint64_t bits = c.bits;
    unsigned char pad = 0x80;
    update(c, std::string_view(reinterpret_cast<const char*>(&pad), 1));
    unsigned char zeros[64] = {};
    const size_t need = (56u - c.len + 64u) % 64u;
    if (need > 0)
        update(c, std::string_view(reinterpret_cast<const char*>(zeros), need));
    unsigned char lenb[8];
    for (int i = 0; i < 8; ++i)
        lenb[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
    update(c, std::string_view(reinterpret_cast<const char*>(lenb), 8));

    Sha1Digest d;
    d.h[0] = c.h[0];
    d.h[1] = c.h[1];
    d.h[2] = c.h[2];
    d.h[3] = c.h[3];
    d.h[4] = c.h[4];
    return d;
}

} /* namespace */

Sha1Digest sha1(std::string_view data) noexcept {
    Ctx c{{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u},
          0, {}, 0};
    update(c, data);
    return finish(c);
}

std::string sha1_hex(std::string_view data) {
    const Sha1Digest d = sha1(data);
    char out[41];
    std::snprintf(out, sizeof out, "%08x%08x%08x%08x%08x",
                  static_cast<unsigned>(d.h[0]), static_cast<unsigned>(d.h[1]),
                  static_cast<unsigned>(d.h[2]), static_cast<unsigned>(d.h[3]),
                  static_cast<unsigned>(d.h[4]));
    return std::string(out, 40);
}

} /* namespace opencode::util */

#include "util/base64.h"

#include <cstring>

namespace opencode::util {

namespace {

constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int decode_char(unsigned char c) noexcept {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

} /* namespace */

size_t b64_encode(const void* data, size_t n, char* out) noexcept {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t o = 0;
    for (size_t i = 0; i + 3 <= n; i += 3) {
        uint32_t v = (uint32_t(p[i]) << 16) | (uint32_t(p[i + 1]) << 8) |
                     uint32_t(p[i + 2]);
        out[o++] = kAlphabet[(v >> 18) & 63];
        out[o++] = kAlphabet[(v >> 12) & 63];
        out[o++] = kAlphabet[(v >> 6) & 63];
        out[o++] = kAlphabet[v & 63];
    }
    size_t rem = n % 3;
    if (rem == 1) {
        uint32_t v = uint32_t(p[n - 1]) << 16;
        out[o++] = kAlphabet[(v >> 18) & 63];
        out[o++] = kAlphabet[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        uint32_t v = (uint32_t(p[n - 2]) << 16) | (uint32_t(p[n - 1]) << 8);
        out[o++] = kAlphabet[(v >> 18) & 63];
        out[o++] = kAlphabet[(v >> 12) & 63];
        out[o++] = kAlphabet[(v >> 6) & 63];
        out[o++] = '=';
    }
    out[o] = '\0';
    return o;
}

core::error_code b64_decode(std::string_view in, void* out, size_t out_cap,
                            size_t& out_len) noexcept {
    out_len = 0;
    if (in.empty()) return core::make_error_code(core::Err::ok);
    uint8_t* q = static_cast<uint8_t*>(out);
    size_t count = 0;
    size_t o = 0;
    uint32_t acc = 0;
    int acc_bits = 0;
    bool saw_pad = false;
    for (unsigned char c : in) {
        if (saw_pad) {
            /* only '=' or whitespace allowed after the pad */
            if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t')
                continue;
            return core::make_error_code(core::Err::e_proto_parse);
        }
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        if (c == '=') {
            saw_pad = true;
            continue;
        }
        int v = decode_char(c);
        if (v < 0) return core::make_error_code(core::Err::e_proto_parse);
        acc = (acc << 6) | uint32_t(v);
        acc_bits += 6;
        if (acc_bits >= 8) {
            acc_bits -= 8;
            uint8_t byte = uint8_t((acc >> acc_bits) & 0xFF);
            if (o + 1 > out_cap) {
                return core::make_error_code(core::Err::e_overflow);
            }
            q[o++] = byte;
            ++count;
        }
    }
    out_len = o;
    return core::make_error_code(core::Err::ok);
}

} /* namespace opencode::util */

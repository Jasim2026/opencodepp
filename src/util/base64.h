/*
 * base64.h -- RFC 4648 base64, allocation-free encode/decode into caller
 * buffers. Used for wire payloads and patch content in messages.
 */
#ifndef OPENCODE_UTIL_BASE64_H
#define OPENCODE_UTIL_BASE64_H

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "core/error.h"

namespace opencode::util {

/* Worst-case sizes. encoded needs 4*((n+2)/3); decoded needs 3*(n/4). */
inline size_t b64_encoded_size(size_t n) noexcept {
    return 4 * ((n + 2) / 3);
}
inline size_t b64_decoded_size(size_t n) noexcept {
    return n / 4 * 3;
}

/* Encodes n bytes into out (must hold b64_encoded_size(n) + 1). Returns the
 * number of characters written (excluding NUL terminator). */
size_t b64_encode(const void* data, size_t n, char* out) noexcept;

/* Decodes an unpadded-or-padded base64 string. Returns error_code ok or
 * core::Err::e_proto_parse. out must hold b64_decoded_size(len) + 1. */
core::error_code b64_decode(std::string_view in, void* out, size_t out_cap,
                            size_t& out_len) noexcept;

} /* namespace opencode::util */

#endif /* OPENCODE_UTIL_BASE64_H */

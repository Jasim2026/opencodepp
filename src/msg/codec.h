/*
 * codec.h -- compact, deterministic, versioned binary codec for Message.
 *
 * Wire layout (v1):
 *
 *   u8  version = 1
 *   str id, str session_id, u8 role, str model, varint created_at,
 *   varint part_count
 *   per part: u8 part_kind, then kind-specific fields (see codec.cpp).
 *
 * Strings are interned per message: the first occurrence is written inline
 * (flag 0x00, varint len, bytes); repeats are written as a reference
 * (flag 0x01, varint index into the first-seen dictionary). Interning is
 * deterministic and ordered, so a decode followed by an encode of the same
 * message yields the same bytes (round-trip byte-identical).
 *
 * Values use LEB128 varints and all lengths are validated on decode. Any
 * malformed input returns `core::Err::e_proto_parse` (ABI
 * OPENCODE_ERR_VALIDATION) with the offending byte offset in `detail()`.
 * A leading version other than kCodecVersion yields detail
 * kCodecDetailVersionMismatch.
 *
 * `encode_message` writes into the caller's arena (never frees, so wrap it in
 * a ScopeArena for the throwaway path) and returns a span into arena memory;
 * on OOM it returns an empty span. `decode_message` fully owns its output.
 */
#ifndef OPENCODE_MSG_CODEC_H
#define OPENCODE_MSG_CODEC_H

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/arena.h"
#include "core/error.h"
#include "msg/message.h"

namespace opencode::msg {

inline constexpr std::uint32_t kCodecVersion = 1;
/* detail() value used for a version-byte mismatch (offsets never reach this). */
inline constexpr std::uint32_t kCodecDetailVersionMismatch = 0xFFFFFFFFu;
/* Hard cap on parts per message; protects decode from hostile varints. */
inline constexpr std::size_t kCodecMaxParts = 1u << 20;

/* Encodes `m` into `arena`. Returns the bytes in arena memory, or an empty
 * span on allocation failure. Never throws. */
std::span<std::byte> encode_message(const Message& m, core::Arena& arena);

/* Decodes `s` into `out` (heap-owning). ok() on success; e_proto_parse with
 * the byte offset otherwise. Never throws. */
core::error_code decode_message(std::span<const std::byte> s, Message& out);

} /* namespace opencode::msg */

#endif /* OPENCODE_MSG_CODEC_H */

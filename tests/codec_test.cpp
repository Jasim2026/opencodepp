// codec_test.cpp -- Phase 2: binary codec. Round-trip byte-identical on a
// 500-part message, determinism, malformed-input rejection with offsets, and a
// 10k-input fuzz (no crash, always ok() or e_proto_parse).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/arena.h"
#include "core/error.h"
#include "msg/codec.h"
#include "msg/message.h"
#include "msg/part.h"
#include "msg/role.h"

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

using namespace opencode::core;
using namespace opencode::msg;

bool part_eq(const Part& a, const Part& b) {
    if (part_kind(a) != part_kind(b)) return false;
    switch (part_kind(a)) {
        case PartKind::text:
            return as<Text>(a)->content == as<Text>(b)->content;
        case PartKind::reasoning:
            return as<Reasoning>(a)->content == as<Reasoning>(b)->content;
        case PartKind::image_url:
            return as<ImageUrl>(a)->url == as<ImageUrl>(b)->url;
        case PartKind::binary:
            return as<Binary>(a)->mime == as<Binary>(b)->mime &&
                   as<Binary>(a)->data == as<Binary>(b)->data;
        case PartKind::tool_call:
            return as<ToolCall>(a)->id == as<ToolCall>(b)->id &&
                   as<ToolCall>(a)->name == as<ToolCall>(b)->name &&
                   as<ToolCall>(a)->input_json == as<ToolCall>(b)->input_json &&
                   as<ToolCall>(a)->finished == as<ToolCall>(b)->finished;
        case PartKind::tool_result:
            return as<ToolResult>(a)->call_id == as<ToolResult>(b)->call_id &&
                   as<ToolResult>(a)->content == as<ToolResult>(b)->content &&
                   as<ToolResult>(a)->is_error == as<ToolResult>(b)->is_error;
        case PartKind::finish:
            return as<Finish>(a)->reason == as<Finish>(b)->reason;
    }
    return false;
}

bool msg_eq(const Message& a, const Message& b) {
    if (a.id != b.id || a.session_id != b.session_id || a.role != b.role ||
        a.model != b.model || a.created_at != b.created_at ||
        a.parts.size() != b.parts.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.parts.size(); i++) {
        if (!part_eq(a.parts[i], b.parts[i])) return false;
    }
    return true;
}

/* A 500-part message exercising every part kind with repeated strings (so the
 * interner does real work). */
Message big_message() {
    Message m;
    m.id = "m-big";
    m.session_id = "session-1";
    m.role = Role::assistant;
    m.model = "gpt-4o";
    m.created_at = 1700000000ull;
    m.parts.reserve(500);
    for (int i = 0; i < 500; i++) {
        switch (i % 7) {
            case 0: m.parts.push_back(Text{"repeat-me content body"}); break;
            case 1: m.parts.push_back(Reasoning{"thinking trace"}); break;
            case 2:
                m.parts.push_back(ImageUrl{"https://example.com/i.png"});
                break;
            case 3:
                m.parts.push_back(Binary{"image/png",
                                         std::vector<std::uint8_t>(64, 0xAB)});
                break;
            case 4:
                m.parts.push_back(ToolCall{"call-1", "bash",
                                           "{\"cmd\":\"ls\"}", i % 2 == 0});
                break;
            case 5:
                m.parts.push_back(ToolResult{"call-1", "ok output", i % 3 == 0});
                break;
            case 6: m.parts.push_back(Finish{FinishReason::end_turn}); break;
        }
    }
    return m;
}

std::vector<std::uint8_t> to_bytes(std::span<std::byte> s) {
    return std::vector<std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(s.data()),
        reinterpret_cast<const std::uint8_t*>(s.data()) + s.size());
}

void test_round_trip_big() {
    const Message m = big_message();
    Arena a;
    const std::vector<std::uint8_t> b1 = to_bytes(encode_message(m, a));

    Message m2;
    const error_code ec = decode_message(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(b1.data()),
                                   b1.size()),
        m2);
    CHECK(ec.ok());
    if (ec.ok()) CHECK(msg_eq(m, m2));

    /* re-encode the decoded message: bytes must be identical */
    Arena a2;
    const std::vector<std::uint8_t> b2 = to_bytes(encode_message(m2, a2));
    CHECK(b1 == b2);
}

void test_determinism() {
    const Message m = big_message();
    Arena a1, a2;
    const std::vector<std::uint8_t> b1 = to_bytes(encode_message(m, a1));
    const std::vector<std::uint8_t> b2 = to_bytes(encode_message(m, a2));
    CHECK(b1 == b2);
}

void test_small_messages() {
    Arena a;
    /* empty everything */
    Message m0;
    const std::vector<std::uint8_t> b0 = to_bytes(encode_message(m0, a));
    Message back0;
    const error_code e0 = decode_message(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(b0.data()),
                                   b0.size()),
        back0);
    CHECK(e0.ok());
    if (e0.ok()) CHECK(msg_eq(m0, back0));

    /* empty binary data */
    Message m1;
    m1.parts.push_back(Binary{"text/plain", {}});
    m1.parts.push_back(Text{""});
    const std::vector<std::uint8_t> b1 = to_bytes(encode_message(m1, a));
    Message back1;
    const error_code e1 = decode_message(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(b1.data()),
                                   b1.size()),
        back1);
    CHECK(e1.ok());
    if (e1.ok()) CHECK(msg_eq(m1, back1));
}

void test_version_mismatch() {
    Message m;
    m.id = "x";
    Arena a;
    std::vector<std::uint8_t> b = to_bytes(encode_message(m, a));
    b[0] = 2;
    Message out;
    const error_code ec = decode_message(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(b.data()),
                                   b.size()),
        out);
    CHECK(ec.code() == Err::e_proto_parse);
    CHECK(ec.detail() == kCodecDetailVersionMismatch);

    std::vector<std::uint8_t> b2 = to_bytes(encode_message(m, a));
    b2[0] = 0;
    Message out2;
    const error_code ec2 = decode_message(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(b2.data()),
                                   b2.size()),
        out2);
    CHECK(ec2.code() == Err::e_proto_parse);
    CHECK(ec2.detail() == kCodecDetailVersionMismatch);
}

void test_malformed_specific() {
    Arena a;
    Message m;
    m.id = "abc";
    m.model = "m";
    m.parts.push_back(Text{"hello"});
    m.parts.push_back(Text{"world"});
    std::vector<std::uint8_t> base = to_bytes(encode_message(m, a));
    const auto decode = [](const std::vector<std::uint8_t>& b) {
        Message out;
        return decode_message(
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(b.data()),
                                       b.size()),
            out);
    };

    /* exact byte layout for this message:
     *  [0] version   [1] flag   [2] len 3   [3..5] "abc"
     *  [6] flag      [7] len 0              [8] role
     *  [9] flag      [10] len 1  [11] 'm'   [12] created_at=0
     *  [13] count 2  [14] tag 0  [15] flag  [16] len 5  [17..21] "hello"
     *  [22] tag 0    [23] flag   [24] len 5 [25..29] "world"           */
    CHECK(base.size() == 30);

    /* empty input */
    CHECK(decode({}).code() == Err::e_proto_parse);
    CHECK(decode({}).detail() == 0);
    /* only the version byte */
    CHECK(decode({1}).code() == Err::e_proto_parse);
    /* version byte only, wrong version */
    CHECK(decode({9}).detail() == kCodecDetailVersionMismatch);

    /* bad string flag */
    {
        std::vector<std::uint8_t> b = base;
        b[1] = 0x7F; /* id string flag -> invalid */
        CHECK(decode(b).code() == Err::e_proto_parse);
    }
    /* bad role byte */
    {
        std::vector<std::uint8_t> b = base;
        b[8] = 0x77;
        CHECK(decode(b).code() == Err::e_proto_parse);
    }
    /* bad part kind */
    {
        std::vector<std::uint8_t> b = base;
        b[22] = 0x7F; /* second part's kind tag */
        CHECK(decode(b).code() == Err::e_proto_parse);
    }
    /* ref index out of range */
    {
        std::vector<std::uint8_t> b = base;
        b[1] = 0x01; /* id as a reference */
        b[2] = 0x05; /* to index 5, but dict has only 1 entry */
        CHECK(decode(b).code() == Err::e_proto_parse);
    }
    /* binary data length beyond the buffer */
    {
        Message mbin;
        mbin.parts.push_back(Binary{"m", {1, 2, 3}});
        std::vector<std::uint8_t> b = to_bytes(encode_message(mbin, a));
        /* layout: [0]ver [1]flag [2]len0 [3]flag [4]len0 [5]role [6]flag
         *         [7]len0 [8]created [9]count [10]tag5 [11]flag [12]len1
         *         [13]'m' [14]datalen 3 [15..17] data                  */
        CHECK(b.size() == 18);
        b[14] = 0x40; /* claim 64 data bytes, only 3 remain */
        CHECK(decode(b).code() == Err::e_proto_parse);
    }
    /* huge part count */
    {
        std::vector<std::uint8_t> b = base;
        b[13] = 0xFF; b[14] = 0xFF; b[15] = 0xFF; b[16] = 0x7F; /* 2^28-1 */
        CHECK(decode(b).code() == Err::e_proto_parse);
    }
    /* truncated: every strict prefix must fail cleanly */
    for (std::size_t n = 0; n < base.size(); n++) {
        std::vector<std::uint8_t> pre(base.begin(), base.begin() + n);
        const error_code ec = decode(pre);
        CHECK(ec.code() == Err::e_proto_parse);
    }
}

/* xorshift32, deterministic */
std::uint32_t rng_state = 0x12345678u;
std::uint32_t rng() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

void test_fuzz() {
    for (int iter = 0; iter < 10000; iter++) {
        const std::size_t len = rng() % 128;
        std::vector<std::uint8_t> buf(len);
        for (std::size_t i = 0; i < len; i++) buf[i] = static_cast<std::uint8_t>(rng());

        Message out;
        const error_code ec = decode_message(
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf.data()),
                                       buf.size()),
            out);
        CHECK(ec.ok() || ec.code() == Err::e_proto_parse);
        /* a random byte flip of a valid message must also never crash */
    }

    /* byte flips of a valid message */
    Arena a;
    const Message m = big_message();
    const std::vector<std::uint8_t> base = to_bytes(encode_message(m, a));
    for (int iter = 0; iter < 2000; iter++) {
        std::vector<std::uint8_t> b = base;
        const std::size_t pos = rng() % b.size();
        b[pos] = static_cast<std::uint8_t>(rng());
        Message out;
        const error_code ec = decode_message(
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(b.data()),
                                       b.size()),
            out);
        CHECK(ec.ok() || ec.code() == Err::e_proto_parse);
    }
}

void test_dedup_compactness() {
    Arena a;
    Message m;
    m.id = "s";
    for (int i = 0; i < 200; i++) {
        m.parts.push_back(Text{"the same long repeated tool output text"});
    }
    const std::vector<std::uint8_t> b = to_bytes(encode_message(m, a));
    /* 200 copies would be ~200*40 bytes naive; dedup keeps it near one copy. */
    CHECK(b.size() < 200 * 20);
    /* and it still round-trips */
    Message out;
    const error_code ec = decode_message(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(b.data()),
                                   b.size()),
        out);
    CHECK(ec.ok());
    if (ec.ok()) CHECK(msg_eq(m, out));
}
} /* namespace */

int main() {
    test_round_trip_big();
    test_determinism();
    test_small_messages();
    test_version_mismatch();
    test_malformed_specific();
    test_fuzz();
    test_dedup_compactness();
    if (failures == 0) {
        std::printf("codec_test: OK\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "codec_test: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}

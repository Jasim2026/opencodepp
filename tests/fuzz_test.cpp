// fuzz_test.cpp -- Phase 13 deterministic cross-module fuzz sweep.
//
// Runs the same adversarial input generators across the parsing/gating paths
// the agent touches on every task: binary codec, SSE stream parser, JSON DOM,
// intent classifier, token estimator, and the verify gate (syntax/symbols/
// impact/diff over garbled proposals). Deterministic per seed, so a CI run is
// reproducible; the dev preset runs this under ASan/UBSan.
//
// Every code path in the engine is declared "never throws"; this test turns
// any escaping exception or crash into a failure, and asserts the codec/JSON
// decoders only ever return ok() or their documented error codes.
//
// Usage: fuzz_test [--seed N] [--iters N]   (defaults 1 / 500)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "agent/intent.h"
#include "core/arena.h"
#include "core/error.h"
#include "msg/codec.h"
#include "msg/message.h"
#include "msg/part.h"
#include "msg/tokens.h"
#include "net/sse.h"
#include "util/json.h"
#include "verify/gate.h"

namespace {

using opencode::core::Arena;
using opencode::core::Err;
using opencode::core::error_code;

int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #cond);                                             \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

/* xorshift64 -- deterministic PRNG. */
std::uint64_t g_seed = 1;
std::uint64_t rng() {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 7;
    g_seed ^= g_seed << 17;
    return g_seed;
}
std::uint8_t rnd8() { return static_cast<std::uint8_t>(rng() >> 33); }

std::string random_bytes(int max_len) {
    const std::size_t n = rng() % static_cast<std::size_t>(max_len);
    std::string s;
    s.reserve(n);
    for (std::size_t i = 0; i < n; i++)
        s.push_back(static_cast<char>(rnd8()));
    return s;
}

std::string random_text(int max_len) {
    /* biased toward printable so parsers see realistic-looking junk. */
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 "
        "\n\t{}[]():;\"'\\,./_=-+*#<>!@$%^&|";
    const std::size_t n = rng() % static_cast<std::size_t>(max_len);
    std::string s;
    s.reserve(n);
    for (std::size_t i = 0; i < n; i++)
        s.push_back(alphabet[rng() % (sizeof alphabet - 1)]);
    return s;
}

/* ---- 1. binary codec: hostile + byte-flipped input must never crash and
 *        only ever return ok() or e_proto_parse ---- */

void fuzz_codec(int iters) {
    Arena a;
    for (int iter = 0; iter < iters; iter++) {
        const std::string raw = random_bytes(256);
        opencode::msg::Message out;
        const error_code ec = opencode::msg::decode_message(
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(raw.data()), raw.size()),
            out);
        CHECK(ec.ok() || ec.code() == Err::e_proto_parse);
    }

    /* round-trip property on random structured messages */
    for (int iter = 0; iter < iters; iter++) {
        opencode::msg::Message m;
        m.id = random_text(24);
        m.model = random_text(16);
        const int nparts = static_cast<int>(rng() % 12);
        for (int p = 0; p < nparts; p++) {
            switch (rng() % 5) {
                case 0:
                    m.parts.push_back(opencode::msg::Text{random_text(64)});
                    break;
                case 1:
                    m.parts.push_back(opencode::msg::Reasoning{random_text(64)});
                    break;
                case 2:
                    m.parts.push_back(opencode::msg::Binary{
                        random_text(8),
                        std::vector<std::uint8_t>(rng() % 16, rnd8())});
                    break;
                case 3:
                    m.parts.push_back(opencode::msg::ToolCall{
                        random_text(8), random_text(8), random_text(32),
                        (rng() & 1u) != 0});
                    break;
                case 4:
                    m.parts.push_back(opencode::msg::ToolResult{
                        random_text(8), random_text(32), (rng() & 1u) != 0});
                    break;
            }
        }
        const std::span<std::byte> enc = opencode::msg::encode_message(m, a);
        if (enc.empty()) {
            CHECK(false); /* OOM/oversize on a small message is a bug */
            continue;
        }
        opencode::msg::Message dec;
        const error_code dec_ec = opencode::msg::decode_message(enc, dec);
        CHECK(dec_ec.ok());
        if (dec_ec.ok()) {
            /* round-trip must be content-preserving: re-encoding the decoded
             * message yields the identical bytes (codec is canonical). */
            Arena a2;
            const std::span<std::byte> re = opencode::msg::encode_message(dec, a2);
            CHECK(re.size() == enc.size());
            if (re.size() == enc.size())
                CHECK(std::memcmp(re.data(), enc.data(), enc.size()) == 0);
        }
    }
}

/* ---- 2. SSE parser: arbitrary byte streams, arbitrary chunking ---- */

void fuzz_sse(int iters) {
    for (int iter = 0; iter < iters; iter++) {
        const std::string raw = random_bytes(1024);
        opencode::net::SseParser sp;
        sp.set_max_frame_bytes(4096);
        sp.set_max_events(64);
        int events = 0;
        std::size_t off = 0;
        while (off < raw.size()) {
            const std::size_t chunk = 1 + (rng() % 32);
            const std::size_t take = (off + chunk < raw.size()) ? chunk
                                                                : raw.size() - off;
            const error_code ec = sp.feed(
                std::string_view(raw).substr(off, take),
                [&](const opencode::net::SseEvent&) { ++events; });
            CHECK(ec.ok() || ec.code() == Err::e_net_overflow ||
                  ec.code() == Err::e_cancelled);
            off += take;
        }
        CHECK(events <= 64);
    }
}

/* ---- 3. JSON DOM: random text must never throw; a successful parse re-dumps
 *        and re-parses stably ---- */

void fuzz_json(int iters) {
    using opencode::util::JVal;
    for (int iter = 0; iter < iters; iter++) {
        const std::string raw = random_text(512);
        JVal v;
        std::size_t pos = 0;
        const error_code ec = opencode::util::parse_json(raw, v, &pos);
        CHECK(ec.ok() || ec.code() == Err::e_proto_parse);
        if (ec.ok()) {
            const std::string dump = opencode::util::to_json(v);
            JVal v2;
            std::size_t pos2 = 0;
            const error_code ec2 = opencode::util::parse_json(dump, v2, &pos2);
            CHECK(ec2.ok());
            if (ec2.ok())
                CHECK(opencode::util::to_json(v2) == dump); /* idempotence */
        }
    }
}

/* ---- 4. intent classifier + token estimator on arbitrary text ---- */

void fuzz_text(int iters) {
    for (int iter = 0; iter < iters; iter++) {
        const std::string raw = random_text(256);
        const opencode::agent::IntentPlan plan =
            opencode::agent::classify_intent(raw);
        CHECK(!plan.budget_profile.empty());
        const std::size_t t = opencode::msg::estimate_tokens(raw);
        CHECK(t <= raw.size() + 4); /* sane upper bound, no blowup */
        const std::size_t tm = opencode::msg::estimate_message_tokens(
            [&]() {
                opencode::msg::Message m;
                m.parts.push_back(opencode::msg::Text{raw});
                return m;
            }());
        CHECK(tm >= t);
    }
}

/* ---- 5. verify gate: garbled proposals must never throw, and anything that
 *        passes must be a syntactically plausible write/patch ---- */

void fuzz_gate(int iters) {
    opencode::verify::Gate gate;
    opencode::verify::Context ctx;
    for (int iter = 0; iter < iters; iter++) {
        opencode::verify::EditProposal p;
        p.tool_name = (rng() & 1u) ? "file.write" : "file.patch";
        p.args_json = random_text(128);
        p.path = "fuzz_" + random_text(16) + ".cpp";
        p.before_content = random_text(200);
        p.after_content = random_text(200);
        p.patch_text = random_text(256);
        const std::vector<opencode::verify::GateResult> rs = gate.run_all(p, ctx);
        CHECK(!rs.empty());
        if (!rs.empty()) {
            /* run_all stops at the first failing stage: a failure's detail
             * must be populated, and every later result is the failure. */
            bool seen_fail = false;
            for (const auto& r : rs) {
                if (seen_fail) CHECK(!r.pass);
                if (!r.pass) seen_fail = true;
                CHECK(!r.detail.empty() || r.pass);
            }
        }
    }
}

} /* namespace */

int main(int argc, char** argv) {
    int iters = 500;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        auto need = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "fuzz_test: %s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--seed") {
            g_seed = static_cast<std::uint64_t>(std::strtoull(need("--seed"),
                                                              nullptr, 0));
        } else if (arg == "--iters") {
            iters = std::atoi(need("--iters"));
            if (iters < 1) iters = 1;
        } else {
            std::fprintf(stderr, "fuzz_test: unknown flag %s\n", arg.c_str());
            return 2;
        }
    }

    std::printf("fuzz_test: seed=%llu iters=%d\n",
                static_cast<unsigned long long>(g_seed), iters);
    fuzz_codec(iters);
    fuzz_sse(iters);
    fuzz_json(iters);
    fuzz_text(iters);
    fuzz_gate(iters);
    if (failures == 0) {
        std::printf("fuzz_test: OK (all 5 targets clean)\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuzz_test: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}

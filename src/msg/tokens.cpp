/*
 * tokens.cpp -- approximate BPE token counting.
 *
 * Regime constants (tune against tests/fixtures/tokens/ corpus; do not touch
 * without re-running tokens_test):
 *   - prose: 1.3 tokens / whitespace-delimited word  (chars/3.8 avg)
 *   - code:  chars / 4                              (dense symbol text)
 *   - CJK:   1 token per UTF-8 3-byte ideograph, rest ascii at chars/4
 * Classification is a single pass: symbol density + newline density select
 * the code regime; a CJK-dominant text selects the CJK regime; otherwise
 * prose. No allocation in the hot path.
 */
#include "msg/tokens.h"

#include <cstdint>

namespace opencode::msg {

namespace {

/* Strongly code-like ASCII characters (parens/quotes/punctuation that also
 * appear in prose do not count as "code symbols"). */
inline bool is_code_symbol(char c) noexcept {
    switch (c) {
        case '{': case '}': case '<': case '>': case '[': case ']':
        case '(': case ')': case '|': case '&': case '=': case '!':
        case '+': case '*': case '/': case '%': case '^': case '~':
        case '`': case ';': case '#': case '@': case ':': case '"':
            return true;
        default:
            return false;
    }
}

std::uint64_t fnv1a64(std::string_view s) noexcept {
    std::uint64_t h = 1469598103934665603ull;
    for (const unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

struct TokenCache {
    static constexpr std::size_t kLines = 8;
    struct Line {
        std::uint64_t key = 0;
        std::size_t val = 0;
        bool live = false;
    };
    Line lines_[kLines];
    std::size_t next_ = 0;

    std::size_t get(std::string_view s) {
        if (s.size() >= 2048) return estimate_tokens(s); /* hashing too costly */
        const std::uint64_t key = fnv1a64(s);
        for (const Line& l : lines_) {
            if (l.live && l.key == key) return l.val;
        }
        const std::size_t v = estimate_tokens(s);
        Line& l = lines_[next_];
        next_ = (next_ + 1) % kLines;
        l = Line{key, v, true};
        return v;
    }
};

} /* namespace */

std::size_t estimate_tokens(std::string_view s) noexcept {
    const std::size_t n = s.size();
    if (n == 0) return 0;

    std::size_t words = 0;
    std::size_t newlines = 0;
    std::size_t symbols = 0;
    std::size_t cjk = 0;
    bool in_word = false;

    for (std::size_t i = 0; i < n; i++) {
        const unsigned char u = static_cast<unsigned char>(s[i]);
        if (u >= 0x80) {
            /* UTF-8: 2-byte lead 0xC0-0xDF, 3-byte lead 0xE0-0xEF. Treat a
             * 3-byte sequence as one CJK-ish ideograph. */
            if ((u & 0xE0) == 0xE0) cjk++;
            if (!in_word) {
                words++;
                in_word = true;
            }
            continue;
        }
        if (u == ' ' || u == '\t' || u == '\r' || u == '\n') {
            if (u == '\n') newlines++;
            in_word = false;
            continue;
        }
        const bool alnum = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
                           (u >= '0' && u <= '9');
        if (!alnum && is_code_symbol(static_cast<char>(u))) symbols++;
        if (!in_word) {
            words++;
            in_word = true;
        }
    }

    if (cjk >= 4 && cjk * 2 >= words) {
        /* CJK-dominant: ideographs ~1.1 tokens each, remaining ascii at
         * chars/4. */
        const std::size_t ascii_bytes = n >= cjk * 3 ? n - cjk * 3 : 0;
        return cjk * 11 / 10 + ascii_bytes / 4;
    }

    const bool code_heavy =
        (newlines >= 2 && symbols >= 3) ||
        (newlines >= 1 && symbols > words / 2) ||
        (symbols > n / 6);
    if (code_heavy) {
        return std::max<std::size_t>(1, n / 4);
    }

    /* Prose: words*1.3; whitespace-only text is ~0 tokens. */
    if (words == 0) return 0;
    const std::size_t t = words * 13 / 10;
    return t == 0 ? 1 : t;
}

std::size_t estimate_message_tokens(const Message& m) noexcept {
    TokenCache cache;
    std::size_t total = 4; /* message frame overhead */
    for (const Part& p : m.parts) {
        switch (part_kind(p)) {
            case PartKind::text: {
                const Text& t = *as<Text>(p);
                total += cache.get(t.content);
                break;
            }
            case PartKind::reasoning: {
                const Reasoning& r = *as<Reasoning>(p);
                total += cache.get(r.content);
                break;
            }
            case PartKind::image_url: {
                const ImageUrl& i = *as<ImageUrl>(p);
                total += 8 + i.url.size() / 4;
                break;
            }
            case PartKind::binary: {
                const Binary& b = *as<Binary>(p);
                total += 4 + b.mime.size() / 4 + b.data.size() / 4;
                break;
            }
            case PartKind::tool_call: {
                const ToolCall& t = *as<ToolCall>(p);
                total += 8 + t.id.size() / 2 + t.name.size() / 4 +
                         estimate_tokens(t.input_json);
                break;
            }
            case PartKind::tool_result: {
                const ToolResult& r = *as<ToolResult>(p);
                total += 2 + cache.get(r.content);
                break;
            }
            case PartKind::finish:
                total += 1;
                break;
        }
    }
    return total;
}

} /* namespace opencode::msg */

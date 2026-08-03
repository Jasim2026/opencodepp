/*
 * codec.cpp -- binary Message codec (see codec.h for the wire layout).
 */
#include "msg/codec.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace opencode::msg {

namespace {

inline constexpr std::size_t kCodecInitialCap = 256;

/* Arena-backed growable output buffer. On growth the old arena region is left
 * in place (bump allocator); the final span is returned to the caller, so the
 * enclosing arena must outlive the use of the span. */
class EncodeBuf {
public:
    explicit EncodeBuf(core::Arena& arena) noexcept : arena_(&arena) {
        ensure(kCodecInitialCap);
    }

    bool ok() const noexcept { return cur_ != nullptr && !failed_; }

    std::uint8_t* put(std::size_t n) {
        if (!ensure(n)) return nullptr;
        std::uint8_t* p = cur_ + pos_;
        pos_ += n;
        return p;
    }
    void put_bytes(const void* src, std::size_t n) {
        if (std::uint8_t* p = put(n); p) std::memcpy(p, src, n);
    }
    void put_u8(std::uint8_t v) {
        if (std::uint8_t* p = put(1); p) *p = v;
    }
    void put_varint(std::uint64_t v) {
        std::uint8_t tmp[10];
        std::size_t n = 0;
        while (v >= 0x80) {
            tmp[n++] = static_cast<std::uint8_t>(v) | 0x80;
            v >>= 7;
        }
        tmp[n++] = static_cast<std::uint8_t>(v);
        put_bytes(tmp, n);
    }

    std::span<std::byte> view() noexcept {
        if (!ok() || cur_ == nullptr) return {};
        return std::span<std::byte>(
            reinterpret_cast<std::byte*>(cur_), pos_);
    }

private:
    bool ensure(std::size_t need) {
        if (cur_ != nullptr && pos_ + need <= cap_) return true;
        const std::size_t new_cap =
            std::max({kCodecInitialCap, cap_ * 2, pos_ + need});
        std::uint8_t* nb =
            static_cast<std::uint8_t*>(arena_->alloc(new_cap, 1));
        if (nb == nullptr) {
            failed_ = true;
            return false;
        }
        if (cur_ != nullptr && pos_ != 0) std::memcpy(nb, cur_, pos_);
        cur_ = nb;
        cap_ = new_cap;
        return true;
    }

    core::Arena* arena_;
    std::uint8_t* cur_ = nullptr;
    std::size_t cap_ = 0;
    std::size_t pos_ = 0;
    bool failed_ = false;
};

/* Per-message string interner: first-seen order, deterministic. */
class StringInterner {
public:
    explicit StringInterner(std::size_t reserve_hint) { dict_.reserve(reserve_hint); }

    void write(EncodeBuf& buf, std::string_view s) {
        const auto it = dict_.find(s);
        if (it != dict_.end()) {
            buf.put_u8(0x01);
            buf.put_varint(it->second);
            return;
        }
        buf.put_u8(0x00);
        buf.put_varint(s.size());
        buf.put_bytes(s.data(), s.size());
        dict_.emplace(s, static_cast<std::uint32_t>(dict_.size()));
    }

private:
    std::unordered_map<std::string_view, std::uint32_t> dict_;
};

/* Bounds-checked cursor over the input bytes. */
class Decoder {
public:
    Decoder(const std::uint8_t* s, std::size_t n) noexcept : s_(s), size_(n) {}

    std::size_t pos() const noexcept { return pos_; }
    std::size_t remaining() const noexcept { return pos_ > size_ ? 0 : size_ - pos_; }
    bool failed() const noexcept { return failed_; }
    core::error_code err() const noexcept { return err_; }

    void fail(std::uint32_t offset) {
        failed_ = true;
        err_ = core::make_error_code(core::Err::e_proto_parse, offset);
    }

    bool take(std::size_t n, const std::uint8_t*& out) {
        if (failed_) return false;
        if (pos_ > size_ || n > size_ - pos_) {
            fail(static_cast<std::uint32_t>(std::min(pos_, size_)));
            return false;
        }
        out = s_ + pos_;
        pos_ += n;
        return true;
    }
    bool take_u8(std::uint8_t& out) {
        const std::uint8_t* p;
        if (!take(1, p)) return false;
        out = *p;
        return true;
    }
    bool take_varint(std::uint64_t& out) {
        if (failed_) return false;
        std::uint64_t v = 0;
        for (int i = 0; i < 10; i++) {
            if (pos_ >= size_) {
                fail(static_cast<std::uint32_t>(pos_));
                return false;
            }
            const std::uint8_t b = s_[pos_++];
            v |= static_cast<std::uint64_t>(b & 0x7F) << (7 * i);
            if ((b & 0x80) == 0) {
                out = v;
                return true;
            }
        }
        fail(static_cast<std::uint32_t>(pos_ - 1)); /* > 10 bytes */
        return false;
    }

    const std::uint8_t* raw_ptr() const noexcept { return s_ + pos_; }

    void skip(std::uint64_t n) noexcept { pos_ += static_cast<std::size_t>(n); }

    /* Inline (0x00) or dictionary reference (0x01) string. */
    bool take_string(std::vector<std::string>& dict, std::string& out) {
        std::uint8_t flag;
        if (!take_u8(flag)) return false;
        if (flag == 0x01) {
            std::uint64_t idx;
            if (!take_varint(idx)) return false;
            if (idx >= dict.size()) {
                fail(static_cast<std::uint32_t>(pos_));
                return false;
            }
            out = dict[static_cast<std::size_t>(idx)];
            return true;
        }
        if (flag != 0x00) {
            fail(static_cast<std::uint32_t>(pos_ - 1));
            return false;
        }
        std::uint64_t len;
        if (!take_varint(len)) return false;
        if (pos_ > size_ || len > static_cast<std::uint64_t>(size_ - pos_)) {
            fail(static_cast<std::uint32_t>(pos_));
            return false;
        }
        std::string str(reinterpret_cast<const char*>(s_ + pos_),
                        static_cast<std::size_t>(len));
        pos_ += static_cast<std::size_t>(len);
        dict.push_back(std::move(str));
        out = dict.back();
        return true;
    }

private:
    const std::uint8_t* s_;
    std::size_t size_;
    std::size_t pos_ = 0;
    bool failed_ = false;
    core::error_code err_;
};

} /* namespace */

std::span<std::byte> encode_message(const Message& m, core::Arena& arena) {
    EncodeBuf buf(arena);
    StringInterner interner(16);

    buf.put_u8(static_cast<std::uint8_t>(kCodecVersion));
    interner.write(buf, m.id);
    interner.write(buf, m.session_id);
    buf.put_u8(static_cast<std::uint8_t>(m.role));
    interner.write(buf, m.model);
    buf.put_varint(m.created_at);
    buf.put_varint(m.parts.size());

    for (const Part& p : m.parts) {
        buf.put_u8(static_cast<std::uint8_t>(part_kind(p)));
        switch (part_kind(p)) {
            case PartKind::text:
                interner.write(buf, as<Text>(p)->content);
                break;
            case PartKind::reasoning:
                interner.write(buf, as<Reasoning>(p)->content);
                break;
            case PartKind::image_url:
                interner.write(buf, as<ImageUrl>(p)->url);
                break;
            case PartKind::binary: {
                const Binary& b = *as<Binary>(p);
                interner.write(buf, b.mime);
                buf.put_varint(b.data.size());
                buf.put_bytes(b.data.data(), b.data.size());
                break;
            }
            case PartKind::tool_call: {
                const ToolCall& t = *as<ToolCall>(p);
                interner.write(buf, t.id);
                interner.write(buf, t.name);
                interner.write(buf, t.input_json);
                buf.put_u8(t.finished ? 1 : 0);
                break;
            }
            case PartKind::tool_result: {
                const ToolResult& r = *as<ToolResult>(p);
                interner.write(buf, r.call_id);
                interner.write(buf, r.content);
                buf.put_u8(r.is_error ? 1 : 0);
                break;
            }
            case PartKind::finish:
                buf.put_u8(static_cast<std::uint8_t>(as<Finish>(p)->reason));
                break;
        }
    }

    return buf.view();
}

core::error_code decode_message(std::span<const std::byte> s, Message& out) {
    Decoder d(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());

    std::uint8_t version;
    if (!d.take_u8(version)) return d.err();
    if (version != kCodecVersion) {
        return core::make_error_code(core::Err::e_proto_parse,
                                     kCodecDetailVersionMismatch);
    }

    Message m;
    std::vector<std::string> dict;
    dict.reserve(16);

    if (!d.take_string(dict, m.id)) return d.err();
    if (!d.take_string(dict, m.session_id)) return d.err();

    std::uint8_t role;
    if (!d.take_u8(role)) return d.err();
    if (role > static_cast<std::uint8_t>(Role::tool)) {
        d.fail(static_cast<std::uint32_t>(d.pos()));
        return d.err();
    }
    m.role = static_cast<Role>(role);

    if (!d.take_string(dict, m.model)) return d.err();

    std::uint64_t created;
    if (!d.take_varint(created)) return d.err();
    m.created_at = created;

    std::uint64_t part_count;
    if (!d.take_varint(part_count)) return d.err();
    if (part_count > kCodecMaxParts) {
        d.fail(static_cast<std::uint32_t>(d.pos()));
        return d.err();
    }
    /* Reserve a hint only: part_count is legal to be large; do not let a
     * hostile value trigger a huge allocation before we validate the body. */
    m.parts.reserve(static_cast<std::size_t>(
        std::min<std::uint64_t>(part_count, 512)));

    for (std::uint64_t i = 0; i < part_count; i++) {
        std::uint8_t kind;
        if (!d.take_u8(kind)) return d.err();
        if (kind > static_cast<std::uint8_t>(PartKind::finish)) {
            d.fail(static_cast<std::uint32_t>(d.pos()));
            return d.err();
        }
        switch (static_cast<PartKind>(kind)) {
            case PartKind::text: {
                Text t;
                if (!d.take_string(dict, t.content)) return d.err();
                m.parts.emplace_back(std::move(t));
                break;
            }
            case PartKind::reasoning: {
                Reasoning r;
                if (!d.take_string(dict, r.content)) return d.err();
                m.parts.emplace_back(std::move(r));
                break;
            }
            case PartKind::image_url: {
                ImageUrl i;
                if (!d.take_string(dict, i.url)) return d.err();
                m.parts.emplace_back(std::move(i));
                break;
            }
            case PartKind::binary: {
                Binary b;
                if (!d.take_string(dict, b.mime)) return d.err();
                std::uint64_t len;
                if (!d.take_varint(len)) return d.err();
                if (static_cast<std::uint64_t>(d.remaining()) < len) {
                    d.fail(static_cast<std::uint32_t>(d.pos()));
                    return d.err();
                }
                b.data.assign(d.raw_ptr(),
                              d.raw_ptr() + static_cast<std::size_t>(len));
                d.skip(len);
                m.parts.emplace_back(std::move(b));
                break;
            }
            case PartKind::tool_call: {
                ToolCall t;
                if (!d.take_string(dict, t.id)) return d.err();
                if (!d.take_string(dict, t.name)) return d.err();
                if (!d.take_string(dict, t.input_json)) return d.err();
                std::uint8_t fin;
                if (!d.take_u8(fin)) return d.err();
                if (fin > 1) {
                    d.fail(static_cast<std::uint32_t>(d.pos()));
                    return d.err();
                }
                t.finished = fin != 0;
                m.parts.emplace_back(std::move(t));
                break;
            }
            case PartKind::tool_result: {
                ToolResult r;
                if (!d.take_string(dict, r.call_id)) return d.err();
                if (!d.take_string(dict, r.content)) return d.err();
                std::uint8_t is_err;
                if (!d.take_u8(is_err)) return d.err();
                if (is_err > 1) {
                    d.fail(static_cast<std::uint32_t>(d.pos()));
                    return d.err();
                }
                r.is_error = is_err != 0;
                m.parts.emplace_back(std::move(r));
                break;
            }
            case PartKind::finish: {
                std::uint8_t reason;
                if (!d.take_u8(reason)) return d.err();
                if (reason > static_cast<std::uint8_t>(FinishReason::unknown)) {
                    d.fail(static_cast<std::uint32_t>(d.pos()));
                    return d.err();
                }
                Finish f;
                f.reason = static_cast<FinishReason>(reason);
                m.parts.emplace_back(std::move(f));
                break;
            }
        }
    }

    out = std::move(m);
    return core::ok();
}

} /* namespace opencode::msg */

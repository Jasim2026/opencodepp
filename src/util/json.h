/*
 * json.h -- minimal JSON DOM: parse + write, no external library. Enough for
 * config, provider request/response framing, and structured tool results.
 *
 * Lifetime: parsed strings reference the input buffer (zero-copy) UNLESS the
 * string contained escapes, in which case the decoded form is stored in
 * JVal::owned and `str` points into it. Parsing errors are core::error_code
 * (category proto_parse) optionally with the byte position.
 */
#ifndef OPENCODE_UTIL_JSON_H
#define OPENCODE_UTIL_JSON_H

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/error.h"

namespace opencode::util {

struct JVal {
    enum class Kind : uint8_t { null, boolean, number, string, array, object };

    Kind kind = Kind::null;
    bool b = false;
    double num = 0;
    std::string_view str;
    std::string owned; /* decoded form of an escaped string, if any */
    std::vector<JVal> arr;
    std::vector<std::pair<std::string_view, JVal>> obj;

    JVal() = default;
    /* Copy/move re-point `str` into the new `owned` buffer when it referenced
     * the source's owned storage (std::string moves are not address-stable
     * under SSO, so plain defaulted moves would dangle). */
    JVal(const JVal& o) : kind(o.kind), b(o.b), num(o.num), str(o.str),
                          owned(o.owned), arr(o.arr), obj(o.obj) {
        repoint_str(o);
    }
    JVal& operator=(const JVal& o) {
        if (this != &o) {
            kind = o.kind; b = o.b; num = o.num;
            owned = o.owned; str = o.str;
            arr = o.arr; obj = o.obj;
            repoint_str(o);
        }
        return *this;
    }
    JVal(JVal&& o) noexcept {
        const std::string_view old_owned = o.owned; /* capture before move */
        kind = o.kind; b = o.b; num = o.num;
        owned = std::move(o.owned); str = o.str;
        arr = std::move(o.arr); obj = std::move(o.obj);
        repoint_str(o, old_owned);
    }
    JVal& operator=(JVal&& o) noexcept {
        if (this != &o) {
            const std::string_view old_owned = o.owned;
            kind = o.kind; b = o.b; num = o.num;
            owned = std::move(o.owned); str = o.str;
            arr = std::move(o.arr); obj = std::move(o.obj);
            repoint_str(o, old_owned);
        }
        return *this;
    }

    static JVal Null() { return {}; }
    static JVal Bool(bool v) {
        JVal j;
        j.kind = Kind::boolean;
        j.b = v;
        return j;
    }
    static JVal Num(double v) {
        JVal j;
        j.kind = Kind::number;
        j.num = v;
        return j;
    }
    static JVal Str(std::string_view v) {
        JVal j;
        j.kind = Kind::string;
        j.str = v;
        return j;
    }
    static JVal Array(std::vector<JVal> v) {
        JVal j;
        j.kind = Kind::array;
        j.arr = std::move(v);
        return j;
    }
    static JVal Object(std::vector<std::pair<std::string_view, JVal>> v) {
        JVal j;
        j.kind = Kind::object;
        j.obj = std::move(v);
        return j;
    }

    const JVal* find(std::string_view key) const {
        if (kind != Kind::object) return nullptr;
        for (const auto& [k, v] : obj) {
            if (k == key) return &v;
        }
        return nullptr;
    }

private:
    /* If `str` pointed into the source's owned buffer, re-point it into our
     * owned buffer (which now holds a copy of those bytes). */
    void repoint_str(const JVal& src) noexcept {
        const std::string_view so = src.owned;
        if (!so.empty() && str.data() >= so.data() &&
            str.data() < so.data() + so.size()) {
            const size_t off = static_cast<size_t>(str.data() - so.data());
            str = std::string_view(owned).substr(off);
        }
    }
    /* Move flavor: the source's owned buffer is already moved-from, so the
     * old range is passed explicitly. */
    void repoint_str(const JVal&, std::string_view old_owned) noexcept {
        if (!old_owned.empty() && str.data() >= old_owned.data() &&
            str.data() < old_owned.data() + old_owned.size()) {
            const size_t off =
                static_cast<size_t>(str.data() - old_owned.data());
            str = std::string_view(owned).substr(off);
        }
    }
};

/* Parse in (must outlive the result unless values are copied). On success
 * returns ok(); on failure returns e_proto_parse and sets *pos to the byte
 * offset (when pos != nullptr). */
core::error_code parse_json(std::string_view in, JVal& out,
                            size_t* pos = nullptr);

/* Serialize v. The writer is strict (no NaN/Infinity). */
std::string to_json(const JVal& v);

} /* namespace opencode::util */

#endif /* OPENCODE_UTIL_JSON_H */

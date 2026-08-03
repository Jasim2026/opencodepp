#include "util/json.h"

#include <charconv>
#include <cstdio>
#include <cstring>

namespace opencode::util {

namespace {

struct Parser {
    std::string_view in;
    size_t pos = 0;

    bool at_end() const { return pos >= in.size(); }
    char peek() const { return in[pos]; }
    bool eat(char c) {
        if (!at_end() && in[pos] == c) {
            ++pos;
            return true;
        }
        return false;
    }
    void skip_ws() {
        while (!at_end() &&
               (in[pos] == ' ' || in[pos] == '\t' || in[pos] == '\n' ||
                in[pos] == '\r')) {
            ++pos;
        }
    }

    core::error_code fail(size_t* out_pos) {
        if (out_pos != nullptr) *out_pos = pos;
        return core::make_error_code(core::Err::e_proto_parse, 1);
    }

    core::error_code parse_string(std::string_view& out, std::string& owned,
                                  size_t* out_pos) {
        if (!eat('"')) return fail(out_pos);
        const size_t start = pos;
        /* fast path: plain string, no escapes */
        while (!at_end() && in[pos] != '"' && in[pos] != '\\' &&
               static_cast<unsigned char>(in[pos]) >= 0x20) {
            ++pos;
        }
        if (!at_end() && in[pos] == '"') {
            out = in.substr(start, pos - start);
            ++pos;
            return core::ok();
        }
        /* slow path: rebuild with escapes */
        pos = start;
        owned.clear();
        while (!at_end()) {
            char c = in[pos++];
            if (c == '"') {
                out = owned;
                return core::ok();
            }
            if (c == '\\') {
                if (at_end()) return fail(out_pos);
                char e = in[pos++];
                switch (e) {
                    case '"': owned.push_back('"'); break;
                    case '\\': owned.push_back('\\'); break;
                    case '/': owned.push_back('/'); break;
                    case 'b': owned.push_back('\b'); break;
                    case 'f': owned.push_back('\f'); break;
                    case 'n': owned.push_back('\n'); break;
                    case 'r': owned.push_back('\r'); break;
                    case 't': owned.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp = 0;
                        if (pos + 4 > in.size()) return fail(out_pos);
                        for (int i = 0; i < 4; ++i) {
                            char h = in[pos++];
                            int v;
                            if (h >= '0' && h <= '9')
                                v = h - '0';
                            else if (h >= 'a' && h <= 'f')
                                v = h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F')
                                v = h - 'A' + 10;
                            else
                                return fail(out_pos);
                            cp = (cp << 4) | uint32_t(v);
                        }
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            /* combine surrogate pair */
                            if (pos + 6 > in.size() || in[pos] != '\\' ||
                                in[pos + 1] != 'u') {
                                return fail(out_pos);
                            }
                            pos += 2;
                            uint32_t lo = 0;
                            for (int i = 0; i < 4; ++i) {
                                char h = in[pos++];
                                int v;
                                if (h >= '0' && h <= '9')
                                    v = h - '0';
                                else if (h >= 'a' && h <= 'f')
                                    v = h - 'a' + 10;
                                else if (h >= 'A' && h <= 'F')
                                    v = h - 'A' + 10;
                                else
                                    return fail(out_pos);
                                lo = (lo << 4) | uint32_t(v);
                            }
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        /* UTF-8 encode */
                        if (cp < 0x80) {
                            owned.push_back(static_cast<char>(cp));
                        } else if (cp < 0x800) {
                            owned.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                            owned.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else if (cp < 0x10000) {
                            owned.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                            owned.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            owned.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else {
                            owned.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                            owned.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                            owned.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            owned.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                    default: return fail(out_pos);
                }
            } else {
                if (static_cast<unsigned char>(c) < 0x20) return fail(out_pos);
                owned.push_back(c);
            }
        }
        return fail(out_pos); /* unterminated */
    }

    core::error_code parse_value(JVal& v, size_t* out_pos) {
        skip_ws();
        if (at_end()) return fail(out_pos);
        char c = peek();
        switch (c) {
            case '{': {
                ++pos;
                std::vector<std::pair<std::string_view, JVal>> obj;
                skip_ws();
                if (eat('}')) {
                    v = JVal::Object(std::move(obj));
                    return core::ok();
                }
                for (;;) {
                    skip_ws();
                    std::string_view key;
                    std::string owned;
                    core::error_code e = parse_string(key, owned, out_pos);
                    if (e) return e;
                    /* Object keys are stored as zero-copy views into the input;
                     * escaped keys cannot be represented safely, so reject. */
                    if (!owned.empty()) return fail(out_pos);
                    skip_ws();
                    if (!eat(':')) return fail(out_pos);
                    JVal val;
                    e = parse_value(val, out_pos);
                    if (e) return e;
                    obj.emplace_back(key, std::move(val));
                    skip_ws();
                    if (eat('}')) break;
                    if (!eat(',')) return fail(out_pos);
                }
                v = JVal::Object(std::move(obj));
                return core::ok();
            }
            case '[': {
                ++pos;
                std::vector<JVal> arr;
                skip_ws();
                if (eat(']')) {
                    v = JVal::Array(std::move(arr));
                    return core::ok();
                }
                for (;;) {
                    JVal val;
                    core::error_code e = parse_value(val, out_pos);
                    if (e) return e;
                    arr.push_back(std::move(val));
                    skip_ws();
                    if (eat(']')) break;
                    if (!eat(',')) return fail(out_pos);
                }
                v = JVal::Array(std::move(arr));
                return core::ok();
            }
            case '"': {
                std::string_view s;
                std::string owned;
                core::error_code e = parse_string(s, owned, out_pos);
                if (e) return e;
                v = JVal::Str(s);
                v.owned = std::move(owned);
                if (!v.owned.empty()) v.str = v.owned;
                return core::ok();
            }
            case 't': {
                if (in.substr(pos, 4) == "true") {
                    pos += 4;
                    v = JVal::Bool(true);
                    return core::ok();
                }
                return fail(out_pos);
            }
            case 'f': {
                if (in.substr(pos, 5) == "false") {
                    pos += 5;
                    v = JVal::Bool(false);
                    return core::ok();
                }
                return fail(out_pos);
            }
            case 'n': {
                if (in.substr(pos, 4) == "null") {
                    pos += 4;
                    v = JVal::Null();
                    return core::ok();
                }
                return fail(out_pos);
            }
            default: {
                /* number: -?digits[.digits][e[+-]digits] */
                const size_t start = pos;
                eat('-');
                if (at_end() || peek() < '0' || peek() > '9') return fail(out_pos);
                while (!at_end() && peek() >= '0' && peek() <= '9') ++pos;
                if (!at_end() && peek() == '.') {
                    ++pos;
                    if (at_end() || peek() < '0' || peek() > '9') return fail(out_pos);
                    while (!at_end() && peek() >= '0' && peek() <= '9') ++pos;
                }
                if (!at_end() && (peek() == 'e' || peek() == 'E')) {
                    ++pos;
                    if (!at_end() && (peek() == '+' || peek() == '-')) ++pos;
                    if (at_end() || peek() < '0' || peek() > '9') return fail(out_pos);
                    while (!at_end() && peek() >= '0' && peek() <= '9') ++pos;
                }
                double d = 0;
                std::from_chars(in.data() + start, in.data() + pos, d);
                v = JVal::Num(d);
                return core::ok();
            }
        }
    }
};

/* ---------- writer ---------- */

void write_escaped(std::string& out, std::string_view s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void write_value(std::string& out, const JVal& v) {
    switch (v.kind) {
        case JVal::Kind::null: out += "null"; break;
        case JVal::Kind::boolean: out += v.b ? "true" : "false"; break;
        case JVal::Kind::number: {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.17g", v.num);
            out += buf;
            break;
        }
        case JVal::Kind::string: write_escaped(out, v.str); break;
        case JVal::Kind::array: {
            out.push_back('[');
            for (size_t i = 0; i < v.arr.size(); ++i) {
                if (i) out.push_back(',');
                write_value(out, v.arr[i]);
            }
            out.push_back(']');
            break;
        }
        case JVal::Kind::object: {
            out.push_back('{');
            for (size_t i = 0; i < v.obj.size(); ++i) {
                if (i) out.push_back(',');
                write_escaped(out, v.obj[i].first);
                out.push_back(':');
                write_value(out, v.obj[i].second);
            }
            out.push_back('}');
            break;
        }
    }
}

} /* namespace */

core::error_code parse_json(std::string_view in, JVal& out, size_t* pos) {
    Parser p{in};
    core::error_code e = p.parse_value(out, pos);
    if (e) return e;
    p.skip_ws();
    if (!p.at_end()) return p.fail(pos);
    return core::ok();
}

std::string to_json(const JVal& v) {
    std::string out;
    write_value(out, v);
    return out;
}

} /* namespace opencode::util */

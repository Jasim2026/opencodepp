/*
 * message.cpp -- Message conveniences and JSON interop (wire / debug logs).
 */
#include "msg/message.h"

#include <cstring>

#include "util/base64.h"

namespace opencode::msg {

std::string Message::content_text() const {
    std::string out;
    for (const Part& p : parts) {
        if (const Text* t = as<Text>(p)) {
            out += t->content;
        }
    }
    return out;
}

std::vector<const ToolCall*> Message::tool_calls() const {
    std::vector<const ToolCall*> out;
    for (const Part& p : parts) {
        if (const ToolCall* t = as<ToolCall>(p)) out.push_back(t);
    }
    return out;
}

std::vector<const ToolResult*> Message::tool_results() const {
    std::vector<const ToolResult*> out;
    for (const Part& p : parts) {
        if (const ToolResult* t = as<ToolResult>(p)) out.push_back(t);
    }
    return out;
}

bool Message::is_finished() const noexcept {
    for (const Part& p : parts) {
        if (holds<Finish>(p)) return true;
    }
    return false;
}

FinishReason Message::finish_reason() const noexcept {
    for (const Part& p : parts) {
        if (const Finish* f = as<Finish>(p)) return f->reason;
    }
    return FinishReason::unknown;
}

namespace {

/* JSON helpers. JVal stores string_views; strings synthesized here (e.g.
 * base64) must be kept in the JVal's owned buffer. */
util::JVal owned_str(std::string s) {
    util::JVal j;
    j.kind = util::JVal::Kind::string;
    j.owned = std::move(s);
    j.str = j.owned;
    return j;
}

std::string_view js(const util::JVal& v, std::string_view key,
                    std::string_view fallback = {}) {
    const util::JVal* f = v.find(key);
    if (f && f->kind == util::JVal::Kind::string) return f->str;
    return fallback;
}

} /* namespace */

util::JVal to_json(const Message& m) {
    std::vector<std::pair<std::string_view, util::JVal>> fields;
    fields.reserve(6 + m.parts.size());

    auto put = [&fields](std::string_view k, util::JVal v) {
        fields.emplace_back(k, std::move(v));
    };

    put("id", util::JVal::Str(m.id));
    put("session_id", util::JVal::Str(m.session_id));
    put("role", util::JVal::Str(to_string(m.role)));
    put("model", util::JVal::Str(m.model));
    put("created_at", util::JVal::Num(static_cast<double>(m.created_at)));

    std::vector<util::JVal> parts;
    parts.reserve(m.parts.size());
    for (const Part& p : m.parts) {
        std::vector<std::pair<std::string_view, util::JVal>> pf;
        auto pk = [&pf](std::string_view k, util::JVal v) {
            pf.emplace_back(k, std::move(v));
        };
        pk("kind", util::JVal::Str(to_string(part_kind(p))));
        switch (part_kind(p)) {
            case PartKind::text: {
                const Text& t = *as<Text>(p);
                pk("content", util::JVal::Str(t.content));
                break;
            }
            case PartKind::reasoning: {
                const Reasoning& r = *as<Reasoning>(p);
                pk("content", util::JVal::Str(r.content));
                break;
            }
            case PartKind::image_url: {
                const ImageUrl& i = *as<ImageUrl>(p);
                pk("url", util::JVal::Str(i.url));
                break;
            }
            case PartKind::binary: {
                const Binary& b = *as<Binary>(p);
                pk("mime", util::JVal::Str(b.mime));
                std::string b64;
                b64.resize(util::b64_encoded_size(b.data.size()));
                if (b.data.empty()) {
                    b64.clear();
                } else {
                    const size_t n =
                        util::b64_encode(b.data.data(), b.data.size(), b64.data());
                    b64.resize(n);
                }
                pk("data", owned_str(std::move(b64)));
                break;
            }
            case PartKind::tool_call: {
                const ToolCall& t = *as<ToolCall>(p);
                pk("id", util::JVal::Str(t.id));
                pk("name", util::JVal::Str(t.name));
                pk("input_json", util::JVal::Str(t.input_json));
                pk("finished", util::JVal::Bool(t.finished));
                break;
            }
            case PartKind::tool_result: {
                const ToolResult& r = *as<ToolResult>(p);
                pk("call_id", util::JVal::Str(r.call_id));
                pk("content", util::JVal::Str(r.content));
                pk("is_error", util::JVal::Bool(r.is_error));
                break;
            }
            case PartKind::finish: {
                const Finish& f = *as<Finish>(p);
                pk("reason", util::JVal::Str(to_string(f.reason)));
                break;
            }
        }
        parts.emplace_back(util::JVal::Object(std::move(pf)));
    }
    put("parts", util::JVal::Array(std::move(parts)));

    return util::JVal::Object(std::move(fields));
}

core::error_code from_json(const util::JVal& v, Message& m) {
    using core::Err;
    if (v.kind != util::JVal::Kind::object) {
        return core::make_error_code(Err::e_proto_parse, 1);
    }
    if (const util::JVal* r = v.find("role"); r) {
        if (r->kind != util::JVal::Kind::string) {
            return core::make_error_code(Err::e_proto_parse, 2);
        }
        std::optional<Role> role = from_string<Role>(r->str);
        if (!role) return core::make_error_code(Err::e_proto_parse, 3);
        m.role = *role;
    }
    m.id = std::string(js(v, "id"));
    m.session_id = std::string(js(v, "session_id"));
    m.model = std::string(js(v, "model"));

    if (const util::JVal* t = v.find("created_at");
        t && t->kind == util::JVal::Kind::number) {
        m.created_at = static_cast<std::uint64_t>(t->num);
    }

    const util::JVal* parts = v.find("parts");
    if (!parts || parts->kind != util::JVal::Kind::array) {
        return core::make_error_code(Err::e_proto_parse, 4);
    }
    m.parts.clear();
    m.parts.reserve(parts->arr.size());
    for (const util::JVal& pj : parts->arr) {
        if (pj.kind != util::JVal::Kind::object) {
            return core::make_error_code(Err::e_proto_parse, 5);
        }
        const std::string_view kind = js(pj, "kind");
        if (kind == "text") {
            m.parts.emplace_back(Text{std::string(js(pj, "content"))});
        } else if (kind == "reasoning") {
            m.parts.emplace_back(Reasoning{std::string(js(pj, "content"))});
        } else if (kind == "image_url") {
            m.parts.emplace_back(ImageUrl{std::string(js(pj, "url"))});
        } else if (kind == "tool_call") {
            ToolCall tc;
            tc.id = std::string(js(pj, "id"));
            tc.name = std::string(js(pj, "name"));
            tc.input_json = std::string(js(pj, "input_json"));
            if (const util::JVal* f = pj.find("finished");
                f && f->kind == util::JVal::Kind::boolean) {
                tc.finished = f->b;
            }
            m.parts.emplace_back(std::move(tc));
        } else if (kind == "tool_result") {
            ToolResult tr;
            tr.call_id = std::string(js(pj, "call_id"));
            tr.content = std::string(js(pj, "content"));
            if (const util::JVal* f = pj.find("is_error");
                f && f->kind == util::JVal::Kind::boolean) {
                tr.is_error = f->b;
            }
            m.parts.emplace_back(std::move(tr));
        } else if (kind == "finish") {
            FinishReason reason = FinishReason::unknown;
            if (const std::optional<FinishReason> r =
                    from_string<FinishReason>(js(pj, "reason"));
                r) {
                reason = *r;
            }
            m.parts.emplace_back(Finish{reason});
        } else if (kind == "binary") {
            Binary b;
            b.mime = std::string(js(pj, "mime"));
            const std::string_view data = js(pj, "data");
            if (!data.empty()) {
                b.data.resize(util::b64_decoded_size(data.size()) + 1);
                size_t n = 0;
                const core::error_code ec = util::b64_decode(
                    data, b.data.data(), b.data.size(), n);
                if (!ec.ok()) return ec;
                b.data.resize(n);
            }
            m.parts.emplace_back(std::move(b));
        } else {
            return core::make_error_code(Err::e_proto_parse, 6);
        }
    }
    return core::ok();
}

} /* namespace opencode::msg */

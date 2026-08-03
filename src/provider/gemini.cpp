/*
 * gemini.cpp -- Google Gemini generateContent adapter (native tool calling).
 *
 * Endpoint: POST <base>/models/{model}:streamGenerateContent?alt=json&key=...
 * (JSONL streaming -- one JSON object per line, reusing net::SseParser in
 * jsonl mode). Maps our msg model to `contents` with `parts`, native
 * functionCall / functionResponse tool calling, and system_instruction.
 * Parses each JSONL line into the normalised StreamEvent set. Never throws.
 */
#include "provider/provider.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "msg/part.h"
#include "util/base64.h"

namespace opencode::provider {
namespace {

using namespace opencode::core;
using namespace opencode::msg;
using opencode::util::JVal;

double get_num(const JVal& o, std::string_view key, double dflt = 0) noexcept {
    if (const JVal* v = o.find(key)) {
        if (v->kind == JVal::Kind::number) return v->num;
    }
    return dflt;
}
std::string_view get_str(const JVal& o, std::string_view key) noexcept {
    if (const JVal* v = o.find(key)) {
        if (v->kind == JVal::Kind::string) return v->str;
    }
    return {};
}
const JVal* get_obj(const JVal& o, std::string_view key) noexcept {
    const JVal* v = o.find(key);
    return (v != nullptr && v->kind == JVal::Kind::object) ? v : nullptr;
}
const JVal* get_arr(const JVal& o, std::string_view key) noexcept {
    const JVal* v = o.find(key);
    return (v != nullptr && v->kind == JVal::Kind::array) ? v : nullptr;
}
bool get_bool(const JVal& o, std::string_view key) noexcept {
    if (const JVal* v = o.find(key)) {
        if (v->kind == JVal::Kind::boolean) return v->b;
    }
    return false;
}

/* JVal is zero-copy; dynamically-built strings must be owned by the JVal. */
JVal owned_str(std::string s) {
    JVal j;
    j.kind = JVal::Kind::string;
    j.owned = std::move(s);
    j.str = j.owned;
    return j;
}

error_code parse_owned(std::string_view text, JVal& out) {
    JVal tmp;
    if (const error_code ec = parse_json(text, tmp); !ec.ok()) return ec;
    out = tmp;
    return ok();
}

error_code args_value(std::string_view input_json, JVal& out) {
    if (input_json.empty()) {
        out = JVal::Object({});
        return ok();
    }
    return parse_owned(input_json, out);
}

FinishReason map_finish(std::string_view r) noexcept {
    if (r == "STOP") return FinishReason::end_turn;
    if (r == "MAX_TOKENS") return FinishReason::max_tokens;
    if (r == "SAFETY" || r == "RECITATION" || r == "BLOCKLIST")
        return FinishReason::error;
    return FinishReason::unknown;
}

JVal build_user_parts(const Message& m,
                      const std::map<std::string, std::string>& call_names) {
    std::vector<JVal> parts;
    for (const Part& p : m.parts) {
        if (const Text* t = as<Text>(p)) {
            parts.push_back(
                JVal::Object({{"text", JVal::Str(t->content)}}));
        } else if (const ImageUrl* img = as<ImageUrl>(p)) {
            parts.push_back(JVal::Object(
                {{"fileData", JVal::Object({{"fileUri", JVal::Str(img->url)}})}}));
        } else if (const Binary* bin = as<Binary>(p)) {
            std::string b64;
            b64.resize(util::b64_encoded_size(bin->data.size()));
            const size_t n = util::b64_encode(bin->data.data(), bin->data.size(),
                                              b64.data());
            b64.resize(n);
            parts.push_back(JVal::Object(
                {{"inlineData",
                  JVal::Object({{"mimeType", JVal::Str(bin->mime)},
                                {"data", owned_str(std::move(b64))}})}}));
        } else if (const ToolResult* r = as<ToolResult>(p)) {
            const auto it = call_names.find(r->call_id);
            if (it != call_names.end()) {
                parts.push_back(JVal::Object(
                    {{"functionResponse",
                      JVal::Object(
                          {{"name", JVal::Str(it->second)},
                           {"response",
                            JVal::Object(
                                {{"content", JVal::Str(r->content)},
                                 {"is_error", JVal::Bool(r->is_error)}})}})}}));
            } else {
                parts.push_back(JVal::Object(
                    {{"text", owned_str("[tool_result " + r->call_id + "] " +
                                       r->content)}}));
            }
        }
        /* reasoning/finish parts in a user message are ignored */
    }
    return JVal::Array(std::move(parts));
}

JVal build_model_parts(const Message& m) {
    std::vector<JVal> parts;
    for (const Part& p : m.parts) {
        if (const Text* t = as<Text>(p)) {
            parts.push_back(JVal::Object({{"text", JVal::Str(t->content)}}));
        } else if (const Reasoning* r = as<Reasoning>(p)) {
            parts.push_back(JVal::Object(
                {{"text", JVal::Str(r->content)}, {"thought", JVal::Bool(true)}}));
        } else if (const ToolCall* c = as<ToolCall>(p)) {
            JVal args;
            if (args_value(c->input_json, args).ok()) {
                parts.push_back(JVal::Object(
                    {{"functionCall",
                      JVal::Object({{"name", JVal::Str(c->name)},
                                    {"args", std::move(args)}})}}));
            }
        }
        /* tool_result / finish in an assistant message are ignored */
    }
    return JVal::Array(std::move(parts));
}

class GeminiProvider final : public Provider {
public:
    GeminiProvider(std::string base_url, std::string api_key,
                   uint32_t default_max_tokens)
        : base_url_(std::move(base_url)),
          api_key_(std::move(api_key)),
          default_max_tokens_(default_max_tokens) {
    }

    error_code build_request(const MsgList& msgs, const ToolsSpec& tools,
                             const ModelSpec& model, const Budget& budget,
                             RequestBytes& out) override {
        UrlParts url;
        if (const error_code ec = split_url(base_url_, url); !ec.ok())
            return ec;

        std::string system_text;
        std::vector<JVal> contents;
        std::map<std::string, std::string> call_names;
        for (const Message& m : msgs) {
            if (m.role == Role::system) {
                system_text += m.content_text();
                continue;
            }
            if (m.role == Role::assistant) {
                for (const Part& p : m.parts)
                    if (const ToolCall* c = as<ToolCall>(p))
                        call_names.emplace(c->id, c->name);
                JVal parts = build_model_parts(m);
                if (parts.arr.empty()) continue;
                contents.push_back(JVal::Object(
                    {{"role", JVal::Str("model")}, {"parts", std::move(parts)}}));
                continue;
            }
            if (m.role == Role::user) {
                JVal parts = build_user_parts(m, call_names);
                if (parts.arr.empty()) continue;
                contents.push_back(JVal::Object(
                    {{"role", JVal::Str("user")}, {"parts", std::move(parts)}}));
                continue;
            }
            /* Role::tool maps into user functionResponse parts (handled above) */
        }

        std::vector<std::pair<std::string_view, JVal>> root;
        root.emplace_back("contents", JVal::Array(std::move(contents)));
        if (!system_text.empty())
            root.emplace_back(
                "systemInstruction",
                JVal::Object(
                    {{"parts",
                      JVal::Array({JVal::Object(
                          {{"text", owned_str(std::move(system_text))}})})}}));
        if (!tools.empty()) {
            std::vector<JVal> decls;
            decls.reserve(tools.size());
            for (const ToolSpec& t : tools) {
                JVal params;
                if (!t.input_schema_json.empty())
                    (void)parse_owned(t.input_schema_json, params);
                if (params.kind != JVal::Kind::object) params = JVal::Object({});
                decls.push_back(JVal::Object(
                    {{"name", JVal::Str(t.name)},
                     {"description", JVal::Str(t.description)},
                     {"parameters", std::move(params)}}));
            }
            root.emplace_back(
                "tools",
                JVal::Array({JVal::Object(
                    {{"functionDeclarations", JVal::Array(std::move(decls))}})}));
        }
        const uint32_t max_tokens =
            budget.max_output_tokens
                ? budget.max_output_tokens
                : (model.default_max_tokens ? model.default_max_tokens
                                            : default_max_tokens_);
        if (max_tokens > 0)
            root.emplace_back(
                "generationConfig",
                JVal::Object(
                    {{"maxOutputTokens", JVal::Num(static_cast<double>(max_tokens))}}));

        out.method = "POST";
        out.path = url.path + "/models/" + model.api_model_name +
                   ":streamGenerateContent?alt=json&key=" + api_key_;
        out.headers = {
            {"Host", host_header(url)},
            {"Content-Type", "application/json"},
            {"Accept", "application/json"},
        };
        out.body = to_json(JVal::Object(std::move(root)));
        return ok();
    }

    error_code parse_frame(const StreamFrame& frame,
                           std::vector<StreamEvent>& out) override {
        JVal d;
        if (const error_code ec = parse_owned(frame.data, d); !ec.ok()) {
            out.push_back(ProviderError{ec, "gemini: bad frame JSON"});
            return ok();
        }

        std::string_view finish;
        const JVal* candidates = get_arr(d, "candidates");
        if (candidates != nullptr && !candidates->arr.empty()) {
            const JVal& cand = candidates->arr[0];
            finish = get_str(cand, "finishReason");
            const JVal* content = get_obj(cand, "content");
            if (const JVal* parts = content ? get_arr(*content, "parts")
                                            : nullptr) {
                for (const JVal& part : parts->arr) {
                    if (const JVal* fn = get_obj(part, "functionCall")) {
                        const std::string name = std::string(get_str(*fn, "name"));
                        const JVal* args = get_obj(*fn, "args");
                        const std::string input =
                            args ? to_json(*args) : "{}";
                        out.push_back(ToolCallDone{
                            "fc" + std::to_string(call_seq_++), name, input});
                    } else if (const std::string_view text =
                                   get_str(part, "text");
                               !text.empty()) {
                        if (get_bool(part, "thought")) {
                            out.push_back(ReasoningDelta{std::string(text)});
                        } else {
                            out.push_back(TextDelta{std::string(text)});
                        }
                    }
                    /* functionResponse parts do not occur in responses */
                }
            }
        }
        if (const JVal* um = get_obj(d, "usageMetadata")) {
            usage_.input_tokens =
                static_cast<uint64_t>(get_num(*um, "promptTokenCount"));
            usage_.output_tokens =
                static_cast<uint64_t>(get_num(*um, "candidatesTokenCount"));
            usage_.cached_input_tokens =
                static_cast<uint64_t>(get_num(*um, "cachedContentTokenCount"));
        }

        if (!finish.empty() && !done_emitted_) {
            out.push_back(MessageDone{map_finish(finish), usage_});
            done_emitted_ = true;
        }
        return ok();
    }

    error_code parse_usage(const JVal& json, Usage& out) override {
        const JVal* u = get_obj(json, "usageMetadata");
        if (u == nullptr) u = &json;
        out = {};
        out.input_tokens = static_cast<uint64_t>(get_num(*u, "promptTokenCount"));
        out.output_tokens =
            static_cast<uint64_t>(get_num(*u, "candidatesTokenCount"));
        out.cached_input_tokens =
            static_cast<uint64_t>(get_num(*u, "cachedContentTokenCount"));
        return ok();
    }

    std::string_view name() const override { return "gemini"; }
    bool supports_native_tools() const override { return true; }
    net::StreamKind stream_kind() const override { return net::StreamKind::jsonl; }
    void reset_stream() override {
        call_seq_ = 0;
        done_emitted_ = false;
        usage_ = {};
    }

private:
    static std::string host_header(const UrlParts& u) {
        std::string h = u.host;
        if (u.port != 0 && !((u.scheme == "http" && u.port == 80) ||
                             (u.scheme == "https" && u.port == 443))) {
            h += ":" + std::to_string(u.port);
        }
        return h;
    }

    std::string base_url_;
    std::string api_key_;
    uint32_t default_max_tokens_ = 0;
    uint32_t call_seq_ = 0;
    bool done_emitted_ = false;
    Usage usage_;
};

} /* namespace */

error_code make_gemini(std::string base_url, std::string api_key,
                       uint32_t default_max_tokens,
                       std::unique_ptr<Provider>& out) {
    out = std::make_unique<GeminiProvider>(std::move(base_url),
                                           std::move(api_key),
                                           default_max_tokens);
    return ok();
}

} /* namespace opencode::provider */

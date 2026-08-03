/*
 * anthropic.cpp -- Anthropic Messages API adapter (native tool calling).
 *
 * Endpoint: POST <base>/v1/messages (SSE streaming). Maps our msg model to
 * Anthropic's content blocks: system messages -> top-level `system` string,
 * user text/image/tool_result blocks, assistant text/thinking/tool_use blocks,
 * native `tools` with JSON-schema input. Parses the typed SSE event stream
 * (message_start / content_block_* / message_delta / message_stop / error)
 * into the normalised StreamEvent set. Never throws.
 */
#include "provider/provider.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "msg/part.h"
#include "util/base64.h"

namespace opencode::provider {
namespace {

using namespace opencode::core;
using namespace opencode::msg;
using opencode::util::JVal;

/* Tolerant member readers: any non-object / missing key yields the default. */
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

/* JVal is zero-copy (string_views into caller memory); dynamically-built
 * strings must be owned by the JVal itself so the wire bytes outlive the
 * build scope. */
JVal owned_str(std::string s) {
    JVal j;
    j.kind = JVal::Kind::string;
    j.owned = std::move(s);
    j.str = j.owned;
    return j;
}

/* Parse a JSON string into a JVal (owned copy so callers are not bound to the
 * source buffer's lifetime). */
error_code parse_owned(std::string_view text, JVal& out) {
    JVal tmp;
    if (const error_code ec = parse_json(text, tmp); !ec.ok()) return ec;
    out = tmp;
    return ok();
}

/* Tool arguments as a JSON object; empty string -> "{}". */
error_code tool_input(std::string_view input_json, JVal& out) {
    if (input_json.empty()) {
        out = JVal::Object({});
        return ok();
    }
    return parse_owned(input_json, out);
}

FinishReason map_stop_reason(std::string_view r) noexcept {
    if (r == "end_turn") return FinishReason::end_turn;
    if (r == "max_tokens") return FinishReason::max_tokens;
    if (r == "tool_use") return FinishReason::tool_use;
    if (r == "stop_sequence") return FinishReason::end_turn;
    return FinishReason::unknown;
}

JVal build_user_blocks(const Message& m) {
    std::vector<JVal> blocks;
    for (const Part& p : m.parts) {
        if (const Text* t = as<Text>(p)) {
            blocks.push_back(
                JVal::Object({{"type", JVal::Str("text")},
                              {"text", JVal::Str(t->content)}}));
        } else if (const ImageUrl* img = as<ImageUrl>(p)) {
            blocks.push_back(JVal::Object(
                {{"type", JVal::Str("image")},
                 {"source",
                  JVal::Object({{"type", JVal::Str("url")},
                                {"url", JVal::Str(img->url)}})}}));
        } else if (const Binary* bin = as<Binary>(p)) {
            std::string b64;
            b64.resize(util::b64_encoded_size(bin->data.size()));
            const size_t n = util::b64_encode(bin->data.data(), bin->data.size(),
                                              b64.data());
            b64.resize(n);
            blocks.push_back(JVal::Object(
                {{"type", JVal::Str("image")},
                 {"source",
                  JVal::Object({{"type", JVal::Str("base64")},
                                {"media_type", JVal::Str(bin->mime)},
                                {"data", owned_str(std::move(b64))}})}}));
        } else if (const ToolResult* r = as<ToolResult>(p)) {
            blocks.push_back(JVal::Object(
                {{"type", JVal::Str("tool_result")},
                 {"tool_use_id", JVal::Str(r->call_id)},
                 {"content", JVal::Str(r->content)},
                 {"is_error", JVal::Bool(r->is_error)}}));
        }
        /* reasoning/finish parts in a user message are ignored */
    }
    return JVal::Array(std::move(blocks));
}

JVal build_assistant_blocks(const Message& m) {
    std::vector<JVal> blocks;
    for (const Part& p : m.parts) {
        if (const Text* t = as<Text>(p)) {
            blocks.push_back(
                JVal::Object({{"type", JVal::Str("text")},
                              {"text", JVal::Str(t->content)}}));
        } else if (const Reasoning* r = as<Reasoning>(p)) {
            blocks.push_back(
                JVal::Object({{"type", JVal::Str("thinking")},
                              {"thinking", JVal::Str(r->content)}}));
        } else if (const ToolCall* c = as<ToolCall>(p)) {
            JVal input;
            if (tool_input(c->input_json, input).ok()) {
                blocks.push_back(JVal::Object(
                    {{"type", JVal::Str("tool_use")},
                     {"id", JVal::Str(c->id)},
                     {"name", JVal::Str(c->name)},
                     {"input", std::move(input)}}));
            }
        }
        /* tool_result / finish in an assistant message are ignored */
    }
    return JVal::Array(std::move(blocks));
}

class AnthropicProvider final : public Provider {
public:
    AnthropicProvider(std::string base_url, std::string api_key,
                      uint32_t default_max_tokens)
        : base_url_(std::move(base_url)),
          api_key_(std::move(api_key)),
          default_max_tokens_(default_max_tokens ? default_max_tokens : 4096) {
    }

    error_code build_request(const MsgList& msgs, const ToolsSpec& tools,
                             const ModelSpec& model, const Budget& budget,
                             RequestBytes& out) override {
        UrlParts url;
        if (const error_code ec = split_url(base_url_, url); !ec.ok())
            return ec;

        std::string system_text;
        std::vector<JVal> messages;
        for (const Message& m : msgs) {
            if (m.role == Role::system) {
                system_text += m.content_text();
                continue;
            }
            if (m.role == Role::user) {
                JVal blocks = build_user_blocks(m);
                if (blocks.arr.empty()) continue;
                messages.push_back(JVal::Object(
                    {{"role", JVal::Str("user")}, {"content", std::move(blocks)}}));
                continue;
            }
            if (m.role == Role::assistant) {
                JVal blocks = build_assistant_blocks(m);
                if (blocks.arr.empty()) continue;
                messages.push_back(JVal::Object(
                    {{"role", JVal::Str("assistant")},
                     {"content", std::move(blocks)}}));
                continue;
            }
            /* Role::tool is not a wire role for Anthropic; tool results live
             * inside user messages as tool_result blocks (handled above). */
        }

        std::vector<std::pair<std::string_view, JVal>> root;
        root.emplace_back("model", JVal::Str(model.api_model_name));
        const uint32_t max_tokens =
            budget.max_output_tokens
                ? budget.max_output_tokens
                : (model.default_max_tokens ? model.default_max_tokens
                                            : default_max_tokens_);
        root.emplace_back("max_tokens",
                          JVal::Num(static_cast<double>(max_tokens)));
        if (!system_text.empty())
            root.emplace_back("system", owned_str(std::move(system_text)));
        root.emplace_back("messages", JVal::Array(std::move(messages)));
        if (!tools.empty()) {
            std::vector<JVal> wire;
            wire.reserve(tools.size());
            for (const ToolSpec& t : tools) {
                JVal schema;
                if (!t.input_schema_json.empty())
                    (void)parse_owned(t.input_schema_json, schema);
                if (schema.kind != JVal::Kind::object) schema = JVal::Object({});
                wire.push_back(JVal::Object(
                    {{"name", JVal::Str(t.name)},
                     {"description", JVal::Str(t.description)},
                     {"input_schema", std::move(schema)}}));
            }
            root.emplace_back("tools", JVal::Array(std::move(wire)));
        }
        root.emplace_back("stream", JVal::Bool(true));

        out.method = "POST";
        out.path = url.path + "/v1/messages";
        out.headers = {
            {"Host", host_header(url)},
            {"Content-Type", "application/json"},
            {"Accept", "text/event-stream"},
            {"x-api-key", api_key_},
            {"anthropic-version", "2023-06-01"},
        };
        out.body = to_json(JVal::Object(std::move(root)));
        return ok();
    }

    error_code parse_frame(const StreamFrame& frame,
                           std::vector<StreamEvent>& out) override {
        JVal d;
        if (const error_code ec = parse_owned(frame.data, d); !ec.ok()) {
            out.push_back(ProviderError{ec, "anthropic: bad frame JSON"});
            return ok();
        }

        if (frame.event == "ping") return ok();

        if (frame.event == "message_start") {
            const JVal* msg = get_obj(d, "message");
            if (const JVal* u = msg ? get_obj(*msg, "usage") : get_obj(d, "usage"))
                accumulate_usage(*u);
            out.push_back(MessageStart{});
            return ok();
        }

        if (frame.event == "content_block_start") {
            const JVal* cb = get_obj(d, "content_block");
            if (cb != nullptr && get_str(*cb, "type") == "tool_use") {
                pending_[static_cast<size_t>(get_num(d, "index"))] =
                    PendingTool{std::string(get_str(*cb, "id")),
                                std::string(get_str(*cb, "name")), ""};
            }
            return ok();
        }

        if (frame.event == "content_block_delta") {
            const JVal* delta = get_obj(d, "delta");
            if (delta != nullptr) {
                const std::string_view type = get_str(*delta, "type");
                if (type == "text_delta") {
                    out.push_back(TextDelta{std::string(get_str(*delta, "text"))});
                } else if (type == "thinking_delta") {
                    out.push_back(
                        ReasoningDelta{std::string(get_str(*delta, "thinking"))});
                } else if (type == "input_json_delta") {
                    const size_t idx = static_cast<size_t>(get_num(d, "index"));
                    PendingTool& pt = pending_[idx];
                    const std::string_view frag = get_str(*delta, "partial_json");
                    pt.args += frag;
                    out.push_back(
                        ToolCallDelta{pt.id, pt.name, std::string(frag)});
                }
            }
            return ok();
        }

        if (frame.event == "content_block_stop") {
            const size_t idx = static_cast<size_t>(get_num(d, "index"));
            const auto it = pending_.find(idx);
            if (it != pending_.end()) {
                out.push_back(ToolCallDone{it->second.id, it->second.name,
                                           it->second.args});
                pending_.erase(it);
            }
            return ok();
        }

        if (frame.event == "message_delta") {
            const JVal* delta = get_obj(d, "delta");
            if (delta != nullptr) {
                const std::string_view stop = get_str(*delta, "stop_reason");
                if (!stop.empty()) stop_ = map_stop_reason(stop);
            }
            if (const JVal* u = get_obj(d, "usage")) accumulate_usage(*u);
            return ok();
        }

        if (frame.event == "message_stop") {
            out.push_back(MessageDone{stop_, usage_});
            return ok();
        }

        if (frame.event == "error") {
            const JVal* err = get_obj(d, "error");
            const std::string_view type = err ? get_str(*err, "type") : "";
            const std::string message =
                std::string(err ? get_str(*err, "message") : "");
            const Err code =
                (type == "authentication_error" || type == "permission_error")
                    ? Err::e_auth
                    : (type == "rate_limit_error" ? Err::e_rate_limit
                                                  : Err::e_provider_err);
            out.push_back(ProviderError{make_error_code(code), message});
            return ok();
        }

        out.push_back(ProviderError{
            make_error_code(Err::e_proto_parse),
            "anthropic: unknown event '" + std::string(frame.event) + "'"});
        return ok();
    }

    error_code parse_usage(const JVal& json, Usage& out) override {
        const JVal* u = get_obj(json, "usage");
        if (u == nullptr) u = &json;
        out = {};
        out.input_tokens = static_cast<uint64_t>(get_num(*u, "input_tokens"));
        out.output_tokens = static_cast<uint64_t>(get_num(*u, "output_tokens"));
        out.cached_input_tokens =
            static_cast<uint64_t>(get_num(*u, "cache_read_input_tokens")) +
            static_cast<uint64_t>(get_num(*u, "cache_creation_input_tokens"));
        return ok();
    }

    std::string_view name() const override { return "anthropic"; }
    bool supports_native_tools() const override { return true; }
    net::StreamKind stream_kind() const override { return net::StreamKind::sse; }
    void reset_stream() override {
        pending_.clear();
        stop_ = FinishReason::unknown;
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

    void accumulate_usage(const JVal& u) noexcept {
        if (const JVal* v = u.find("input_tokens"))
            if (v->kind == JVal::Kind::number)
                usage_.input_tokens = static_cast<uint64_t>(v->num);
        if (const JVal* v = u.find("output_tokens"))
            if (v->kind == JVal::Kind::number)
                usage_.output_tokens = static_cast<uint64_t>(v->num);
    }

    struct PendingTool {
        std::string id;
        std::string name;
        std::string args;
    };

    std::string base_url_;
    std::string api_key_;
    uint32_t default_max_tokens_ = 4096;
    std::map<size_t, PendingTool> pending_;
    FinishReason stop_ = FinishReason::unknown;
    Usage usage_;
};

} /* namespace */

error_code make_anthropic(std::string base_url, std::string api_key,
                          uint32_t default_max_tokens,
                          std::unique_ptr<Provider>& out) {
    out = std::make_unique<AnthropicProvider>(std::move(base_url),
                                              std::move(api_key),
                                              default_max_tokens);
    return ok();
}

} /* namespace opencode::provider */

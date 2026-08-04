/*
 * openai.cpp -- OpenAI Chat Completions adapter (native tool calling).
 *
 * Endpoint: POST <base>/chat/completions (SSE streaming). Maps our msg model
 * to roles system/user/assistant/tool with native `tools` + `tool_calls`.
 * Streaming parses `chat.completion.chunk` frames and stitches partial tool
 * arguments across deltas (per tool index) into complete ToolCallDone events.
 * Never throws.
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

FinishReason map_finish_reason(std::string_view r) noexcept {
    if (r == "stop") return FinishReason::end_turn;
    if (r == "length") return FinishReason::max_tokens;
    if (r == "tool_calls" || r == "function_call") return FinishReason::tool_use;
    if (r == "content_filter") return FinishReason::error;
    return FinishReason::unknown;
}

JVal build_user_content(const Message& m) {
    std::vector<const Text*> texts;
    std::vector<JVal> parts;
    bool multi = false;
    for (const Part& p : m.parts) {
        if (const Text* t = as<Text>(p)) {
            texts.push_back(t);
        } else if (const ImageUrl* img = as<ImageUrl>(p)) {
            multi = true;
            parts.push_back(JVal::Object(
                {{"type", JVal::Str("image_url")},
                 {"image_url", JVal::Object({{"url", JVal::Str(img->url)}})}}));
        } else if (const Binary* bin = as<Binary>(p)) {
            multi = true;
            std::string b64;
            b64.resize(util::b64_encoded_size(bin->data.size()));
            const size_t n = util::b64_encode(bin->data.data(), bin->data.size(),
                                              b64.data());
            b64.resize(n);
            parts.push_back(JVal::Object(
                {{"type", JVal::Str("image_url")},
                 {"image_url",
                  JVal::Object({{"url",
                                 owned_str("data:" + bin->mime + ";base64," +
                                           b64)}})}}));
        }
        /* tool_result parts are split into role:tool messages by the caller */
    }
    if (!multi) {
        std::string all;
        for (const Text* t : texts) all += t->content;
        return owned_str(std::move(all));
    }
    std::vector<JVal> arr;
    for (const Text* t : texts) {
        arr.push_back(JVal::Object(
            {{"type", JVal::Str("text")}, {"text", JVal::Str(t->content)}}));
    }
    for (JVal& p : parts) arr.push_back(std::move(p));
    return JVal::Array(std::move(arr));
}

class OpenAiProvider final : public Provider {
public:
    OpenAiProvider(std::string base_url, std::string api_key,
                   uint32_t default_max_tokens, std::string endpoint_path)
        : base_url_(std::move(base_url)),
          api_key_(std::move(api_key)),
          default_max_tokens_(default_max_tokens),
          endpoint_path_(std::move(endpoint_path)) {
        if (endpoint_path_.empty() || endpoint_path_[0] != '/')
            endpoint_path_.insert(endpoint_path_.begin(), '/');
    }

    error_code build_request(const MsgList& msgs, const ToolsSpec& tools,
                             const ModelSpec& model, const Budget& budget,
                             RequestBytes& out) override {
        UrlParts url;
        if (const error_code ec = split_url(base_url_, url); !ec.ok())
            return ec;

        std::vector<JVal> messages;
        for (const Message& m : msgs) {
            switch (m.role) {
                case Role::system: {
                    const std::string content = m.content_text();
                    if (content.empty()) continue;
                    messages.push_back(JVal::Object(
                        {{"role", JVal::Str("system")},
                         {"content", owned_str(std::move(content))}}));
                    break;
                }
                case Role::user: {
                    /* tool results become role:tool messages; the rest of the
                     * parts (text/images) become one user message. */
                    std::vector<JVal> tools;
                    for (const Part& p : m.parts) {
                        if (const ToolResult* r = as<ToolResult>(p)) {
                            tools.push_back(JVal::Object(
                                {{"role", JVal::Str("tool")},
                                 {"tool_call_id", JVal::Str(r->call_id)},
                                 {"content", JVal::Str(r->content)}}));
                        }
                    }
                    JVal content = build_user_content(m);
                    const bool empty_content =
                        (content.kind == JVal::Kind::string &&
                         content.str.empty()) ||
                        (content.kind == JVal::Kind::array && content.arr.empty());
                    if (empty_content && tools.empty()) continue;
                    if (!empty_content)
                        messages.push_back(JVal::Object(
                            {{"role", JVal::Str("user")},
                             {"content", std::move(content)}}));
                    for (JVal& t : tools) messages.push_back(std::move(t));
                    break;
                }
                case Role::assistant: {
                    std::string content;
                    std::vector<JVal> calls;
                    for (const Part& p : m.parts) {
                        if (const Text* t = as<Text>(p)) {
                            content += t->content;
                        } else if (const Reasoning* r = as<Reasoning>(p)) {
                            content += r->content;
                        } else if (const ToolCall* c = as<ToolCall>(p)) {
                            calls.push_back(JVal::Object(
                                {{"id", JVal::Str(c->id)},
                                 {"type", JVal::Str("function")},
                                 {"function",
                                  JVal::Object({{"name", JVal::Str(c->name)},
                                                {"arguments",
                                                 JVal::Str(c->input_json)}})}}));
                        }
                    }
                    if (content.empty() && calls.empty()) continue;
                    std::vector<std::pair<std::string_view, JVal>> m2;
                    m2.emplace_back("role", JVal::Str("assistant"));
                    if (!content.empty() || calls.empty())
                        m2.emplace_back("content", owned_str(std::move(content)));
                    if (!calls.empty())
                        m2.emplace_back("tool_calls", JVal::Array(std::move(calls)));
                    messages.push_back(JVal::Object(std::move(m2)));
                    break;
                }
                case Role::tool:
                    break; /* tool results are split out of user messages */
            }
        }

        std::vector<std::pair<std::string_view, JVal>> root;
        root.emplace_back("model", JVal::Str(model.api_model_name));
        root.emplace_back("messages", JVal::Array(std::move(messages)));
        if (!tools.empty()) {
            std::vector<JVal> wire;
            wire.reserve(tools.size());
            for (const ToolSpec& t : tools) {
                JVal params;
                if (!t.input_schema_json.empty())
                    (void)parse_owned(t.input_schema_json, params);
                if (params.kind != JVal::Kind::object) params = JVal::Object({});
                wire.push_back(JVal::Object(
                    {{"type", JVal::Str("function")},
                     {"function",
                      JVal::Object({{"name", JVal::Str(t.name)},
                                    {"description", JVal::Str(t.description)},
                                    {"parameters", std::move(params)}})}}));
            }
            root.emplace_back("tools", JVal::Array(std::move(wire)));
        }
        root.emplace_back("stream", JVal::Bool(true));
        root.emplace_back("stream_options",
                          JVal::Object({{"include_usage", JVal::Bool(true)}}));
        const uint32_t max_tokens =
            budget.max_output_tokens
                ? budget.max_output_tokens
                : (model.default_max_tokens ? model.default_max_tokens
                                            : default_max_tokens_);
        if (max_tokens > 0)
            root.emplace_back("max_tokens", JVal::Num(static_cast<double>(max_tokens)));

        out.method = "POST";
        out.path = url.path + endpoint_path_;
        out.headers = {
            {"Host", host_header(url)},
            {"Content-Type", "application/json"},
            {"Accept", "text/event-stream"},
            {"Authorization", "Bearer " + api_key_},
        };
        out.body = to_json(JVal::Object(std::move(root)));
        return ok();
    }

    error_code parse_frame(const StreamFrame& frame,
                           std::vector<StreamEvent>& out) override {
        JVal d;
        if (const error_code ec = parse_owned(frame.data, d); !ec.ok()) {
            out.push_back(ProviderError{ec, "openai: bad frame JSON"});
            return ok();
        }

        const JVal* err = get_obj(d, "error");
        if (err != nullptr) {
            const std::string_view type = get_str(*err, "type");
            const std::string message =
                std::string(get_str(*err, "message"));
            const Err code =
                (type == "authentication_error" ||
                 type == "invalid_api_key" || type == "insufficient_quota")
                    ? Err::e_auth
                    : (type == "rate_limit_error" ? Err::e_rate_limit
                                                  : Err::e_provider_err);
            out.push_back(ProviderError{make_error_code(code), message});
            return ok();
        }

        const JVal* choices = get_arr(d, "choices");
        if (choices != nullptr) {
            for (const JVal& c : choices->arr) {
                const JVal* delta = get_obj(c, "delta");
                if (delta != nullptr) {
                    const std::string_view content = get_str(*delta, "content");
                    if (!content.empty())
                        out.push_back(TextDelta{std::string(content)});
                    if (const JVal* tcs = get_arr(*delta, "tool_calls")) {
                        for (const JVal& tc : tcs->arr) {
                            const size_t idx =
                                static_cast<size_t>(get_num(tc, "index"));
                            PendingTool& pt = pending_[idx];
                            const std::string_view id = get_str(tc, "id");
                            if (!id.empty() && pt.id.empty()) pt.id = std::string(id);
                            if (const JVal* fn = get_obj(tc, "function")) {
                                const std::string_view name = get_str(*fn, "name");
                                if (!name.empty()) pt.name += std::string(name);
                                const std::string_view args =
                                    get_str(*fn, "arguments");
                                if (!args.empty()) {
                                    pt.args += args;
                                    out.push_back(ToolCallDelta{
                                        pt.id, pt.name, std::string(args)});
                                }
                            }
                        }
                    }
                }
                const std::string_view fr = get_str(c, "finish_reason");
                if (!fr.empty() && finish_ == FinishReason::unknown)
                    finish_ = map_finish_reason(fr);
            }
        }
        if (const JVal* u = get_obj(d, "usage")) accumulate_usage(*u);

        if (finish_ != FinishReason::unknown && !done_emitted_) {
            for (auto& [idx, pt] : pending_)
                out.push_back(ToolCallDone{pt.id, pt.name, pt.args});
            out.push_back(MessageDone{finish_, usage_});
            done_emitted_ = true;
        }
        return ok();
    }

    error_code parse_usage(const JVal& json, Usage& out) override {
        out = {};
        out.input_tokens = static_cast<uint64_t>(get_num(json, "prompt_tokens"));
        out.output_tokens =
            static_cast<uint64_t>(get_num(json, "completion_tokens"));
        if (const JVal* det = get_obj(json, "prompt_tokens_details"))
            out.cached_input_tokens =
                static_cast<uint64_t>(get_num(*det, "cached_tokens"));
        return ok();
    }

    std::string_view name() const override { return "openai"; }
    bool supports_native_tools() const override { return true; }
    net::StreamKind stream_kind() const override { return net::StreamKind::sse; }
    void reset_stream() override {
        pending_.clear();
        finish_ = FinishReason::unknown;
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

    void accumulate_usage(const JVal& u) noexcept {
        usage_.input_tokens = static_cast<uint64_t>(get_num(u, "prompt_tokens"));
        usage_.output_tokens =
            static_cast<uint64_t>(get_num(u, "completion_tokens"));
        if (const JVal* det = get_obj(u, "prompt_tokens_details"))
            usage_.cached_input_tokens =
                static_cast<uint64_t>(get_num(*det, "cached_tokens"));
    }

    struct PendingTool {
        std::string id;
        std::string name;
        std::string args;
    };

    std::string base_url_;
    std::string api_key_;
    uint32_t default_max_tokens_ = 0;
    std::string endpoint_path_ = "/chat/completions";
    std::map<size_t, PendingTool> pending_;
    FinishReason finish_ = FinishReason::unknown;
    bool done_emitted_ = false;
    Usage usage_;
};

} /* namespace */

error_code make_openai(std::string base_url, std::string api_key,
                       uint32_t default_max_tokens,
                       std::string_view endpoint_path,
                       std::unique_ptr<Provider>& out) {
    out = std::make_unique<OpenAiProvider>(
        std::move(base_url), std::move(api_key), default_max_tokens,
        endpoint_path.empty() ? "/chat/completions" : std::string(endpoint_path));
    return ok();
}

} /* namespace opencode::provider */

/*
 * provider.h -- the one Provider interface all wire formats implement.
 *
 * The agent (Phase 10) only ever talks through this vocabulary. Each adapter
 * (anthropic / openai / gemini / openai_compat) translates between our `msg`
 * model and the provider's native JSON wire format, and normalises every
 * streamed frame into the shared StreamEvent set below. Swapping providers
 * never changes agent code.
 *
 * Lifetime model: a Provider instance is owned by one session. Stream-parsing
 * adapters keep per-stream accumulation state (OpenAI partial tool-call JSON,
 * Anthropic input_json deltas, usage rollups); call reset_stream() before each
 * request. The interface never throws.
 */
#ifndef OPENCODE_PROVIDER_PROVIDER_H
#define OPENCODE_PROVIDER_PROVIDER_H

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/error.h"
#include "msg/message.h"
#include "net/http1.h"
#include "net/sse.h"
#include "util/json.h"

namespace opencode::provider {

/* A session history: ordered messages as the engine stores them. */
using MsgList = std::vector<msg::Message>;

/* One tool the agent can offer to a model (Phase 8 defines the registry; the
 * provider layer only needs the wire projection). */
struct ToolSpec {
    std::string id;                 /* stable engine id */
    std::string name;               /* wire name the model sees */
    std::string description;
    std::string input_schema_json;  /* JSON Schema of the arguments object */
};
using ToolsSpec = std::vector<ToolSpec>;

/* The resolved model description (from resolver.cpp / catalog + config). */
struct ModelSpec {
    std::string provider;           /* catalog provider id */
    std::string api_family;         /* wire family: anthropic|openai|google */
    std::string api_model_name;     /* wire model name */
    std::string base_url;           /* origin, e.g. "https://api.anthropic.com" */
    bool can_reason = false;
    bool supports_attachments = false;
    uint32_t context_window = 0;
    uint32_t default_max_tokens = 0;
};

/* Per-request output/token budget (T1). Phase 6 (prompts/context) owns the
 * full budget machinery; the provider layer just projects these onto the wire.
 * 0 = use the provider/model default. */
struct Budget {
    uint32_t max_output_tokens = 0;
    uint32_t max_context_tokens = 0;
    uint32_t max_tool_args_bytes = 0;
};

/* The fully-built wire request. The agent loop sends it verbatim through the
 * Phase 4 net stack (Transport + http1). */
struct RequestBytes {
    std::string method = "POST";
    std::string path;
    net::HttpHeaders headers;
    std::string body;
};

/* One server frame fed to parse_frame: the SSE event name ("" for JSONL
 * lines) plus the raw payload bytes (always JSON for our adapters). */
struct StreamFrame {
    std::string_view event;
    std::string_view data;
};

/* Token usage rolled up from a provider's usage payload. */
struct Usage {
    uint64_t input_tokens = 0;
    uint64_t output_tokens = 0;
    uint64_t cached_input_tokens = 0;
};

/* The normalised stream event set -- the ONLY vocabulary the agent loop
 * consumes. `text`/`reasoning` are deltas (append); ToolCallDelta carries an
 * incremental argument fragment that the adapter stitches into ToolCallDone. */
struct TextDelta {
    std::string text;
};
struct ReasoningDelta {
    std::string text;
};
struct ToolCallDelta {
    std::string id;
    std::string name;        /* may be partial until done */
    std::string args_fragment; /* incremental, not yet valid JSON */
};
struct ToolCallDone {
    std::string id;
    std::string name;
    std::string input_json;  /* assembled, validated JSON */
};
struct MessageStart {};
struct MessageDone {
    msg::FinishReason finish = msg::FinishReason::end_turn;
    Usage usage;
};
struct ProviderError {
    core::error_code ec;
    std::string message;
};

using StreamEvent = std::variant<TextDelta, ReasoningDelta, ToolCallDelta,
                                 ToolCallDone, MessageStart, MessageDone,
                                 ProviderError>;

inline constexpr size_t kStreamEventCount = std::variant_size_v<StreamEvent>;

/* Typed accessors (mirror msg/part.h). */
template <class T>
const T* as(const StreamEvent& e) noexcept {
    return std::get_if<T>(&e);
}
template <class T>
T* as(StreamEvent& e) noexcept {
    return std::get_if<T>(&e);
}
template <class T>
bool holds(const StreamEvent& e) noexcept {
    return std::holds_alternative<T>(e);
}

/* One-line human description (tools/probe + debug logs). */
inline std::string describe(const StreamEvent& e) {
    if (const TextDelta* t = as<TextDelta>(e)) return "text:" + t->text;
    if (const ReasoningDelta* r = as<ReasoningDelta>(e))
        return "reasoning:" + r->text;
    if (const ToolCallDelta* t = as<ToolCallDelta>(e))
        return "tool_delta:" + t->name + " += " + t->args_fragment;
    if (const ToolCallDone* t = as<ToolCallDone>(e))
        return "tool_call:" + t->name + " " + t->input_json;
    if (holds<MessageStart>(e)) return "message_start";
    if (const MessageDone* d = as<MessageDone>(e))
        return "message_done:" + std::string(msg::to_string(d->finish)) +
               " in=" + std::to_string(d->usage.input_tokens) +
               " out=" + std::to_string(d->usage.output_tokens);
    if (const ProviderError* err = as<ProviderError>(e))
        return "error:" + std::string(err->ec.message()) + " " + err->message;
    return "?";
}

class Provider {
public:
    virtual ~Provider() = default;

    /* Serialize messages + tools + model + budget to a wire request. ok() on
     * success; never throws. */
    virtual core::error_code build_request(const MsgList& msgs,
                                           const ToolsSpec& tools,
                                           const ModelSpec& model,
                                           const Budget& budget,
                                           RequestBytes& out) = 0;

    /* Consume one server frame; zero or more normalised events are appended
     * to `out`. Provider stream state accumulates across frames until
     * reset_stream(). ok() even for no-op frames. Never throws. */
    virtual core::error_code parse_frame(const StreamFrame& frame,
                                         std::vector<StreamEvent>& out) = 0;

    /* Extract Usage from a provider usage JSON object (used for non-streamed
     * responses and rollups). */
    virtual core::error_code parse_usage(const util::JVal& json,
                                         Usage& out) = 0;

    virtual std::string_view name() const = 0;
    virtual bool supports_native_tools() const = 0;
    /* How the wire frames are delimited (drives net::SseParser config). */
    virtual net::StreamKind stream_kind() const = 0;
    /* Clear per-stream accumulation before a new request. */
    virtual void reset_stream() = 0;
};

/* -------------------------------------------------------------------------
 * Factory + resolver
 * ---------------------------------------------------------------------- */

/* Configuration to construct a provider (merged config::ProviderCfg + model
 * resolution by the caller). base_url/path override the catalog defaults. */
struct ProviderConfig {
    std::string id;         /* provider id: anthropic|openai|google|openai_compat */
    std::string api_key;
    std::string base_url;   /* origin override; empty = catalog default */
    std::string path;       /* endpoint path override (compat) */
    uint32_t default_max_tokens = 0;
};

/* Build a Provider for `cfg` (id selects the adapter). e_model_unsup for an
 * unknown id. Never throws. */
core::error_code make_provider(const ProviderConfig& cfg,
                               std::unique_ptr<Provider>& out);

/* Per-adapter constructors (used by factory.cpp and by tests that need a
 * specific wire family without id dispatch). */
core::error_code make_anthropic(std::string base_url, std::string api_key,
                                uint32_t default_max_tokens,
                                std::unique_ptr<Provider>& out);
core::error_code make_openai(std::string base_url, std::string api_key,
                             uint32_t default_max_tokens,
                             std::string_view endpoint_path,
                             std::unique_ptr<Provider>& out);
core::error_code make_gemini(std::string base_url, std::string api_key,
                             uint32_t default_max_tokens,
                             std::unique_ptr<Provider>& out);
core::error_code make_openai_compat(std::string base_url, std::string api_key,
                                    uint32_t default_max_tokens,
                                    std::string_view endpoint_path,
                                    std::unique_ptr<Provider>& out);

/* Resolve a model id (exact, alias, or catalog id) to its wire projection.
 * Unknown ids -> e_model_unsup; close suggestions are appended to
 * `suggestions` when non-null (engine surfaces them to the host). */
core::error_code resolve_model(std::string_view model_id, ModelSpec& out,
                               std::vector<std::string>* suggestions = nullptr);

/* Split a base_url origin ("scheme://host[:port][/base]") into its parts.
 * ok() for http/https; e_invalid_cfg otherwise. port 0 = default for scheme. */
struct UrlParts {
    std::string scheme;
    std::string host;
    uint16_t port = 0;
    std::string path; /* leading slash, "" when none */
};
core::error_code split_url(std::string_view url, UrlParts& out);

} /* namespace opencode::provider */

#endif /* OPENCODE_PROVIDER_PROVIDER_H */

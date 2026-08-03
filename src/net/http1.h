/*
 * http1.h -- minimal HTTP/1.1 client on a Transport.
 *
 * Request builder + incremental response parser (head, content-length,
 * chunked, EOF-framed bodies) + blocking-over-loop read helpers. Only what the
 * provider layer needs: no redirects, no cookies, no pipelining. Never throws;
 * every failure is a core::error_code in the net vocabulary.
 */
#ifndef OPENCODE_NET_HTTP1_H
#define OPENCODE_NET_HTTP1_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/clock.h"
#include "core/error.h"
#include "core/event_loop.h"
#include "net/transport.h"

namespace opencode::net {

struct HttpHeader {
    std::string name;
    std::string value;
};
using HttpHeaders = std::vector<HttpHeader>;

struct HttpRequest {
    std::string method = "GET";
    std::string path = "/";
    HttpHeaders headers;
    std::string body;
    std::string request_id; /* stable idempotency key (X-Request-Id) */
};

struct HttpResponse {
    int code = 0;
    std::string reason;
    HttpHeaders headers;
    std::string body;     /* collected only by http_exchange (bounded) */
    bool truncated = false;
    bool keep_alive = true;
};

/* Case-insensitive header lookup; "" when absent. */
std::string_view http_header(const HttpHeaders& hs, std::string_view name);

/* Serialize the request head + body. e_net_overflow when the head exceeds the
 * guard (64 KiB). Content-Length is added for non-empty bodies unless the
 * caller already set it; X-Request-Id mirrors HttpRequest::request_id. */
core::error_code http_build_request(const HttpRequest& req, std::string& out);

/* Incremental response parser. Feed raw bytes from the transport in any
 * chunking; the head is parsed once its blank line lands, then the body is
 * decoded per framing (content-length / chunked / EOF-terminated). */
class HttpParser {
public:
    static constexpr uint32_t kMaxHeadBytes = 64 * 1024;
    static constexpr uint64_t kMaxBodyBytes = 256u * 1024 * 1024;

    core::error_code feed(std::string_view in);

    bool head_done() const noexcept { return head_done_; }
    const HttpResponse& head() const noexcept { return head_; }

    bool chunked() const noexcept { return chunked_; }
    bool eof_framed() const noexcept { return eof_framed_; }
    uint64_t expected_body() const noexcept { return content_length_; }
    uint64_t body_received() const noexcept { return body_got_; }

    /* Decoded body bytes buffered and awaiting take(). */
    size_t buffered() const noexcept { return body_buf_.size(); }
    size_t take(char* dst, size_t n);

    /* Body fully received per framing. EOF-framed responses never return true
     * here; the drain loop treats transport EOF as the terminator. */
    bool body_done() const noexcept;    bool keep_alive() const noexcept { return head_.keep_alive; }

    void set_max_body(uint64_t n) noexcept { max_body_ = n; }

private:
    core::error_code parse_head();
    core::error_code decode_chunked();

    HttpResponse head_;
    std::string buf_;       /* unparsed bytes: head first, then raw body */
    std::string body_buf_;  /* decoded body awaiting take() */
    uint64_t content_length_ = 0;
    uint64_t body_got_ = 0;
    uint64_t max_body_ = kMaxBodyBytes;
    bool head_done_ = false;
    bool chunked_ = false;
    bool chunked_size_seen_ = false;
    uint64_t chunk_size_ = 0;
    bool chunked_done_ = false;
    bool eof_framed_ = false;
    bool length_framed_ = false; /* Content-Length present (incl. 0) */
};

/* Drive transport reads into `p` until the body is complete per framing,
 * invoking on_body (when non-null) for each decoded span; a null on_body
 * discards. Returns e_net_connect on premature EOF for a length-framed body.
 * Blocking-over-loop on the caller's loop. */
core::error_code http_drain_body(
    core::EventLoop& loop, Transport& t, HttpParser& p, uint64_t deadline_ms,
    const std::function<core::error_code(std::string_view)>& on_body);

/* Classify a completed exchange: ok for 2xx; e_rate_limit for 429; e_auth for
 * 401/403; e_provider_err for other 4xx/5xx; e_net_http for 3xx/unknown. */
core::error_code http_status_error(const HttpResponse& resp);

/* One-shot exchange on an already-connected transport: build, send, parse the
 * head, drain the body into resp.body up to max_resp_bytes (then truncated).
 * The status code is returned in resp regardless of value; use
 * http_status_error() to classify. */
core::error_code http_exchange(core::EventLoop& loop, Transport& t,
                               const HttpRequest& req, uint64_t deadline_ms,
                               HttpResponse& resp, uint32_t max_resp_bytes = 0);

} /* namespace opencode::net */

#endif /* OPENCODE_NET_HTTP1_H */

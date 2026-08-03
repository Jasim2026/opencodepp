/*
 * sse.h -- incremental SSE / JSONL stream parser.
 *
 * Consumes an HTTP response body in arbitrary chunks and emits a typed
 * SseEvent per completed frame: SSE frames are terminated by a blank line,
 * JSONL frames by '\n'. A final unterminated frame stays buffered and is
 * dropped by the caller on stream end (never partially emitted). The parser
 * is abortable between frames and guards per-frame bytes and event count.
 * Never throws.
 */
#ifndef OPENCODE_NET_SSE_H
#define OPENCODE_NET_SSE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "core/error.h"
#include "net/http1.h"

namespace opencode::net {

enum class StreamKind { sse, jsonl };

struct SseEvent {
    std::string id;     /* last "id:" field in the frame */
    std::string event;  /* "event:" or "" (the default message event) */
    std::string data;   /* "data:" lines joined with '\n' */
};

class SseParser {
public:
    static constexpr uint32_t kMaxFrameBytes = 4u * 1024 * 1024;

    explicit SseParser(StreamKind kind = StreamKind::sse) noexcept : kind_(kind) {}

    void set_abort_flag(const std::atomic<bool>* flag) noexcept { abort_ = flag; }
    void set_max_frame_bytes(uint32_t n) noexcept { max_frame_ = n; }
    void set_max_events(uint32_t n) noexcept { max_events_ = n; } /* 0 = off */

    /* Feed bytes; each completed frame is delivered to `sink` as it closes.
     * e_net_overflow on a per-frame cap breach or event-count limit reached;
     * e_cancelled when the abort flag is set between frames. */
    core::error_code feed(std::string_view in,
                          const std::function<void(const SseEvent&)>& sink);

    size_t events() const noexcept { return emitted_; }
    uint32_t dropped() const noexcept { return dropped_; }

private:
    core::error_code emit(const std::function<void(const SseEvent&)>& sink);
    core::error_code line(std::string_view l,
                          const std::function<void(const SseEvent&)>& sink);

    StreamKind kind_;
    std::string frame_; /* raw, unterminated tail between feeds */
    std::string data_, id_, event_;
    size_t emitted_ = 0;
    uint32_t dropped_ = 0;
    uint32_t max_frame_ = kMaxFrameBytes;
    uint32_t max_events_ = 0;
    const std::atomic<bool>* abort_ = nullptr;
};

/* Convenience: stream the response body of an already head-parsed HttpParser
 * into the parser. Composes http_drain_body + SseParser::feed. */
core::error_code sse_stream(
    core::EventLoop& loop, Transport& t, HttpParser& hp, uint64_t deadline_ms,
    SseParser& sp, const std::function<void(const SseEvent&)>& sink);

} /* namespace opencode::net */

#endif /* OPENCODE_NET_SSE_H */

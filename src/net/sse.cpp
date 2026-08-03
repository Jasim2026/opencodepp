#include "net/sse.h"

namespace opencode::net {

core::error_code SseParser::emit(
    const std::function<void(const SseEvent&)>& sink) {
    if (kind_ == StreamKind::jsonl) {
        if (!data_.empty()) {
            SseEvent ev;
            ev.event = "message";
            ev.data = std::move(data_);
            data_.clear();
            if (sink) sink(ev);
            ++emitted_;
            if (max_events_ != 0 && emitted_ >= max_events_)
                return core::make_error_code(core::Err::e_net_overflow);
        }
        return core::ok();
    }
    if (data_.empty() && id_.empty() && event_.empty()) {
        ++dropped_; /* bare blank line (heartbeat): nothing to deliver */
        return core::ok();
    }
    SseEvent ev;
    ev.id = std::move(id_);
    ev.event = std::move(event_);
    if (!data_.empty() && data_.back() == '\n') data_.pop_back();
    ev.data = std::move(data_);
    data_.clear();
    id_.clear();
    event_.clear();
    if (sink) sink(ev);
    ++emitted_;
    if (max_events_ != 0 && emitted_ >= max_events_)
        return core::make_error_code(core::Err::e_net_overflow);
    return core::ok();
}

core::error_code SseParser::line(
    std::string_view l, const std::function<void(const SseEvent&)>& sink) {
    if (kind_ == StreamKind::jsonl) {
        data_ = std::string(l);
        return emit(sink);
    }
    if (l.empty()) return emit(sink); /* frame terminator */
    if (l.front() == ':') return core::ok(); /* comment */
    if (l.front() == ' ') return core::ok(); /* ignore leading-space data */
    const size_t colon = l.find(':');
    if (colon == std::string::npos) return core::ok(); /* bare field: ignore */
    const std::string_view field = l.substr(0, colon);
    std::string_view value = l.substr(colon + 1);
    if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    if (field == "data") {
        data_ += value;
        data_ += '\n';
    } else if (field == "id") {
        id_ = std::string(value);
    } else if (field == "event") {
        event_ = std::string(value);
    }
    return core::ok();
}

core::error_code SseParser::feed(
    std::string_view in, const std::function<void(const SseEvent&)>& sink) {
    if (in.empty()) return core::ok();
    if (abort_ != nullptr && abort_->load())
        return core::make_error_code(core::Err::e_cancelled);
    if (frame_.size() + in.size() > max_frame_)
        return core::make_error_code(core::Err::e_net_overflow);
    frame_.append(in.data(), in.size());

    size_t off = 0;
    while (true) {
        const size_t nl = frame_.find('\n', off);
        if (nl == std::string::npos) break;
        std::string_view l = std::string_view(frame_).substr(off, nl - off);
        if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
        off = nl + 1;
        const core::error_code ec = line(l, sink);
        if (!ec.ok()) {
            frame_.erase(0, off);
            return ec;
        }
    }
    frame_.erase(0, off);
    return core::ok();
}

core::error_code sse_stream(
    core::EventLoop& loop, Transport& t, HttpParser& hp, uint64_t deadline_ms,
    SseParser& sp, const std::function<void(const SseEvent&)>& sink) {
    return http_drain_body(loop, t, hp, deadline_ms,
                           [&sp, &sink](std::string_view chunk) {
                               return sp.feed(chunk, sink);
                           });
}

} /* namespace opencode::net */

#include "net/http1.h"

#include <cctype>
#include <cstring>

namespace opencode::net {

namespace {

bool ci_equals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

bool parse_u64(std::string_view s, uint64_t& out) {
    if (s.empty()) return false;
    uint64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    out = v;
    return true;
}

} /* namespace */

std::string_view http_header(const HttpHeaders& hs, std::string_view name) {
    for (const auto& h : hs) {
        if (ci_equals(h.name, name)) return h.value;
    }
    return "";
}

core::error_code http_build_request(const HttpRequest& req, std::string& out) {
    out.clear();
    out.reserve(512 + req.body.size());

    out += req.method;
    out += ' ';
    out += req.path.empty() ? "/" : req.path;
    out += " HTTP/1.1\r\n";

    bool has_len = false;
    bool has_rid = false;
    for (const auto& h : req.headers) {
        if (ci_equals(h.name, "Content-Length")) has_len = true;
        if (ci_equals(h.name, "X-Request-Id")) has_rid = true;
        out += h.name;
        out += ": ";
        out += h.value;
        out += "\r\n";
        if (out.size() > HttpParser::kMaxHeadBytes)
            return core::make_error_code(core::Err::e_net_overflow);
    }
    if (!req.body.empty() && !has_len) {
        out += "Content-Length: ";
        out += std::to_string(req.body.size());
        out += "\r\n";
    }
    if (!req.request_id.empty() && !has_rid) {
        out += "X-Request-Id: ";
        out += req.request_id;
        out += "\r\n";
    }
    if (out.size() > HttpParser::kMaxHeadBytes)
        return core::make_error_code(core::Err::e_net_overflow);

    out += "\r\n";
    out += req.body;
    return core::ok();
}

core::error_code HttpParser::feed(std::string_view in) {
    if (in.empty()) return core::ok();
    buf_.append(in.data(), in.size());

    for (;;) {
        if (!head_done_) {
            const size_t term = buf_.find("\r\n\r\n");
            const size_t alt = term == std::string::npos ? buf_.find("\n\n") : std::string::npos;
            if (term == std::string::npos && alt == std::string::npos) {
                if (buf_.size() > kMaxHeadBytes)
                    return core::make_error_code(core::Err::e_net_overflow);
                return core::ok();
            }
            const size_t end = term != std::string::npos ? term + 4 : alt + 2;
            const core::error_code ec = parse_head();
            if (!ec.ok()) return ec;
            buf_.erase(0, end);
            continue;
        }

        if (chunked_ && !chunked_done_) {
            const core::error_code ec = decode_chunked();
            if (!ec.ok()) return ec;
            if (chunked_done_) break;
            return core::ok(); /* waiting for more chunk bytes */
        }
        if (chunked_) return core::ok(); /* done: drop any trailing bytes */

        /* content-length or EOF-framed: move everything into body_buf_, but no
         * more than what the framing allows. */
        uint64_t room = kMaxBodyBytes;
        if (content_length_ > 0) {
            if (body_got_ >= content_length_) {
                buf_.clear();
                return core::ok();
            }
            room = content_length_ - body_got_;
        }
        const size_t take_ = static_cast<size_t>(room) < buf_.size()
                                 ? static_cast<size_t>(room)
                                 : buf_.size();
        body_buf_.append(buf_.data(), take_);
        body_got_ += take_;
        buf_.erase(0, take_);
        if (body_got_ > max_body_) return core::make_error_code(core::Err::e_net_overflow);
        if (content_length_ > 0 && body_got_ >= content_length_) return core::ok();
        if (buf_.empty()) return core::ok();
    }
    return core::ok();
}

core::error_code HttpParser::parse_head() {
    /* Split head into lines ("\r\n" or "\n"). */
    const size_t block = buf_.find("\r\n\r\n");
    std::string_view head = buf_;
    if (block != std::string::npos) head = std::string_view(buf_).substr(0, block);
    else {
        const size_t alt = buf_.find("\n\n");
        if (alt == std::string::npos) return core::make_error_code(core::Err::e_proto_parse);
        head = std::string_view(buf_).substr(0, alt);
    }

    std::string_view first = head;
    std::string_view rest = {};
    const size_t eol = head.find('\n');
    if (eol != std::string::npos) {
        first = head.substr(0, eol);
        if (!first.empty() && first.back() == '\r') first.remove_suffix(1);
        rest = head.substr(eol + 1);
    }

    /* Status line: HTTP/1.x SP code SP reason. */
    size_t sp1 = first.find(' ');
    size_t sp2 = first.find(' ', sp1 + 1);
    if (sp1 == std::string::npos) return core::make_error_code(core::Err::e_proto_parse);
    const std::string_view version = first.substr(0, sp1);
    if (version.size() < 5 || version.substr(0, 5) != "HTTP/")
        return core::make_error_code(core::Err::e_proto_parse);
    std::string_view code_sv;
    std::string_view reason;
    if (sp2 != std::string::npos) {
        code_sv = first.substr(sp1 + 1, sp2 - sp1 - 1);
        reason = first.substr(sp2 + 1);
    } else {
        code_sv = first.substr(sp1 + 1);
    }
    uint64_t code = 0;
    if (!parse_u64(code_sv, code) || code > 999)
        return core::make_error_code(core::Err::e_proto_parse);

    HttpResponse st;
    st.code = static_cast<int>(code);
    st.reason.assign(reason.data(), reason.size());
    st.keep_alive = true;

    /* Header fields. */
    size_t off = 0;
    while (off < rest.size()) {
        size_t nl = rest.find('\n', off);
        const size_t end = nl == std::string::npos ? rest.size() : nl;
        std::string_view line = rest.substr(off, end - off);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        off = end + 1;
        if (line.empty()) break;

        const size_t colon = line.find(':');
        if (colon == std::string::npos)
            return core::make_error_code(core::Err::e_proto_parse);
        HttpHeader h;
        h.name.assign(line.data(), colon);
        h.value.assign(trim(line.substr(colon + 1)).data(),
                       trim(line.substr(colon + 1)).size());
        st.headers.push_back(std::move(h));
    }

    /* Framing: chunked wins; else Content-Length; else EOF-framed. */
    const std::string_view te = http_header(st.headers, "Transfer-Encoding");
    if (ci_equals(te, "chunked")) {
        chunked_ = true;
        chunked_size_seen_ = false;
        chunk_size_ = 0;
        chunked_done_ = false;
    } else {
        const std::string_view cl = http_header(st.headers, "Content-Length");
        if (!cl.empty()) {
            uint64_t len = 0;
            if (!parse_u64(cl, len))
                return core::make_error_code(core::Err::e_proto_parse);
            content_length_ = len;
            length_framed_ = true;
            if (len > max_body_) return core::make_error_code(core::Err::e_net_overflow);
        } else {
            eof_framed_ = true;
        }
    }
    if (ci_equals(http_header(st.headers, "Connection"), "close")) {
        st.keep_alive = false;
    }
    head_ = std::move(st);
    head_done_ = true;
    return core::ok();
}

core::error_code HttpParser::decode_chunked() {
    size_t off = 0;
    while (off < buf_.size()) {
        if (!chunked_size_seen_) {
            size_t nl = buf_.find('\n', off);
            if (nl == std::string::npos) break;
            std::string_view line = std::string_view(buf_).substr(off, nl - off);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            off = nl + 1;
            const size_t sem = line.find(';');
            if (sem != std::string::npos) line = line.substr(0, sem);
            std::string_view hex = trim(line);
            uint64_t size = 0;
            for (const char c : hex) {
                uint64_t d;
                if (c >= '0' && c <= '9') d = static_cast<uint64_t>(c - '0');
                else if (c >= 'a' && c <= 'f') d = static_cast<uint64_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') d = static_cast<uint64_t>(c - 'A' + 10);
                else return core::make_error_code(core::Err::e_proto_parse);
                size = size * 16 + d;
            }
            chunk_size_ = size;
            chunked_size_seen_ = true;
            if (chunk_size_ == 0) {
                /* Last chunk: skip optional trailers up to a blank line. */
                while (off < buf_.size()) {
                    size_t tl = buf_.find('\n', off);
                    const size_t tend = tl == std::string::npos ? buf_.size() : tl;
                    std::string_view tline = std::string_view(buf_).substr(off, tend - off);
                    if (!tline.empty() && tline.back() == '\r') tline.remove_suffix(1);
                    off = tl == std::string::npos ? buf_.size() : tl + 1;
                    if (tline.empty()) break;
                }
                chunked_done_ = true;
                break;
            }
            continue;
        }
        const uint64_t need = chunk_size_ + 2; /* data + CRLF */
        if (buf_.size() - off < need) break;
        body_buf_.append(buf_.data() + off, static_cast<size_t>(chunk_size_));
        body_got_ += chunk_size_;
        off += static_cast<size_t>(need);
        chunked_size_seen_ = false;
        if (body_got_ > max_body_) return core::make_error_code(core::Err::e_net_overflow);
    }
    buf_.erase(0, off);
    return core::ok();
}

size_t HttpParser::take(char* dst, size_t n) {
    const size_t m = n < body_buf_.size() ? n : body_buf_.size();
    std::memcpy(dst, body_buf_.data(), m);
    body_buf_.erase(0, m);
    return m;
}

bool HttpParser::body_done() const noexcept {
    if (!head_done_) return false;
    if (chunked_) return chunked_done_;
    if (length_framed_) return body_got_ >= content_length_; /* incl. 0 */
    return false; /* EOF-framed: only the transport knows */
}

core::error_code http_drain_body(
    core::EventLoop& loop, Transport& t, HttpParser& p, uint64_t deadline_ms,
    const std::function<core::error_code(std::string_view)>& on_body) {
    char scratch[16 * 1024];
    for (;;) {
        if (on_body) {
            while (p.buffered() > 0) {
                const size_t m = p.take(scratch, sizeof scratch);
                const core::error_code ec = on_body(std::string_view(scratch, m));
                if (!ec.ok()) return ec;
            }
        } else if (p.buffered() > 0) {
            p.take(scratch, sizeof scratch); /* discard */
        }
        if (p.body_done()) return core::ok();

        ssize_t got = 0;
        const core::error_code re = t.read(loop, reinterpret_cast<uint8_t*>(scratch),
                                           sizeof scratch, deadline_ms, got);
        if (!re.ok()) return re;
        if (got == 0) {
            /* Transport EOF. For a length-framed body this is a premature
             * mid-stream close (retryable per doctrine); EOF-framed bodies are
             * complete here. */
            if (p.eof_framed()) return core::ok();
            if (p.body_done()) return core::ok();
            return core::make_error_code(core::Err::e_net_connect, 1);
        }
        const core::error_code fe = p.feed(std::string_view(scratch, static_cast<size_t>(got)));
        if (!fe.ok()) return fe;
    }
}

core::error_code http_status_error(const HttpResponse& resp) {
    if (resp.code >= 200 && resp.code < 300) return core::ok();
    switch (resp.code) {
        case 429: return core::make_error_code(core::Err::e_rate_limit);
        case 401: case 403: return core::make_error_code(core::Err::e_auth);
    }
    if (resp.code >= 400 && resp.code < 600)
        return core::make_error_code(core::Err::e_provider_err,
                                     static_cast<uint32_t>(resp.code));
    return core::make_error_code(core::Err::e_net_http,
                                 static_cast<uint32_t>(resp.code));
}

core::error_code http_exchange(core::EventLoop& loop, Transport& t,
                               const HttpRequest& req, uint64_t deadline_ms,
                               HttpResponse& resp, uint32_t max_resp_bytes) {
    std::string wire;
    core::error_code ec = http_build_request(req, wire);
    if (!ec.ok()) return ec;

    ssize_t sent = 0;
    size_t off = 0;
    while (off < wire.size()) {
        ec = t.write(loop, reinterpret_cast<const uint8_t*>(wire.data() + off),
                     wire.size() - off, deadline_ms, sent);
        if (!ec.ok()) return ec;
        if (sent <= 0) return core::make_error_code(core::Err::e_net_connect, 2);
        off += static_cast<size_t>(sent);
    }

    HttpParser p;
    char scratch[16 * 1024];
    for (;;) {
        ssize_t got = 0;
        ec = t.read(loop, reinterpret_cast<uint8_t*>(scratch), sizeof scratch,
                    deadline_ms, got);
        if (!ec.ok()) return ec;
        if (got == 0) {
            if (!p.head_done()) return core::make_error_code(core::Err::e_net_connect, 3);
            break;
        }
        ec = p.feed(std::string_view(scratch, static_cast<size_t>(got)));
        if (!ec.ok()) return ec;
        if (p.head_done() && p.body_done()) break;
        if (p.head_done() && !p.eof_framed() && got == 0) break;
    }

    resp = p.head();
    if (max_resp_bytes != 0) {
        resp.body.reserve(max_resp_bytes < 4096 ? max_resp_bytes : 4096);
    }
    ec = http_drain_body(
        loop, t, p, deadline_ms,
        [&resp, max_resp_bytes](std::string_view chunk) -> core::error_code {
            if (max_resp_bytes == 0 ||
                resp.body.size() + chunk.size() <= max_resp_bytes) {
                resp.body.append(chunk.data(), chunk.size());
            } else {
                resp.body.append(chunk.data(),
                                 max_resp_bytes - resp.body.size());
                resp.truncated = true;
            }
            return core::ok();
        });
    return ec;
}

} /* namespace opencode::net */

// probe.cpp -- developer sanity CLI for the Phase 5 provider layer.
//
// Builds a Provider from CLI config, sends a fixed prompt over the real Phase 4
// net stack (Pool + http1 + SseParser), prints the normalised StreamEvent
// sequence and a meter/usage summary. --mock starts an in-process plaintext
// OpenAI-format mock (same pattern as tools/drill) so the whole path runs
// offline and in CI. Never throws.
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/clock.h"
#include "core/error.h"
#include "core/event_loop.h"
#include "msg/message.h"
#include "msg/part.h"
#include "net/http1.h"
#include "net/meter.h"
#include "net/pool.h"
#include "net/socket.h"
#include "net/sse.h"
#include "net/transport.h"
#include "provider/provider.h"

namespace {

using namespace opencode;
using namespace opencode::core;
using namespace opencode::msg;
using namespace opencode::net;
using namespace opencode::provider;

struct Args {
    std::string provider = "openai_compat";
    std::string base;
    std::string key = "sk-test";
    std::string path;
    std::string model;
    std::string prompt = "Hello, what is 2+2?";
    uint32_t max_tokens = 0;
    bool mock = false;
    uint64_t timeout_ms = 20'000;
};

/* In-process plaintext mock serving an OpenAI chat.completion.chunk stream. */
struct Mock {
    std::thread th;
    std::shared_ptr<std::atomic<bool>> stop;
    int ls = -1;
    uint16_t port = 0;

    Mock() = default;
    Mock(const Mock&) = delete;
    Mock& operator=(const Mock&) = delete;
    Mock(Mock&& o) noexcept
        : th(std::move(o.th)),
          stop(std::move(o.stop)),
          ls(o.ls),
          port(o.port) {
        o.ls = -1;
        o.port = 0;
    }
    Mock& operator=(Mock&& o) noexcept {
        if (this != &o) {
            if (stop) stop->store(true);
            if (ls >= 0) ::shutdown(ls, SHUT_RDWR);
            if (th.joinable()) th.join();
            th = std::move(o.th);
            stop = std::move(o.stop);
            ls = o.ls;
            port = o.port;
            o.ls = -1;
            o.port = 0;
        }
        return *this;
    }
    ~Mock() {
        if (stop) stop->store(true);
        if (ls >= 0) ::shutdown(ls, SHUT_RDWR);
        if (th.joinable()) th.join();
        ls = -1;
    }
};

void send_all(int fd, const char* data, size_t n) {
    size_t off = 0;
    while (off < n) {
        const ssize_t w = ::write(fd, data + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        off += static_cast<size_t>(w);
    }
}

std::string read_request(int fd) {
    std::string raw;
    char buf[1024];
    size_t keep = 0;
    while (true) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0) break;
        raw.append(buf, static_cast<size_t>(n));
        keep += static_cast<size_t>(n);
        if (raw.find("\r\n\r\n") != std::string::npos) break;
        if (keep > 1u << 20) break;
    }
    return raw.substr(0, raw.find("\r\n"));
}

void serve_conn(int fd, const char* script, size_t len) {
    (void)read_request(fd);
    char head[512];
    const int n = std::snprintf(head, sizeof head,
                                "HTTP/1.1 200 OK\r\n"
                                "Content-Type: text/event-stream\r\n"
                                "Cache-Control: no-cache\r\n"
                                "Connection: close\r\n"
                                "Content-Length: %zu\r\n"
                                "\r\n",
                                len);
    send_all(fd, head, static_cast<size_t>(n));
    send_all(fd, script, len);
    ::close(fd);
}

Mock start_mock() {
    Mock m;
    m.stop = std::make_shared<std::atomic<bool>>(false);
    m.ls = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(m.ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (::bind(m.ls, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0 ||
        ::listen(m.ls, 16) < 0) {
        std::fprintf(stderr, "probe: mock bind/listen failed\n");
        std::exit(2);
    }
    sockaddr_in g{};
    socklen_t gl = sizeof g;
    ::getsockname(m.ls, reinterpret_cast<sockaddr*>(&g), &gl);
    m.port = ntohs(g.sin_port);

    const int ls = m.ls;
    const std::shared_ptr<std::atomic<bool>> stop = m.stop;
    const char* script =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\","
        "\"content\":\"Hello\"}}]}\r\n"
        "\r\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\" from probe\"}}]}\r\n"
        "\r\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":"
        "\"stop\"}],\"usage\":{\"prompt_tokens\":8,\"completion_tokens\":4}}\r\n"
        "\r\n";
    const size_t len = std::strlen(script);
    m.th = std::thread([ls, stop, script, len]() {
        while (!stop->load()) {
            struct pollfd pfd{};
            pfd.fd = ls;
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, 100) <= 0) continue;
            const int c = ::accept(ls, nullptr, nullptr);
            if (c < 0) continue;
            std::thread([c, script, len]() { serve_conn(c, script, len); })
                .detach();
        }
        ::close(ls);
    });
    return m;
}

void kill_mock(Mock& m) {
    m.stop->store(true);
    if (m.ls >= 0) ::shutdown(m.ls, SHUT_RDWR);
    if (m.th.joinable()) m.th.join();
    m.ls = -1;
}

std::string ec_str(error_code ec) { return std::string(ec.message()); }

void print_usage(const char* prog) {
    std::printf(
        "usage: %s [--provider anthropic|openai|google|openai_compat]\n"
        "           [--base URL] [--key KEY] [--path PATH] [--model ID]\n"
        "           [--prompt TEXT] [--max-tokens N] [--timeout-ms N]\n"
        "           [--mock]\n"
        "  --mock  run against an in-process plaintext OpenAI-format mock\n",
        prog);
}

bool parse_args(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "probe: %s needs a value\n", flag);
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (arg == "--mock") {
            a.mock = true;
        } else if (arg == "--provider") {
            const char* v = need("--provider");
            if (v == nullptr) return false;
            a.provider = v;
        } else if (arg == "--base") {
            const char* v = need("--base");
            if (v == nullptr) return false;
            a.base = v;
        } else if (arg == "--key") {
            const char* v = need("--key");
            if (v == nullptr) return false;
            a.key = v;
        } else if (arg == "--path") {
            const char* v = need("--path");
            if (v == nullptr) return false;
            a.path = v;
        } else if (arg == "--model") {
            const char* v = need("--model");
            if (v == nullptr) return false;
            a.model = v;
        } else if (arg == "--prompt") {
            const char* v = need("--prompt");
            if (v == nullptr) return false;
            a.prompt = v;
        } else if (arg == "--max-tokens") {
            const char* v = need("--max-tokens");
            if (v == nullptr) return false;
            a.max_tokens = static_cast<uint32_t>(std::atoi(v));
        } else if (arg == "--timeout-ms") {
            const char* v = need("--timeout-ms");
            if (v == nullptr) return false;
            a.timeout_ms = static_cast<uint64_t>(std::atoll(v));
        } else {
            std::fprintf(stderr, "probe: unknown flag '%s'\n", arg.c_str());
            print_usage(argv[0]);
            return false;
        }
    }
    return true;
}

} /* namespace */

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);

    Args args;
    if (!parse_args(argc, argv, args)) return EXIT_FAILURE;

    Mock mock;
    if (args.mock) {
        mock = start_mock();
        args.provider = "openai_compat";
        args.base = "http://127.0.0.1:" + std::to_string(mock.port);
        args.path = "/v1/chat";
        args.key = "sk-test";
        if (args.model.empty()) args.model = "gpt-4o";
        std::printf("probe: mock listening on 127.0.0.1:%u (plaintext)\n",
                    mock.port);
    }
    if (args.base.empty()) {
        std::fprintf(stderr, "probe: --base URL is required (or use --mock)\n");
        return EXIT_FAILURE;
    }

    ProviderConfig cfg;
    cfg.id = args.provider;
    cfg.api_key = args.key;
    cfg.base_url = args.base;
    cfg.path = args.path;
    cfg.default_max_tokens = args.max_tokens;

    std::unique_ptr<Provider> p;
    const error_code make_ec = make_provider(cfg, p);
    if (!make_ec.ok()) {
        std::fprintf(stderr, "probe: provider %s: %s\n", cfg.id.c_str(),
                     ec_str(make_ec).c_str());
        return EXIT_FAILURE;
    }

    ModelSpec ms;
    if (!args.model.empty()) {
        std::vector<std::string> sug;
        const error_code mec = resolve_model(args.model, ms, &sug);
        if (!mec.ok()) {
            std::fprintf(stderr, "probe: model '%s': %s\n", args.model.c_str(),
                         ec_str(mec).c_str());
            for (const std::string& s : sug)
                std::fprintf(stderr, "  did you mean '%s'?\n", s.c_str());
            return EXIT_FAILURE;
        }
        if (args.provider == "openai_compat") {
            /* The mock endpoint decides its own model name. */
            ms.api_model_name = args.model;
        }
    } else {
        ms.api_model_name = "probe-model";
        ms.provider = args.provider;
        ms.api_family = args.provider == "google" ? "google" : "openai";
        if (args.provider == "anthropic") ms.api_family = "anthropic";
    }

    MsgList msgs;
    {
        Message m;
        m.role = Role::user;
        m.parts.push_back(Text{args.prompt});
        msgs.push_back(std::move(m));
    }

    const Budget budget;
    RequestBytes rb;
    const error_code build_ec = p->build_request(msgs, {}, ms, budget, rb);
    if (!build_ec.ok()) {
        std::fprintf(stderr, "probe: build_request: %s\n",
                     ec_str(build_ec).c_str());
        return EXIT_FAILURE;
    }

    UrlParts u;
    if (const error_code sec = split_url(args.base, u); !sec.ok()) {
        std::fprintf(stderr, "probe: base_url: %s\n", ec_str(sec).c_str());
        return EXIT_FAILURE;
    }

    EventLoop loop;
    Pool pool;
    Meter meter;
    const uint64_t deadline = now_mono_ms() + args.timeout_ms;

    const Pool::Key key{u.host, u.port, u.scheme == "https"};
    std::unique_ptr<TlsConfig> tls;
    if (u.scheme == "https") {
        tls = std::make_unique<TlsConfig>();
        tls->sni = u.host;
        tls->alpn = "http/1.1";
    }
    Transport t;
    const error_code aq = pool.acquire(loop, key, tls.get(), deadline, t);
    if (!aq.ok()) {
        std::fprintf(stderr, "probe: connect %s:%u: %s\n", u.host.c_str(),
                     u.port, ec_str(aq).c_str());
        return EXIT_FAILURE;
    }

    HttpRequest req;
    req.method = rb.method;
    req.path = rb.path.empty() ? "/" : rb.path;
    req.headers = std::move(rb.headers);
    req.body = std::move(rb.body);
    req.request_id = "probe";

    std::string wire;
    const error_code wreq = http_build_request(req, wire);
    if (!wreq.ok()) {
        std::fprintf(stderr, "probe: build http: %s\n", ec_str(wreq).c_str());
        return EXIT_FAILURE;
    }
    ssize_t sent = 0;
    while (sent < static_cast<ssize_t>(wire.size())) {
        ssize_t n = 0;
        const error_code we = t.write(
            loop, reinterpret_cast<const uint8_t*>(wire.data() + sent),
            wire.size() - static_cast<size_t>(sent), deadline, n);
        if (!we.ok()) {
            std::fprintf(stderr, "probe: write: %s\n", ec_str(we).c_str());
            return EXIT_FAILURE;
        }
        if (n <= 0) break;
        sent += n;
    }
    meter.add_bytes_out(wire.size());

    HttpParser hp;
    {
        char buf[4096];
        while (!hp.head_done()) {
            ssize_t got = 0;
            const error_code re =
                t.read(loop, reinterpret_cast<uint8_t*>(buf), sizeof buf,
                       deadline, got);
            if (!re.ok()) {
                std::fprintf(stderr, "probe: read head: %s\n",
                             ec_str(re).c_str());
                return EXIT_FAILURE;
            }
            if (got == 0) {
                std::fprintf(stderr, "probe: connection closed before head\n");
                return EXIT_FAILURE;
            }
            const error_code fe = hp.feed(std::string_view(buf, got));
            if (!fe.ok()) {
                std::fprintf(stderr, "probe: parse head: %s\n",
                             ec_str(fe).c_str());
                return EXIT_FAILURE;
            }
        }
    }
    const error_code status = http_status_error(hp.head());
    if (!status.ok()) {
        std::fprintf(stderr, "probe: HTTP %d %s: %s\n", hp.head().code,
                     hp.head().reason.c_str(), ec_str(status).c_str());
        return EXIT_FAILURE;
    }

    p->reset_stream();
    std::vector<StreamEvent> events;
    const uint64_t t0 = now_mono_ms();
    uint64_t bytes_in = 0;
    bool saw_done = false;
    bool saw_error = false;
    Usage usage;

    SseParser sp(p->stream_kind());
    const error_code se = sse_stream(
        loop, t, hp, deadline, sp,
        [&](const SseEvent& ev) {
            bytes_in += ev.data.size() + ev.event.size() + 1;
            events.clear();
            p->parse_frame(StreamFrame{ev.event, ev.data}, events);
            for (const StreamEvent& e : events) {
                std::printf("  %s\n", describe(e).c_str());
                if (const MessageDone* d = as<MessageDone>(e)) {
                    saw_done = true;
                    usage = d->usage;
                } else if (const ProviderError* err = as<ProviderError>(e)) {
                    saw_error = true;
                    std::fprintf(stderr, "probe: provider error: %s\n",
                                 err->message.c_str());
                }
            }
        });
    if (!se.ok()) {
        std::fprintf(stderr, "probe: stream: %s\n", ec_str(se).c_str());
        return EXIT_FAILURE;
    }
    meter.add_bytes_in(bytes_in);
    meter.add_tokens_in(usage.input_tokens + usage.output_tokens);
    meter.record_rtt(now_mono_ms() - t0);
    meter.inc_round_trip();

    std::printf("probe: stream end (events: %zu frames)\n", sp.events());
    std::printf("probe: usage in=%llu out=%llu cached=%llu\n",
                static_cast<unsigned long long>(usage.input_tokens),
                static_cast<unsigned long long>(usage.output_tokens),
                static_cast<unsigned long long>(usage.cached_input_tokens));
    const Meter::Snapshot s = meter.snapshot();
    std::printf("probe: meter rtt_p50=%llums rtt_p95=%llums round_trips=%u "
                "retries=%u bytes_out=%llu bytes_in=%llu\n",
                static_cast<unsigned long long>(s.rtt_p50),
                static_cast<unsigned long long>(s.rtt_p95), s.round_trips,
                s.retries, static_cast<unsigned long long>(s.bytes_out),
                static_cast<unsigned long long>(s.bytes_in));

    if (mock.ls >= 0) kill_mock(mock);

    if (saw_error) return EXIT_FAILURE;
    if (!saw_done) {
        std::fprintf(stderr, "probe: no MessageDone before stream end\n");
        return EXIT_FAILURE;
    }
    std::printf("probe: clean event stream\n");
    return EXIT_SUCCESS;
}

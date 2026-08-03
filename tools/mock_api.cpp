// mock_api.cpp -- deterministic mock LLM provider (Phase 0).
//
// A minimal HTTP server over stdlib sockets (POSIX) that serves ONE streaming
// SSE chat endpoint. No third-party libraries. Used by Phases 4-11 as the
// wire-testing fixture; Phase 4 extends it with fault-injection scenarios
// (drop frames, 429/5xx, truncation).
//
// Endpoints:
//   GET  /health        -> 200 text/plain "ok\n"
//   POST /v1/chat       -> 200 text/event-stream (scripted SSE events)
//
// Usage:
//   mock_api [--port N] [--response-file PATH] [--listen 127.0.0.1|0.0.0.0]
//            [--scenario NAME]
//
// --response-file: file whose raw bytes become the SSE response body (headers
// are added by the server). Default: a built-in 3-event script.
//
// --scenario NAME: Phase 4 fault injection, applied to every POST /v1/chat:
//   ok            default scripted SSE
//   rate_limit    429 with Retry-After: 1
//   server_error  500 with an empty body
//   truncate      headers claim the full body, then the stream is cut mid-frame
//   garbage       non-HTTP garbage bytes, then close
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
namespace {

constexpr const char* kDefaultScript =
    "data: {\"type\":\"text\",\"text\":\"Hello\"}\r\n"
    "\r\n"
    "data: {\"type\":\"text\",\"text\":\" world\"}\r\n"
    "\r\n"
    "data: {\"type\":\"done\",\"finish_reason\":\"stop\"}\r\n"
    "\r\n";

std::string load_file_or_default(const char* path) {
    if (path == nullptr) return kDefaultScript;
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        std::fprintf(stderr, "mock_api: cannot open %s: %s\n", path,
                     std::strerror(errno));
        std::exit(2);
    }
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

int make_listener(uint16_t port, bool loopback_only) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::fprintf(stderr, "mock_api: socket: %s\n", std::strerror(errno));
        std::exit(2);
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (loopback_only)
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    else
        addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        std::fprintf(stderr, "mock_api: bind %u: %s\n", port,
                     std::strerror(errno));
        std::exit(2);
    }
    if (::listen(fd, 16) < 0) {
        std::fprintf(stderr, "mock_api: listen: %s\n", std::strerror(errno));
        std::exit(2);
    }
    return fd;
}

/* Read the request line + headers ("\r\n\r\n"); body is discarded.
 * Returns the first line (e.g. "GET /health HTTP/1.1") or "" on error. */
std::string read_request(int fd) {
    std::string raw;
    char buf[1024];
    size_t keep = 0;
    while (true) {
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0) break;
        raw.append(buf, static_cast<size_t>(n));
        keep += static_cast<size_t>(n);
        if (raw.find("\r\n\r\n") != std::string::npos) break;
        if (keep > 1u << 20) break; /* runaway request: give up */
    }
    size_t eol = raw.find("\r\n");
    return raw.substr(0, eol);
}

void send_all(int fd, const char* data, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = ::write(fd, data + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        off += static_cast<size_t>(w);
    }
}

void respond(int fd, int status, const char* status_text, const char* content_type,
             const std::string& body) {
    char head[512];
    int n = std::snprintf(head, sizeof head,
                          "HTTP/1.1 %d %s\r\n"
                          "Content-Type: %s\r\n"
                          "Cache-Control: no-cache\r\n"
                          "Connection: close\r\n"
                          "X-Mock-Api: opencodepp\r\n"
                          "Content-Length: %zu\r\n"
                          "\r\n",
                          status, status_text, content_type, body.size());
    send_all(fd, head, static_cast<size_t>(n));
    send_all(fd, body.data(), body.size());
}

/* Route by method + path.
 *   GET  /health -> 200 text/plain "ok\n"
 *   POST /v1/chat -> scripted SSE, or the configured fault scenario
 * Anything else -> 404. */
void handle(int fd, const std::string& script, bool verbose,
            const std::string& scenario) {
    const std::string reqline = read_request(fd);
    std::string method = "GET", path = "/";
    {
        size_t sp1 = reqline.find(' ');
        size_t sp2 = reqline.find(' ', sp1 + 1);
        if (sp1 != std::string::npos && sp2 != std::string::npos) {
            method = reqline.substr(0, sp1);
            path = reqline.substr(sp1 + 1, sp2 - sp1 - 1);
        }
    }

    if (method == "GET" && path == "/health") {
        respond(fd, 200, "OK", "text/plain", "ok\n");
        return;
    }
    if (method == "POST" && path == "/v1/chat") {
        if (scenario == "rate_limit") {
            const char* h = "HTTP/1.1 429 Too Many Requests\r\n"
                            "Retry-After: 1\r\n"
                            "Content-Length: 0\r\n"
                            "Connection: close\r\n"
                            "\r\n";
            send_all(fd, h, std::strlen(h));
            if (verbose)
                std::fprintf(stderr, "mock_api: injected 429\n");
            return;
        }
        if (scenario == "server_error") {
            const char* h = "HTTP/1.1 500 Internal Server Error\r\n"
                            "Content-Length: 0\r\n"
                            "Connection: close\r\n"
                            "\r\n";
            send_all(fd, h, std::strlen(h));
            if (verbose)
                std::fprintf(stderr, "mock_api: injected 500\n");
            return;
        }
        if (scenario == "truncate") {
            char head[256];
            int n = std::snprintf(head, sizeof head,
                                  "HTTP/1.1 200 OK\r\n"
                                  "Content-Type: text/event-stream\r\n"
                                  "Connection: close\r\n"
                                  "Content-Length: %zu\r\n"
                                  "\r\n",
                                  script.size());
            send_all(fd, head, static_cast<size_t>(n));
            send_all(fd, script.data(), script.size() / 2);
            if (verbose)
                std::fprintf(stderr, "mock_api: truncated SSE stream\n");
            return;
        }
        if (scenario == "garbage") {
            send_all(fd, "NOT HTTP\r\n\r\n%^&*garbage\r\n", 24);
            if (verbose)
                std::fprintf(stderr, "mock_api: sent garbage bytes\n");
            return;
        }
        respond(fd, 200, "OK", "text/event-stream", script);
        if (verbose)
            std::fprintf(stderr, "mock_api: served %zu-byte SSE script\n",
                         script.size());
        return;
    }
    respond(fd, 404, "Not Found", "text/plain", "not found\n");
    if (verbose) std::fprintf(stderr, "mock_api: 404 %s %s\n", method.c_str(), path.c_str());
}

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s [--port N] [--response-file PATH] [--listen all]"
                 " [--scenario NAME]\n"
                 "  --port N         listen port (default 8123)\n"
                 "  --response-file  raw SSE body to serve (default: built-in)\n"
                 "  --listen all     bind 0.0.0.0 instead of 127.0.0.1\n"
                 "  --scenario       ok|rate_limit|server_error|truncate|garbage"
                 " (default ok)\n",
                 argv0);
}

} /* namespace */

int main(int argc, char** argv) {
    uint16_t port = 8123;
    const char* response_file = nullptr;
    bool loopback = true;
    bool verbose = true;
    std::string scenario = "ok";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--response-file") == 0 && i + 1 < argc) {
            response_file = argv[++i];
        } else if (std::strcmp(argv[i], "--listen") == 0 && i + 1 < argc &&
                   std::strcmp(argv[i + 1], "all") == 0) {
            loopback = false;
            ++i;
        } else if (std::strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            scenario = argv[++i];
        } else if (std::strcmp(argv[i], "-q") == 0) {
            verbose = false;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    const std::string script = load_file_or_default(response_file);
    const int listener = make_listener(port, loopback);
    std::fprintf(stderr, "mock_api: listening on %s:%u (pid %d)\n",
                 loopback ? "127.0.0.1" : "0.0.0.0", port, ::getpid());

    while (true) {
        int cfd = ::accept(listener, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "mock_api: accept: %s\n", std::strerror(errno));
            break;
        }
        handle(cfd, script, verbose, scenario);
        ::close(cfd);
    }
    ::close(listener);
    return 0;
}

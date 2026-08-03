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
//
// --response-file: file whose raw bytes become the SSE response body (headers
// are added by the server). Default: a built-in 3-event script.
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

/* Read until the end of headers ("\r\n\r\n"); body is discarded. */
void read_request(int fd) {
    char buf[1024];
    size_t keep = 0;
    while (true) {
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0) return;
        keep += static_cast<size_t>(n);
        /* a trailing-scan is enough for a mock: look for the header terminator */
        for (ssize_t i = 0; i + 3 < n; ++i) {
            if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
                buf[i + 3] == '\n')
                return;
        }
        if (keep > 1u << 20) return; /* runaway request: give up */
    }
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

void handle(int fd, const std::string& script, bool verbose) {
    read_request(fd);
    /* Phase 0: we don't parse the request line; every POST gets the script.
       Phase 4 upgrades this to real routing + fault scenarios. */
    respond(fd, 200, "OK", "text/event-stream", script);
    if (verbose) std::fprintf(stderr, "mock_api: served %zu-byte SSE script\n", script.size());
}

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s [--port N] [--response-file PATH] [--listen all]\n"
                 "  --port N         listen port (default 8123)\n"
                 "  --response-file  raw SSE body to serve (default: built-in)\n"
                 "  --listen all     bind 0.0.0.0 instead of 127.0.0.1\n",
                 argv0);
}

} /* namespace */

int main(int argc, char** argv) {
    uint16_t port = 8123;
    const char* response_file = nullptr;
    bool loopback = true;
    bool verbose = true;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--response-file") == 0 && i + 1 < argc) {
            response_file = argv[++i];
        } else if (std::strcmp(argv[i], "--listen") == 0 && i + 1 < argc &&
                   std::strcmp(argv[i + 1], "all") == 0) {
            loopback = false;
            ++i;
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
        handle(cfd, script, verbose);
        ::close(cfd);
    }
    ::close(listener);
    return 0;
}

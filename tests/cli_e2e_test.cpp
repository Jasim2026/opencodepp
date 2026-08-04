// cli_e2e_test.cpp -- Phase 12 commit 3: the reference CLI end-to-end.
//
// Hosts a multi-round SSE mock (round 0: file.write tool call, round 1:
// memory.write tool call, round 2: final text) and drives the real
// `opencode_cli` binary as a subprocess, then asserts the one-shot report and
// the --stats metrics. This keeps opencode_cli itself ABI-only.
//
// Usage: cli_e2e_test <mock_api_path> <opencode_cli_path>
// Runs from the repo root (prompt templates) and creates a scratch workspace
// under /tmp. Never throws.
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

const char* g_cli_path = nullptr;

/* ---------------- tiny SSE server (multi-round) ---------------- */

struct Server {
    int port = 0;
    int listen_fd = -1;
    volatile int stop = 0;
    pthread_t th;
};

constexpr const char* kFrames[3] = {
    /* round 0: file.write through the gate */
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_2\",\"function\":{\"name\":\"file.write\",\"arguments\":\"{\\\"path\\\":\\\"out.txt\\\",\\\"content\\\":\\\"hello cli\\\\n\\\",\\\"create\\\":true}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\n"
    "data: {\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":8}}\n\n",
    /* round 1: memory.write */
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_3\",\"function\":{\"name\":\"memory.write\",\"arguments\":\"{\\\"key\\\":\\\"cli_key\\\",\\\"value\\\":\\\"cli_value\\\",\\\"kind\\\":\\\"fact\\\"}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\n"
    "data: {\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":8}}\n\n",
    /* round 2: final text + stop */
    "data: {\"choices\":[{\"delta\":{\"content\":\"hello cli\"},\"finish_reason\":null}]}\n\n"
    "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
    "data: {\"usage\":{\"prompt_tokens\":40,\"completion_tokens\":2}}\n\n",
};

int ephemeral_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a;
    std::memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    socklen_t gl = sizeof a;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &gl);
    int port = ntohs(a.sin_port);
    ::close(fd);
    return port;
}

std::string read_head(int fd) {
    std::string out;
    char buf[512];
    while (out.find("\r\n\r\n") == std::string::npos) {
        ssize_t r = ::read(fd, buf, sizeof buf);
        if (r <= 0) break;
        out.append(buf, static_cast<size_t>(r));
    }
    return out;
}

void send_all(int fd, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t w = ::write(fd, data + off, len - off);
        if (w <= 0) break;
        off += static_cast<size_t>(w);
    }
}

void* server_loop(void* arg) {
    Server* s = static_cast<Server*>(arg);
    int round = 0;
    while (!s->stop) {
        pollfd pfd;
        pfd.fd = s->listen_fd;
        pfd.events = POLLIN;
        if (::poll(&pfd, 1, 200) <= 0) continue;
        int c = ::accept(s->listen_fd, nullptr, nullptr);
        if (c < 0) continue;
        read_head(c);
        std::string body;
        if (round < 3) body = kFrames[round];
        round = (round + 1) % 3;
        char head[256];
        int n = std::snprintf(head, sizeof head,
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: text/event-stream\r\n"
                              "Connection: close\r\n"
                              "Content-Length: %zu\r\n\r\n",
                              body.size());
        send_all(c, head, static_cast<size_t>(n));
        send_all(c, body.data(), body.size());
        ::close(c);
    }
    ::close(s->listen_fd);
    return nullptr;
}

bool start_server(Server& s) {
    s.port = ephemeral_port();
    s.listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(s.listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a;
    std::memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<uint16_t>(s.port));
    if (::bind(s.listen_fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 ||
        ::listen(s.listen_fd, 8) != 0) {
        std::fprintf(stderr, "cli_e2e_test: bind failed: %s\n", std::strerror(errno));
        return false;
    }
    if (::pthread_create(&s.th, nullptr, &server_loop, &s) != 0) return false;
    return true;
}

void stop_server(Server& s) {
    s.stop = 1;
    ::pthread_join(s.th, nullptr);
}

/* ---------------- subprocess helpers ---------------- */

int run_cli(const std::vector<std::string>& args, std::string& out,
            std::string& err) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(g_cli_path));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    int pout[2], perr[2];
    if (::pipe(pout) != 0 || ::pipe(perr) != 0) return -1;
    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        ::dup2(pout[1], STDOUT_FILENO);
        ::dup2(perr[1], STDERR_FILENO);
        ::close(pout[0]);
        ::close(pout[1]);
        ::close(perr[0]);
        ::close(perr[1]);
        ::execv(g_cli_path, argv.data());
        std::fprintf(stderr, "cli_e2e_test: exec failed: %s\n", std::strerror(errno));
        ::_exit(127);
    }
    ::close(pout[1]);
    ::close(perr[1]);
    char buf[4096];
    ssize_t r;
    while ((r = ::read(pout[0], buf, sizeof buf)) > 0)
        out.append(buf, static_cast<size_t>(r));
    ::close(pout[0]);
    while ((r = ::read(perr[0], buf, sizeof buf)) > 0)
        err.append(buf, static_cast<size_t>(r));
    ::close(perr[0]);
    int st = 0;
    ::waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WEXITSTATUS(st);
}

int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

} /* namespace */

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: cli_e2e_test <opencode_cli_path>\n");
        return 2;
    }
    g_cli_path = argv[1];

    const std::string ws = "/tmp/opencode_cli_ws_e2e";
    const std::string rm = "rm -rf " + ws + " && mkdir -p " + ws;
    std::system(rm.c_str());

    Server srv;
    CHECK(start_server(srv));

    std::string out, err;
    char base[64];
    std::snprintf(base, sizeof base, "http://127.0.0.1:%d", srv.port);

    /* One-shot multi-tool task with --stats. */
    {
        std::vector<std::string> args = {
            "--workspace", ws, "--base", base, "--policy", "allow",
            "--stats", "run", "create out.txt and remember cli_key"};
        out.clear();
        err.clear();
        int rc = run_cli(args, out, err);
        CHECK(rc == 0);
        CHECK(out.find("[done]") != std::string::npos);
        CHECK(out.find("[report] status=ok done=1 failed=0") != std::string::npos);
        CHECK(out.find("metric engine.tasks") != std::string::npos);
        CHECK(out.find("metric engine.iterations") != std::string::npos);
        std::printf("  cli one-shot multi-tool + stats: OK\n");
    }

    /* The gate-passing file.write actually landed. */
    {
        std::string content;
        FILE* f = std::fopen((ws + "/out.txt").c_str(), "rb");
        if (f != nullptr) {
            char buf[256];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
                content.append(buf, n);
            std::fclose(f);
        }
        CHECK(content == "hello cli\n");
        std::printf("  cli applied file.write through the gate: OK\n");
    }

    /* repl mode: run a task, then `exit`. Driven via a stdin redirect. */
    {
        const std::string feed = "write nothing\n"
                                 "exit\n";
        FILE* f = std::fopen("/tmp/opencode_cli_repl_in", "wb");
        if (f != nullptr) {
            std::fwrite(feed.data(), 1, feed.size(), f);
            std::fclose(f);
        }
        std::string cmd = std::string(g_cli_path) + " --workspace " + ws +
                          " --base " + base +
                          " repl < /tmp/opencode_cli_repl_in";
        std::string captured;
        char buf[4096];
        FILE* pipe = ::popen(cmd.c_str(), "r");
        if (pipe != nullptr) {
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, pipe)) > 0)
                captured.append(buf, n);
            int prc = ::pclose(pipe);
            CHECK(WIFEXITED(prc) && WEXITSTATUS(prc) == 0);
        } else {
            CHECK(false);
        }
        CHECK(captured.find("opencode>") != std::string::npos);
        CHECK(captured.find("[report] status=ok") != std::string::npos);
        std::printf("  cli repl loop: OK\n");
    }

    stop_server(srv);
    std::system(("rm -rf " + ws).c_str());

    if (g_failures != 0) {
        std::fprintf(stderr, "cli_e2e_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("cli_e2e_test: all sections OK\n");
    return 0;
}

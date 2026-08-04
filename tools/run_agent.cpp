// run_agent.cpp -- CLI driver for the Phase 10 agent loop.
//
// Builds a Session + Agent from CLI/config input and runs one task through the
// full pipeline: intent -> context -> prompt -> cloud call -> verify-gated
// tools. `--mock` starts an in-process OpenAI-compat SSE server so the whole
// path runs offline and in CI (same pattern as tools/probe --mock).
// Never throws.
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include "agent/loop.h"
#include "agent/session.h"
#include "core/error.h"
#include "core/event_loop.h"
#include "prompt/registry.h"
#include "tools/permission.h"
#include "tools/registry.h"
#include "verify/gate.h"

namespace {

using namespace opencode;

struct Args {
    std::string prompt = "List the files in the workspace and report.";
    std::string provider_id = "openai_compat";
    std::string base = "http://127.0.0.1:8080";
    std::string key = "sk-test";
    std::string model = "mock-model";
    std::string agent = "default";
    std::string workspace = "/tmp/opencode_agent_ws";
    tools::Policy policy = tools::Policy::allow_readonly;
    bool mock = false;
};

void usage() {
    std::fprintf(stderr,
                 "usage: run_agent [--prompt TEXT] [--base URL] [--key KEY]\n"
                 "                  [--model NAME] [--provider ID]\n"
                 "                  [--workspace DIR] [--policy "
                 "allow|readonly|ask|deny]\n"
                 "                  [--mock]\n");
}

/* In-process OpenAI-compat SSE server (plaintext; TLS is Phase 12/13). */
struct FakeServer {
    std::shared_ptr<std::atomic<bool>> stop =
        std::make_shared<std::atomic<bool>>(false);
    std::thread th;
    int port = 0;
    FakeServer() = default;
    FakeServer(FakeServer&& o) noexcept
        : stop(std::move(o.stop)), th(std::move(o.th)), port(o.port) {}
    FakeServer(const FakeServer&) = delete;
    FakeServer& operator=(const FakeServer&) = delete;
    ~FakeServer() {
        if (stop) stop->store(true);
        if (th.joinable()) th.join();
    }
};

int ephemeral_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) {
        ::close(fd);
        return 0;
    }
    sockaddr_in got{};
    socklen_t gl = sizeof got;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&got), &gl);
    const int port = ntohs(got.sin_port);
    ::close(fd);
    return port;
}

std::string read_head(int fd) {
    std::string out;
    char buf[1024];
    while (true) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
        if (out.find("\r\n\r\n") != std::string::npos) break;
    }
    return out;
}

std::string sse_resp(const std::vector<std::string>& frames) {
    std::string body;
    for (const std::string& f : frames) body += "data: " + f + "\r\n\r\n";
    char head[256];
    std::snprintf(head, sizeof head,
                  "HTTP/1.1 200 OK\r\n"
                  "Content-Type: text/event-stream\r\n"
                  "Transfer-Encoding: chunked\r\n"
                  "Connection: close\r\n\r\n");
    std::string out = head;
    auto hex = [](size_t n) {
        char b[32];
        std::snprintf(b, sizeof b, "%zx", n);
        return std::string(b);
    };
    out += hex(body.size()) + "\r\n" + body + "\r\n0\r\n\r\n";
    return out;
}

FakeServer start_server(const std::function<std::string(int)>& h) {
    const int port = ephemeral_port();
    const int ls = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 ||
        ::listen(ls, 8) != 0) {
        ::close(ls);
        return FakeServer{};
    }
    FakeServer s;
    s.port = port;
    std::atomic<int>* reqs = new std::atomic<int>(0);
    s.th = std::thread([ls, stop = s.stop, h, reqs]() {
        while (!stop->load()) {
            struct pollfd pfd{};
            pfd.fd = ls;
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, 200) <= 0) continue;
            const int c = ::accept(ls, nullptr, nullptr);
            if (c < 0) continue;
            const std::string req = read_head(c);
            const std::string resp = h(reqs->fetch_add(1));
            size_t off = 0;
            while (off < resp.size()) {
                const ssize_t w =
                    ::write(c, resp.data() + off, resp.size() - off);
                if (w <= 0) break;
                off += static_cast<size_t>(w);
            }
            ::close(c);
        }
        ::close(ls);
        delete reqs;
    });
    return s;
}

void on_event(void* /*userdata*/, const agent::AgentEvent& ev) {
    std::fprintf(stdout, "  [event] %s\n",
                 agent::format_event(ev).c_str());
}

int run(const Args& a) {
    ::system(("rm -rf " + a.workspace + " && mkdir -p " + a.workspace).c_str());

    config::Config cfg;
    config::ProviderCfg pc;
    pc.id = a.provider_id;
    pc.base_url = a.base;
    pc.api_key = a.key;
    cfg.providers.push_back(pc);
    config::AgentCfg ac;
    ac.id = a.agent;
    ac.model = a.model;
    cfg.agents.push_back(ac);
    cfg.network.timeout_ms = 20'000;

    agent::SessionOptions sopts;
    if (const core::error_code ec =
            agent::session_options_from_config(cfg, a.agent, a.workspace, sopts);
        !ec.ok()) {
        std::fprintf(stderr, "run_agent: bad config: %d\n",
                     static_cast<int>(ec.code()));
        return 1;
    }

    prompt::PromptRegistry prompts;
    bool loaded = false;
    for (const char* dir :
         {"src/prompt/templates", "../src/prompt/templates",
          "opencodepp/src/prompt/templates"}) {
        if (prompt::load_templates(dir, prompts).ok()) {
            loaded = true;
            break;
        }
    }
    if (!loaded) {
        std::fprintf(stderr, "run_agent: templates not found (run from the "
                             "repo root)\n");
        return 1;
    }

    tools::ToolRegistry reg;
    if (const core::error_code ec =
            tools::register_defaults(reg, {a.workspace, nullptr, true});
        !ec.ok()) {
        std::fprintf(stderr, "run_agent: tool registration failed: %d\n",
                     static_cast<int>(ec.code()));
        return 1;
    }
    tools::Gate perm(a.policy);
    verify::Gate vgate;
    verify::Context vctx;
    vctx.workspace_root = a.workspace;

    agent::Session session(sopts);
    session.set_event_fn(on_event, nullptr);
    core::EventLoop loop;
    agent::LoopOptions lo;
    lo.loop = &loop;
    lo.prompt = &prompts;
    lo.tools = &reg;
    lo.permission = &perm;
    lo.verify = &vgate;
    lo.verify_ctx = vctx;
    lo.retry.max_retries = 2;

    agent::Agent agent(session, lo);
    const agent::DriveResult r = agent.drive(a.prompt);

    std::fprintf(stdout, "  drive: ec=%d iterations=%u tokens=%llu\n",
                 static_cast<int>(r.ec.code()), r.iterations,
                 static_cast<unsigned long long>(r.tokens_used));
    if (!r.summary.empty())
        std::fprintf(stdout, "  summary: %s\n", r.summary.c_str());
    for (const std::string& e : r.applied_edits)
        std::fprintf(stdout, "  edit: %s\n", e.c_str());
    for (const std::string& f : r.feedback)
        std::fprintf(stdout, "  feedback: %s\n", f.c_str());
    if (!r.ec.ok())
        std::fprintf(stdout, "  final state: %.*s\n",
                     static_cast<int>(agent::to_string(session.state()).size()),
                     agent::to_string(session.state()).data());
    return r.ec.ok() ? 0 : 1;
}

} /* namespace */

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "run_agent: --%s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--prompt") {
            a.prompt = need("prompt");
        } else if (arg == "--base") {
            a.base = need("base");
        } else if (arg == "--key") {
            a.key = need("key");
        } else if (arg == "--model") {
            a.model = need("model");
        } else if (arg == "--provider") {
            a.provider_id = need("provider");
        } else if (arg == "--workspace") {
            a.workspace = need("workspace");
        } else if (arg == "--policy") {
            const std::string p = need("policy");
            if (p == "allow")
                a.policy = tools::Policy::allow;
            else if (p == "readonly")
                a.policy = tools::Policy::allow_readonly;
            else if (p == "ask")
                a.policy = tools::Policy::ask;
            else if (p == "deny")
                a.policy = tools::Policy::deny;
            else {
                usage();
                return 2;
            }
        } else if (arg == "--mock") {
            a.mock = true;
        } else if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else {
            usage();
            return 2;
        }
    }

    if (a.mock) {
        /* One text-only round against the in-process fake server. The server
         * must outlive the whole drive, so run() happens inside this scope. */
        auto srv = start_server([](int i) {
            if (i == 0)
                return sse_resp({
                    "{\"choices\":[{\"delta\":{\"content\":\"Hello \"},\"finish_reason\":null}]}",
                    "{\"choices\":[{\"delta\":{\"content\":\"world\"},\"finish_reason\":null}]}",
                    "{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":12,\"completion_tokens\":3}}",
                });
            return sse_resp({
                "{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}"});
        });
        a.base = "http://127.0.0.1:" + std::to_string(srv.port);
        a.prompt = "tell me a greeting please";
        return run(a);
    }

    return run(a);
}

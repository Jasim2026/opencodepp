/*
 * agent_loop_test.cpp -- Phase 10 commit 3: end-to-end loop scenarios.
 *
 * Drives Agent::drive() against an in-process fake OpenAI-compat SSE server:
 *   A. text-only round (usage accounting, terminal event emission)
 *   B. read-tool round (file.read then final text)
 *   C. write round through the verify gate (file actually created)
 *   D. provider-level error over a clean 200 ({"error":{...}} frame)
 *
 * No real network is used; the server binds 127.0.0.1 on an ephemeral port.
 * Runs from the repo root (see tests/CMakeLists.txt).
 */
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "agent/loop.h"
#include "agent/session.h"
#include "core/event_loop.h"
#include "prompt/registry.h"
#include "provider/provider.h"
#include "tools/permission.h"
#include "tools/registry.h"
#include "verify/gate.h"

namespace {
using namespace opencode;

int failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,   \
                         #cond);                                           \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

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

struct Sink {
    std::vector<agent::AgentEvent> events;
};
void on_event(void* userdata, const agent::AgentEvent& ev) {
    auto* s = static_cast<Sink*>(userdata);
    s->events.push_back(ev);
}

/* Build everything needed for one drive() invocation. */
struct Rig {
    config::Config cfg;
    prompt::PromptRegistry prompts;
    tools::ToolRegistry reg;
    tools::Gate perm;
    verify::Gate vgate;
    verify::Context vctx;
    core::EventLoop loop;
    std::string ws;

    Rig(int port, std::string workspace, tools::Policy pol)
        : perm(pol), ws(std::move(workspace)) {
        config::ProviderCfg pc;
        pc.id = "openai_compat";
        pc.base_url = "http://127.0.0.1:" + std::to_string(port);
        pc.api_key = "test";
        cfg.providers.push_back(pc);
        config::AgentCfg ac;
        ac.id = "default";
        ac.model = "mock-model";
        cfg.agents.push_back(ac);
        cfg.network.timeout_ms = 8000;

        bool loaded = false;
        for (const char* dir :
             {"src/prompt/templates", "../src/prompt/templates"}) {
            if (prompt::load_templates(dir, prompts).ok()) {
                loaded = true;
                break;
            }
        }
        CHECK(loaded);
        CHECK(tools::register_defaults(reg, {ws, nullptr, true}).ok());
        vctx.workspace_root = ws;
    }

    agent::LoopOptions opts() {
        agent::LoopOptions lo;
        lo.loop = &loop;
        lo.prompt = &prompts;
        lo.tools = &reg;
        lo.permission = &perm;
        lo.verify = &vgate;
        lo.verify_ctx = vctx;
        lo.retry.max_retries = 0;
        return lo;
    }
};

} /* namespace */

int main() {
    const std::string ws = "/tmp/opencode_agent_ws";
    ::system("rm -rf /tmp/opencode_agent_ws && "
             "mkdir -p /tmp/opencode_agent_ws");

    /* ---- scenario A: text-only round ---- */
    {
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
        Rig fx(srv.port, ws, tools::Policy::allow_readonly);
        agent::SessionOptions sopts;
        CHECK(agent::session_options_from_config(fx.cfg, "default", ws, sopts).ok());
        agent::Session session(sopts);
        Sink sink;
        session.set_event_fn(on_event, &sink);
        agent::Agent a(session, fx.opts());
        const agent::DriveResult r = a.drive("tell me a greeting please");
        CHECK(r.ec.ok());
        CHECK(r.iterations == 1);
        CHECK(r.summary == "Hello world");
        CHECK(session.state() == agent::AgentState::done);
        CHECK(session.tokens_used() == 15);
        bool saw_done = false;
        for (const auto& e : sink.events)
            if (e.state == agent::AgentState::done) saw_done = true;
        CHECK(saw_done);
        std::printf("  loop A (text-only): OK\n");
    }

    /* ---- scenario B: read-tool round then final text ---- */
    {
        ::system("printf 'hello from notes\\n' > "
                 "/tmp/opencode_agent_ws/notes.txt");
        auto srv = start_server([](int i) {
            if (i == 0)
                return sse_resp({
                    "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"function\":{\"name\":\"file.read\",\"arguments\":\"{\\\"path\\\":\\\"notes.txt\\\"}\"}}]},\"finish_reason\":\"tool_calls\"}]}",
                    "{\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":8}}",
                });
            return sse_resp({
                "{\"choices\":[{\"delta\":{\"content\":\"read it\"},\"finish_reason\":null}]}",
                "{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}",
                "{\"usage\":{\"prompt_tokens\":40,\"completion_tokens\":2}}",
            });
        });
        Rig fx(srv.port, ws, tools::Policy::allow_readonly);
        agent::SessionOptions sopts;
        CHECK(agent::session_options_from_config(fx.cfg, "default", ws, sopts).ok());
        agent::Session session(sopts);
        Sink sink;
        session.set_event_fn(on_event, &sink);
        agent::Agent a(session, fx.opts());
        const agent::DriveResult r = a.drive("read notes.txt please");
        CHECK(r.ec.ok());
        CHECK(r.iterations == 2);
        CHECK(r.summary == "read it");
        CHECK(r.applied_edits.empty());
        /* user, assistant(tool_call), tool-result(user), assistant(final) */
        CHECK(session.messages().size() == 4);
        CHECK(session.messages()[2].tool_results().size() == 1);
        CHECK(session.messages()[3].content_text() == "read it");
        std::printf("  loop B (read-tool round): OK\n");
    }

    /* ---- scenario C: write through the gate ---- */
    {
        auto srv = start_server([](int i) {
            if (i == 0)
                return sse_resp({
                    "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_2\",\"function\":{\"name\":\"file.write\",\"arguments\":\"{\\\"path\\\":\\\"out.txt\\\",\\\"content\\\":\\\"hello agent\\\\n\\\",\\\"create\\\":true}\"}}]},\"finish_reason\":\"tool_calls\"}]}",
                    "{\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":8}}",
                });
            return sse_resp({
                "{\"choices\":[{\"delta\":{\"content\":\"wrote out.txt\"},\"finish_reason\":null}]}",
                "{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}",
                "{\"usage\":{\"prompt_tokens\":40,\"completion_tokens\":2}}",
            });
        });
        Rig fx(srv.port, ws, tools::Policy::allow);
        agent::SessionOptions sopts;
        CHECK(agent::session_options_from_config(fx.cfg, "default", ws, sopts).ok());
        agent::Session session(sopts);
        Sink sink;
        session.set_event_fn(on_event, &sink);
        agent::Agent a(session, fx.opts());
        const agent::DriveResult r = a.drive("create out.txt in the workspace");
        CHECK(r.ec.ok());
        CHECK(r.iterations == 2);
        CHECK(r.applied_edits.size() == 1);
        std::string out;
        FILE* f = std::fopen("/tmp/opencode_agent_ws/out.txt", "rb");
        if (f != nullptr) {
            char buf[256];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
                out.append(buf, n);
            std::fclose(f);
        }
        CHECK(out == "hello agent\n");
        std::printf("  loop C (write through gate): OK\n");
    }

    /* ---- scenario D: provider-level error over a clean 200 ---- */
    {
        auto srv = start_server([](int) {
            return sse_resp({
                "{\"choices\":[{\"delta\":{\"content\":\"partial\"},\"finish_reason\":null}]}",
                "{\"error\":{\"message\":\"overloaded\",\"type\":\"server_error\"}}",
            });
        });
        Rig fx(srv.port, ws, tools::Policy::allow_readonly);
        agent::SessionOptions sopts;
        CHECK(agent::session_options_from_config(fx.cfg, "default", ws, sopts).ok());
        agent::Session session(sopts);
        agent::Agent a(session, fx.opts());
        const agent::DriveResult r = a.drive("say hi");
        CHECK(!r.ec.ok());
        CHECK(r.ec == core::Err::e_provider_err);
        CHECK(r.iterations == 0);
        CHECK(r.applied_edits.empty());
        std::printf("  loop D (provider error): OK\n");
    }

    if (failures == 0) std::printf("agent_loop_test: all OK\n");
    return failures == 0 ? 0 : 1;
}

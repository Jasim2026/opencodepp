// opencode_cli.cpp -- the Phase 12 reference host.
//
// A small CLI that exercises the public C ABI (include/opencode/opencode.h)
// through the C++ RAII wrapper (src/abi/opencode.hpp). It is the smoke/QA
// surface for Phases 13-14 and never touches engine internals.
//
//   opencode_cli [flags] run "task"   one-shot task; prints the final report
//   opencode_cli [flags] repl         interactive loop; prints every event
//
// Flags:
//   --config PATH     load an opencodepp JSON config file first
//   --workspace DIR   tool/gate sandbox (default /tmp/opencode_cli_ws)
//   --base URL        provider base URL (default http://127.0.0.1:8123)
//   --key KEY         provider API key
//   --model NAME      model id
//   --agent NAME      agent profile id
//   --policy P        allow | readonly | ask | deny (default readonly)
//   --stats           after a run, print the engine metrics snapshot
//
// Cancel: SIGINT (Ctrl-C) cancels the in-flight run (the only thread-safe
// entry). In repl mode the current run is cancelled; the next prompt returns.
// Never throws.
#define _POSIX_C_SOURCE 200809L

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <unistd.h>

#include "abi/opencode.hpp"

namespace {

using namespace opencode;

struct Args {
    std::string config_path;
    std::string workspace = "/tmp/opencode_cli_ws";
    std::string base = "http://127.0.0.1:8123";
    std::string key = "sk-test";
    std::string model = "mock-model";
    std::string agent = "default";
    opencode_tool_policy_t policy = OPENCODE_POLICY_ALLOW_READONLY;
    bool stats = false;
};

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s [flags] (run \"task\" | repl)\n"
                 "  --config PATH     load an opencodepp JSON config file\n"
                 "  --workspace DIR   tool/gate sandbox (default /tmp/opencode_cli_ws)\n"
                 "  --base URL        provider base URL (default http://127.0.0.1:8123)\n"
                 "  --key KEY         provider API key\n"
                 "  --model NAME      model id\n"
                 "  --agent NAME      agent profile id\n"
                 "  --policy P        allow | readonly | ask | deny (default readonly)\n"
                  "  --stats           print the metrics snapshot after a run\n"
                 "  --version         print version and ABI version, then exit\n"
                 "  Ctrl-C cancels the in-flight run.\n",
                 argv0);
}

const char* policy_name(opencode_tool_policy_t p) {
    switch (p) {
        case OPENCODE_POLICY_DENY: return "deny";
        case OPENCODE_POLICY_ASK: return "ask";
        case OPENCODE_POLICY_ALLOW: return "allow";
        default: return "readonly";
    }
}

const char* status_name(opencode_status_t s) {
    switch (s) {
        case OPENCODE_OK: return "ok";
        case OPENCODE_ERR_NETWORK: return "network error";
        case OPENCODE_ERR_AUTH: return "auth error";
        case OPENCODE_ERR_VALIDATION: return "validation error";
        case OPENCODE_ERR_BUSY: return "busy";
        case OPENCODE_ERR_CANCELLED: return "cancelled";
        case OPENCODE_ERR_FATAL: return "fatal";
        case OPENCODE_ERR_NO_NETWORK: return "offline";
        default: return "unknown";
    }
}

const char* kind_name(opencode_event_kind_t k) {
    switch (k) {
        case OPENCODE_EVENT_LOG: return "log";
        case OPENCODE_EVENT_PREPARING: return "preparing";
        case OPENCODE_EVENT_CONNECTING: return "connecting";
        case OPENCODE_EVENT_STREAMING: return "streaming";
        case OPENCODE_EVENT_TOOL_PHASE: return "tool";
        case OPENCODE_EVENT_VERIFYING: return "verifying";
        case OPENCODE_EVENT_APPLYING: return "applying";
        case OPENCODE_EVENT_DONE: return "done";
        case OPENCODE_EVENT_FAILED: return "failed";
        case OPENCODE_EVENT_CANCELLED: return "cancelled";
        case OPENCODE_EVENT_FOLD: return "fold";
        default: return "?";
    }
}

/* Event sink: render each phase on one line. userdata = a status word tracker. */
struct RunRec {
    opencode_status_t final_status = OPENCODE_OK;
    bool saw_done = false;
    bool saw_failed = false;
};

opencode_status_t on_event(void* userdata, const opencode_event_t* ev) {
    RunRec* rec = static_cast<RunRec*>(userdata);
    char text[OPENCODE_EVENT_TEXT_MAX] = {0};
    const char* txt = "";
    if (ev->text != nullptr && ev->text_len > 0) {
        size_t n = ev->text_len < sizeof text - 1 ? ev->text_len : sizeof text - 1;
        std::memcpy(text, ev->text, n);
        text[n] = '\0';
        txt = text;
    }
    std::printf("[%s] %s\n", kind_name(ev->kind), txt);
    if (ev->kind == OPENCODE_EVENT_DONE) {
        rec->saw_done = true;
        rec->final_status = OPENCODE_OK;
    } else if (ev->kind == OPENCODE_EVENT_FAILED) {
        rec->saw_failed = true;
        rec->final_status = OPENCODE_ERR_FATAL;
    } else if (ev->kind == OPENCODE_EVENT_CANCELLED) {
        rec->final_status = OPENCODE_ERR_CANCELLED;
    }
    return OPENCODE_OK;
}

void on_metric(void* userdata, const char* name, opencode_metric_kind_t kind,
               double value, uint64_t count) {
    (void)userdata;
    (void)kind;
    (void)count;
    std::printf("metric %s = %g\n", name, value);
}

int on_permission(void* userdata, const char* tool, const char* params_json) {
    (void)userdata;
    std::fprintf(stderr, "[permission] %s %s\n", tool, params_json);
    if (isatty(STDIN_FILENO) == 0) return 0; /* non-interactive: deny */
    std::fprintf(stderr, "allow? [y/N] ");
    std::fflush(stderr);
    char line[64] = {0};
    if (std::fgets(line, sizeof line, stdin) == nullptr) return 0;
    return (line[0] == 'y' || line[0] == 'Y') ? 1 : 0;
}

/* Engine pointer visible to the SIGINT handler. */
abi::Engine* g_engine = nullptr;

extern "C" void on_sigint(int) {
    if (g_engine != nullptr && g_engine->valid()) g_engine->cancel();
}

opencode_config_t build_config(const Args& a) {
    opencode_config_t c;
    std::memset(&c, 0, sizeof c);
    c.version = OPENCODE_CONFIG_VERSION;
    c.workspace = a.workspace.c_str();
    if (!a.config_path.empty()) c.config_path = a.config_path.c_str();
    c.provider = "openai_compat";
    c.base_url = a.base.c_str();
    c.api_key = a.key.c_str();
    c.model = a.model.c_str();
    c.agent = a.agent.c_str();
    c.network_timeout_ms = 30'000;
    c.tool_policy = a.policy;
    c.on_event = &on_event;
    c.on_permission = &on_permission;
    return c;
}

int run_one(abi::Engine& engine, const char* prompt) {
    RunRec rec;
    const opencode_status_t s = engine.run(prompt, &on_event, &rec);
    std::printf("[report] status=%s done=%d failed=%d\n", status_name(s),
                rec.saw_done ? 1 : 0, rec.saw_failed ? 1 : 0);
    return s == OPENCODE_OK && rec.saw_done ? 0 : 1;
}

} /* namespace */

int main(int argc, char** argv) {
    Args a;
    std::string mode;
    std::string task;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        auto need = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "opencode_cli: %s needs a value\n", what);
                usage(argv[0]);
                std::exit(2);
            }
            return argv[++i];
        };
        if (std::strcmp(arg, "--config") == 0) {
            a.config_path = need("--config");
        } else if (std::strcmp(arg, "--workspace") == 0) {
            a.workspace = need("--workspace");
        } else if (std::strcmp(arg, "--base") == 0) {
            a.base = need("--base");
        } else if (std::strcmp(arg, "--key") == 0) {
            a.key = need("--key");
        } else if (std::strcmp(arg, "--model") == 0) {
            a.model = need("--model");
        } else if (std::strcmp(arg, "--agent") == 0) {
            a.agent = need("--agent");
        } else if (std::strcmp(arg, "--policy") == 0) {
            const char* p = need("--policy");
            if (std::strcmp(p, "allow") == 0) a.policy = OPENCODE_POLICY_ALLOW;
            else if (std::strcmp(p, "readonly") == 0) a.policy = OPENCODE_POLICY_ALLOW_READONLY;
            else if (std::strcmp(p, "ask") == 0) a.policy = OPENCODE_POLICY_ASK;
            else if (std::strcmp(p, "deny") == 0) a.policy = OPENCODE_POLICY_DENY;
            else {
                std::fprintf(stderr, "opencode_cli: bad --policy %s\n", p);
                usage(argv[0]);
                return 2;
            }
        } else if (std::strcmp(arg, "--stats") == 0) {
            a.stats = true;
        } else if (std::strcmp(arg, "--version") == 0 ||
                   std::strcmp(arg, "-v") == 0) {
            std::printf("opencodepp %u.%u.%u (abi %u, config %u, event %u)\n",
                        OPENCODE_VERSION_MAJOR, OPENCODE_VERSION_MINOR,
                        OPENCODE_VERSION_PATCH, OPENCODE_ABI_VERSION,
                        OPENCODE_CONFIG_VERSION, OPENCODE_EVENT_VERSION);
            return 0;
        } else if (std::strcmp(arg, "run") == 0) {
            mode = "run";
            if (i + 1 < argc) task = argv[++i];
        } else if (std::strcmp(arg, "repl") == 0) {
            mode = "repl";
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (mode.empty() || (mode == "run" && task.empty())) {
        usage(argv[0]);
        return 2;
    }

    std::string mk = "mkdir -p " + a.workspace;
    if (std::system(mk.c_str()) != 0) {
        std::fprintf(stderr, "opencode_cli: cannot create %s\n",
                     a.workspace.c_str());
        return 1;
    }

    const opencode_config_t cfg = build_config(a);
    abi::Engine engine(&cfg);
    if (!engine.valid()) {
        std::fprintf(stderr, "opencode_cli: engine create failed\n");
        return 1;
    }
    g_engine = &engine;
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_handler = &on_sigint;
    sigaction(SIGINT, &sa, nullptr);

    std::printf("[engine] provider=openai_compat policy=%s workspace=%s\n",
                policy_name(a.policy), a.workspace.c_str());

    int rc = 0;
    if (mode == "run") {
        rc = run_one(engine, task.c_str());
        if (a.stats) engine.metrics(&on_metric, nullptr);
    } else {
        std::printf("opencode> type a task, or `exit`. Ctrl-C cancels the run.\n");
        char line[8192];
        for (;;) {
            std::printf("opencode> ");
            std::fflush(stdout);
            if (std::fgets(line, sizeof line, stdin) == nullptr) break;
            line[std::strcspn(line, "\n")] = '\0';
            if (std::strcmp(line, "exit") == 0) break;
            if (line[0] == '\0') continue;
            const int r = run_one(engine, line);
            if (a.stats) engine.metrics(&on_metric, nullptr);
            if (r != 0) rc = r; /* remember failures but keep the repl alive */
        }
    }

    g_engine = nullptr;
    return rc;
}

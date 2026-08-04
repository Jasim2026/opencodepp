// soak.cpp -- Phase 13 long-run resilience driver.
//
// Runs the real `run_agent --mock` binary repeatedly against scripted task
// prompts, with a per-task watchdog, child RSS sampling, and transient faults
// injected every K tasks (the retry/backoff path). It is the resilience half
// of the T2/T3 gate:
//
//   no hangs (every task finishes before its watchdog)
//   no aborts (WIFSIGNALED, or an unclean path)
//   graceful provider errors are counted, not failures
//   no unbounded RSS growth across the run (max-min under a slack bound)
//
// Never throws. Usage:
//   soak [--bin PATH] [--tasks N] [--task-timeout SECS] [--fault-every K]
//        [--workspace DIR] [--verbose]
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct Args {
    std::string bin = "run_agent";
    int tasks = 10;
    int task_timeout_s = 30;
    int fault_every = 4; /* 0 = no fault injection */
    std::string workspace = "/tmp/opencode_soak_ws";
    bool verbose = false;
};

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s [--bin PATH] [--tasks N] [--task-timeout SECS]\n"
                 "             [--fault-every K] [--workspace DIR] [--verbose]\n"
                 "  --tasks N         scripted tasks to run (default 10)\n"
                 "  --task-timeout S  per-task watchdog in seconds (default 30)\n"
                 "  --fault-every K   inject a transient mock fault every Kth\n"
                 "                    task, alternating 500/truncate (default 4;\n"
                 "                    0 disables)\n"
                 "  Exits non-zero on any hang, abort, or unbounded RSS growth.\n",
                 argv0);
}

/* Scripted task prompts: same shapes the golden suite uses, so the mock loop
 * takes a variety of intent-classified turns. */
const char* kTasks[] = {
    "List the files in the workspace and report what you see.",
    "Fix the failing test in src/core/loop.cpp.",
    "Refactor tools/registry.cpp to use unique_ptr everywhere.",
    "Explain what the channel does in the codebase.",
    "Add a regression test for the mock SSE parser.",
    "Find the symbol foo in the workspace.",
    "Run the tests and report.",
    "Write a comment explaining the retry policy.",
};

int kTaskCount = static_cast<int>(sizeof kTasks / sizeof kTasks[0]);

long child_rss_kb(int pid) {
    char path[64];
    std::snprintf(path, sizeof path, "/proc/%d/status", pid);
    FILE* f = std::fopen(path, "r");
    if (f == nullptr) return -1;
    char line[256];
    long v = -1;
    while (std::fgets(line, sizeof line, f) != nullptr) {
        if (std::sscanf(line, "VmRSS: %ld kB", &v) == 1) break;
    }
    std::fclose(f);
    return v;
}

int spawn(const Args& a, int task_idx, int fault) {
    const pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* child: exec run_agent --mock with the scripted prompt */
        std::string prompt = kTasks[task_idx % kTaskCount];
        std::vector<std::string> argv = {
            a.bin, "--mock", "--prompt", prompt, "--workspace", a.workspace};
        if (fault > 0) {
            argv.push_back("--fault");
            argv.push_back(std::to_string(fault));
        }
        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (auto& s : argv) cargv.push_back(s.data());
        cargv.push_back(nullptr);
        ::execvp(cargv[0], cargv.data());
        std::fprintf(stderr, "soak: exec %s failed: %s\n", a.bin.c_str(),
                     std::strerror(errno));
        std::_Exit(127);
    }
    return static_cast<int>(pid);
}

} /* namespace */

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "soak: %s needs a value\n", what);
                usage(argv[0]);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--bin") {
            a.bin = need("--bin");
        } else if (arg == "--tasks") {
            a.tasks = std::atoi(need("--tasks"));
            if (a.tasks < 1) a.tasks = 1;
        } else if (arg == "--task-timeout") {
            a.task_timeout_s = std::atoi(need("--task-timeout"));
            if (a.task_timeout_s < 1) a.task_timeout_s = 1;
        } else if (arg == "--fault-every") {
            a.fault_every = std::atoi(need("--fault-every"));
            if (a.fault_every < 0) a.fault_every = 0;
        } else if (arg == "--workspace") {
            a.workspace = need("--workspace");
        } else if (arg == "--verbose") {
            a.verbose = true;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    int passes = 0;
    int graceful_errors = 0;
    int aborts = 0;
    int hangs = 0;
    std::vector<long> rss_ends; /* steady-state RSS of each completed task */
    const long t0 = ::time(nullptr);

    std::printf("soak: bin=%s tasks=%d timeout=%ds fault_every=%d\n",
                a.bin.c_str(), a.tasks, a.task_timeout_s, a.fault_every);

    int faults[] = {1, 2};
    int fault_idx = 0;
    for (int task = 0; task < a.tasks; ++task) {
        const int fault =
            (a.fault_every > 0 && (task + 1) % a.fault_every == 0)
                ? faults[fault_idx++ % 2]
                : 0;
        const int pid = spawn(a, task, fault);
        if (pid < 0) {
            std::fprintf(stderr, "soak: fork failed\n");
            return 1;
        }
        if (a.verbose)
            std::printf("  task %2d pid=%d fault=%s\n", task + 1, pid,
                        fault > 0 ? (fault == 1 ? "500" : "truncate") : "none");

        /* watchdog loop: poll the child, sample RSS, honor the timeout. */
        bool hung = false;
        int status = 0;
        long rss_last = -1;
        const long deadline = ::time(nullptr) + a.task_timeout_s;
        for (;;) {
            const pid_t w = ::waitpid(pid, &status, WNOHANG);
            if (w == pid) break;
            if (w < 0) break; /* ECHILD: already reaped elsewhere */
            const long rss = child_rss_kb(pid);
            if (rss > 0) rss_last = rss; /* keep the newest steady-state sample */
            if (::time(nullptr) >= deadline) {
                ::kill(pid, SIGKILL);
                ::waitpid(pid, nullptr, 0);
                hung = true;
                break;
            }
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 100'000'000; /* poll every 100 ms */
            ::nanosleep(&ts, nullptr);
        }

        if (hung) {
            ++hangs;
            std::fprintf(stderr, "  task %2d: HANG (killed)\n", task + 1);
            continue;
        }
        if (WIFEXITED(status)) {
            if (WEXITSTATUS(status) == 0) {
                ++passes;
            } else if (WEXITSTATUS(status) == 127) {
                ++aborts;
                std::fprintf(stderr, "  task %2d: EXEC FAILURE\n", task + 1);
            } else {
                ++graceful_errors;
                if (a.verbose)
                    std::printf("  task %2d: graceful error (exit %d)\n",
                                task + 1, WEXITSTATUS(status));
            }
        } else if (WIFSIGNALED(status)) {
            ++aborts;
            std::fprintf(stderr, "  task %2d: ABORT (signal %d)\n", task + 1,
                         WTERMSIG(status));
        } else {
            ++aborts;
            std::fprintf(stderr, "  task %2d: ABORT (unknown exit)\n", task + 1);
        }
        if (rss_last > 0) rss_ends.push_back(rss_last);
    }

    const long elapsed = ::time(nullptr) - t0;
    /* Each task is a fresh process, so its end-of-task RSS is a steady-state
     * footprint sample; growth across tasks signals a leaking per-task
     * footprint (e.g. workspace accumulation), not intra-run ASan warmup. */
    long rss_min = -1, rss_max = -1;
    for (const long v : rss_ends) {
        if (rss_min < 0 || v < rss_min) rss_min = v;
        if (rss_max < 0 || v > rss_max) rss_max = v;
    }
    const long rss_growth = (rss_min >= 0 && rss_max >= 0)
                                ? (rss_max - rss_min)
                                : -1;
    const bool rss_ok = rss_growth < 0 || rss_growth <= 20 * 1024; /* 20 MB */
    const bool ok = aborts == 0 && hangs == 0 && rss_ok;

    std::printf("\nsoak summary (%lds):\n", elapsed);
    std::printf("  passes=%d graceful_errors=%d aborts=%d hangs=%d\n",
                passes, graceful_errors, aborts, hangs);
    std::printf("  child rss: min=%ld kB max=%ld kB growth=%ld kB (%s)\n",
                rss_min, rss_max, rss_growth,
                rss_ok ? "ok" : "UNBOUNDED GROWTH");
    std::printf("soak: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

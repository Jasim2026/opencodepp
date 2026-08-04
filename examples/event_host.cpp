// examples/event_host.cpp -- a fuller host using the C++ RAII wrapper.
//
// Demonstrates, against the same frozen ABI:
//   1. a config with every callback wired (events, permission, log);
//   2. opencode_engine_drive() as an idle pump before/after a run
//      (v1 runs tasks synchronously via run(); drive returns OPENCODE_OK when
//      the engine is idle and OPENCODE_ERR_BUSY mid-task);
//   3. a hermetic memory write/read round-trip that needs no network;
//   4. the metrics snapshot.
//
//   ./event_host ["prompt"]
#define _POSIX_C_SOURCE 200809L

#include "abi/opencode.hpp"

#include <cstdio>
#include <cstring>

namespace {

using namespace opencode;

const char* kind_name(opencode_event_kind_t k) {
    switch (k) {
        case OPENCODE_EVENT_LOG:        return "log";
        case OPENCODE_EVENT_PREPARING:  return "preparing";
        case OPENCODE_EVENT_CONNECTING: return "connecting";
        case OPENCODE_EVENT_STREAMING:  return "streaming";
        case OPENCODE_EVENT_TOOL_PHASE: return "tool";
        case OPENCODE_EVENT_VERIFYING:  return "verifying";
        case OPENCODE_EVENT_APPLYING:   return "applying";
        case OPENCODE_EVENT_DONE:       return "done";
        case OPENCODE_EVENT_FAILED:     return "failed";
        case OPENCODE_EVENT_CANCELLED:  return "cancelled";
        case OPENCODE_EVENT_FOLD:       return "fold";
    }
    return "?";
}

opencode_status_t on_event(void*, const opencode_event_t* ev) {
    char text[128];
    text[0] = '\0';
    if (ev->text != nullptr && ev->text_len > 0) {
        const size_t n = ev->text_len < sizeof text - 1 ? ev->text_len
                                                        : sizeof text - 1;
        memcpy(text, ev->text, n);
        text[n] = '\0';
    }
    std::printf("  [%s] session=%u %s\n", kind_name(ev->kind), ev->session_id,
                text);
    return OPENCODE_OK;
}

int on_permission(void*, const char* tool, const char* params_json) {
    std::printf("  permission? %s %s -> allow\n", tool, params_json);
    return 1;
}

void on_log(void*, int level, const char* msg) {
    std::printf("  [log/%d] %s\n", level, msg);
}

void on_metric(void*, const char* name, opencode_metric_kind_t kind,
               double value, uint64_t count) {
    const char* k = kind == OPENCODE_METRIC_COUNTER   ? "counter"
                    : kind == OPENCODE_METRIC_GAUGE   ? "gauge"
                                                      : "hist";
    std::printf("  [metric] %-24s %-7s %-8.3f (%llu)\n", name, k, value,
                static_cast<unsigned long long>(count));
}

} /* namespace */

int main(int argc, char** argv) {
    const char* prompt = argc > 1 ? argv[1]
                                  : "Say hello in one sentence.";

    opencode_config_t cfg;
    std::memset(&cfg, 0, sizeof cfg);
    cfg.version = OPENCODE_CONFIG_VERSION;
    cfg.workspace = ".";
    cfg.base_url = "http://127.0.0.1:8123";
    cfg.model = "mock-model";
    cfg.tool_policy = OPENCODE_POLICY_ASK;
    cfg.on_event = on_event;
    cfg.on_permission = on_permission;
    cfg.on_log = on_log;

    abi::Engine eng(&cfg);
    if (!eng.valid()) {
        std::fprintf(stderr, "event_host: create failed\n");
        return 1;
    }
    std::printf("event_host: abi=%u engine created\n", opencode_abi_version());

    /* (1) drive() when idle must return OPENCODE_OK and pump nothing. */
    const opencode_status_t idle = eng.drive(0);
    std::printf("event_host: drive(0) before run -> %d (expect %d OK)\n",
                static_cast<int>(idle), static_cast<int>(OPENCODE_OK));

    /* (2) hermetic memory round-trip: no provider needed. */
    const char kKey[] = "demo.note", kVal[] = "hello from event_host";
    opencode_status_t st = eng.memory_write(OPENCODE_MEMORY_FACT, kKey, kVal,
                                            nullptr);
    std::printf("event_host: memory_write -> %d (expect %d OK)\n",
                static_cast<int>(st), static_cast<int>(OPENCODE_OK));
    char buf[256];
    size_t n = 0;
    st = eng.memory_read(OPENCODE_MEMORY_FACT, "[]", buf, sizeof buf, &n);
    std::printf("event_host: memory_read -> %d len=%zu: %.*s\n",
                static_cast<int>(st), n, static_cast<int>(n), buf);

    /* (3) a full run against the provider (mock_api for the demo). */
    std::printf("event_host: running...\n");
    st = eng.run(prompt);
    std::printf("event_host: run -> %d\n", static_cast<int>(st));

    /* (4) metrics snapshot (runs emit counters/histograms even on error). */
    uint32_t m = eng.metrics(on_metric, nullptr);
    std::printf("event_host: %u metrics\n", m);

    st = eng.drive(0);
    std::printf("event_host: drive(0) after run -> %d\n", static_cast<int>(st));
    return 0;
}

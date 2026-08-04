// examples/embed_cli.cpp -- the smallest OpenCode++ host (frozen C ABI only).
//
// Links against nothing but include/opencode/opencode.h + libopencodepp.
// No C++ wrapper, no engine internals, no exceptions. This is the minimum an
// embedder needs: create -> run (streaming events) -> destroy.
//
//   ./embed_cli [base_url] ["prompt"]
//   defaults: base http://127.0.0.1:8123 , prompt "Say hello in one sentence."
//
// With a mock provider running (tools/mock_api), the run streams every phase
// to stderr and exits 0. Without one, the run returns OPENCODE_ERR_NETWORK
// (a retryable, non-fatal status) and this host reports it and exits 1.
#define _POSIX_C_SOURCE 200809L

#include "opencode/opencode.h"

#include <stdio.h>
#include <string.h>

static opencode_status_t on_event(void* userdata, const opencode_event_t* ev) {
    const char* phase = "?";
    switch (ev->kind) {
        case OPENCODE_EVENT_LOG:        phase = "log";        break;
        case OPENCODE_EVENT_PREPARING:  phase = "preparing";  break;
        case OPENCODE_EVENT_CONNECTING: phase = "connecting"; break;
        case OPENCODE_EVENT_STREAMING:  phase = "streaming";  break;
        case OPENCODE_EVENT_TOOL_PHASE: phase = "tool";       break;
        case OPENCODE_EVENT_VERIFYING:  phase = "verifying";  break;
        case OPENCODE_EVENT_APPLYING:   phase = "applying";   break;
        case OPENCODE_EVENT_DONE:       phase = "done";       break;
        case OPENCODE_EVENT_FAILED:     phase = "failed";     break;
        case OPENCODE_EVENT_CANCELLED:  phase = "cancelled";  break;
        case OPENCODE_EVENT_FOLD:       phase = "fold";       break;
    }
    char text[256];
    text[0] = '\0';
    if (ev->text != NULL && ev->text_len > 0) {
        const size_t n = ev->text_len < sizeof text - 1 ? ev->text_len
                                                        : sizeof text - 1;
        memcpy(text, ev->text, n);
        text[n] = '\0';
    }
    fprintf(stderr, "[%s] session=%u %s\n", phase, ev->session_id, text);
    (void)userdata;
    return OPENCODE_OK;
}

int main(int argc, char** argv) {
    const char* base = argc > 1 ? argv[1] : "http://127.0.0.1:8123";
    const char* prompt = argc > 2 ? argv[2] : "Say hello in one sentence.";

    opencode_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.version = OPENCODE_CONFIG_VERSION;
    cfg.workspace = ".";
    cfg.base_url = base;
    cfg.model = "mock-model";
    cfg.tool_policy = OPENCODE_POLICY_ALLOW_READONLY;
    cfg.on_event = on_event;

    opencode_engine_t* eng = NULL;
    opencode_status_t st = opencode_engine_create(&cfg, &eng);
    if (st != OPENCODE_OK || eng == NULL) {
        fprintf(stderr, "embed_cli: create failed (status %d)\n", (int)st);
        return 1;
    }
    fprintf(stderr, "embed_cli: abi=%u engine created, running...\n",
            opencode_abi_version());

    st = opencode_engine_run(eng, prompt, NULL, NULL);
    fprintf(stderr, "embed_cli: run status %d\n", (int)st);

    opencode_engine_destroy(eng);
    return st == OPENCODE_OK ? 0 : 1;
}

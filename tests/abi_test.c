/*
 * abi_test.c -- pure C11 conformance + end-to-end test of the C ABI.
 *
 * Drives a real engine (opencodepp_shared) against an in-process SSE mock
 * server written in C, so the whole test is offline and self-contained. Covers:
 *   A. version + create/destroy + NULL-handle validation
 *   B. one full text task vs mock (events, DONE, metrics, drive, cancel)
 *   C. policy=ASK permission callback denying a write tool
 *   D. cooperative cancel mid-stream (from a second thread)
 *   E. memory.write / memory.read round-trip + bad-kind validation
 *   F. set_config (NULL + reconfig to a new workspace)
 *
 * The ABI header must compile as pure C11 (this file is compiled by the C
 * compiler in CI) and no C++ symbol is referenced directly.
 */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

#include <opencode/opencode.h>

static int g_failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

/* ------------------------------------------------------------------ */
/* Mini SSE mock server (in C; mirrors tools/run_agent's FakeServer).  */
/* ------------------------------------------------------------------ */

/* Handlers return a chunked HTTP/1.1 SSE response; request index is the
 * per-server connection count. May sleep `sleep_ms` before responding. */
typedef const char* (*mock_handler_t)(int req_index, int sleep_ms);

struct mock_server {
    int port;
    int listen_fd;
    int sleep_ms;
    pthread_t th;
    int stop;
    mock_handler_t handler;
};

static void mock_build_sse(const char* const* frames, int nframes,
                           char* out, size_t cap) {
    char body[8192];
    size_t blen = 0;
    for (int i = 0; i < nframes; ++i) {
        blen += (size_t)snprintf(body + blen, sizeof body - blen, "data: %s\r\n\r\n",
                                 frames[i]);
    }
    size_t n = (size_t)snprintf(out, cap,
                                "HTTP/1.1 200 OK\r\n"
                                "Content-Type: text/event-stream\r\n"
                                "Transfer-Encoding: chunked\r\n"
                                "Connection: close\r\n\r\n");
    char hx[32];
    snprintf(hx, sizeof hx, "%zx", blen);
    n += (size_t)snprintf(out + n, cap - n, "%s\r\n", hx);
    n += (size_t)snprintf(out + n, cap - n, "%.*s", (int)blen, body);
    n += (size_t)snprintf(out + n, cap - n, "0\r\n\r\n");
}

static int mock_ephemeral_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr*)&a, sizeof a) < 0) {
        close(fd);
        return 0;
    }
    socklen_t gl = sizeof a;
    getsockname(fd, (struct sockaddr*)&a, &gl);
    int port = ntohs(a.sin_port);
    close(fd);
    return port;
}

static int mock_read_head(int fd, char* out, size_t cap) {
    size_t n = 0;
    while (n < cap - 1) {
        ssize_t r = read(fd, out + n, cap - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
        if (strstr(out, "\r\n\r\n") != NULL) break;
    }
    out[n] = '\0';
    return (int)n;
}

static void mock_msleep(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int mock_bind(struct mock_server* m) {
    m->port = mock_ephemeral_port();
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)m->port);
    if (bind(ls, (struct sockaddr*)&a, sizeof a) != 0 ||
        listen(ls, 8) != 0) {
        close(ls);
        return -1;
    }
    return ls;
}

static void* mock_accept_loop(void* arg) {
    struct mock_server* m = (struct mock_server*)arg;
    const int ls = m->listen_fd;
    if (ls < 0) return NULL;
    int reqs = 0;
    while (!m->stop) {
        struct pollfd pfd;
        pfd.fd = ls;
        pfd.events = POLLIN;
        if (poll(&pfd, 1, 200) <= 0) continue;
        int c = accept(ls, NULL, NULL);
        if (c < 0) continue;
        char head[4096];
        mock_read_head(c, head, sizeof head);
        if (m->sleep_ms > 0) mock_msleep(m->sleep_ms);
        const char* resp = m->handler(reqs++, m->sleep_ms);
        size_t off = 0;
        size_t len = strlen(resp);
        while (off < len) {
            ssize_t w = write(c, resp + off, len - off);
            if (w <= 0) break;
            off += (size_t)w;
        }
        close(c);
    }
    close(ls);
    return NULL;
}

static void mock_start(struct mock_server* m, mock_handler_t handler,
                       int sleep_ms) {
    memset(m, 0, sizeof *m);
    m->handler = handler;
    m->sleep_ms = sleep_ms;
    m->listen_fd = mock_bind(m);
    m->stop = 0;
    pthread_create(&m->th, NULL, mock_accept_loop, m);
}

static void mock_stop(struct mock_server* m) {
    m->stop = 1;
    pthread_join(m->th, NULL);
}

/* ------------------------------------------------------------------ */
/* Shared SSE frame sets                                               */
/* ------------------------------------------------------------------ */

static const char* kTextDoneFrames[] = {
    "{\"choices\":[{\"delta\":{\"content\":\"Hello \"},\"finish_reason\":null}]}",
    "{\"choices\":[{\"delta\":{\"content\":\"world\"},\"finish_reason\":null}]}",
    "{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":12,\"completion_tokens\":3}}",
};

static char g_buf[2][16384];
static const char* mock_text_only(int req_index, int sleep_ms) {
    (void)sleep_ms;
    if (req_index == 0)
        mock_build_sse(kTextDoneFrames, 3, g_buf[0], sizeof g_buf[0]);
    else
        mock_build_sse(kTextDoneFrames, 3, g_buf[1], sizeof g_buf[1]);
    return req_index == 0 ? g_buf[0] : g_buf[1];
}

/* Round 0: a file.write tool call; round 1: final text. */
static const char* mock_tool_then_done(int req_index, int sleep_ms) {
    (void)sleep_ms;
    static const char* f0[] = {
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"function\":{\"name\":\"file.write\",\"arguments\":\"{\\\"path\\\":\\\"out.txt\\\",\\\"content\\\":\\\"hello agent\\\\n\\\",\\\"create\\\":true}\"}}]},\"finish_reason\":\"tool_calls\"}]}",
        "{\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":8}}",
    };
    static const char* f1[] = {
        "{\"choices\":[{\"delta\":{\"content\":\"done\"},\"finish_reason\":null}]}",
        "{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":40,\"completion_tokens\":2}}",
    };
    if (req_index == 0)
        mock_build_sse(f0, 2, g_buf[0], sizeof g_buf[0]);
    else
        mock_build_sse(f1, 2, g_buf[1], sizeof g_buf[1]);
    return req_index == 0 ? g_buf[0] : g_buf[1];
}

/* ------------------------------------------------------------------ */
/* Event / metric recorders                                            */
/* ------------------------------------------------------------------ */

struct recorder {
    opencode_event_kind_t kinds[256];
    size_t count;
    int saw_done;
    int saw_failed;
    int cancelled_by_host;
};

static opencode_status_t on_event(void* ud, const opencode_event_t* ev) {
    struct recorder* r = (struct recorder*)ud;
    if (r->count < 256) r->kinds[r->count++] = ev->kind;
    if (ev->kind == OPENCODE_EVENT_DONE) r->saw_done = 1;
    if (ev->kind == OPENCODE_EVENT_FAILED) r->saw_failed = 1;
    return OPENCODE_OK;
}

struct metric_state {
    uint32_t count;
    int saw_tasks;
};

static void on_metric(void* ud, const char* name, opencode_metric_kind_t kind,
                      double value, uint64_t count) {
    (void)kind;
    (void)value;
    (void)count;
    struct metric_state* m = (struct metric_state*)ud;
    ++m->count;
    if (strcmp(name, "engine.tasks") == 0) m->saw_tasks = 1;
}

/* ------------------------------------------------------------------ */
/* Permission / log hooks                                              */
/* ------------------------------------------------------------------ */

static int g_permission_calls = 0;
static int deny_all(void* ud, const char* tool, const char* params_json) {
    (void)ud;
    (void)tool;
    (void)params_json;
    ++g_permission_calls;
    return 0; /* deny */
}

static int g_log_lines = 0;
static void log_sink(void* ud, int level, const char* msg) {
    (void)ud;
    (void)level;
    (void)msg;
    ++g_log_lines;
}

/* Cancel-after-delay thread. */
struct cancel_args {
    opencode_engine_t* eng;
    useconds_t delay_us;
};
static void* cancel_later(void* arg) {
    struct cancel_args* ca = (struct cancel_args*)arg;
    mock_msleep((int)(ca->delay_us / 1000));
    opencode_engine_cancel(ca->eng);
    return NULL;
}

static const char* kWs = "/tmp/opencode_abi_ws";

static opencode_config_t make_cfg(const char* base_url, opencode_tool_policy_t policy) {
    opencode_config_t c;
    memset(&c, 0, sizeof c);
    c.version = OPENCODE_CONFIG_VERSION;
    c.workspace = kWs;
    c.provider = "openai_compat";
    c.base_url = base_url;
    c.api_key = "sk-test";
    c.model = "mock-model";
    c.agent = "default";
    c.network_timeout_ms = 15000;
    c.tool_policy = policy;
    return c;
}

int main(void) {
    system("rm -rf /tmp/opencode_abi_ws && mkdir -p /tmp/opencode_abi_ws");

    /* ---- A. version + lifecycle + validation ---- */
    CHECK(opencode_abi_version() == OPENCODE_ABI_VERSION);
    CHECK(opencode_event_version() == OPENCODE_EVENT_VERSION);
    CHECK(opencode_config_version() == OPENCODE_CONFIG_VERSION);

    opencode_engine_t* eng = NULL;
    CHECK(opencode_engine_create(NULL, NULL) == OPENCODE_ERR_VALIDATION);
    CHECK(opencode_engine_destroy(NULL) == OPENCODE_ERR_VALIDATION);
    CHECK(opencode_engine_create(NULL, &eng) == OPENCODE_OK);
    CHECK(eng != NULL);
    CHECK(opencode_engine_cancel(eng) == OPENCODE_OK);
    CHECK(opencode_engine_drive(eng, -1) == OPENCODE_OK);
    CHECK(opencode_engine_set_config(eng, NULL) == OPENCODE_ERR_VALIDATION);
    CHECK(opencode_engine_destroy(eng) == OPENCODE_OK);

    /* ---- B. full text task vs mock ---- */
    {
        struct mock_server srv;
        mock_start(&srv, &mock_text_only, 0);
        char base[64];
        snprintf(base, sizeof base, "http://127.0.0.1:%d", srv.port);

        opencode_config_t c = make_cfg(base, OPENCODE_POLICY_ALLOW_READONLY);
        c.on_event = &on_event;
        c.on_log = &log_sink;
        struct recorder rec;
        memset(&rec, 0, sizeof rec);
        c.userdata = &rec;

        CHECK(opencode_engine_create(&c, &eng) == OPENCODE_OK);

        struct recorder runrec;
        memset(&runrec, 0, sizeof runrec);
        CHECK(opencode_engine_run(eng, "tell me a greeting please",
                                  &on_event, &runrec) == OPENCODE_OK);
        CHECK(runrec.saw_done);
        CHECK(!runrec.saw_failed);
        /* ordering: preparing ... streaming ... done */
        CHECK(runrec.count >= 3);
        CHECK(runrec.kinds[0] == OPENCODE_EVENT_PREPARING);
        CHECK(runrec.kinds[runrec.count - 1] == OPENCODE_EVENT_DONE);

        /* drive() on an idle engine is OK; a second task still works. */
        CHECK(opencode_engine_drive(eng, 0) == OPENCODE_OK);
        struct recorder rec2;
        memset(&rec2, 0, sizeof rec2);
        CHECK(opencode_engine_run(eng, "again", &on_event, &rec2) == OPENCODE_OK);
        CHECK(rec2.saw_done);

        /* metrics snapshot reflects completed tasks. */
        struct metric_state ms;
        memset(&ms, 0, sizeof ms);
        uint32_t n = 0;
        CHECK(opencode_metrics_snapshot(eng, &on_metric, &ms, &n) == OPENCODE_OK);
        CHECK(ms.saw_tasks);
        CHECK(ms.count >= 2);

        CHECK(opencode_engine_destroy(eng) == OPENCODE_OK);
        eng = NULL;
        mock_stop(&srv);
    }

    /* ---- C. policy=ASK permission callback denying a write tool ---- */
    {
        struct mock_server srv;
        mock_start(&srv, &mock_tool_then_done, 0);
        char base[64];
        snprintf(base, sizeof base, "http://127.0.0.1:%d", srv.port);

        opencode_config_t c = make_cfg(base, OPENCODE_POLICY_ASK);
        c.on_permission = &deny_all;
        g_permission_calls = 0;

        CHECK(opencode_engine_create(&c, &eng) == OPENCODE_OK);
        struct recorder rec;
        memset(&rec, 0, sizeof rec);
        CHECK(opencode_engine_run(eng, "write out.txt please",
                                  &on_event, &rec) == OPENCODE_OK);
        CHECK(rec.saw_done);
        CHECK(g_permission_calls >= 1); /* the write was offered to the host */

        /* denied write must not have touched the filesystem */
        struct stat st;
        CHECK(stat("/tmp/opencode_abi_ws/out.txt", &st) != 0);

        CHECK(opencode_engine_destroy(eng) == OPENCODE_OK);
        eng = NULL;
        mock_stop(&srv);
    }

    /* ---- D. cooperative cancel mid-stream ---- */
    {
        struct mock_server srv;
        mock_start(&srv, &mock_text_only, 300);
        char base[64];
        snprintf(base, sizeof base, "http://127.0.0.1:%d", srv.port);

        opencode_config_t c = make_cfg(base, OPENCODE_POLICY_ALLOW_READONLY);
        CHECK(opencode_engine_create(&c, &eng) == OPENCODE_OK);

        struct cancel_args ca;
        ca.eng = eng;
        ca.delay_us = 50000;
        pthread_t ct;
        pthread_create(&ct, NULL, &cancel_later, &ca);

        struct recorder rec;
        memset(&rec, 0, sizeof rec);
        const opencode_status_t st =
            opencode_engine_run(eng, "run until cancelled", &on_event, &rec);
        pthread_join(ct, NULL);
        CHECK(st == OPENCODE_ERR_CANCELLED);

        CHECK(opencode_engine_destroy(eng) == OPENCODE_OK);
        eng = NULL;
        mock_stop(&srv);
    }

    /* ---- E. memory ops ---- */
    {
        struct mock_server srv;
        mock_start(&srv, &mock_text_only, 0);
        char base[64];
        snprintf(base, sizeof base, "http://127.0.0.1:%d", srv.port);

        opencode_config_t c = make_cfg(base, OPENCODE_POLICY_ALLOW_READONLY);
        CHECK(opencode_engine_create(&c, &eng) == OPENCODE_OK);

        char id[128];
        memset(id, 0, sizeof id);
        CHECK(opencode_memory_write(eng, OPENCODE_MEMORY_FACT, "build_tool",
                                    "the build uses cmake", "[\"build\",\"c++\"]",
                                    id, sizeof id) == OPENCODE_OK);
        CHECK(strlen(id) > 0);

        /* out-of-range kind is rejected. */
        CHECK(opencode_memory_write(eng, (opencode_memory_kind_t)99, "k", "v",
                                    NULL, NULL, 0) == OPENCODE_ERR_VALIDATION);

        char ctx[512];
        memset(ctx, 0, sizeof ctx);
        size_t len = 0;
        CHECK(opencode_memory_read(eng, OPENCODE_MEMORY_FACT, "[\"cmake\"]",
                                   ctx, sizeof ctx, &len) == OPENCODE_OK);
        CHECK(strstr(ctx, "build_tool") != NULL);
        CHECK(strstr(ctx, "cmake") != NULL);

        /* empty keywords + kind returns everything of that kind. */
        CHECK(opencode_memory_read(eng, OPENCODE_MEMORY_FACT, "",
                                   ctx, sizeof ctx, &len) == OPENCODE_OK);
        CHECK(strstr(ctx, "build_tool") != NULL);

        CHECK(opencode_engine_destroy(eng) == OPENCODE_OK);
        eng = NULL;
        mock_stop(&srv);
    }

    /* ---- F. set_config reconfigures to a new workspace ---- */
    {
        struct mock_server srv;
        mock_start(&srv, &mock_text_only, 0);
        char base[64];
        snprintf(base, sizeof base, "http://127.0.0.1:%d", srv.port);

        opencode_config_t c = make_cfg(base, OPENCODE_POLICY_ALLOW_READONLY);
        CHECK(opencode_engine_create(&c, &eng) == OPENCODE_OK);

        opencode_config_t c2 = make_cfg(base, OPENCODE_POLICY_ALLOW_READONLY);
        c2.workspace = "/tmp/opencode_abi_ws2";
        CHECK(opencode_engine_set_config(eng, &c2) == OPENCODE_OK);

        struct recorder rec;
        memset(&rec, 0, sizeof rec);
        CHECK(opencode_engine_run(eng, "hello", &on_event, &rec) == OPENCODE_OK);
        CHECK(rec.saw_done);

        CHECK(opencode_engine_destroy(eng) == OPENCODE_OK);
        eng = NULL;
        mock_stop(&srv);
    }

    if (g_failures == 0) {
        printf("abi_test: all sections OK\n");
        return 0;
    }
    fprintf(stderr, "abi_test: %d failure(s)\n", g_failures);
    return 1;
}

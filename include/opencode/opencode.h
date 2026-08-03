/*
 * opencode.h -- the OpenCode++ C ABI (public, stable contract).
 *
 * This header is the ONLY public interface of libopencode. Hosts (CLI, IDE
 * plugins, Android apps, servers) link against it and nothing else.
 *
 * Versioning policy (locked after Phase 12):
 *   - OPENCODE_ABI_VERSION is bumped on ANY breaking change (signature,
 *     semantics, enum values, struct layout).
 *   - New symbols or fields are additive-only within a version and must not
 *     change existing behavior.
 *
 * Conventions:
 *   - All functions return opencode_status_t (OPENCODE_OK on success).
 *   - All OUT pointers must be non-NULL; NULL handle arguments return
 *     OPENCODE_ERR_VALIDATION.
 *   - No function throws; no exception crosses this boundary (C++ impls wrap
 *     everything in error handling).
 *   - The header compiles as pure C11 (checked in CI with -std=c11).
 */
#ifndef OPENCODE_OPENCODE_H
#define OPENCODE_OPENCODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Version                                                            */
/* ------------------------------------------------------------------ */

#define OPENCODE_ABI_VERSION 1u

/* Version of the opencode_event_t payload layout (increments independently). */
#define OPENCODE_EVENT_VERSION 1u

/*
 * opencode_abi_version() -- returns OPENCODE_ABI_VERSION.
 *
 * Two forms (never both in one TU): by default this header provides a
 * static-inline implementation (header-only, no link dependency). Define
 * OPENCODE_ABI_HEADER_ONLY before including to suppress it and instead link
 * the exported symbol from libopencode (e.g. when dlopen'ing the shared lib
 * without the header).
 */
#ifndef OPENCODE_ABI_HEADER_ONLY
static inline uint32_t opencode_abi_version(void) { return OPENCODE_ABI_VERSION; }
#endif

/* ------------------------------------------------------------------ */
/* Opaque handles                                                     */
/* ------------------------------------------------------------------ */

typedef struct opencode_engine opencode_engine_t;   /* one self-contained engine instance */
typedef struct opencode_session opencode_session_t; /* one task session inside an engine    */
typedef struct opencode_config opencode_config_t;   /* immutable configuration snapshot     */

/* ------------------------------------------------------------------ */
/* Status codes                                                       */
/* ------------------------------------------------------------------ */

typedef enum opencode_status {
    OPENCODE_OK              = 0,
    OPENCODE_ERR_NETWORK     = 1, /* transient network failure (retryable)      */
    OPENCODE_ERR_AUTH        = 2, /* bad/missing credentials (non-retryable)     */
    OPENCODE_ERR_VALIDATION  = 3, /* bad argument / config (non-retryable)       */
    OPENCODE_ERR_BUSY        = 4, /* engine is mid-task; try later               */
    OPENCODE_ERR_CANCELLED   = 5, /* operation cancelled by host or budget       */
    OPENCODE_ERR_FATAL       = 6, /* internal invariant broken (do not retry)    */
    OPENCODE_ERR_NO_NETWORK  = 7  /* offline; queued, will resume                */
} opencode_status_t;

/* ------------------------------------------------------------------ */
/* Events (variant payload, size-bounded, versioned)                  */
/* ------------------------------------------------------------------ */

typedef enum opencode_event_kind {
    OPENCODE_EVENT_LOG           = 1, /* text = formatted log line               */
    OPENCODE_EVENT_TOOL_CALL     = 2, /* text = tool name, data_i64 = call id    */
    OPENCODE_EVENT_TOOL_RESULT   = 3, /* text = result snippet                  */
    OPENCODE_EVENT_EDIT_APPLIED  = 4, /* text = path, data_i64 = bytes changed  */
    OPENCODE_EVENT_EDIT_REJECTED = 5, /* text = path, data_i64 = gate reason id */
    OPENCODE_EVENT_TASK_DONE     = 6, /* text = summary, data_i64 = tokens used */
    OPENCODE_EVENT_TASK_FAILED   = 7  /* text = failure reason                  */
} opencode_event_kind_t;

#define OPENCODE_EVENT_TEXT_MAX 4096u /* fixed upper bound for ev->text          */

typedef struct opencode_event {
    uint32_t            version;   /* OPENCODE_EVENT_VERSION                      */
    uint32_t            session_id;/* owning session (0 = engine-global)          */
    opencode_event_kind_t kind;    /* which variant this is                       */
    /* Host-owned text buffer (may be NULL; host then ignores text fields).       */
    char*               text;      /* points into host buffer; engine fills in    */
    size_t              text_cap;  /* bytes available in text                     */
    size_t              text_len;  /* bytes written by the engine (<= text_cap)   */
    int64_t             data_i64;  /* generic int slot (tokens, bytes, call id)   */
} opencode_event_t;

/* ------------------------------------------------------------------ */
/* Callbacks (all invoked on the engine thread)                       */
/* ------------------------------------------------------------------ */

/* Event sink. Return OPENCODE_OK to continue; any other status cancels the run. */
typedef opencode_status_t (*opencode_event_fn)(void* userdata, const opencode_event_t* ev);

/* Ask the host whether a tool call is permitted.
 * Returns 1 to allow, 0 to deny (denial surfaces as a structured error to the
 * model). `tool` is the tool name; `params_json` is the argument payload. */
typedef int (*opencode_permission_fn)(void* userdata, const char* tool, const char* params_json);

/* Ask the host for explicit consent to a destructive action (e.g. `rm -rf`,
 * overwrite outside the workspace). Returns 1 to allow, 0 to deny. */
typedef int (*opencode_consent_fn)(void* userdata, const char* description);

/* Log sink. `level`: 0=debug 1=info 2=warn 3=error. `msg` valid during the call. */
typedef void (*opencode_log_fn)(void* userdata, int level, const char* msg);

/* ------------------------------------------------------------------ */
/* Entry points (declared in Phase 0; implemented with Phase 12 ABI)  */
/* ------------------------------------------------------------------ */

/* Create/destroy an engine. cfg may be NULL (defaults). */
opencode_status_t opencode_engine_create(const opencode_config_t* cfg, opencode_engine_t** out);
opencode_status_t opencode_engine_destroy(opencode_engine_t* eng);

/* Replace configuration. Active sessions are unaffected; applies to the next
 * session. cfg must outlive the call (engine copies what it needs). */
opencode_status_t opencode_engine_set_config(opencode_engine_t* eng, const opencode_config_t* cfg);

/* Pump the engine event loop for up to wait_ms (-1 = until idle). The host
 * calls this on its own thread; the engine never spawns threads by default. */
opencode_status_t opencode_engine_drive(opencode_engine_t* eng, int32_t wait_ms);

/* Run a task to completion (or budget/cancel). Blocks until the task finishes,
 * dispatching events to `on_event` as they occur. */
opencode_status_t opencode_engine_run(opencode_engine_t* eng, const char* prompt,
                                      opencode_event_fn on_event, void* userdata);

/* Cancel the current run at the next safe point. Safe to call from any thread. */
opencode_status_t opencode_engine_cancel(opencode_engine_t* eng);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENCODE_OPENCODE_H */

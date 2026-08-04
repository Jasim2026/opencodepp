/*
 * opencode.h -- the OpenCode++ C ABI (public, stable contract). FROZEN v1.
 *
 * This header is the ONLY public interface of libopencode. Hosts (CLI, IDE
 * plugins, Android apps, servers) link against it and nothing else.
 *
 * Versioning policy (LOCKED from Phase 12 onward):
 *   - OPENCODE_ABI_VERSION is bumped on ANY breaking change (signature,
 *     semantics, enum values, struct layout).
 *   - New symbols or fields are additive-only within a version and must not
 *     change existing behavior.
 *   - A breaking change requires a version bump AND a compatibility shim;
 *     never a silent break. See docs/ABI.md.
 *
 * Conventions:
 *   - All functions return opencode_status_t (OPENCODE_OK on success).
 *   - All OUT pointers must be non-NULL; NULL handle arguments return
 *     OPENCODE_ERR_VALIDATION.
 *   - No function throws; no exception crosses this boundary (C++ impls wrap
 *     everything in error handling).
 *   - The header compiles as pure C11 (checked in CI with -std=c11).
 *   - String pointers passed in (config fields, prompts) are copied by the
 *     engine before the call returns; the caller's buffers need not outlive
 *     the call. Text OUT buffers are caller-owned and must not be NULL with a
 *     non-zero cap.
 *   - All callbacks are invoked on the engine thread only (the thread that
 *     called opencode_engine_run). opencode_engine_cancel is the single
 *     function safe to call from any thread.
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

/* Version of the opencode_config_t layout (increments independently). */
#define OPENCODE_CONFIG_VERSION 1u

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
static inline uint32_t opencode_event_version(void) { return OPENCODE_EVENT_VERSION; }
static inline uint32_t opencode_config_version(void) { return OPENCODE_CONFIG_VERSION; }
#endif

/* ------------------------------------------------------------------ */
/* Opaque handles                                                     */
/* ------------------------------------------------------------------ */

typedef struct opencode_engine opencode_engine_t;   /* one self-contained engine instance */
typedef struct opencode_session opencode_session_t; /* one task session inside an engine
                                                        (reserved; returned by future calls) */
typedef struct opencode_config opencode_config_t;   /* immutable configuration snapshot   */

/* ------------------------------------------------------------------ */
/* Status codes                                                       */
/* ------------------------------------------------------------------ */

typedef enum opencode_status {
    OPENCODE_OK              = 0,  /* success                                  */
    OPENCODE_ERR_NETWORK     = 1,  /* transient network failure (retryable)    */
    OPENCODE_ERR_AUTH        = 2,  /* bad/missing credentials (non-retryable)   */
    OPENCODE_ERR_VALIDATION  = 3,  /* bad argument / config (non-retryable)     */
    OPENCODE_ERR_BUSY        = 4,  /* engine is mid-task; try later             */
    OPENCODE_ERR_CANCELLED   = 5,  /* operation cancelled by host or budget     */
    OPENCODE_ERR_FATAL       = 6,  /* internal invariant broken (do not retry)  */
    OPENCODE_ERR_NO_NETWORK  = 7   /* offline; queued, will resume              */
} opencode_status_t;

/* ------------------------------------------------------------------ */
/* Tool policy + memory kinds (mirror the engine enums 1:1)           */
/* ------------------------------------------------------------------ */

typedef enum opencode_tool_policy {
    OPENCODE_POLICY_DENY           = 0, /* always deny write/shell tools     */
    OPENCODE_POLICY_ASK            = 1, /* ask the host callback per call    */
    OPENCODE_POLICY_ALLOW          = 2, /* always allow                      */
    OPENCODE_POLICY_ALLOW_READONLY = 3  /* allow read-only tools; deny writes */
} opencode_tool_policy_t;

typedef enum opencode_memory_kind {
    OPENCODE_MEMORY_DECISION   = 0,
    OPENCODE_MEMORY_FACT       = 1,
    OPENCODE_MEMORY_TASK_STATE = 2,
    OPENCODE_MEMORY_REPO_RULE  = 3,
    OPENCODE_MEMORY_LESSON     = 4,
    OPENCODE_MEMORY_USER_PREF  = 5
} opencode_memory_kind_t;

/* ------------------------------------------------------------------ */
/* Events (variant payload, size-bounded, versioned)                  */
/* ------------------------------------------------------------------ */

/* Event kinds mirror the agent state machine (src/agent/states.h) 1:1 so a
 * host can render every phase without interpreting free text. */
typedef enum opencode_event_kind {
    OPENCODE_EVENT_LOG           = 1,  /* text = formatted log line               */
    OPENCODE_EVENT_PREPARING     = 2,  /* intent + context assembly              */
    OPENCODE_EVENT_CONNECTING    = 3,  /* transport connect / request send        */
    OPENCODE_EVENT_STREAMING     = 4,  /* response frames flowing                 */
    OPENCODE_EVENT_TOOL_PHASE    = 5,  /* tool dispatch (gated writes serialized) */
    OPENCODE_EVENT_VERIFYING     = 6,  /* Phase 9 gate over write proposals       */
    OPENCODE_EVENT_APPLYING      = 7,  /* gate-passing edits written to workspace */
    OPENCODE_EVENT_DONE          = 8,  /* task finished; text = summary, data_i64 = tokens */
    OPENCODE_EVENT_FAILED        = 9,  /* non-retryable failure; text = reason    */
    OPENCODE_EVENT_CANCELLED     = 10, /* host/budget cancelled; rolled back      */
    OPENCODE_EVENT_FOLD          = 11  /* lossy history fold; text = reason       */
} opencode_event_kind_t;

/* Agent lane (orthogonal to the main state; 0 = none). Mirrors states.h. */
typedef enum opencode_lane {
    OPENCODE_LANE_NONE    = 0,
    OPENCODE_LANE_BACKOFF = 1, /* retry delay after a transient failure */
    OPENCODE_LANE_OFFLINE = 2, /* queued while unreachable              */
    OPENCODE_LANE_PAUSED  = 3  /* host-held pause                        */
} opencode_lane_t;

#define OPENCODE_EVENT_TEXT_MAX 4096u /* fixed upper bound for ev->text          */

typedef struct opencode_event {
    uint32_t            version;   /* OPENCODE_EVENT_VERSION                      */
    uint32_t            session_id;/* owning session (engine-assigned task id)    */
    opencode_event_kind_t kind;    /* which variant this is                       */
    /* Host-owned text buffer (may be NULL; host then ignores text fields).       */
    char*               text;      /* points into host buffer; engine fills in    */
    size_t              text_cap;  /* bytes available in text                     */
    size_t              text_len;  /* bytes written by the engine (<= text_cap)   */
    int64_t             data_i64;  /* generic int slot (tokens, bytes, call id)   */
    int32_t             status;    /* 0 = ok, else a core error code value        */
    opencode_lane_t     lane;      /* active agent lane during this event         */
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
 * overwrite outside the workspace). Returns 1 to allow, 0 to deny.
 * In v1 the permission callback is the consent point for tool calls; this
 * hook is reserved for host-invoked destructive operations. */
typedef int (*opencode_consent_fn)(void* userdata, const char* description);

/* Log sink. `level`: 0=debug 1=info 2=warn 3=error. `msg` valid during the call. */
typedef void (*opencode_log_fn)(void* userdata, int level, const char* msg);

/* Metrics sink. `name` and `kind` classify the entry; `value` is the counter/
 * gauge value or a histogram percentile (seconds); `count` is 0 for
 * counters/gauges and the sample count for histograms. `name` is valid for
 * the duration of the call. */
typedef enum opencode_metric_kind {
    OPENCODE_METRIC_COUNTER   = 0,
    OPENCODE_METRIC_GAUGE     = 1,
    OPENCODE_METRIC_HISTOGRAM = 2
} opencode_metric_kind_t;
typedef void (*opencode_metric_fn)(void* userdata, const char* name,
                                   opencode_metric_kind_t kind, double value,
                                   uint64_t count);

/* ------------------------------------------------------------------ */
/* Configuration                                                      */
/* ------------------------------------------------------------------ */

/* Immutable snapshot handed to opencode_engine_create/set_config. The engine
 * copies everything it needs before the call returns. Two configuration
 * styles (they compose):
 *   - `config_path` set  -> load an opencodepp JSON config file, then overlay
 *                           any non-NULL direct fields below on top.
 *   - `config_path` NULL -> use the direct fields only.
 * A NULL cfg on create = defaults (workspace ".", provider "openai_compat"
 * on 127.0.0.1:8080, model "mock-model", agent "default", policy deny). */
typedef struct opencode_config {
    uint32_t            version;      /* OPENCODE_CONFIG_VERSION                */

    const char*         workspace;    /* sandbox base for tools/gate (required) */
    const char*         config_path;  /* optional opencodepp JSON config file   */
    const char*         prompt_dir;   /* optional prompt template dir           */
    const char*         provider;     /* provider id (e.g. "openai_compat")     */
    const char*         base_url;     /* provider base URL                      */
    const char*         api_key;      /* provider API key                       */
    const char*         model;        /* model id (catalog or passthrough)      */
    const char*         agent;        /* agent profile id                       */

    uint32_t            network_timeout_ms; /* per-request timeout; 0 = default */

    opencode_tool_policy_t tool_policy;     /* default write/shell policy       */
    uint32_t            memory_max_entries;         /* per-scope entry cap      */
    uint32_t            memory_max_entries_per_task;/* Tier-2 injection cap     */
    uint32_t            memory_max_value_chars;     /* per-entry value cap      */

    /* Callbacks (any may be NULL). All fire on the engine thread only. */
    opencode_event_fn   on_event;      /* event sink for opencode_engine_run    */
    opencode_permission_fn on_permission; /* Policy::ask gate hook              */
    opencode_consent_fn on_consent;    /* reserved destructive-action consent   */
    opencode_log_fn     on_log;        /* engine log sink                       */

    void*               userdata;      /* passed to every callback              */
} opencode_config_t;

/* ------------------------------------------------------------------ */
/* Entry points                                                       */
/* ------------------------------------------------------------------ */

/* Create/destroy an engine. cfg may be NULL (defaults). */
opencode_status_t opencode_engine_create(const opencode_config_t* cfg, opencode_engine_t** out);
opencode_status_t opencode_engine_destroy(opencode_engine_t* eng);

/* Replace configuration. Active sessions are unaffected; applies to the next
 * session. cfg must outlive the call (engine copies what it needs). */
opencode_status_t opencode_engine_set_config(opencode_engine_t* eng, const opencode_config_t* cfg);

/* Pump the engine event loop for up to wait_ms (-1 = until idle). The host
 * calls this on its own thread; the engine never spawns threads by default.
 * In v1 tasks run synchronously on the calling thread via opencode_engine_run;
 * drive() returns OPENCODE_OK when idle and OPENCODE_ERR_BUSY mid-task. */
opencode_status_t opencode_engine_drive(opencode_engine_t* eng, int32_t wait_ms);

/* Run a task to completion (or budget/cancel). Blocks until the task finishes,
 * dispatching events to `on_event` (override, may be NULL to use the engine's
 * configured sink) as they occur. */
opencode_status_t opencode_engine_run(opencode_engine_t* eng, const char* prompt,
                                      opencode_event_fn on_event, void* userdata);

/* Cancel the current run at the next safe point. Safe to call from any thread. */
opencode_status_t opencode_engine_cancel(opencode_engine_t* eng);

/* Metrics snapshot: enumerates every engine metric through `sink` (counters,
 * gauges, histogram percentiles). Returns the count of metrics emitted. */
opencode_status_t opencode_metrics_snapshot(opencode_engine_t* eng,
                                            opencode_metric_fn sink, void* userdata,
                                            uint32_t* out_count);

/* Memory ops (Phase 11 workspace memory). All scoped to the engine workspace.
 * opencode_memory_write: kind/key/value with an optional JSON array of tags;
 * on success *out_id (when non-NULL with a non-zero cap) receives the entry id.
 * opencode_memory_read: writes the keyword/kind-scoped memory context
 * ("[kind] key: value" lines, budget-bounded) into `out`; keywords_json is a
 * JSON array of strings (may be empty to read all matching `kind`). */
opencode_status_t opencode_memory_write(opencode_engine_t* eng, opencode_memory_kind_t kind,
                                        const char* key, const char* value,
                                        const char* tags_json,
                                        char* out_id, size_t out_id_cap);
opencode_status_t opencode_memory_read(opencode_engine_t* eng, opencode_memory_kind_t kind,
                                       const char* keywords_json,
                                       char* out, size_t out_cap, size_t* out_len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENCODE_OPENCODE_H */

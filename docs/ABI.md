# ABI — the frozen OpenCode++ C contract (v1)

`include/opencode/opencode.h` is the **only public interface** of
`libopencodepp`. Hosts — the reference CLI, IDE plugins, Android apps,
servers — link against this header and nothing else. Everything else in the
tree (engine, tools, provider, memory) is internal and may change freely
between releases.

This document is the normative reference for the frozen v1 surface. The
header is checked as pure C11 (`-std=c11`) and C++20 (`-std=c++20`) under
`-Wall -Wextra -Wpedantic -Werror` on every CI run.

## Versioning policy (locked from Phase 12 onward)

Three independent versions are exposed:

| Macro                    | Bumped when                                            |
|--------------------------|--------------------------------------------------------|
| `OPENCODE_ABI_VERSION`   | any breaking change: signature, semantics, enum value, struct layout |
| `OPENCODE_CONFIG_VERSION`| the `opencode_config_t` layout changes                |
| `OPENCODE_EVENT_VERSION` | the `opencode_event_t` layout changes                 |

Rules:

1. **Additive-only within a version.** New functions, fields, or enum values
   must not alter the meaning or layout of existing ones. Existing behavior
   never changes behind the same version number.
2. **Breaking changes require a version bump AND a compatibility shim.**
   Never a silent break. The shim keeps at least the previous major callable
   (e.g. the old struct layout accepted and mapped onto the new one) for one
   release, and it is documented in this file.
3. `opencode_abi_version()` / `opencode_config_version()` /
   `opencode_event_version()` return the macros. A host must check
   `opencode_abi_version()` before calling anything and refuse a mismatch.

## General conventions

- Every function returns `opencode_status_t`; `OPENCODE_OK (0)` on success.
- Every OUT pointer argument must be non-NULL. Passing a NULL handle
  argument returns `OPENCODE_ERR_VALIDATION` — never a crash.
- **Nothing throws across the boundary.** The C++ implementation wraps every
  call in error handling; hosts must not build any exception expectations.
- **Inputs are copied.** String pointers passed in (config fields, prompts,
  memory keys/values) are copied by the engine before the call returns; the
  caller's buffers do not need to outlive the call.
- **Outputs are caller-owned.** Text OUT buffers (event `text`,
  `opencode_memory_read`) are written by the engine up to the caller's
  `*_cap` and NUL-terminated where the type allows. A non-NULL buffer with
  zero capacity is a validation error.

## Status codes

| Code | Value | Meaning | Retry? |
|------|-------|---------|--------|
| `OPENCODE_OK` | 0 | success | — |
| `OPENCODE_ERR_NETWORK` | 1 | transient network failure | yes |
| `OPENCODE_ERR_AUTH` | 2 | bad/missing credentials | no |
| `OPENCODE_ERR_VALIDATION` | 3 | bad argument / config | no |
| `OPENCODE_ERR_BUSY` | 4 | engine mid-task | later |
| `OPENCODE_ERR_CANCELLED` | 5 | cancelled by host or budget | no |
| `OPENCODE_ERR_FATAL` | 6 | internal invariant broken | no |
| `OPENCODE_ERR_NO_NETWORK` | 7 | offline; queued, resumes later | yes |

## Object model

- `opencode_engine_t` — one self-contained engine instance. Owns the
  configuration, provider wiring, tool registry, gate, verify gates, store,
  and metrics. Created by `opencode_engine_create`, destroyed by
  `opencode_engine_destroy`. **Never free it any other way.**
- `opencode_session_t` — reserved for future per-task handles; declared but
  not yet returned by any v1 function.
- `opencode_config_t` — an immutable configuration snapshot. The engine
  copies every field it needs during `create`/`set_config`; the host may
  reuse or free the struct immediately after the call. `version` must equal
  `OPENCODE_CONFIG_VERSION` or the call returns `OPENCODE_ERR_VALIDATION`.

## Entry points (v1)

| Function | Notes |
|----------|-------|
| `opencode_engine_create(cfg, &eng)` | `cfg` may be NULL (= defaults). Returns the handle or a status. |
| `opencode_engine_destroy(eng)` | idempotent-safe; NULL → VALIDATION. |
| `opencode_engine_set_config(eng, cfg)` | applies to the next task; active tasks unaffected; BUSY while a task runs. |
| `opencode_engine_run(eng, prompt, on_event, userdata)` | **synchronous in v1.** Runs one task to completion on the calling thread. `on_event` may be NULL. |
| `opencode_engine_drive(eng, wait_ms)` | no-op pump in v1: OK when idle, BUSY mid-task. |
| `opencode_engine_cancel(eng)` | the **only** entry point safe to call from any thread. Requests cancellation; the running task stops at the next safe point and reports CANCELLED. |
| `opencode_metrics_snapshot(eng, sink, ud, &count)` | `sink` must be non-NULL; `count` may be NULL. Iterates all metrics. |
| `opencode_memory_write(eng, kind, key, value, tags_json, out_id, cap)` | tags as a JSON array string (`"[]"` ok). Returns the entry id via `out_id`. |
| `opencode_memory_read(eng, kind, keywords_json, out, cap, &len)` | keyword/kind-scoped memory context into a caller-owned buffer. |

## Configuration (`opencode_config_t`)

Two composition styles:

- `config_path` set → load an opencodepp JSON config file, then overlay any
  non-NULL direct fields on top.
- `config_path` NULL → use the direct fields only.
- NULL cfg on create → defaults: workspace `.`, provider `openai_compat` at
  `127.0.0.1:8080`, model `mock-model`, agent `default`, policy
  `OPENCODE_POLICY_DENY`.

Key fields: `workspace` (the tool/gate sandbox root — **required** for real
use), `base_url`, `api_key`, `model`, `agent`, `network_timeout_ms`,
`tool_policy`, the memory caps, and the four callback slots (`on_event`,
`on_permission`, `on_consent`, `on_log`) plus `userdata`.

## Callback contract

- **All callbacks fire on the engine thread only** — the thread that called
  `opencode_engine_run`. Hosts must not touch engine state from inside a
  callback (no re-entrant `run`/`set_config`); defer to your own loop if you
  need that.
- Callback-provided strings (`name`, `msg`, `params_json`) are valid only for
  the duration of the call.
- The event sink returns `OPENCODE_OK` to continue; **any other status
  cancels the run** and becomes the run's return value.
- The permission hook returns 1 to allow / 0 to deny a tool call; denial is
  surfaced to the model as a structured tool error (it is not a run failure).
- The consent hook is reserved in v1 (the permission hook is the tool-call
  consent point); it is always invoked on the engine thread.

## Events (`opencode_event_t`)

Variant payload keyed by `kind`, mirroring the agent state machine 1:1 so a
host can render every phase without parsing free text. The host supplies the
`text` buffer (`text`/`text_cap`); the engine fills it and sets `text_len`.
`data_i64` is the generic integer slot (e.g. tokens on DONE), `status` is a
core error code value (0 = ok), `lane` is the orthogonal agent lane
(backoff/offline/paused). `OPENCODE_EVENT_TEXT_MAX` bounds `text_cap`.

The buffer is **reused across events in one run** — copy what you need inside
the callback (the Python binding snapshots the whole event).

## Thread safety matrix

| Call                     | Engine thread | Any thread |
|--------------------------|---------------|------------|
| `run` / `set_config` / `drive` / memory ops | yes | no |
| `metrics_snapshot`        | yes | no (engine thread preferred) |
| `cancel`                  | yes | **yes** |
| `destroy`                 | yes | no |

External wakeups (e.g. a signal handler cancelling a long run) go through
`opencode_engine_cancel`; v1 tasks run synchronously, so no additional
pumping is needed.

## Bindings

All three bindings target the frozen header and are exercised end-to-end in
CI against a mock SSE provider:

- **C++** — `src/abi/opencode.hpp`: `opencode::abi::Engine`, a move-only
  RAII wrapper. Used by `tools/opencode_cli`.
- **Python** — `bindings/python/opencode.py`: pure-stdlib `ctypes` binding,
  no build step. Loads `libopencodepp.so` (set `OPENCODE_LIB` or use the
  default CMake output paths). `Engine.run(...)` snapshots every event so
  host code may keep event objects after the callback.
- **JNI** — `bindings/jni/jni_opencode.cpp`: JNI glue over the same ABI,
  plus `bindings/jni/java/io/opencode/Opencode.java`, a JDK-only harness with
  an in-JVM mock server. Built when a JDK is present
  (`find_package(JNI)`); no NDK networking deps — host TLS lives in the engine.

## ABI surface (v1 snapshot)

- Handles: `opencode_engine_t` (opaque).
- Enums: `opencode_status_t` (0–7), `opencode_tool_policy_t` (0–3),
  `opencode_memory_kind_t` (0–5), `opencode_event_kind_t` (1–11),
  `opencode_lane_t` (0–3), `opencode_metric_kind_t` (0–2).
- Structs: `opencode_config_t`, `opencode_event_t`.
- Functions: `engine_create/destroy/set_config/run/drive/cancel`,
  `metrics_snapshot`, `memory_write/read` — 9 exported entry points, plus the
  3 header-only version helpers (`abi/event/config_version`).

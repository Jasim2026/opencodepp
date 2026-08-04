# API — the frozen OpenCode++ C contract (v1)

`include/opencode/opencode.h` is the **only public interface** of
`libopencodepp`. The header is normative; this page is a guide to it. It
compiles as pure C11 and C++20 and is checked under
`-Wall -Wextra -Wpedantic -Werror` on every CI run.

Companion reference: `docs/ABI.md` (versioning rules, struct layout, shim
policy). The ABI document is normative for anything this page summarizes.

## Handles

| Type | Meaning |
|------|---------|
| `opencode_engine_t` | one self-contained engine instance (owns config, store, sessions) |
| `opencode_session_t` | reserved: one task session inside an engine (future) |
| `opencode_config_t` | immutable configuration snapshot handed to create/set_config |

Handles are opaque pointers. A NULL handle argument returns
`OPENCODE_ERR_VALIDATION`; all OUT pointers must be non-NULL.

## Status codes

| Code | Meaning | Retryable? |
|------|---------|-----------|
| `OPENCODE_OK`            | success | — |
| `OPENCODE_ERR_NETWORK`   | transient network failure | yes (backoff) |
| `OPENCODE_ERR_AUTH`      | bad/missing credentials | no |
| `OPENCODE_ERR_VALIDATION`| bad argument / config | no |
| `OPENCODE_ERR_BUSY`      | engine mid-task; try later | yes (poll) |
| `OPENCODE_ERR_CANCELLED` | cancelled by host or budget | no |
| `OPENCODE_ERR_FATAL`     | internal invariant broken | never retry |
| `OPENCODE_ERR_NO_NETWORK`| offline; queued, resumes | yes |

Every ABI function returns `opencode_status_t` and never throws.

## Configuration

`opencode_config_t` (version `OPENCODE_CONFIG_VERSION`):

- `workspace` (required): sandbox base for tools and the verify gate.
- `config_path`: optional opencodepp JSON config file; non-NULL direct fields
  overlay it.
- `prompt_dir`: optional prompt template directory. If NULL the engine probes
  the repo-root template locations relative to the process CWD
  (`src/prompt/templates`, `../src/prompt/templates`,
  `opencodepp/src/prompt/templates`).
- `provider` / `base_url` / `api_key` / `model` / `agent`: provider selection
  (see `docs/PROVIDERS.md`).
- `network_timeout_ms`: per-request timeout; 0 = default.
- `tool_policy`: `OPENCODE_POLICY_{DENY,ASK,ALLOW,ALLOW_READONLY}`.
- memory caps: `memory_max_entries`, `memory_max_entries_per_task`,
  `memory_max_value_chars`.
- callbacks (all on the engine thread; any may be NULL):
  `on_event`, `on_permission`, `on_consent`, `on_log`, `on_metric`, `userdata`.

A NULL `cfg` on create = defaults (workspace ".", provider
`openai_compat` on `127.0.0.1:8080`, model `mock-model`, agent `default`,
policy `deny`).

## Entry points

| Function | Purpose |
|----------|---------|
| `opencode_abi_version()` | header-inline; returns `OPENCODE_ABI_VERSION` |
| `opencode_engine_create(cfg, &eng)` | build an engine; cfg may be NULL |
| `opencode_engine_destroy(eng)` | release everything owned by the engine |
| `opencode_engine_set_config(eng, cfg)` | swap config for the *next* session |
| `opencode_engine_drive(eng, wait_ms)` | pump the event loop up to `wait_ms` (-1 = idle). v1 runs tasks synchronously via `run`; returns `OPENCODE_OK` when idle, `OPENCODE_ERR_BUSY` mid-task |
| `opencode_engine_run(eng, prompt, on_event, userdata)` | run one task to completion (or budget/cancel); events stream to `on_event` |
| `opencode_engine_cancel(eng)` | cancel at the next safe point; the **only** function safe from any thread |
| `opencode_metrics_snapshot(eng, sink, userdata, &count)` | enumerate all metrics through `sink` |
| `opencode_memory_write(eng, kind, key, value, tags_json, out_id, cap)` | store a Phase 11 memory entry |
| `opencode_memory_read(eng, kind, keywords_json, out, cap, &len)` | read budget-bounded memory context |

## Events

`opencode_event_t` is a versioned, size-bounded variant. `kind` mirrors the
agent state machine so a host can render every phase without interpreting free
text:

`OPENCODE_EVENT_LOG, PREPARING, CONNECTING, STREAMING, TOOL_PHASE, VERIFYING,
APPLYING, DONE, FAILED, CANCELLED, FOLD`.

- `text` points into a host-owned buffer (`text_cap` bytes; engine writes
  `text_len`); it may be NULL if the host ignores text.
- `data_i64` is a generic int slot (tokens on `DONE`, bytes, call ids).
- `lane` reports an orthogonal agent lane: `BACKOFF`, `OFFLINE`, `PAUSED`.
- Return `OPENCODE_OK` from a callback to continue; any other status cancels
  the run.

## Memory ops

Keys allow `[A-Za-z0-9._-]` only (max `max_key_chars`); values are capped at
`memory_max_value_chars`; keys/values matching secret patterns
(`token`/`password`/`secret`/`sk-`) are rejected at write time. `tags_json`
is a JSON array of strings (may be NULL/empty). `memory_read` returns
`[kind] key: value` lines, budget-bounded.

## C++ wrapper

`src/abi/opencode.hpp` provides a move-only RAII `opencode::abi::Engine` over
the same ABI (used by the reference CLI and examples). It is header-only,
throws nothing, and is not part of the frozen public surface.

# Architecture

Condensed from the phase plans in `/sdcard/project/plan/` (the reference).
The source of truth for "which phase added what" is `src/CMakeLists.txt`
(the `OPENCODE_CORE_SOURCES` list is the module map).

## Design constraints (locked)

- **Stdlib-first.** Every third-party dep is optional and OFF by default
  (`04_DEPENDENCY_POLICY.md`): mbedTLS (TLS), sqlite (durable store),
  tree-sitter (parse backend), zstd (compression). Optional backends are
  isolated behind interfaces in their own TU and never imported elsewhere.
- **One public surface.** `include/opencode/opencode.h` (frozen v1). Engine
  internals may change freely between releases.
- **Never abort, never throw across the ABI.** All errors are `core::error`
  codes; the agent treats network errors as retryable (backoff/offline lanes).
- **T1/T2/T3 targets are locked** — see `docs/TARGETS.md`. Changes that move
  a measured number must prove it in `tools/measure`.

## Module map

```
include/opencode/opencode.h   frozen C ABI (the only public header)
src/abi/                      ABI entry points + C++ RAII wrapper
src/agent/                    state machine: intent, loop, session, feedback
src/app.cpp                   the Engine: config, store, session orchestration
src/config/                   config parsing + env snapshot
src/core/                     arena, event_loop, channel, task, log, metrics
src/graph/                    symbol index, call graph, queries, snippets
src/memory/                   entries, session/workspace memory, summarizer
src/msg/                      messages, codec, token estimator
src/model/                    provider catalog + model resolution
src/net/                      socket, http1, sse, tls (+backends), pool, policy, meter, offline
src/provider/                 one interface, four wire adapters + factory
src/prompt/                   templates, compiler, context assembler, budget
src/store/                    mem_store (default) + optional sqlite_store
src/tools/                    registry, schema, exec (shell/write/read-only/patch), memory tool
src/verify/                   gates, diff, impact, syntax, symbol verification
src/util/                     base64, json, sha1
```

## Thread model

- The engine **spawns no threads** for a task; `opencode_engine_run` runs the
  task synchronously on the calling thread.
- All callbacks (`on_event`, `on_permission`, `on_log`, `on_metric`) fire on
  the engine thread only.
- `opencode_engine_cancel` is the single thread-safe entry (safe from any
  thread, cancels at the next safe point).
- `std::thread` is used only for agent backoff sleeps (`src/agent/loop.cpp`);
  the packaged library links `Threads::Threads` because of this.

## Data flow (one task)

```
host  --open/run-->  abi  --Engine::run-->  agent::loop
                                                |
        intent classify -> context assembly (prompt/)
        provider request  -> net/ transport (http1 + sse, TLS via backend)
        streamed frames   -> provider adapter normalizes to StreamEvents
        tool calls        -> tools/ registry (gated by permission callback)
        write proposals   -> verify/ gates (syntax/diff/impact/testmap)
        apply             -> workspace files (writes serialized + gated)
        feedback          -> session (messages/tokens; T1 budget)
        done/failed       --events--> host (on_event), metrics snapshot
```

Memory (`opencode_memory_write/read`) and metrics (`opencode_metrics_snapshot`)
are host-callable at any idle point; both are scoped to the engine workspace.

## Resilience lanes

Transient network failures never abort: the loop transitions to the
`BACKOFF` lane (retry with backoff) or `OFFLINE` (queued, resumes when
reachable). Malformed payloads (`e_proto_parse`) are non-retryable by
doctrine — a provider that sends garbage ends the task with `FAILED`, never a
crash. `tools/soak` drives the real binary against injected faults
(HTTP 500, truncated SSE, malformed frames) to prove this.

## Prompts

All prompt text lives in `src/prompt/templates/` (nothing prompt-shaped lives
in code). The compiler resolves templates relative to the process CWD
(`src/prompt/templates`, `../src/prompt/templates`,
`opencodepp/src/prompt/templates`) unless `config.prompt_dir` is set.

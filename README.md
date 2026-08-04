# OpenCode++ (libopencodepp)

A hyper-efficient, embeddable C++ agentic-coding engine. One frozen C ABI
(`include/opencode/opencode.h`), no required third-party dependencies, and
locked size/latency/memory targets. It drives a model provider through an
agent loop with tools, a verification gate, and workspace memory — in a
process footprint measured in single-digit megabytes.

## What it is

- **An embeddable engine**, not a chat client: hosts link one static or
  shared library and drive tasks through a versioned C ABI.
- **Stdlib-first.** Every third-party backend (mbedTLS, sqlite, tree-sitter,
  zstd) is optional and OFF by default.
- **Agentic with guardrails:** intent/context assembly, streaming provider
  adapters, gated tool dispatch (permission callback), a Phase 9 verify gate
  over write proposals, and Phase 11 workspace memory.
- **Resilient by doctrine:** network failures retry with backoff or queue
  offline; malformed payloads fail gracefully; nothing ever aborts.

## Quickstart (reference CLI)

```
cmake --preset release && cmake --build --preset release -j
./build/release/tools/mock_api &        # offline provider on :8123
./build/release/tools/opencodepp_cli run "Refactor add() to take a callback"
./build/release/tools/opencodepp_cli repl
```

`opencodepp_cli --version` prints the release and ABI version.

## Embedding (smallest host)

```c
#include <opencode/opencode.h>

static opencode_status_t on_event(void*, const opencode_event_t* ev) {
    /* ev->kind + ev->text drive a UI; return OPENCODE_OK to continue */
    return OPENCODE_OK;
}

int main(void) {
    opencode_engine_t* eng = 0;
    opencode_config_t cfg; /* memset + fill: workspace, base_url, policy, on_event */
    if (opencode_engine_create(&cfg, &eng) != OPENCODE_OK) return 1;
    opencode_status_t st = opencode_engine_run(eng, "my task", 0, 0);
    opencode_engine_destroy(eng);
    return st == OPENCODE_OK ? 0 : 1;
}
```

Full hosts: `examples/embed_cli.cpp` (C ABI only), `examples/event_host.cpp`
(C++ RAII wrapper + memory/metrics), `examples/android/` (JNI harness).

## Installing

```
cmake --install build/release --prefix /usr/local
```

Consumers use either CMake or pkg-config:

```cmake
find_package(opencodepp CONFIG REQUIRED)
target_link_libraries(app PRIVATE opencodepp::opencodepp_static)
```

```
cc $(pkg-config --cflags --libs opencodepp) app.c
```

`find_package(opencodepp CONFIG)` works from a scratch project and is
verified on every CI run. Installed targets: static + shared libs, the public
header, the reference CLI, package config, and pkg-config files.

## Status

- Phases 0–14 complete (core → ABI/bindings → optimization/hardening →
  packaging/docs/handover). ABI frozen at v1 (0.13.0).
- CI: 13 jobs — build matrix (dev ASan/UBSan, release, `-Os` size), sqlite +
  mbedTLS backends, python (ctypes) + JNI bindings, ABI C11/C++20 compile,
  fuzz sweep, soak, mock smoke, install + scratch consumer, hygiene.
- Targets: locked T1/T2/T3 table with evidence — see `docs/TARGETS.md` and
  `reports/`.

## Documentation

- `docs/ABI.md` — frozen C contract, versioning, shims (normative)
- `docs/API.md` — the public API, function by function
- `docs/ARCHITECTURE.md` — module map, thread model, data flow
- `docs/PROVIDERS.md` — adding a provider adapter
- `docs/CONTRIBUTING.md` — day-to-day contribution contract
- `docs/TARGETS.md` — locked performance targets and how they are asserted

## License

See `LICENSE`.

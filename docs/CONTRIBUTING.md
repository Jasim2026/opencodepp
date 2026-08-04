# Contributing

OpenCode++ follows `02_CODING_PROTOCOL.md` (in `/sdcard/project/plan/`).
Read it first — it is the normative development contract. This page points
at the parts that matter day-to-day.

## Ground rules

- **One public surface:** `include/opencode/opencode.h` (frozen ABI v1). Any
  change to it requires a version bump + compatibility shim (see
  `docs/ABI.md`).
- **Stdlib-first:** every third-party dep is optional and OFF by default. Do
  not add a required dependency (`04_DEPENDENCY_POLICY.md`).
- **Never abort, never throw across the ABI.** All errors are `core::error`
  codes; network errors are retryable by doctrine.
- **No TODO stubs in production code.** Deferred ideas go in `NOTES.md`
  (protocol §2.4).
- **No secrets in code/tests/commits.** The secret filter (Phase 11) is
  enforced in CI.
- **Prompt text lives only in `src/prompt/templates/`.** Tests live in
  `tests/<mod>_test.cpp`.

## Building and testing

The authoritative test site is CI (GitHub Actions, 13 jobs): dev/release/size
presets (ASan/UBSan in dev), sqlite + mbedTLS backend matrices, python
(ctypes) and JNI bindings, ABI C11/C++20 compile, fuzz sweep, soak, mock
smoke, install + `find_package` scratch consumer, and source hygiene.

Local low-memory constraint on the dev box: never `ninja`/`cmake --build`.
Verify single TUs directly, e.g.:

```
g++ -std=c++20 -I src -I include -I . -Wall -Wextra -Wpedantic -Werror \
  -fsyntax-only tests/<mod>_test.cpp
```

`cmake --preset <p>` (configure only) is fine.

## Before committing

```
bash scripts/check_hidden_chars.sh        # no non-ASCII in source
```

Then commit with a conventional type (`feat|fix|build|docs|test|chore|perf`),
push, wait for CI, and only proceed when **every** job concludes `success`.
Fix any failure before the next commit.

## Targets

`docs/TARGETS.md` holds the locked T1/T2/T3 table. A change that moves a
measured number must prove it in `tools/measure`; the `hardening` ctest
asserts the T2 budgets with slack on every CI run.

## Phase rhythm

Each phase has a plan file in `/sdcard/project/plan/` and a close-out report
in `reports/`. Close a phase by running its acceptance gate, updating
`CHECKPOINTS.md` (append, never rewrite history), and writing the report.
See `CHECKPOINTS.md`'s close-a-phase ritual.

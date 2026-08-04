# NOTES.md — deferred ideas + debugging lessons

Per `02_CODING_PROTOCOL.md` §2.4: deferred ideas and debugging lessons live here, **never** in
production code as TODO stubs. Checkpoint reports reference this file by section.

---

## Deferred ideas (candidate improvements, awaiting approval)

- **TLS for local dev loop:** Phase 0 CI uses plaintext `mock_api` only. mbedTLS/host-callback TLS
  lands with Phase 4 (`net/tls`). No earlier action.
- **CMake on CI:** GitHub Actions runner `ubuntu-latest` ships cmake ≥ 3.28; we require ≥ 3.20 and
  CMakePresets v6 works. No pinning needed yet.
- **Optional deps are all OFF in presets until their phase lands** (Phase 00 instructs all OFF):
  mbedTLS + tree-sitter will flip ON in dev/release at Phase 4/9, sqlite at Phase 3, zstd at
  Phase 13 — per `04_DEPENDENCY_POLICY.md` §3.4.
- **`opencode_abi_version()`:** implemented header-only (macro + inline fn) in Phase 0 per plan; the
  exported symbol seed (`src/abi/version.cpp`) feeds the shared lib and becomes part of the full ABI
  surface in Phase 12.

## Debugging lessons

- **This edge device is a midrange phone (low RAM); heavy builds crash it.** Never run
  `ninja`/`cmake --build` (parallel, RAM-heavy) locally. Local verification is light-only: compile
  individual TUs with plain `g++ -std=c++20 -I src -I include -I . -Wall -Wextra -Werror -c`,
  then link a single test binary with `g++ ... -o` (a one-shot link is OK). Delegate ALL full builds
  (dev/release/size/ctest matrix) to GitHub Actions CI; commit/push then `gh run watch` is the
  authoritative build+test run. The local cmake build dir in `/tmp/opencode/cmake-dev` was a one-off
  sanity check; do not rebuild it here.
- **ASan cannot run in the proot sandbox** (local box): ASan fails to mmap its
  shadow memory (`heap size exceeds max user virtual address`). Symptom: test
  binary crashes with `AddressSanitizer: CHECK failed`. Fix: run local sanity
  builds without `OPENCODE_ENABLE_SANITIZERS`; CI (real Linux VM) is the
  authoritative sanitizer run. Never assume a local ASan crash is a code bug
  when the process map shows the proot loader.
- **Do not build on fuse-mounted /sdcard**: CMake/Ninja enter an infinite
  "Re-running CMake..." loop because the FUSE layer's timestamps defeat ninja's
  staleness check. Symptom: `[0/71] Re-running CMake...` then `[0/72]` ... forever.
  Fix: configure/build in a real-fs path (e.g. `/tmp`), keeping `/sdcard/project/opencodepp`
  as pure source. CI builds on native Linux ext4 and is unaffected.
- **CMake forbids empty SHARED libraries** ("No SOURCES given to target"). Fix:
  build static+shared from a shared OBJECT library; seed it with one real TU
  (`src/abi/version.cpp`). Object-lib + `$<TARGET_OBJECTS>` is the scalable
  pattern for adding phase sources.
- **`constexpr` members need the keyword on every constructor** (GCC 15 treats
  the class as non-literal otherwise: "has no 'constexpr' constructor that is
  not a copy or move"). Obvious in hindsight; cost one CI-ish cycle.
- **Relative source paths in a subdir CMakeLists resolve against that subdir**:
  `src/CMakeLists.txt` must name `abi/version.cpp`, not `src/abi/version.cpp`
  (which resolves to `src/src/abi/version.cpp`).
- **`$<TARGET_OBJECTS>` does NOT propagate link usage requirements**: linking
  `sqlite3` to the OBJECT lib silently does nothing for the static/shared
  consumers, so CI failed at link with `undefined reference to sqlite3_*`
  while local plain-g++ runs passed. Fix: `target_link_libraries(opencodepp_static
  PUBLIC sqlite3)` (+ shared) inside `if(OPENCODE_USE_SQLITE)`. When adding any
  optional library behind a flag, always link it on the real consumer targets.
- **`std::make_unique<T>` in a factory can't reach a private ctor**, even from
  a `friend` function declared as the only way to build `T`. Fix: the friend
  factory uses `out = std::unique_ptr<T>(new T(std::move(impl)))` directly.
- **Two `to_json` overloads collide** once `Message` and `JVal` both have one
  (`util::to_json` vs `msg::to_json`); qualify by namespace. `JVal` also has no
  `operator==`, so tests compare `util::to_json(x)` strings.
- **Feature-guard a test at compile time with a CMake bool**: `#if
  OPENCODE_USE_SQLITE` in `store_test.cpp` + `target_compile_definitions(store_test
  PRIVATE OPENCODE_USE_SQLITE=$<BOOL:${OPENCODE_USE_SQLITE}>)`. The suite runs
  on sqlite only when the backend is ON, and the define is always a 0/1.
- **git detection in tests**: `config_test` needs `WORKING_DIRECTORY` = repo
  root, else env_snapshot sees no `.git` and the git fields are empty. The
  `git_dirty` field is intentionally a *staged-proxy* heuristic (`.git/index`
  mtime vs HEAD) so the snapshot never spawns a subprocess.
- **`Content-Length: 0` is length-framed, not EOF-framed**: `HttpParser`
  originally treated CL:0 responses as EOF-framed, so 429/500 empty bodies
  surfaced as premature-EOF `e_net_connect detail=1` instead of a parsed
  response. Fix: `length_framed_` flag set whenever Content-Length is present
  (incl. 0); `body_done()` uses `body_got_ >= content_length_` when set. Found
  by `drill sc_rate_limit/sc_server_error`.
- **SIGPIPE kills net test tools**: a server writing to a socket whose peer
  already closed dies on SIGPIPE before the client can finish the scenario
  (drill's probe connect-then-close pattern). Fix: `signal(SIGPIPE, SIG_IGN)`
  in tool `main()`.
- **`e_proto_parse` is deliberately non-retryable**: a garbage-bytes test must
  assert a clean structured error + session survival, NOT a retry. Same for
  `e_auth`. Only 429/5xx/net-* retry.
- **mbedTLS is API-versioned 2.x vs 3.x**: `mbedtls_ssl_session_copy` does not
  exist even in 3.6.5; the portable 2-arg `mbedtls_ssl_get_session(ctx, copy)`
  is identical in 2.x and 3.x. CI apt ships 2.28 while local Termux has 3.6.5,
  so any version-dependent call must be checked against BOTH (the original
  inverted `#if >= 0x03000000` guard only built locally). Prefer the portable
  form when one exists.
- **Server-side accept counting races the client's `connect()`**: the accept
  thread increments its atomic after the connect returns, so asserting
  `accepted.load() == N` right after connecting is flaky. Fix: bounded
  `wait_accepted()` yield loop before each assertion.
- **`rtt_p50() > 0` is flaky on fast localhost**: an -O3 exchange can complete
  in < 1 ms, so percentile assertions failed in release while passing in debug.
  Assert `rtt_samples() >= 1` (count) instead of a duration inequality.
- **`util::JVal` is zero-copy: `JVal::Str(std::string)` fed a temporary
  dangles** — the `str` view points into caller memory that dies with the
  temporary. Every dynamically built string must go through the local
  `owned_str()` idiom (`j.kind = string; j.owned = std::move(s); j.str =
  j.owned`). Missed ownership crashes exactly at the first `j.str` read (found
  while generating anthropic request bodies).
- **`split_url` strips the trailing slash and leaves `path` empty when
  absent** — adapters must append their own `/v1/...` (`url.path +
  "/v1/chat/completions"`). Pinned by the golden-byte fixtures
  (`https://api.openai.com/v1` → `/v1/chat/completions`).
- **Anthropic `message_delta.usage` carries only `output_tokens`**:
  overwriting the whole `Usage` from each delta wipes `input_tokens` to 0.
  Update per-field, only when the key is present (field-existence check, not a
  tolerant `get_num` default).
- **Hand-written golden fixtures gain a trailing `\n`** → byte-comparison with
  the adapter's compact `to_json` output (no trailing newline) fails by exactly
  one byte. Trim the trailing newline in the test harness's `read_file` and
  keep the fixtures readable.
- **`std::string_view` (from `error_code::message()`) has no `.c_str()`** —
  wrap `std::string(ec.message())` before `printf("%s", ...)`; a blanket sed
  of `.message().c_str()` → `.c_str()` mis-rewrites it, so grep after.
- **A user-declared `~T` suppresses the implicit move ctor**: a factory that
  returns `Mock` by value (`start_mock()`) broke with "use of deleted copy".
  Fix: explicit move ctor + move-assign, delete copy. Related: a joinable
  `std::thread` abandoned on an early `return` calls `std::terminate` — give
  the RAII wrapper a destructor that stops + joins the server thread on every
  path.
- **`estimate_tokens` treats an unbroken token as ONE word**: a 1 MB `'x'`
  blob estimates to ~1 token (single prose word), so "huge message" truncation
  tests silently pass. Use spaced text (`"word "` repeated) to build a
  realistically heavy message; assert the truncation suffix, not just a count.
- **The tiered-window scan must not overwrite its boundary**: the first cut of
  the Phase 6 assembler set `tier1_from` while walking the newest assistant
  turns, then overwrote it to `i+1` when hitting the (N+1)-th assistant —
  silently demoting the newest assistant response to Tier 2 (omittable). Fix:
  find the N-th-newest assistant and set the boundary once. The newest user
  message stays Tier 1 even with no assistant turns in history.
- **The system message has real token weight**: SYSTEM_BASE.md ~ 1.5 KB of
  markdown text estimates to ~330-377 tokens (code-heavy path). Budget-gated
  fixtures must reserve for it: Tier-2 room = cap − floor − tools − system, so
  a "forces omission" fixture needs fillers that are individually heavy, not
  just numerous. Filler tokens were repeatedly under-estimated (a 360-char
  prose sentence ≈ 66-86 tokens).
- **`JVal::find(key)` returns `nullptr` for a missing key**: corpus fixtures
  read optional fields as `doc.find("available_tokens") ? (uint32_t)v->num :
  default`; the ternary is required, dereferencing a missing key crashes.
- **CMake `SUPPRESS_REGENERATION` breaks target discovery**: with
  `CMAKE_SUPPRESS_REGENERATION=ON` (needed to dodge the FUSE ninja re-run
  loop), ninja does NOT see new targets added to a CMakeLists — you must
  re-run `cmake -S . -B build/<p> ...` by hand before building a newly-added
  executable. `ninja: no work to do` despite a missing target = stale build
  manifest.
- **FUSE also strips the `+x` bit from built binaries** (separate from the
  ninja mtime loop): `ctest` on /sdcard fails with `permission denied` even
  though every binary links. Local verification copies each binary to `/tmp`
  and runs it there; CI (native ext4) is unaffected.
- **`remove_file(file)` on every extract is O(total) even for fresh files**:
  the Phase 7 index called it unconditionally, so indexing N files scanned the
  growing syms/deps arrays N times (200 files 780 ms → 1000 files 7.4 s —
  super-linear). Fix: skip the removal for files not yet in `files_`, and
  tombstone by recorded range (syms/deps are append-only, so ranges never
  drift; deps tombstones are reclaimed by lazy compaction when dead ≥ live).
  Queries must filter tombstoned deps (`from_file.empty()`).
- **Dup "def sym" emitted twice breaks lookup precedence**: the Go extractor
  pushed each func into `extra` AND the defs loop appended it again, so
  `by_name_` held two ids and `lookup`'s fast path preferred the second — a
  call-graph query on the returned id came back empty. Any backend that
  "defs first" must not also emit the same sym through a second path.
- **The I/O floor of this sandbox is ~0.3–0.7 s per 1000 small-file reads**
  (95%-full f2fs under proot; independent `open+read+close` microbench).
  `mmap`+`fstat` in one fd is ~1.7× faster than `stat`+`fread` and removes the
  per-block read syscalls. When a benchmark can't hit its time budget locally,
  first measure the read path in isolation before blaming the algorithm;
  index compute for 1k files is ~0.2 s.
- **`/dev/shm` is a bind of the root fs under proot, not a real tmpfs** — the
  `bench_graph` tmpfs fallback is still worth keeping for real Linux (CI)
  where /dev/shm is tmpfs and reads are near-free.

- **`patch::apply` lost ctx/rem interleaving** — the original parser stored
  only `ctx` (context lines) and `rem` (old-file removal lines), then matched
  on `old_lines = rem+ctx`. A patch that alternates removals with context lines
  (e.g. `ctx rem ctx rem`) was flattened into `rem rem ctx ctx ctx`, producing
  `old_lines` with wrong ordering vs the file content. Match failed with
  `e_tool_reject`. Fix: the `Hunk` now carries `old_body` and `new_body` as
  ordered vectors of `(is_ctx, text)` entries; matching and replacement operate
  on these ordered sequences. The test fixture `patch_multi_interleave` covers
  the previously-broken pattern.
- **`split_lines` trailing empty element on `\n`-terminated input** —
  `pos <= text.size()` let the final `find` land one past the last `\n`, adding
  a spurious empty string to the vector. When fed to `join_lines`, the empty
  element became an extra `\n`, doubling the trailing newline. One-char fix:
  `pos < text.size()`. The round-trip test (`lines → join → split` equality
  assertion) now catches this.
- **Hidden Unicode arrows in comments → CI failure** — `patch.cpp` and
  `util.cpp` contained literal `→` (U+2192 RIGHTWARDS ARROW) characters in
  comment text, copied from human-readable spec snippets. CI source-hygiene
  treats non-ASCII in non-string-literal C++ as a warning. The fix is
  mechanical: replace with ASCII `->`. When porting spec prose into code, scan
  for non-ASCII before committing.
- **`util::split` delimiter type mismatch** — `split(path, '/')` passes a
  `char` to a function that takes `std::string_view`. GCC allows the implicit
  conversion but Clang (and CI `-Werror`) reject it. Always use `"/"` (string
  literal, implicitly `string_view`) not `'/'` (char).
- **Phase 09 lessons:**
  - **`for(;;)` double-semicolon false positive** — the double-semicolon
    detector in `syntax.cpp` initially flagged `for(;;)` as a syntax error.
    Fix: when `;;` is encountered, check the delimiter stack for a preceding
    `(` (for-loop header). If found, suppress the warning.
  - **Unterminated string at EOF without newline** — the syntax scanner
    tracked string state but only emitted "unterminated" when a `\n` was found
    inside a string. At EOF without a trailing newline, the state was left
    dangling. Fix: after the main loop, check if still in a string/comment
    state and report accordingly.
  - **Hidden Unicode in gate/syntax comments** — same issue as Phase 08:
    `→` and `—` in comments cause CI source-hygiene failure. Always scan
    new `.cpp` files for non-ASCII before committing.
  - **Golden test "patch result mismatch" false accept** — the test fixture
    had a patch that correctly produced the `after_content`, so the gate
    correctly passed it. Fix: change the patch to produce wrong output.
  - **Golden test "patch replace multiple" false reject** — the minimal-diff
    heuristic in `diff.cpp` flags >80% line churn for small edits (<20 lines).
    A 3-line file with all 3 lines changed triggered it. Fix: expand the
    fixture to 21 lines to bypass the threshold.
  - **GateResult aggregate init** — `GateResult{Stage::syntax, true, ok(), ""}`
    triggers `-Wmissing-field-initializers` because `line`, `col`, and `file`
    are omitted. Fix: provide all fields explicitly.

## Phase 10 lessons

- **A user-declared `~T` suppresses the implicit move ctor (revisited for
  `FakeServer`)**: the test server struct had a `stop` `shared_ptr`, a move
  ctor that moved it, and a destructor that called `stop->store(true)` — the
  *moved-from* object's `stop` was null, so the moved-from destructor
  segfaulted on return-from-factory. Symptom: crash right after
  `start_server` returned, before any scenario printed. Fix: guard the
  destructor with `if (stop)`. Any RAII wrapper with a moved-from-able member
  must null-check before use in `~T`.
- **`{"error":{...}}` frames are not handled by every adapter**: `openai.cpp`
  ignored error frames (no `choices`), so a 200-with-error body looked like an
  empty successful turn. The anthropic adapter already mapped them; the openai
  adapter now mirrors it (`e_auth`/`e_rate_limit`/`e_provider_err`). When
  adding a provider adapter, error frames must be first-class.
- **Provider-level errors must be checked even when the transport closed
  cleanly**: `one_round` broke out of the retry loop on `ec.ok()` before
  inspecting `provider_err`, so clean-200 errors returned success. Structure:
  on `ec.ok()`, return `provider_err` if set, else `break`.
- **SSE usage accounting depends on frame order**: the fake server emitted
  `usage` in a separate frame *after* the `finish_reason` frame, so
  `MessageDone` fired with zero usage (the adapter emits done as soon as
  finish is known). Put `usage` in the same frame as `finish_reason`, or
  accept `tokens_used() == 0` in the test.
- **A fake server must outlive the client code that uses it**: `run_agent`
  built its `--mock` server in one scope then called `run()` after the scope,
  so the server thread was joined (port closed) before any connect →
  `e_net_connect`. Keep the server alive for the entire drive (same block, or
  pass it in).
- **`set_tests_properties` for a CTest must come AFTER its `add_test`**:
  referencing an as-yet-undeclared test is a hard configure-time CMake error
  on every CI job (the local light-compile pass never notices). Verify CMake
  wiring with a cheap `cmake -S . -B /tmp/...` configure before pushing.
- **`agent::to_string(state)` returns `string_view`, not `const char*`** —
  `printf("%s", ...)` is `-Werror=format`. Use `%.*s` with size/data.
- **`fprintf`/`fflush` probes are the local debugger** (no gdb, no ASan in
  proot, `ulimit -c` disabled): isolate a startup segfault by printing
  between each construction step, then minimize to a standalone fixture
  (see `srv_dbg.cpp` pattern).

---

## Phase 11 lessons

- **`JVal::Str` stores a `std::string_view` — never wrap a temporary
  `std::string` in it.** `JVal::Str(std::string(kind_name(e.kind)))` created a
  view onto a temporary that died at the end of the full expression; ASan
  reported `stack-use-after-scope` in `write_escaped` (`util/json.cpp:258`).
  Any `JVal::Str(expr)` whose `expr` is not already a `string_view`/literal of
  static duration must be reviewed. Fix: `JVal::Str(kind_name(e.kind))` (the
  enum-name table is `constexpr` static storage).
- **A "completed" CI run is not a green run.** All of Phase 11's first three
  commits failed CI on the identical ASan bug, but the runs were recorded as
  "green" because `gh run list --json status` says `completed` for *both*
  success and failure. Always gate on `conclusion == "success"` (e.g.
  `gh run watch <id> --exit-status`), never on `status`.
- **One introduced bug can silently poison several subsequent commits.** The
  dangling-view bug was born in the entries commit and masked by CI 3/8
  failures on the same three jobs for two more commits. When a new failure
  pattern appears, check the *introducing* commit, not just the latest one.
- **No local ASan in proot** (`AddressSanitizer: out of memory` — heap size
  exceeds max user virtual address). ASan-only issues must be caught by code
  inspection plus CI's ASan jobs; keep `JVal::Str`/`string_view` lifetime in
  mind when serializing.
- **Aggregate-init call sites break when a struct gains a field** — adding
  `store`/`memory_cfg` to `RegistryOptions` broke the brace-init sites in
  `run_agent.cpp` and `agent_loop_test.cpp`. Update every aggregate init in
  the same commit as the struct change (`-Wmissing-field-initializers` does
  not fire on incomplete brace lists that happen to be valid).
- **`needs_fold` thresholds interact with the fixture** — a 10×3-line test
  history needs `context_window=100` to actually trigger the fold (400 was a
  no-op). Assert the *whole set* on the boundary (`folded_count == 6`) rather
  than just "non-empty".

## Phase 12 lessons

- **JSON string escaping has a double-decoding trap in mock tool calls.** A
  tool-call `content` with a newline must be escaped at BOTH layers: the
  outer SSE `arguments` string and the inner JSON body. Bytes `\\n` (2
  backslashes in the literal) decode once to a raw newline → the strict proto
  parser rejects the args (`verify invalid file.write: proto_parse`) and the
  tool silently never runs — yet the run still returns OK because the model
  just "finishes" on the error feedback. Need `\\\\n` (4) in the literal so
  the args stay valid JSON with a `\n` escape. The C++ e2e had it right; the
  Python tests were the only ones bitten. Diagnose tool rounds by dumping the
  mock's request bodies (the tool feedback message names the failing stage).
- **Mock providers must match the real wire path.** The python test handler
  checked `/v1/chat`; the openai_compat provider posts to
  `/chat/completions` → 404 → retries exhausted. Serve the path the client
  actually uses.
- **Never hold `opencode_event_t*` past the callback.** The engine reuses the
  event buffer across a run; a host that reads `kind`/`text` lazily after
  `run()` returns reads stale or garbage values. Snapshot every field
  (including a text copy) inside the callback — the Python binding does this
  now.
- **ctypes CFUNCTYPE slots cannot be `None`.** Assigning `None` to a callback
  struct field or passing it to a function whose argtype is a CFUNCTYPE
  raises `ArgumentError` (not a graceful null). Pass a real no-op callback or
  a NULL pointer explicitly; `byref(None)` also crashes.
- **ASan-in-a-dlopened-library.** The `dev` preset links the sanitizer
  runtime into `libopencodepp.so`; Python's dlopen then dies ("ASan runtime
  does not come first in initial library list"). Hosts that dlopen the shared
  lib must link against a non-sanitized build (CI uses the `release` preset
  for the python binding job).
- **Engine configure is CWD-sensitive for prompt templates.** With no
  `prompt_dir`, template resolution tries `src/prompt/templates` etc. relative
  to the process CWD → `e_invalid_cfg` (VALIDATION) from a subdir. All hosts
  that need the default templates (CLI e2e, ABI test, Java harness) must run
  from the repo root; bindings that embed a `prompt_dir` are immune.

---

<!-- appends only; do not rewrite history -->

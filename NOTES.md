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

---

<!-- appends only; do not rewrite history -->

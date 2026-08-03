# NOTES.md — deferred ideas + debugging lessons

Per `02_CODING_PROTOCOL.md` §2.4: deferred ideas and debugging lessons live here, **never** in
production code as TODO stubs. Checkpoint reports reference this file by section.

---

## Deferred ideas (candidate improvements, awaiting approval)

- **TLS for local dev loop:** Phase 0 CI uses plaintext `mock_api` only. mbedTLS/host-callback TLS
  lands with Phase 4 (`net/tls`). No earlier action.
- **CMake on CI:** GitHub Actions runner `ubuntu-latest` ships cmake ≥ 3.28; we require ≥ 3.20 and
  CMakePresets v6 works. No pinning needed yet.
- **Empty static/shared libs in Phase 0:** libraries exist with no sources until Phase 1+ populates
  them; CMake tolerates this on GCC/Clang. If a platform errors, add a stub symbol then.
- **`opencode_abi_version()`:** implemented header-only (macro + inline fn) in Phase 0 per plan; the
  exported symbol form comes with the full ABI in Phase 12.

## Debugging lessons

- (none yet)

---

<!-- appends only; do not rewrite history -->

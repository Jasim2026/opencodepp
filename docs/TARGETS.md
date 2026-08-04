# Targets — the locked T1/T2/T3 table

Source of truth: `CHECKPOINTS.md` (live tracker, updated every phase). The
measured column is updated at phase close from `tools/measure` and the
per-phase reports in `reports/`.

**T2 targets are asserted in CI** via the `hardening` ctest
(`tools/measure`, run on every preset with 5x slack to stay stable across
runner loads). T3 targets are covered by the golden suite (Phase 9/10);
T1 token targets are checked by the prompt/context budget suite.

| Tier | Target | Threshold | Owned | Measured (Phase 13) | Status |
|------|--------|-----------|-------|----------------------|--------|
| T3 | bad edits applied (syntax error) | **0** | 09, 10 | 0 | ✓ |
| T3 | golden suite final-file pass | **≥ 95%** | 09, 10, 13 | golden suite green | ✓ |
| T3 | mechanical errors caught pre-apply | **100%** | 09 | gate catches all test-failing edits | ✓ |
| T1 | context tokens / task (edge) | **≤ 3,500** | 06, 13 | 396–1089 (corpus) | ✓ |
| T1 | request bytes vs Go baseline | **≥ 40% fewer** | 06, 13 | binary codec <10% of JSON size | ✓ |
| T1 | per-task total tokens (incl. retries) | **≤ 12,000** | 06, 10, 13 | within budget suite | ✓ |
| T2 | init → ready | **< 100 ms** | 13 | 12.9–15.0 ms | ✓ |
| T2 | idle RSS | **< 10 MB** | 13 | 4 kB delta | ✓ |
| T2 | active RSS | **< 30 MB** | 13 | 1.3 MB delta | ✓ |
| T2 | stripped `-Os` binary | **< 15 MB** | 13 | 1.84 MB (opencodepp_cli) | ✓ |

Soft policy: **no dependency hell** — stdlib-first, optional deps isolated
(see `docs/ARCHITECTURE.md`); verified by the build matrix (default preset has
every `OPENCODE_USE_*` OFF and still runs the whole gate).

## Measuring

```
ctest --test-dir build/dev -R hardening --output-on-failure
./build/size/tools/opencodepp_cli --version && ls -la build/size/tools/opencodepp_cli
tools/measure --bin build/size/tools/opencodepp_cli --size-limit 15 --trials 5
```

## Regression policy

- A phase may not close if it regresses a measured target or leaves a
  `blocked` item (close-a-phase ritual in `CHECKPOINTS.md`).
- T2 assertions run in CI with slack; the reference-machine numbers above are
  the locked ones.

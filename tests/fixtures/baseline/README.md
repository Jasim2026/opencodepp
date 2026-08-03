# tests/fixtures/baseline -- Go-baseline session traces (T1 comparison only)

These are **byte fixtures** of request/session traces recorded from the Go
baseline (the older implementation). Their sole purpose is the T1 metric:
*request bytes vs Go baseline (≥ 40% smaller)*, measured by `tools/bench_engine`.

- Fixtures are read-only reference data, **never** fed to an LLM.
- Format per trace: one file per task; layout documented by a header comment.
- Two example traces ship in Phase 0; the real corpus lands in Phase 6/13.

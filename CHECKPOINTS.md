# CHECKPOINTS.md — the coder's scoreboard (phase tracker)

> Live copy of `/sdcard/project/plan/21_CHECKPOINT_TRACKER.md`. Updated at the end of
> **every** phase (per `02_CODING_PROTOCOL.md` §7). Reports live in `reports/NN.md`.

## Legend
`open` = not started · `active` = in progress · `done` = phase closed with report ·
`blocked` = stuck (note reason + next action). The three locked targets are **T3 > T1 > T2**;
a phase may not be closed on any target regression.

| Phase | Plan file | Status | Report | Notes |
|-------|-----------|--------|--------|-------|
| 00 Foundation | `06_PHASE_00.md` | **done** | `reports/00.md` | CI (GitHub Actions) = test site |
| 01 Core | `07_PHASE_01.md` | **done** | `reports/01.md` | arena 13x vs malloc; coroutines green |
| 02 Message/codec | `08_PHASE_02.md` | **done** | `reports/02.md` | codec decode 9.4x vs JSON; corpus <10% |
| 03 Config/model/store | `09_PHASE_03.md` | **done** | `reports/03.md` | config, catalog, mem/sqlite store |
| 04 Networking | `10_PHASE_04.md` | **done** | `reports/04.md` | socket/tls/http/sse/pool/policy/offline/drill; CI 8/8 (run 30832057009) |
| 05 Providers | `11_PHASE_05.md` | **done** | `reports/05.md` | 4 adapters + factory/resolver + probe; CI 8/8 (run 30838418453) |
| 06 Prompts/context | `12_PHASE_06.md` | **done** | `reports/06.md` | templates, assembler, budget; CI 8/8 (run 30842898205) |
| 07 Code graph | `13_PHASE_07.md` | **done** | `reports/07.md` | symbol index, call graph, snippets; CI 8/8 (run 30850083191); 1k files ~0.2s compute / 11.7MB RSS |
| 08 Tools | `14_PHASE_08.md` | **done** | `reports/08.md` | 24 local suites; CI 8/8 (runs 30881000945, 30880646109, 30878884892); patch-interleave bug, split_lines trailing fix, hidden Unicode |
| 09 Verification | `15_PHASE_09.md` | **done** | `reports/09.md` | gates + golden suite (T3); CI 8/8 (run 30886416695) |
| 10 Agent loop | `16_PHASE_10.md` | **done** | `reports/10.md` | states, session, intent, loop, feedback, e2e scenarios, run_agent CLI; CI 8/8 (runs 30889823117, 30903600328, 30903963276, 30904639979); fault-injection suite deferred to P13 |
| 11 Memory | `17_PHASE_11.md` | open | `reports/11.md` | session resume, workspace memory |
| 12 ABI/bindings | `18_PHASE_12.md` | open | `reports/12.md` | C ABI v1, CLI, python, jni |
| 13 Optimization/hardening | `19_PHASE_13.md` | open | `reports/13.md` | T2, fuzz, soak, measure |
| 14 Packaging/handover | `20_PHASE_14.md` | open | `reports/14.md` | install, docs, final measure |

## Locked targets (record evidence each phase; do not erase history)

| Target | Metric | Number | Phase | Measured | Pass? |
|--------|--------|--------|-------|----------|-------|
| T3 | bad edits applied (syntax error) | **0** | 09, 10 | — | — |
| T3 | golden suite final-file pass | **≥ 95%** | 09, 10, 13 | — | — |
| T3 | mechanical errors caught pre-apply | **100%** | 09 | — | — |
| T1 | context tokens / task (edge) | **≤ 3,500** | 06, 13 | 06: 396–1089 (corpus) | ✓ |
| T1 | request bytes vs Go baseline | **≥ 40% fewer** | 06, 13 | — | — |
| T1 | per-task total tokens (incl. retries) | **≤ 12,000** | 06, 10, 13 | — | — |
| T2 | init → ready | **< 100 ms** | 13 | — | — |
| T2 | idle RSS | **< 10 MB** | 13 | — | — |
| T2 | active RSS | **< 30 MB** | 13 | — | — |
| T2 | stripped `-Os` binary | **< 15 MB** | 13 | 69 KB (lib, Phase 0) | ✓ |

## Global invariants (any regression blocks phase close)
- [ ] `dev` preset builds `-Wall -Wextra -Wpedantic -Werror` clean
- [ ] `ctest` green on the phase's suites (ASan/UBSan in dev; ASan asserted in CI)
- [ ] no aborts on retryable network errors anywhere (never-abort)
- [ ] no secrets in code/tests/commits; secret filter enforced (Phase 11)
- [ ] all prompt text only in `src/prompt/templates/`; all tests in `tests/<mod>_test.cpp`
- [ ] ABI frozen after Phase 12 (any change → version bump + `docs/ABI.md` note)

## Close-a-phase ritual (do this in the same commit as the code)
1. run the phase's acceptance gate commands verbatim (on CI = the authoritative site);
2. write `reports/NN.md` with: commands run, numbers, deviations, decisions, next-phase blockers;
3. update this table + measured column (append, don't overwrite history);
4. mark `status = done` only if no target regression and no open `blocked` item.

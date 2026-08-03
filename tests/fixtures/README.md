# tests/fixtures -- test data only, never an ML dataset.

| Directory  | Purpose |
|------------|---------|
| `golden/`  | Golden end-to-end task suite for T3 (land in Phase 9/10): scripted fix/refactor/feature tasks with expected final file states. |
| `negative/`| Negative corpus for T3: inputs that MUST be caught before the filesystem is touched (syntax error, undefined symbol, caller breakage, oversized diff). Land in Phase 9. |
| `prompts/` | Prompt-under-test fixtures for `prompt/` budget assertions (Phase 6). |
| `responses/`| Wire-contract fixtures for provider adapters + mock_api scripted responses (Phases 4/5). |
| `baseline/`| Byte fixtures of the Go baseline session traces, used only for T1 request-byte comparisons (Phase 6/13). See `baseline/README.md`. |

Naming: fixtures are referenced by tests as `tests/fixtures/<group>/...` -- use a
path-relative helper, never absolute paths.

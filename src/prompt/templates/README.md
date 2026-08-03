# templates/ -- the only home of prompt text.

Only prompt text lives here: no code, no build logic, no styling beyond the
`{{PLACEHOLDER}}` substitution mechanism handled by `src/prompt/compiler.cpp`.

- `SYSTEM_BASE.md`   -- the stable core system prompt (always present).
- `SYSTEM_VERIFY.md` -- verification-focused rules (cooperates with Phase 9).
- `SYSTEM_MEMORY.md` -- memory protocols (used with Phase 11).
- `TOOLS.md`         -- tool-schema instructions; `{{TOOLS_SCHEMA}}` is filled
                        with the provider-native schema by the assembler.
- `EXAMPLES.md`      -- few-shot examples (kept minimal to protect the budget).
- `CODE_STYLE.md`    -- per-language style notes, injected only when the target
                        files imply them (`{{LANG_STYLE}}`).

Each file compiles to a `PromptRef` (sha1-pinned) at startup. Editing a file
here changes the prompt the agent ships; run `prompt_test` to re-pin the
compiled hashes and token estimates.

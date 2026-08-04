# Adding a provider adapter

The agent (Phase 10) only ever talks through the vocabulary in
`src/provider/provider.h`. Each adapter translates between our `msg` model
and the provider's native JSON wire format and normalizes every streamed
frame into the shared `StreamEvent` set. **Swapping providers never changes
agent code.**

## The interface

`provider::Provider` (see `src/provider/provider.h`) has three responsibilities:

1. `build_request(...)` — project our session history + tools + budgets onto
   the provider's request body (one HTTP/1.1 call over the `net` transport).
2. Stream parsing — consume SSE frames and emit the shared `StreamEvent`s
   (text delta, tool-call deltas, usage rollup, done). Adapters keep
   per-stream accumulation state (OpenAI partial tool-call JSON, Anthropic
   `input_json` deltas); call `reset_stream()` before each request.
3. `provider`/`api_family` — report the wire family so the engine picks the
   right request/stream shape.

The interface never throws; stream state is reset per request; a `Provider`
instance is owned by one session.

## The factory

`src/provider/factory.cpp` maps a provider id to an adapter. Model ids are
resolved separately by `src/provider/resolver.cpp` (catalog + config), which
passes a merged `ModelSpec` down to `build_request`.

To add a provider:

1. `src/provider/<name>.cpp` — implement `Provider`, following
   `openai.cpp`/`anthropic.cpp`/`gemini.cpp` for structure.
2. `src/provider/factory.cpp` — add an id branch that constructs your adapter
   (id must match `config.provider` and the catalog entry if any).
3. `src/model/catalog` — optionally register the provider's defaults
   (base_url, default model, context window) in the model catalog.
4. Tests — a wire-format test in `tests/` plus a provider round-trip against
   `tools/mock_api`. All provider tests run in CI on the default preset with
   every optional backend OFF.

## Reference machine

The default `openai_compat` adapter targets
`http://127.0.0.1:8080/chat/completions` (the mock provider) so the engine is
fully testable offline; real providers are configured via `base_url`/`api_key`
in `opencode_config_t` or a JSON config file.

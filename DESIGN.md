# DS Input — LLM Whole‑Sentence Pinyin Input Method

## Goal
A Windows pinyin input method (IME) that converts a whole pinyin sentence into
Chinese using an **OpenAI‑compatible chat API** (default model
`deepseek-v4-flash`). The user **never picks a candidate** — they type pinyin,
press Space, and the converted sentence is written into the document.
Providers/models/keys are user‑configurable in Settings.

## Why this architecture
Rime ships one cross‑platform engine (`librime`) wrapped by thin native
frontends. We keep that split — a single Rust core behind a C ABI — but replace
the local dictionary engine with an LLM converter. Only one frontend exists
today (Windows/TSF); the split is retained because it keeps every piece of
platform‑independent logic in one testable crate.

```
        ┌───────────────────────────────────────────────┐
        │  core/  — Rust crate `dsime` (cdylib)          │
        │  • session/buffer state machine                │
        │  • OpenAI‑compatible async client (reqwest)    │
        │  • config load/save (JSON)                     │
        │  • C ABI  →  core/include/dsime.h              │
        └───────────────┬───────────────────────────────┘
                        │ C FFI (stable, see dsime.h)
                        ▼
        windows/  C++ + TSF (Text Services Framework) text service
```

The core owns *all* logic that is not OS‑specific. The frontend only: captures
keys, renders the pre‑edit, commits text, and hosts the Settings UI.

## Input flow (no candidate selection, no live conversion)

1. User types ASCII pinyin (plus apostrophe and punctuation) → frontend appends
   to its buffer → `ds_session_set_input`, and the pre‑edit shows the raw buffer.
2. **Nothing is converted while typing.** There is no debounce timer and no
   background request — the pre‑edit is exactly what was typed.
3. Space → `ds_session_convert_stream` on the whole buffer. The frontend holds a
   *pending‑commit* flag for that request.
4. Streamed partials redraw the pre‑edit as the sentence arrives.
5. On the terminal result the frontend writes the sentence and ends the
   composition — convert and commit in one step. A failure or empty result
   leaves the raw buffer on screen, editable, and Space can simply be retried.
6. Enter writes the raw buffer verbatim (no conversion); Esc discards everything
   without writing a character; Backspace edits the buffer and supersedes any
   in‑flight conversion.

Punctuation is kept **in** the buffer as ASCII and sent to the model with the
pinyin; the system prompt tells the model to render Chinese punctuation
full‑width (`nihaoshijie, woshi...` → `你好世界，我是…`).

## Conversion prompt
System prompt instructs the model to treat the user message as toneless Hanyu
Pinyin (syllables possibly run together or separated by spaces/apostrophes) and
emit *only* the most natural Chinese sentence — no pinyin, no explanation, no
quotes. Latin words, digits and punctuation pass through. See `core/src/config.rs`
`DEFAULT_SYSTEM_PROMPT`.

## Reasoning ("thinking") models
The default provider model is a reasoning model: it emits a hidden chain of
thought before the visible answer, and those tokens are billed against
`max_tokens`. Two consequences shape the config defaults:

- `max_tokens` must be generous (default `1024`). A budget exhausted by
  reasoning returns **empty content**, not an error.
- `temperature` is ignored while thinking is on (the provider documents that it
  neither errors nor takes effect), so the core omits it unless `thinking` is
  explicitly set to `disabled`.
- `reasoning_effort` (default `low`) trades a little latency for a lot of it:
  `low` measured the same accuracy as the provider's default `high` on hard
  unsegmented input, at roughly half the wall clock.

Both `reasoning_effort` and `thinking` are omitted from the request when left
empty, so an endpoint that rejects them can still be used.

## Default provider
- `base_url`: `https://api.deepseek.com/v1`
- `model`: `deepseek-v4-flash`
- `api_key`: empty (user must set it in Settings)

Any OpenAI‑compatible endpoint works (OpenAI, Azure, OpenRouter, local Ollama/
vLLM/LM Studio) by changing `base_url` + `model` + `api_key`.

## Config file
`%APPDATA%\DSInput\DSInput\config\config.json`. Schema = `core::config::Config`.
The Settings UI reads/writes it via `ds_engine_get_config_json` /
`ds_engine_set_config_json` so there is a single source of truth.

## Threading contract
The result callback fires on a Tokio worker thread. The frontend MUST hop to the
UI thread (the TSF STA thread, via `PostMessage` to a message‑only window)
before touching composition state. `ds_session_convert_stream` cancels the
previous in‑flight request for that session; a superseded request still delivers
exactly one terminal callback, with `DS_ERR_CANCELLED`.

See `core/include/dsime.h` for the authoritative C ABI.

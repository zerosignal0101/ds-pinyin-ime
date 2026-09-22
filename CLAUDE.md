# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

DS Input is an LLM whole-sentence pinyin IME for Windows. The user types toneless
ASCII pinyin (`nihaoshijie, woshiyigechengxuyuan`); an OpenAI-compatible chat
model converts the *whole sentence* to Chinese (`你好世界，我是一个程序员`).
**There is no candidate window / no candidate picking.**

Nothing is converted while typing — the pre-edit shows exactly what was typed,
with no network traffic. The full interaction model:

| Key | Effect |
|-----|--------|
| `a`–`z`, `'`, and the punctuation we own | append to the raw buffer (the underlined pre-edit) |
| **Space** | convert the whole buffer and write the result — convert *and* commit in one step |
| **Enter** | write the buffer verbatim, no conversion |
| **Esc** | discard; nothing is written |
| **Backspace** | edit the buffer |

Punctuation is kept in the buffer as ASCII and sent to the model, which renders it
full-width. Enter is the escape hatch for English/identifiers.

## Architecture

One Rust core wrapped by a thin native Windows frontend.

```
core/    Rust crate `dsime` → dsime.dll (cdylib + staticlib).  Owns ALL
         OS-independent logic: pinyin buffer state machine, OpenAI-compatible
         async client (reqwest+tokio), JSON config load/save, single-in-flight
         cancellation.  Exposes a C ABI.
   │ C FFI — the ONE contract, authoritatively defined in core/include/dsime.h
   └── windows/  C++ + Text Services Framework (TSF) in-proc COM server, dsime_tsf.dll
```

The frontend only: captures keys, renders the inline pre-edit, commits text,
hosts Settings. It is deliberately thin — when adding behavior, prefer putting
logic in `core/`. `core/include/dsime.h` is the source of truth for the boundary;
`core/examples/cli.rs` drives that exact FFI and doubles as a runnable reference
frontend.

### Key invariants of the C ABI (don't break these)

- **Exactly-once callback**: every `ds_session_convert[_stream]` that returns a
  non-zero id invokes its callback exactly once, on a worker thread — even when
  superseded (then with `DS_ERR_CANCELLED`). The `engine.rs` `convert()` /
  `stream_with` `tokio::select!` + `active_gen` generation counter implements it.
- **Threading**: the result callback fires on a Tokio worker thread. The frontend
  MUST hop to the UI thread (a message-only window `PostMessage` to the TSF STA
  thread) before touching composition state. A single `DsSession` must be called
  from one thread at a time; `DsEngine` is internally synchronized and shared
  across sessions.
- **Strings**: all UTF-8, NUL-terminated. Anything documented "caller frees" must
  go through `ds_string_free`.
- **Status codes** (`DS_OK`, `DS_ERR_*`) in `dsime.h` must stay in sync with
  `api.rs::ConvertError::status_code()` and `lib.rs` — there's a test asserting this.

## Common commands

Core (run from `core/`):
```bash
cargo build --release            # dsime.dll; also via windows/build.ps1
cargo test --all                 # unit tests + tests/mock_server.rs integration test
cargo test --lib                 # unit tests only
# end-to-end against a real provider (also the reference frontend):
DSIME_API_KEY=sk-... cargo run --example cli -- ni hao shi jie
# override DSIME_BASE_URL / DSIME_MODEL to target any OpenAI-compatible endpoint
```

CI (`.github/workflows/ci.yml`) gates strictly on the core; match it before pushing:
```bash
cargo fmt --all -- --check
cargo clippy --all-targets -- -D warnings
cargo test --all
```
The `windows-frontend` job is `continue-on-error` (WIP), but the core jobs are not.

Windows frontend (run on Windows, from a *x64 Native Tools Command Prompt for VS 2022*):
```powershell
cd windows; ./build.ps1          # core dsime.dll + CMake builds dsime_tsf.dll + DSInputSettings.exe
```
`windows/install-dsinput.ps1` (elevated) installs the trio into
`C:\Program Files\DSInput` and registers the text service — the scripted
equivalent of running the guided installer.

## Things that bite

- **Reasoning models and `max_tokens`**: the default model (`deepseek-v4-flash`)
  is a reasoning model. It spends part of `max_tokens` on a hidden chain of
  thought *before* emitting any answer, and an exhausted budget returns **empty
  content with `finish_reason: length`** — not an error. Symptoms are confusing
  (the IME appears to produce nothing, or to "keep" whatever was last shown), so
  when conversion output looks wrong or empty, check the token budget first. The
  default is `1024`; measured reasoning for one 62-character unsegmented pinyin
  sentence ran 700–2200 tokens depending on `reasoning_effort`.
- **`temperature` is ignored in thinking mode** (the provider documents that it
  neither errors nor takes effect). `api.rs` therefore omits it unless
  `thinking` is explicitly `disabled`.
- **`reasoning_effort` / `thinking` are provider-specific.** They are config
  fields that default to `low` / empty and are omitted from the request body when
  empty, so other OpenAI-compatible endpoints keep working.
- **Windows DLL co-location**: `dsime_tsf.dll` and `DSInputSettings.exe` load
  `dsime.dll` at runtime — all three must live in the same folder, and `regsvr32`
  records the exact path it was registered from. The DLL's own directory is only
  searched because regsvr32 / COM use `LOAD_WITH_ALTERED_SEARCH_PATH`; a plain
  `LoadLibrary` of `dsime_tsf.dll` fails with error 126 even when `dsime.dll` is
  sitting right next to it.
- **TSF edit sessions are synchronous** (`TF_ES_SYNC | TF_ES_READWRITE`) and every
  composition mutation must run on the STA thread. Esc "writes nothing" by
  ending the composition with an *empty* final text — passing `hasFinal = FALSE`
  would leave the pre-edit stranded in the document.

## Config

Single source of truth: a JSON file at `%APPDATA%\DSInput\DSInput\config\config.json`
matching `core::config::Config`. The Settings UI reads/writes it *only* through
`ds_engine_get_config_json` / `ds_engine_set_config_json` — never parse or write
the file from a frontend. Defaults target DeepSeek
(`https://api.deepseek.com/v1`, `deepseek-v4-flash`); the API key is empty and the
user must set it. The conversion behavior lives in
`config.rs::DEFAULT_SYSTEM_PROMPT`.

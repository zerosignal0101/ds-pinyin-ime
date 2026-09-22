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

The core owns *all* logic that is not OS‑specific: the buffer state machine, the
async client, config, the conversation context and its compaction, and
single‑in‑flight cancellation. The frontend captures keys, draws the pre‑edit in
its own floating box, runs the conversion queue, and hosts the Settings UI.

## Input flow (no candidate selection, no live conversion)

1. User types ASCII pinyin (plus apostrophe and punctuation) → frontend appends
   to its buffer and **draws it in a floating box**. The document is not touched:
   no text is inserted, and no composition text is ever written.
2. **Nothing is converted while typing.** There is no debounce timer and no
   background request — the box shows exactly what was typed.
3. Space snapshots the buffer and the caret into a queue entry, and the pinyin
   vanishes from the box immediately. Typing the next sentence starts at once.
4. The queue converts **one sentence at a time**, in order, and each result is
   written at the insertion point captured when its Space was pressed — so a
   sentence still lands in the document it came from even if focus has moved on.
   Results go in through `ITfInsertAtSelection::InsertTextAtSelection`, never
   `ITfRange::SetText`: the latter is served by the host's `ITextStoreACP::SetText`,
   which the documented contract says must **not** notify, so a conforming host
   never re‑lays‑out and the sentence sits in the document invisible — wrong
   glyphs, caret in front of it — until the next key or click. See
   `windows/README.md` for the shape this has to take in practice.
5. Enter writes the raw buffer verbatim (no conversion); Esc discards what is
   being typed but leaves the queue running; Backspace edits the buffer.
6. If a result cannot be written at all (document closed or read‑only), it goes
   to the clipboard and the box says so. If the *conversion* fails, the raw
   pinyin is written instead — the same escape hatch Enter provides.

Punctuation is kept **in** the buffer as ASCII and sent to the model with the
pinyin; the system prompt tells the model to render Chinese punctuation
full‑width (`nihaoshijie, woshi...` → `你好世界，我是…`).

Characters typed with **nothing in the buffer** never reach the model: they are
written on the spot — unless a conversion is still on its way, in which case they
join the queue behind it. That is the punctuation we own (rendered full‑width), a
space, a digit, anything else the host would have typed as text. They have to
queue. A result is written at the selection *as it stands when its turn comes*, so
emitting the comma first moves the caret past it and the sentence still in flight
then lands behind it: the user reads `，你好` for what they meant as `你好，`.
Waiting also matches the intent — the character belongs to the sentence just
typed. **Enter is the one exception**, and always goes to the host: it is as much
a command as a character (a newline in an editor, *submit* in a search box), so a
line break typed while a sentence is converting still lands in front of it.

### Why the pre‑edit is not in the document
An inline pre‑edit means rewriting the document on every keystroke. Applications
treat that as real text and run incremental searches — a search box queries on
every letter. Keeping the pre‑edit in our own window means the document changes
only when a finished sentence lands in it.

A **zero‑width `ITfComposition`** is still opened while typing, and that is not
vestigial: it probes writability (a failure means the key is handed back to the
host rather than swallowed), it tells TSF and the application that a text edit is
under way, and it gives an insertion point TSF keeps valid across edits.

### The queue lives in the frontend
This is a deliberate exception to "logic belongs in the core". The core's session
is a single‑flight buffer with supersession semantics, and the queue needs the
opposite of supersession: every request must run to completion, in order. On the
frontend, one rule — *issue the next request from the previous one's terminal
callback* — delivers both ordering and the guarantee that each request sees the
previous result. A core‑side queue would additionally have to solve `user_data`
lifetime across the C ABI for payloads that are never delivered, and would turn
one slow request into a global stall. The core still owns everything about *how*
a request is made; the frontend owns *when*.

## Conversation context
Each request carries a per‑input‑window history of what has already been
converted, so the model sees the domain, terminology and style of the document.
Structure (append‑only, which is what makes the provider's prefix cache hit):

```
[system: SYSTEM_PROMPT]     ← byte‑stable
[system: 语境摘要：…]        ← changes only on compaction
[user: 拼音₁][assistant: 中文₁] …
[user: 当前拼音]
```

Windows are keyed by application (`{exe}|{window class}`), so every Chrome tab
shares one context — the industry vocabulary being bought is a property of the
application, not of a tab. Token accounting trusts the provider's `usage` and
estimates only what was appended since; past
`context_window_tokens × context_compact_ratio` the history is summarised by a
second model call, keeping the most recent `context_keep_recent` turns verbatim.
Contexts are persisted beside the config file and loaded back trimmed to the
current window, so shrinking the window does not leave oversized prompts behind.

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

Saving **replaces the whole object**, and every field is `#[serde(default)]` — so
a field the frontend omits is not preserved, it is reset to its default. Any new
config field must therefore be surfaced in the Settings dialog or carried through
it verbatim.

The conversation contexts live in `%APPDATA%\DSInput\DSInput\context\` as one
JSON file per window, so the config stays a settings file rather than a data
store. `ds_engine_clear_contexts` is the way to be rid of them.

## Threading contract
The result callback fires on a Tokio worker thread. The frontend MUST hop to the
UI thread (the TSF STA thread, via `PostMessage` to a message‑only window)
before touching document state. A request that returns a non‑zero id delivers
exactly one terminal callback, including when it is superseded (then with
`DS_ERR_CANCELLED`) — and `ds_engine_free` now cancels outstanding work and waits
(bounded) for those callbacks to land, so the guarantee holds at teardown too.

See `core/include/dsime.h` for the authoritative C ABI.

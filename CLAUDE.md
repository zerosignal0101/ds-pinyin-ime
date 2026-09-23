# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

DS Input is an LLM whole-sentence pinyin IME for Windows. The user types toneless
ASCII pinyin (`nihaoshijie, woshiyigechengxuyuan`); an OpenAI-compatible chat
model converts the *whole sentence* to Chinese (`你好世界，我是一个程序员`).
**There is no candidate window / no candidate picking.**

Nothing is converted while typing, and — more importantly — **nothing is written
to the document while typing either**. The pinyin is drawn in a floating box of
our own, over a zero-width `ITfComposition`. That is the whole point: rewriting
the document on every keystroke makes a browser search box run a query on every
letter. The full interaction model:

| Key | Effect |
|-----|--------|
| `a`–`z`, `'`, and the punctuation we own | append to the raw buffer (drawn in the floating box) |
| **Shift+letter** | same, but the letter is kept **upper-case** — see below |
| **Space** | hand the buffer to the queue; the pinyin disappears at once and typing continues |
| **Enter** | write the buffer verbatim, no conversion |
| **Esc** | discard what is being typed; the queue keeps running |
| **Backspace** | edit the buffer |
| **Ctrl+Space** | toggle Chinese/English mode — see below |

**Shift is how an abbreviation survives.** `shiyongAI` is converted to `使用AI`;
the flattened `shiyongai` gives the model no reason to prefer `AI` over `爱`,
because everything around it is pinyin. So a capital is kept as a capital, and the
case is read from the **Shift key**, never from the character `ToUnicode` produced
— with CapsLock on, the latter returns capitals for ordinary typing too, and every
sentence would arrive as `NIHAO`. The rule the model gets is scoped to *runs of two
or more* capitals, so a stuck Shift cannot invert the IME into writing English.

**English mode (Ctrl+Space) has no buffer at all.** Nothing typed is pinyin and
nothing is remapped to full-width, so every printable key takes the literal path:
queued behind any sentence still on its way, and handed to the host when the queue
is empty — which is what makes typing a whole sentence of English feel native.
A buffer in progress when the mode flips is flushed **verbatim**, not converted.

Conversions run **strictly one at a time, in order**, each writing at the anchor
captured when its Space was pressed — so a sentence still lands in the document
it came from after focus has moved. Punctuation is kept in the buffer as ASCII
and sent to the model, which renders it full-width. Enter is the escape hatch for
English/identifiers.

Characters typed with **nothing in the buffer** never reach the model: they are
written on the spot — *unless* a conversion is still on its way, in which case
they join the queue behind it as jobs that are born already converted. That covers
everything the host would otherwise have typed as text: the punctuation we own
(rendered full-width), a space, a digit, any other printable character. Writing
them on the spot is what produces `，你好` for what was meant as `你好，`, and it is
not a gravity problem: a queued job is inserted at the selection **as it stands
when its turn comes** (`StartComposition` at the anchor is what puts it there), so
the character moves the caret past itself and the sentence lands behind it. Note
`_IsKeyEaten` has to agree — while the queue is non-empty it eats these keys, and
with an empty queue it hands them to the host.

**Enter is deliberately excluded.** It is the one key here that is as much a
command as a character — a newline in an editor, but *submit* in a search box — so
it always goes to the host. The cost is that a line break typed while a sentence
is still converting lands in front of it.

**The queue lives in the frontend, and that is a deliberate exception** to "put
the logic in `core/`". The core's session is single-flight: a new request
supersedes the old one. The queue needs the opposite — every request completes,
in order — and one rule delivers it on the frontend: *issue the next request from
the previous one's terminal callback*. That also makes each request see the
previous result, which is what the context needs. Pushing it into the core would
mean solving `user_data` lifetime across the C ABI for payloads that are never
delivered, and letting one slow request stall every window. The core owns *how* a
request is made; the frontend owns *when*.

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

The frontend only: captures keys, draws the floating pre-edit box, runs the
conversion queue, commits text, hosts Settings. It is deliberately thin — when
adding behavior, prefer putting logic in `core/`. `core/include/dsime.h` is the
source of truth for the boundary; `core/examples/cli.rs` drives that exact FFI
and doubles as a runnable reference frontend.

### Key invariants of the C ABI (don't break these)

- **Exactly-once callback**: every `ds_session_convert[_stream]` that returns a
  non-zero id invokes its callback exactly once, on a worker thread — even when
  superseded (then with `DS_ERR_CANCELLED`). The `engine.rs` `convert()` /
  `stream_with` `tokio::select!` + `active_gen` generation counter implements it.
  `ds_engine_free` honours it too: `EngineHandle::drop` cancels outstanding work
  and waits (bounded, 2s) for the pending callbacks to land *before* dropping the
  runtime, which would otherwise cancel the tasks outright. Frontends hang
  per-request resources on that callback — the TSF frontend's `AddRef`, the
  Settings window's wait loop — so a swallowed terminal is a leak, and a leaked
  `CTextService` keeps the DLL resident in the host process forever. Both layers
  keep a belt-and-braces path that does not depend on the callback.
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
- **TSF allows one composition per context, so writing a queued sentence
  terminates the one the user is typing into.** `StartComposition` at the
  insertion anchor kills the live zero-width composition, and the notification
  (`ITfCompositionSink::OnCompositionTerminated`) is indistinguishable from the app
  yanking it away. So the handler that clears the buffer on termination also fired
  whenever a result landed mid-typing, and the pinyin typed since silently
  vanished. The composition really is gone either way — but the buffer never
  depended on it (the insertion anchor is captured fresh at Space), so `_FinishJob`
  holds `_suppressTermination` across the insert and leaves `_reanchorDue` to
  rebuild the composition on the next keystroke.
- **`ITfContext` edit sessions**: check `hrSession` (the out-param), not just the
  `RequestEditSession` return value — they report different failures.
  `TS_E_READONLY` / `TF_E_DISCONNECTED` mean the document said no;
  `TF_E_LOCKED` means try again shortly. And **`GetTextExt` returning `S_OK`
  proves nothing**: a minimised window yields `S_OK` with an all-zero rect, so
  callers must test the rect (`IsDegenerate` in `windows/Globals.h`).
- **`TF_S_ASYNC` is a success code that is not a completion.** `0x00040300` —
  severity 0 — means "accepted and queued", and `FAILED()` is false for it, so it
  slips through every failure check. Which of the two readings applies depends on
  the caller, and getting it wrong is silent in both directions:
  - A session whose *output* you need has not run, so a null out-param is **not**
    a refusal. `Dsime_RequestInsertText` opens a composition before writing, and
    reading a queued (so still-null) composition handle as "this host refused"
    dropped it into the bare-write fallback — the path documented not to notify.
    The sentence landed in the document and was never repainted: present, and
    invisible. It now returns `TF_S_ASYNC` upward untranslated, and `_FinishJob`
    retries it on the pump timer alongside `TF_E_LOCKED` — both mean "nothing
    written yet".
  - A session that only has to *eventually happen* may be accepted as success;
    TSF runs queued sessions in submission order. The write and close steps do
    this, and normalise the code to `S_OK` so the caller does not retry a write
    that is already queued — that would insert the sentence twice.
  Asking for an edit session during a **key event's test phase** is what triggers
  the deferral in practice; the same call from a posted message is granted
  inline.
- **A new `core::config::Config` field must be added to the Settings dialog** (or
  carried through it). Saving replaces the whole JSON object and every field is
  `#[serde(default)]`, so an omitted field is not preserved — it silently reverts
  to its default.
- **Changing `DEFAULT_SYSTEM_PROMPT` reaches nobody on its own.** The prompt is
  *stored* in config.json, so every install that has ever run the app keeps the
  text it was first given — a rule the user cannot receive is not a fix. Pair the
  change with the outgoing value frozen into `LEGACY_SYSTEM_PROMPTS`
  (`core/src/config.rs`), which `Config::load_or_create` matches byte-for-byte and
  upgrades; anything else is the user's own edit and is left alone. Add the
  example to the *rule* too: few-shot examples dominate instructions here.
- **Ctrl+Space has two separate ways to go wrong, and they need opposite fixes.**
  1. *Windows may own the chord.* The Chinese language pack ships a legacy hotkey,
     "输入法/非输入法切换", bound to Ctrl+Space and handled by the input-language
     layer, below TSF. When it fires, the whole text service is deactivated: the
     trace shows `deactivate` and no key event at all. `PreserveKey` cannot help
     here — the OS has already consumed the key. The user has to give the chord up
     under 高级键盘设置, which is a prerequisite, not a workaround. (Note the
     dialog has no "None" option: you must untick **启用按键顺序**.)
  2. *Even once we own it, the handle phase is not reliably delivered.* With a
     composition live, the trace shows `OnTestKeyDown` running, `_IsKeyEaten`
     answering "eaten" — and **`OnKeyDown` never arriving**, so a switch written
     in `OnKeyDown` worked with an empty buffer and silently did nothing with a
     full one. The fix is to **do the switch in `OnTestKeyDown`**, the one
     callback delivered in both states. Weasel does the same, for the same class
     of host misbehaviour: its `OnTestKeyDown` runs the engine and the composition
     update, and its `OnKeyDown` only eats the key. (`ITfKeystrokeMgr::PreserveKey`
     is the documented answer and is *worse* here — it takes the key at the test
     stage while `OnPreservedKey` is delivered from the missing handle phase, and
     the chord went completely dead. Tried, measured, removed.) `_ToggleEnglishMode`
     drops a repeat inside 200 ms, which is what makes the test phase safe: hosts
     that reach both callbacks, and hosts that send several tests per press (MS
     Word 2010 x64), cannot turn one chord into two switches.
- **The mode flash needs a timer id of its own.** `SetTimer` on an id that is
  already armed *replaces* that timer, interval included — id 2 is the
  `TF_E_LOCKED` insert retry, so sharing it would let a mode change silently
  cancel a retry and with it a sentence.
- **Insert committed text with `ITfInsertAtSelection::InsertTextAtSelection`, not
  with `ITfRange::SetText`.** MSDN's `ITextStoreACP::SetText` contract says an
  application *"should not call the `ITextStoreACPSink::OnTextChange` method in
  response to this method"*, and that it *"should call `SetSelection`, then
  `InsertTextAtSelection`, to perform the actual change"*. `SetText` is the one
  API that is specified **not** to notify, so a host that follows the contract
  does not re-lay-out or re-style the text: it is in the store, it is not on
  screen in the right shape. Every symptom is downstream of that — the caret
  stays put (so a literal space typed next lands *before* the sentence), the new
  characters keep the previous font (Simplified Chinese rendered with Japanese
  glyph variants), and even text the host inserted itself sits invisible. All of
  it snaps right the instant the user presses a key or clicks, because that is
  the host processing its own input. Chrome notifies for `SetText` anyway, which
  is what makes this look like a host-specific bug rather than ours; Notepad3's
  Scintilla does not.
  `CWriteCompositionEditSession` inserts via `InsertTextAtSelection` and falls
  back to `SetText` only if the host refuses. Note a composition around the write
  is **not** sufficient on its own — that was tried, and the text appeared while
  the caret and the glyphs stayed stale.
- **`TF_ES_SYNC` only works from a key event or a TSF callback.** MSDN:
  "The edit session must be synchronous or the request will fail (with
  `TF_E_SYNCHRONOUS`). This flag should only be used in documented situations
  (such as keystroke handling) … Otherwise the call will likely fail." A refusal
  arrives as `S_OK` from `RequestEditSession` with `TF_E_SYNCHRONOUS` in
  `hrSession` — so a caller that checks only the return value sees success and
  silently writes nothing. The composition start/end/capture/probe sessions stay
  `TF_ES_SYNC` (they run from key events); the **queued insert** uses
  `TF_ES_ASYNCDONTCARE`, because it runs from a posted message. Anything that
  needs a synchronous session from a posted message must be deferred to the next
  keystroke instead — that is what `_reanchorDue` is.
- **Every composition mutation must run on the STA thread.** Esc "writes nothing"
  by ending the composition with an *empty* final text — passing `hasFinal =
  FALSE` would leave the pre-edit stranded in the document.

## Config

Single source of truth: a JSON file at `%APPDATA%\DSInput\DSInput\config\config.json`
matching `core::config::Config`. The Settings UI reads/writes it *only* through
`ds_engine_get_config_json` / `ds_engine_set_config_json` — never parse or write
the file from a frontend. Defaults target DeepSeek
(`https://api.deepseek.com/v1`, `deepseek-v4-flash`); the API key is empty and the
user must set it. The conversion behavior lives in
`config.rs::DEFAULT_SYSTEM_PROMPT`, and the compaction prompt in
`config.rs::DEFAULT_CONTEXT_PROMPT`.

The **conversation context** is a separate store, not config, and lives **in
memory only** (`core/src/context.rs`): what the user has typed in this program
instance, carried into the next conversion. Nothing is written to disk — it is a
record of document text, and a copy on disk outlived the document it described
while still being prepended to every request at token prices. `ds_engine_clear_contexts`
forgets it now and deletes whatever a version that *did* persist left beside the
config file; it is wired to the Clear button in Settings.

Contexts are keyed `{exe}|{pid}` — per **process**, which is what separates two
Notepad3 instances (they share a window class, so keying on that mixed one file's
vocabulary into another's). Two consequences worth knowing: the engine is built
per activation, so **switching input methods away and back loses the history**
(previously it came back from disk), and one process is one context, so Electron
and VS Code still share across their windows.

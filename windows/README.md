# DS Input — Windows TSF frontend

A native Windows **Text Services Framework (TSF)** text service that turns whole
pinyin sentences into Chinese using the shared `dsime` core engine (an
OpenAI-compatible LLM converter) — a thin native frontend over a Rust core.

There is **no candidate window**, and nothing converts while you type. The
pre-edit is the raw pinyin you entered; Space sends the whole buffer to the model
and writes the returned sentence straight into the document, in one step. Enter
writes the raw buffer verbatim (no conversion), and Esc discards everything.

Two keys do something else. **Shift+letter** keeps the letter upper-case, so
`shiyongAI` reaches the model as an abbreviation and comes back as 使用AI instead
of the model reading `ai` as pinyin — see "Case and abbreviations" below.
**Ctrl+Space** switches between Chinese and English input.

## Prerequisites

- **Visual Studio 2022** with the *Desktop development with C++* workload
  (MSVC v143) and a **Windows 10/11 SDK**.
- **CMake** 3.20+ (the VS 2022 installer bundles one).
- **Rust** with the MSVC target:
  ```
  rustup target add x86_64-pc-windows-msvc
  ```

## Build

From a *x64 Native Tools Command Prompt for VS 2022* (so MSVC + SDK are on PATH):

```powershell
cd windows
./build.ps1                 # Release; use -Config Debug for a debug build
```

`build.ps1`:
1. Builds the Rust core for `x86_64-pc-windows-msvc` →
   `core/target/x86_64-pc-windows-msvc/release/dsime.dll` (+ import lib).
2. Configures and builds the C++ targets with CMake (VS 2022 generator, x64).
3. Prints the registration commands.

Outputs land in `windows/build/Release/`:
- `dsime_tsf.dll` — the TSF text service (COM in-proc server).
- `DSInputSettings.exe` — the settings dialog.
- `dsime.dll` — the core, staged next to them for local testing.

### Building without the script

```powershell
cargo build --release --target x86_64-pc-windows-msvc   # in core/
cmake -S windows -B windows/build -G "Visual Studio 17 2022" -A x64 `
      "-DDSIME_CORE_DIR=core/target/x86_64-pc-windows-msvc/release"
cmake --build windows/build --config Release
```

## Register / unregister

`dsime_tsf.dll` is a self-registering COM server. From an **elevated** prompt:

```powershell
regsvr32 windows\build\Release\dsime_tsf.dll      # register
regsvr32 /u windows\build\Release\dsime_tsf.dll   # unregister
```

`DllRegisterServer` writes the COM `InprocServer32` entry and, via the TSF COM
APIs (`ITfInputProcessorProfiles`, `ITfCategoryMgr`), the language profile
(`zh-Hans`, with icon) and the capability categories:
`GUID_TFCAT_TIP_KEYBOARD`, `..._UIELEMENTENABLED`, `..._SECUREMODE`,
`..._IMMERSIVESUPPORT`, `..._SYSTRAYSUPPORT`, and `DISPLAYATTRIBUTEPROVIDER`.

> The DLL and `DSInputSettings.exe` load `dsime.dll` at runtime. Keep all three
> in the same folder (the build stages `dsime.dll` for you), and register the
> DLL from its final install location — `regsvr32` records that exact path.

## Enable the IME

After registering, add it as a keyboard:

**Settings ▸ Time & language ▸ Language & region ▸ Chinese (Simplified) ▸ ⋯ ▸
Language options ▸ Add a keyboard ▸ “DS Input (LLM Pinyin)”.**

(If Chinese (Simplified) is not installed, add it first under *Add a language*.)
Switch to it with the language switcher (Win+Space).

## Configure (API key, model, …)

Open the language-bar / system-tray entry for DS Input and choose **Settings…**
(or run `DSInputSettings.exe` directly). Fields: Base URL, API Key, Model,
Temperature, Max tokens, Reasoning, Thinking, Timeout, System prompt. The
defaults target DeepSeek (`https://api.deepseek.com/v1`, `deepseek-v4-flash`) —
**set your API key** before first use. Settings are written to
`%APPDATA%\DSInput\DSInput\config\config.json`, the same file the text service
reads, so there is one source of truth.

> **Reasoning** and **Thinking** map to the provider's `reasoning_effort` and
> `thinking` fields. Leave either blank to omit it from the request, which is
> what you want for an endpoint that doesn't accept them. Note that `Max tokens`
> must also cover the model's hidden *reasoning* tokens — a budget that is too
> small comes back as an empty result, not an error.

## How it works (design notes)

### Threading / marshaling
TSF runs the text service on a single-threaded apartment (STA) UI thread; every
document mutation must happen there. The core's conversion callback fires on a
Tokio worker thread. We bridge them with a hidden **message-only window**
created on the STA thread:

- The queue's head is dispatched on the STA thread, which calls
  `ds_session_convert`. Nothing else ever contacts the provider.
- The core callback (a static C thunk) packages the result into a heap struct and
  `PostMessage`s `WM_DSIME_CONVERT_RESULT`. The STA-thread window proc hands it
  to the queue.
- Results are matched to their job by `request_id` via lookup, and a reference on
  the text service is held across each in-flight request so it can't be destroyed
  before the result is delivered.

The same window also carries `WM_DSIME_RELOCATE` and two `WM_TIMER`s — see
"Floating input box" below.

### Composition lifecycle
The first pinyin key opens a **zero-width** `ITfComposition` (synchronous
read/write edit session). It holds no text and never will: the pre-edit is drawn
in our own floating box instead. Keeping the composition anyway is deliberate, and
buys three things:

- **A free writability probe.** `StartComposition` fails on a read-only document.
  We then report the key as *not eaten*, so it reaches the host — the same as if
  no IME were installed. Without the composition, the failure mode would invert
  into "we swallow the key and the pinyin silently disappears".
- **TSF's "text edit in progress" signal**, which is what stops an application
  from running an incremental search on every keystroke. That is the behaviour
  this design exists to avoid.
- **A live insertion point** that TSF keeps valid across edits.

Space snapshots the buffer and the caret into a `PendingJob` and hands it to the
queue; the pinyin disappears at once. Enter writes the raw buffer verbatim. Esc
discards the buffer and leaves the queue alone. If TSF terminates the composition
itself (`ITfCompositionSink::OnCompositionTerminated`) we drop the typing state —
the next key simply starts a new one.

Typing is never blocked by a conversion, and never cancels one: a sentence
already committed with Space is owed to the document whether or not the user has
moved on.

### The queue
Conversions run **strictly one at a time, in the order they were asked for**. The
next request is only issued when the previous terminal result has landed, which
is also what makes "every request sees the previous one's result" true. The queue
is frontend state on purpose — see the note in `CLAUDE.md`.

- **Anchors are captured when Space is pressed**, not when the result arrives,
  and carry *forward gravity*. Two Spaces in a row capture the identical position
  (typing never touches the document), so gravity is what keeps results in order.
- **Characters typed with nothing in the buffer join the queue too**, as jobs born
  already "converted", so the pump hands them straight to the inserter and the
  model is never asked about them. That is the punctuation we own (written
  full-width), a space, a digit, any other printable character — everything the
  host would otherwise have typed as text — and every character typed in English
  mode. They have to queue: a result is inserted at *the selection as it stands
  when its turn comes* — the anchor only decides where the composition is parked,
  and starting one moves the selection there — so writing the comma on the spot
  moves the caret past it, and the sentence still in flight then lands behind it.
  The user reads `，你好` for what they meant as `你好，`. Taking a number also
  matches the intent: the character belongs to the sentence just typed.
  `_IsKeyEaten` has to agree, since it decides before `_HandleKey` ever runs. The
  box's 待转换 badge counts conversions only, so a comma does not make it jump.
- **Consecutive literal characters merge into one job.** English mode produces
  whole words, and one job per character would mean one anchor capture (a
  synchronous edit session) and one inserted composition each, for text that lands
  in exactly the same place. `_EnqueueIdleChar` appends to the literal job at the
  tail instead. Two guards make that safe: `_jobs.size() > 1`, because the head is
  the one the pump may be inserting, and a check that the tail job belongs to the
  *same context* — a queued job outlives a focus change, so without it an alt-tab
  mid-word would merge one document's characters into another's job.
- **Literal jobs are not counted against `queue_max_pending`.** That bound exists
  to limit model requests, which literals are not; the box's 待转换 count ignores
  them for the same reason.
- **Enter is the exception**, and stays with the host: it is as much a command as
  a character (a newline in an editor, *submit* in a search box). A line break
  typed while a sentence is converting therefore still lands in front of it.
- **A result landing must not disturb the box the user is typing into.** It will
  try to: TSF allows one composition per context, so the composition the insert
  opens at the anchor terminates the live zero-width one, and
  `OnCompositionTerminated` — which cannot tell that apart from the app yanking the
  composition away — throws the buffer out. `_FinishJob` therefore holds
  `_suppressTermination` across the insert; the composition is rebuilt from
  `_reanchorDue` on the next keystroke, and the pinyin survives because it never
  depended on the composition in the first place.
- **Results are inserted with `ITfInsertAtSelection::InsertTextAtSelection`**, in a
  composition of their own, over three separate edit sessions (open / write /
  close). Both halves of that matter, and both were found the hard way:
  `ITfRange::SetText` is served by the host's `ITextStoreACP::SetText`, which the
  contract says must **not** notify `OnTextChange` — so a conforming host never
  re-lays-out or re-styles the text, and the sentence sits in the document
  invisible (or with the previous font, or with the caret still in front of it)
  until the user presses a key or clicks. `InsertTextAtSelection` is the
  documented insertion path that does notify. Chrome notifies either way, so this
  surfaces only in hosts that follow the contract — Notepad3's Scintilla does.
- The three sessions are also load-bearing: with the whole open/write/close folded
  into one, Notepad3 never re-laid-out at all.
- Both the `RequestEditSession` return value **and** `hrSession` are checked —
  they are different failures. `TF_ES_SYNC` is deliberately *not* used for the
  insert: it is only valid from a key event or a TSF callback, and queue delivery
  runs from a posted message, where TSF refuses it with `TF_E_SYNCHRONOUS`
  (returned in `hrSession` while the call itself reports `S_OK`). `TF_E_LOCKED`
  (document mid-edit) is retried on a timer; anything else falls back to the
  clipboard with a red badge in the box, and the HRESULT is appended to
  `%TEMP%\dsinput-error.log`.
- **`TF_S_ASYNC` is retried, not accepted.** It is a *success* code
  (`0x00040300`) meaning "queued", and `FAILED()` is false for it — so treating it
  as done pops the job with the sentence unwritten, and no error, badge or trace
  says so. It is grouped with `TF_E_LOCKED` on the retry timer, because both mean
  the same thing here: nothing has been written yet. The compose step is also why
  it must not be *interpreted*: a queued session has not produced the composition
  handle yet, and reading that null as "this host refused" is what routed the
  write into the non-notifying fallback — the sentence in the document and
  invisible on screen. Asking for the session during a key event's **test phase**
  is what makes TSF defer it; from a posted message the same call is granted
  inline, which is why the flush posts `WM_DSIME_PUMP` instead of pumping inline.
- Depth is bounded by `queue_max_pending`; past it, Space is swallowed rather
  than passed to the host (a literal space in the document would be worse).
- A lost thread focus does **not** cancel the queue, only the typing session.

### Case, and Chinese / English mode

**Shift+letter keeps its case in the buffer.** The buffer is drawn in the box and
sent to the model as typed, so `shiyongAI` converts to 使用AI; flattening it to
`shiyongai` would leave the model no reason to prefer `AI` over 爱, because
everything around it is pinyin. The case is taken from the **Shift key**, not from
the character `ToUnicode` produced: with CapsLock on, the latter returns capitals
for ordinary typing too, and every sentence would go to the model as `NIHAO` — the
exact shape that is meant to mean "this is English". The consequence is that
CapsLock+Shift yields a lower-case letter, which is Windows' own behaviour and the
harmless direction to be wrong in.

The prompt rule that goes with it is scoped to **runs of two or more** capitals: a
single stray capital stays plausible pinyin, so a stuck Shift cannot quietly turn
the IME into a way of writing English. The rule lives in the *config file*, not
just in the code — see the note on `LEGACY_SYSTEM_PROMPTS` in `CLAUDE.md`.

**Ctrl+Space toggles English mode, and it is handled in `OnTestKeyDown`** — the
test phase, not the handle phase, and that is deliberate. With a composition live,
the trace shows TSF calling `OnTestKeyDown`, this service answering "eaten", and
`OnKeyDown` never arriving: a switch written in the handle phase therefore worked
with an empty buffer and did nothing at all with a full one. The test callback is
the only one delivered in both states, and real IMEs work there for the same
reason — Weasel runs its whole key engine from `OnTestKeyDown` and leaves
`OnKeyDown` to eat the key. `_ToggleEnglishMode` drops a repeat inside 200 ms, so
hosts that reach both callbacks, or that send several tests for one press, cannot
turn a single chord into two switches. Windows also binds this chord to
"输入法/非输入法切换" — see Troubleshooting for what to do about that.

English mode has no buffer at all: nothing typed is
pinyin and nothing is remapped to full-width, so every printable key takes the
literal path above. With a sentence still converting, the English queues behind it
and lands in order; with the queue empty, `_IsKeyEaten` hands the key to the host
and the application types it natively — which is what makes a whole sentence of
English feel like nothing is installed. Space, Enter, Esc and Backspace likewise
belong to the host in that mode (Esc deliberately does *not* leave the mode: a key
that silently changes the input mode is worse than one that does nothing, and the
box already says which mode is on).

Ctrl+Space with a buffer in progress flushes it **verbatim**, as a literal job —
not converted. The user is switching to English mid-word; spending a request to
turn an unfinished fragment into Chinese is not what they asked for, and the queue
is what makes it land after the sentences already owed to the document. If the
anchor cannot be captured the buffer is left exactly as it is and the flush is
retried at the top of the next keystroke (`_flushBufferAsLiteralDue`, the same
idiom as `_reanchorDue`) — never written out of turn.

The mode is per activation and not persisted. It is announced by a 中/英 marker on
the box's status line, which also **flashes the box** for about a second when
Ctrl+Space is pressed — that is its only feedback when there is nothing else to
show, which is the case it is usually pressed in. The marker is drawn whenever the
box is up; only the flash holds it open, or a panel would sit over the document
for as long as the mode lasted. The flash uses **timer id 3**: `SetTimer` on an
armed id replaces that timer, interval and all, so sharing the insert-retry id 2
would let a mode change cancel a pending sentence.

### Floating input box
`InputWindow.h/.cpp` — a `WS_POPUP | WS_DISABLED` window with
`WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST`, owned by the document's
host window so it minimises with it. `WS_DISABLED` alongside `WS_EX_NOACTIVATE`
means it cannot be activated at all; `WM_NCHITTEST` returns `HTTRANSPARENT` so
clicks pass through to the document.

Positioning (`Dsime_GetCaretPos`) degrades through four rungs: `GetTextExt` on a
synchronous read-only session → the classic caret via `GetGUIThreadInfo` → the
view's `GetScreenExt` → keep the last position. A **degenerate rect is the "no
position" signal, not a failed HRESULT**: `GetTextExt` returns `S_OK` and an
all-zero rect when the window is minimised. A collapsed range reports no line
height, so the probe widens by one character to get a real one. `GetGUIThreadInfo`
reports `rcCaret` in the **client** coordinates of `hwndCaret`, while every other
rung — and everything downstream that places a window — is in screen coordinates,
so rung 2 converts.

Which document is probed depends on what the box is doing. While typing it is the
context the composition is anchored in; once Space has ended that composition the
box is still up showing the pending count, and the caret to follow is the *focused*
document's. Without that fallback the box froze on the line where the last
sentence was typed, and stayed there through an Enter, a scroll or a click. The
probe is a synchronous edit session, so `_UpdateInputBox(canProbeCaret)` is false
everywhere that runs from a TSF callback (a focus change, a composition torn down
under us): those can run with the document locked, where a synchronous session is
at best refused and at worst deadlocks.

The line under the pinyin is the status line, and it is always there: it carries
the 中/英 marker. `failed` outranks both the marker and the 待转换 count, because it
is the only signal that text was lost and it is sticky until the next successful
write — neither a mode marker nor a count may crowd it out. Its width is measured
from the widest text it *can* hold rather than from the current one, or the box
would twitch as a pending count went from 9 to 10.

Repositioning is driven by `ITfTextLayoutSink::OnLayoutChange` plus a 150 ms timer
for hosts that don't fire it (and for caret moves that change no text). Both are
advised on the focused context.

> **A mouse click is not tracked in Notepad3, and that is the host's doing.**
> A click changes the selection and nothing else, so no layout callback fires and
> the timer is all that is left — and Scintilla does not publish the new selection
> to TSF, so every probe (including a probe using the host's own insertion point)
> keeps answering with the position of the last *programmatic* edit. The box
> therefore only moves once a keystroke starts a composition, which re-anchors the
> selection. 搜狗拼音 behaves identically there, which is what a host that never
> announces the change looks like from any TSF text service. `ITfTextEditSink` was
> implemented to catch it and removed again: the trace showed `OnEndEdit` firing
> only for our own edit sessions, never for a click.
`OnLayoutChange` runs under a document lock, where a synchronous session is not
safe — so it only *posts* `WM_DSIME_RELOCATE` to ourselves and the work happens
once the lock is released.

### Core ownership
One `DsEngine` per activation (shared, internally synchronized) and one
`DsSession` per activation. C strings returned by the core are freed with
`ds_string_free` (the `dsime::CoreString` RAII guard). See `DsimeCore.h`.

## File map

| File | Role |
|------|------|
| `Guids.h` / `Guids.cpp` | Stable CLSID / profile / display-attr / lang-bar GUIDs. |
| `DsimeCore.h` | RAII C++ wrapper over the `dsime` C ABI + UTF-8↔UTF-16 helpers. |
| `Globals.h` | Module handle, DLL ref counter, shared names/ids. |
| `dllmain.cpp` | COM exports, class factory, `Dll{Register,Unregister}Server`. |
| `Registry.h` / `Registry.cpp` | COM + TSF profile/category registration. |
| `TextService.h` | The text-service class declaration (all interfaces). |
| `TextService.cpp` | Lifecycle, IUnknown, sink wiring, marshaling window. |
| `KeyEventSink.cpp` | `ITfKeyEventSink`: which keys we eat and how we act. |
| `Composition.cpp` | Composition orchestration, the floating box, and the queue. |
| `EditSessions.cpp` | `ITfEditSession`s (start / end composition, insert, caret probe, anchor capture). |
| `InputWindow.h` / `.cpp` | The floating input box: painting, placement, DPI. |
| `LangBarButton.cpp` | `ITfLangBarItemButton` that opens Settings. |
| `resource.h`, `dsime_tsf.rc`, `dsime.ico` | Icon + version resources. |
| `settings/` | `DSInputSettings.exe` (Win32 dialog over the core config). |
| `CMakeLists.txt`, `build.ps1` | Build system. |
| `dsime_tsf.def` | DLL export list. |

## Troubleshooting

- **IME doesn't appear in the keyboard list** — registration failed or wasn't
  elevated. Re-run `regsvr32` from an elevated prompt; check it's the 64-bit
  `regsvr32` for the 64-bit DLL.
- **Typing inserts pinyin but never converts** — no/invalid API key, or the
  endpoint is unreachable. Open Settings and verify Base URL / API Key / Model.
  When a conversion fails, the raw pinyin is written instead (the same escape
  hatch Enter provides), so nothing is lost and the failure is visible.
- **DLL fails to load (0x8007007E)** — `dsime.dll` isn't next to
  `dsime_tsf.dll`. Keep them in the same folder.
- **The floating box is missing or in the wrong place** — the host doesn't
  implement `ITfContextView::GetTextExt`. Typing still works; the box falls back
  to the classic caret, then to the corner of the view. Nothing is ever lost.
- **Ctrl+Space turns the IME off instead of switching to English** — a legacy
  Windows hotkey has the chord, not us. Under *Settings ▸ Time & language ▸
  Typing ▸ Advanced keyboard settings ▸ Input language hot keys*, open
  "中文(简体，中国) — 输入法/非输入法切换" and **untick 「启用按键顺序」**. Do not try
  to pick "Not Assigned" from the dropdown — there is no such entry, so the change
  does not stick and the chord silently comes back on the next open. That hotkey is
  handled by the input-language layer, below TSF, so the key never reaches the text
  service at all: the trace shows a `deactivate` with no key event, which is also
  why this IME cannot claim the chord back for itself.
- **Nothing happens when I type pinyin** — the document is read-only, so
  `StartComposition` failed and the keys are deliberately being handed to the
  host.
- **Sentences appear out of order** — should not happen: results are inserted
  strictly serially. If it does, the insertion anchor's gravity is not surviving
  the host's edits; check `Dsime_CaptureInsertAnchor` in `EditSessions.cpp`.
- **Settings changes don't take effect** — the IME re-reads its config when the
  thread regains focus, so alt-tab away and back. Key handling and window changes
  need the host process restarted.

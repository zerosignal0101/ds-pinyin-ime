# DS Input — Windows TSF frontend

A native Windows **Text Services Framework (TSF)** text service that turns whole
pinyin sentences into Chinese using the shared `dsime` core engine (an
OpenAI-compatible LLM converter) — a thin native frontend over a Rust core.

There is **no candidate window**, and nothing converts while you type. The
pre-edit is the raw pinyin you entered; Space sends the whole buffer to the model
and writes the returned sentence straight into the document, in one step. Enter
writes the raw buffer verbatim (no conversion), and Esc discards everything.

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
  host would otherwise have typed as text. They have to queue: a result is
  inserted at *the selection as it stands when its turn comes* — the anchor only
  decides where the composition is parked, and starting one moves the selection
  there — so writing the comma on the spot moves the caret past it, and the
  sentence still in flight then lands behind it. The user reads `，你好` for what
  they meant as `你好，`. Taking a number also matches the intent: the character
  belongs to the sentence just typed. `_IsKeyEaten` has to agree, since it decides
  before `_HandleKey` ever runs. The box's 待转换 badge counts conversions only, so
  a comma does not make it jump.
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
- Depth is bounded by `queue_max_pending`; past it, Space is swallowed rather
  than passed to the host (a literal space in the document would be worse).
- A lost thread focus does **not** cancel the queue, only the typing session.

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
- **Nothing happens when I type pinyin** — the document is read-only, so
  `StartComposition` failed and the keys are deliberately being handed to the
  host.
- **Sentences appear out of order** — should not happen: results are inserted
  strictly serially. If it does, the insertion anchor's gravity is not surviving
  the host's edits; check `Dsime_CaptureInsertAnchor` in `EditSessions.cpp`.
- **Settings changes don't take effect** — the IME re-reads its config when the
  thread regains focus, so alt-tab away and back. Key handling and window changes
  need the host process restarted.

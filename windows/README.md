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
composition mutation must happen there. The core's conversion callback fires on
a Tokio worker thread. We bridge them with a hidden **message-only window**
created on the STA thread:

- Space is handled on the STA thread, which calls `ds_session_convert_stream`.
  Nothing else ever contacts the provider.
- The core callback (a static C thunk) packages each update into a heap struct
  and `PostMessage`s `WM_DSIME_CONVERT_PARTIAL` (cumulative preview) or the
  terminal `WM_DSIME_CONVERT_RESULT`. The STA-thread window proc runs an edit
  session to update the composition.
- Stale results are dropped by comparing `request_id` against the most recent
  request id. A reference on the text service is held across each in-flight
  request so it can't be destroyed before the result is delivered.

### Composition lifecycle
The first pinyin key opens an `ITfComposition` (synchronous read/write edit
session). Each keystroke — including punctuation, which joins the buffer — just
rewrites the pre-edit to the raw buffer; there is no timer and no request. Space
sets `_commitOnResult` and fires the conversion: streamed partials repaint the
pre-edit as the sentence arrives, and the terminal result is written and the
composition ended. A failure or empty result clears the flag and leaves the raw
buffer on screen, so Space can simply be pressed again.

Editing while a conversion is in flight (any letter, or Backspace) cancels it via
`_AbandonPendingConversion`, so a late result can never land on top of the user's
correction. If TSF terminates the composition itself
(`ITfCompositionSink::OnCompositionTerminated`), we drop our state cleanly.

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
| `Composition.cpp` | Composition orchestration, conversion plumbing. |
| `EditSessions.cpp` | `ITfEditSession`s (start / set-text / end composition). |
| `DisplayAttribute.cpp` | Underline display attribute + provider/enumerator. |
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
  On any conversion error the IME keeps showing raw pinyin and never blocks.
- **DLL fails to load (0x8007007E)** — `dsime.dll` isn't next to
  `dsime_tsf.dll`. Keep them in the same folder.

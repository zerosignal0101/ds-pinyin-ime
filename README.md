# DS Input — LLM Whole‑Sentence Pinyin IME

Type a whole sentence in pinyin; an LLM converts it to Chinese. **No candidate
picking** — the model chooses the best sentence and you press Space to take it.
Works with any **OpenAI‑compatible** API; defaults to `deepseek-v4-flash`.
Windows.

```
type:      nihaoshijie, woshiyigechengxuyuan
           ↓ Space
committed: 你好世界，我是一个程序员
```

Nothing is converted while you type: the pinyin lives in a **floating input box
of our own**, the document is not touched at all, and there is no network
traffic in the background. Space is the single trigger — it hands the buffer to
a queue and the pinyin disappears immediately, so you carry straight on with the
next sentence. Conversions run **one at a time, in order**, and each result is
written back where its sentence was typed.

That last part is what keeps a search box from querying on every keystroke: the
document changes only when a finished Chinese sentence lands in it.

| Key | Effect |
|-----|--------|
| `a`–`z`, `'`, and any other printable | append to the buffer (shown in the floating box) |
| **Shift**+letter | same, but the letter is kept **upper‑case** |
| **Space** | convert the whole buffer and write the result; typing continues immediately |
| **Enter** | write the buffer verbatim, no conversion (for English, identifiers, …) |
| **Esc** | discard what is being typed; the queue keeps running |
| **Backspace** | edit the buffer |
| **Ctrl+Space** | toggle Chinese / English mode |

**Shift is how an abbreviation survives.** `shiyongAI` is converted to `使用AI`;
flattened to `shiyongai`, nothing distinguishes it from pinyin. The case is read
from the **Shift key**, never from the character `ToUnicode` produced — with
CapsLock on that returns capitals for ordinary typing too, and every sentence
would arrive as `NIHAO`.

**English mode (Ctrl+Space) has no buffer at all.** Nothing is converted and
nothing is remapped to full‑width, so a whole sentence of English types
natively; a buffer in progress when the mode flips is written **verbatim**.
Characters typed with nothing in the buffer are written on the spot, unless a
conversion is still on its way — then they queue behind it. Enter is the one
exception: it is as much a command as a character (a newline in an editor,
*submit* in a search box), so it always goes to the host.

The box shows the current mode and how many conversions are still queued. A
result that cannot be written (the document was closed, or became read‑only) is
put on the clipboard, and the box says so rather than dropping the sentence.

## Context

The model is given a history of what has already been converted **in that
program**, so it picks up the domain, terminology and wording of the document it
is filling in. Contexts are keyed per **process** (`code.exe|1234`), which is
what stops two Notepad3 windows editing different files from teaching the model
each other's vocabulary. Electron apps and VS Code run every window in one
process, so they still share a context.

The history grows append‑only (which is what lets the provider's prefix cache
hit) and is summarised automatically once it outgrows
`context_window_tokens × context_compact_ratio`. It lives **in memory only** —
it is a record of document text, and a copy on disk would outlive the document
it described while still being prepended to every request at token prices. Two
consequences worth knowing: it does not survive a restart, and switching input
methods away and back starts a fresh one. It is on by default, can be switched
off, and can be cleared from Settings.

## Architecture

| Layer | Path | Tech | Status |
|-------|------|------|--------|
| Shared core engine (C ABI) | [`core/`](core/) | Rust → `dsime.dll` cdylib | ✅ builds, tested |
| Windows frontend | [`windows/`](windows/) | C++ + Text Services Framework | see `windows/README.md` |

The core owns everything OS‑independent: the pinyin buffer state machine, the
async OpenAI‑compatible client, config load/save, the conversation context and
its compaction, and single‑in‑flight cancellation. The frontend captures keys,
draws the floating box, and runs the conversion queue. The one contract between
them is [`core/include/dsime.h`](core/include/dsime.h). See
[`DESIGN.md`](DESIGN.md).

## Quick start (core)

```bash
cd core
cargo build --release          # produces target/release/dsime.dll
cargo test --lib               # unit tests
# end-to-end smoke test against a real provider:
DSIME_API_KEY=sk-... cargo run --example cli -- ni hao shi jie
#  → 你好世界
```

`cli` drives the exact FFI the frontend uses, so it doubles as a runnable
reference. Override `DSIME_BASE_URL` / `DSIME_MODEL` to point at any
OpenAI‑compatible endpoint (OpenAI, OpenRouter, Azure, local Ollama/vLLM/LM
Studio).

## Configuration

Stored as JSON next to the user's config directory (Windows:
`%APPDATA%\DSInput\DSInput\config\config.json`), and edited through the Settings
window (single source of truth via `ds_engine_{get,set}_config_json`):

| Field | Default | Meaning |
|-------|---------|---------|
| `base_url` | `https://api.deepseek.com/v1` | OpenAI‑compatible endpoint |
| `api_key` | _(empty — set this!)_ | Bearer key |
| `model` | `deepseek-v4-flash` | Chat model id |
| `system_prompt` | _(pinyin→Chinese instruction)_ | Conversion behaviour |
| `temperature` | `0.3` | Ignored by providers while their thinking mode is on |
| `max_tokens` | `1024` | Cap per request — must also cover the model's *reasoning* tokens |
| `reasoning_effort` | `low` | Thinking‑effort hint (`reasoning_effort`). Empty = omit the field |
| `thinking` | _(empty)_ | Thinking switch: `enabled` / `disabled`. Empty = omit the field |
| `timeout_ms` | `15000` | Per‑request network timeout — must clear the model's *reasoning*, not just its answer |
| `stream` | `true` | Use the streaming API. No frontend streams: the queue is non‑streaming by design |
| `context_enabled` | `true` | Carry the per‑process history into the next request |
| `context_window_tokens` | `16384` | History budget; compaction fires at `× context_compact_ratio` |
| `context_keep_recent` | `10` | Turns kept verbatim across a compaction |
| `context_compact_ratio` | `0.75` | Fraction of the window at which compaction fires |
| `context_max_windows` | `20` | How many per‑process contexts are remembered (in‑memory LRU) |
| `context_prompt` | _(compaction instruction)_ | How the history is summarised |
| `queue_max_pending` | `8` | Conversions a frontend may queue before it stops accepting more |

> **Reasoning models need headroom.** The default model spends part of
> `max_tokens` on a hidden chain of thought *before* emitting any answer; a cap
> that is too small comes back as empty content rather than an error. `low`
> effort measured the same accuracy as the provider default on hard unsegmented
> pinyin at roughly half the latency.

## Building the Windows frontend

```powershell
cd windows; ./build.ps1        # core dsime.dll + CMake builds dsime_tsf.dll + DSInputSettings.exe
```

Requires VS 2022 (Desktop C++), a Windows SDK, and the Rust MSVC toolchain.
Then `regsvr32` the DLL from an elevated prompt and add the input method in
Settings ▸ Time & Language ▸ Language. Details in
[`windows/README.md`](windows/README.md).

## License

MIT.

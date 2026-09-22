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
| `a`–`z`, `'`, `,` `.` `?` `!` `;` `:` `(` `)` `\` | append to the buffer (shown in the floating box) |
| **Space** | convert the whole buffer and write the result; typing continues immediately |
| **Enter** | write the buffer verbatim, no conversion (for English, identifiers, …) |
| **Esc** | discard what is being typed; the queue keeps running |
| **Backspace** | edit the buffer |

The box also shows how many conversions are still queued. A result that cannot
be written (the document was closed, or became read‑only) is put on the
clipboard, and the box says so rather than dropping the sentence.

## Context

The model is given a **per‑input‑window** history of what has already been
converted, so it picks up the domain, terminology and wording of the document it
is filling in. Windows are keyed by application (`code.exe|Chrome_WidgetWin_1`),
which means every Chrome tab shares one context — the point is industry
vocabulary, which is a property of the app, not of a tab.

The history grows append‑only (which is what lets the provider's prefix cache
hit) and is summarised automatically once it outgrows
`context_window_tokens × context_compact_ratio`. Context is **persisted to disk**
beside the config file, so it accumulates across restarts. It is on by default,
can be switched off, and can be cleared from Settings — the context is a record
of what you typed, so there has to be a way to be rid of it.

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
| `timeout_ms` | `8000` | Per‑request network timeout |
| `stream` | `true` | Stream the answer into the pre‑edit as it arrives |
| `max_context_tokens` | `1000` | Soft per‑request context budget |

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

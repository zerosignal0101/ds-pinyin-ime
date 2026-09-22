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

Nothing is converted while you type: the pre‑edit shows exactly the pinyin (and
punctuation) you entered, with no network traffic in the background. Space is the
single trigger — it sends the whole buffer to the model and writes the returned
sentence in one step.

| Key | Effect |
|-----|--------|
| `a`–`z`, `'`, `,` `.` `?` `!` `;` `:` `(` `)` `\` | append to the buffer (shown as the underlined pre‑edit) |
| **Space** | convert the whole buffer and write the result — convert and commit in one step |
| **Enter** | write the buffer verbatim, no conversion (for English, identifiers, …) |
| **Esc** | discard everything; nothing is written |
| **Backspace** | edit the buffer |

## Architecture

| Layer | Path | Tech | Status |
|-------|------|------|--------|
| Shared core engine (C ABI) | [`core/`](core/) | Rust → `dsime.dll` cdylib | ✅ builds, tested |
| Windows frontend | [`windows/`](windows/) | C++ + Text Services Framework | see `windows/README.md` |

The core owns everything OS‑independent: the pinyin buffer state machine, the
async OpenAI‑compatible client, config load/save, and single‑in‑flight
cancellation. The frontend is thin: capture keys, render the inline pre‑edit,
commit text, host Settings. The one contract between them is
[`core/include/dsime.h`](core/include/dsime.h). See [`DESIGN.md`](DESIGN.md).

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

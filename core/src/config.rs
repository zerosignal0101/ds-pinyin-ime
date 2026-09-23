//! User configuration: the OpenAI-compatible provider, model, key, and tuning.
//!
//! Persisted as JSON so the platform Settings UIs can read/write it verbatim
//! through `ds_engine_get_config_json` / `ds_engine_set_config_json`.

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// Default endpoint: DeepSeek's OpenAI-compatible API.
pub const DEFAULT_BASE_URL: &str = "https://api.deepseek.com/v1";
/// Default model — a fast, cheap chat model well suited to inline conversion.
pub const DEFAULT_MODEL: &str = "deepseek-v4-flash";
/// Default reasoning effort for the provider's thinking mode, sent as
/// `reasoning_effort`. Providers default to "high", which is markedly slower on
/// long input; "low" measured the same accuracy on hard unsegmented pinyin at
/// roughly half the latency.
pub const DEFAULT_REASONING_EFFORT: &str = "low";

/// Per-request network timeout. It has to clear the *reasoning* tail, not just
/// the answer: on hard unsegmented pinyin a reasoning model routinely takes
/// 2–4 s, has been measured finishing a legitimate conversion at 8.1 s, and
/// spends ~10.5 s before giving up and returning an empty completion (see
/// `api::convert`'s rescue retry). The 8 s this used to be cut off good work.
pub const DEFAULT_TIMEOUT_MS: u64 = 15_000;

/// Every past value of the [`DEFAULT_TIMEOUT_MS`] default, oldest first — the
/// same contract as [`LEGACY_SYSTEM_PROMPTS`], and needed for the same reason:
/// the timeout is *stored* in config.json, so changing the constant in code
/// reaches nobody who has ever run the app. Append only; never edit an entry
/// after the fact.
///
/// The exact-match inference is weaker here than it is for the prompt — a user
/// could have chosen 8000 deliberately — but the direction is what makes it
/// safe: this upgrade only ever extends patience, and cannot discard anything
/// the user wrote.
pub const LEGACY_TIMEOUT_MS: &[u64] = &[8000];

/// Instruction that turns a chat model into a whole-sentence pinyin converter.
// Kept byte-stable and sent as the first (system) message on every request so it
// forms a constant cacheable prefix — DeepSeek context caching then bills it at
// the cache-hit rate and doesn't reprocess it on each incremental keystroke.
//
// Changing this text is NOT enough to ship the change: the prompt is *stored* in
// config.json, so every install that has ever run the app keeps whatever it was
// given at the time. Freeze the outgoing value in [`LEGACY_SYSTEM_PROMPTS`] and
// the upgrade in [`Config::upgrade_default_prompt`] will carry the new one to
// users who never customised it. Editing this string without adding the old one
// there means the edit reaches nobody.
pub const DEFAULT_SYSTEM_PROMPT: &str = "\
Convert toneless Hanyu Pinyin into the single most natural sentence. The input \
may MIX pinyin with English words, numbers, emails, URLs, and code identifiers: \
convert the pinyin parts to Chinese and keep the non-pinyin parts verbatim. Use \
spaces and context to tell pinyin from English; an apostrophe only marks a pinyin \
syllable boundary (xi'an = 西安).\n\
Rules:\n\
- Output ONLY the result: no explanation, quotes, extra whitespace, or \
alternatives.\n\
- Convert pinyin to Chinese; keep English words, numbers, emails, URLs, and code \
identifiers exactly as written.\n\
- A run of two or more UPPER-CASE letters (AI, ICT, PDF, URL) is an English \
abbreviation: keep it exactly as written and never read it as pinyin. A lower-case \
run is pinyin, as usual.\n\
- Use full-width Chinese punctuation amid Chinese; keep ASCII punctuation inside \
English and identifiers.\n\
- Examples: \"wo yong python xie daima\" -> \"我用python写代码\"; \"shiyongAI\" -> \
\"使用AI\".\n\
- If the input is empty or has no pinyin, return it unchanged.";

/// Every past value of [`DEFAULT_SYSTEM_PROMPT`], verbatim, oldest first.
///
/// A config file carries its own copy of the prompt, so a user who has launched
/// the app even once still holds the text they were first given. This list is
/// how that text is recognised on load: an exact match means "never customised",
/// and the current default replaces it. Anything else is a user's own edit and
/// is left alone — which is why the comparison is byte-for-byte and why an entry
/// must never be edited after the fact, only appended to.
pub const LEGACY_SYSTEM_PROMPTS: &[&str] = &["\
Convert toneless Hanyu Pinyin into the single most natural sentence. The input \
may MIX pinyin with English words, numbers, emails, URLs, and code identifiers: \
convert the pinyin parts to Chinese and keep the non-pinyin parts verbatim. Use \
spaces and context to tell pinyin from English; an apostrophe only marks a pinyin \
syllable boundary (xi'an = 西安).\n\
Rules:\n\
- Output ONLY the result: no explanation, quotes, extra whitespace, or \
alternatives.\n\
- Convert pinyin to Chinese; keep English words, numbers, emails, URLs, and code \
identifiers exactly as written.\n\
- Use full-width Chinese punctuation amid Chinese; keep ASCII punctuation inside \
English and identifiers.\n\
- Example: \"wo yong python xie daima\" -> \"我用python写代码\".\n\
- If the input is empty or has no pinyin, return it unchanged."];

/// Instruction for the context-compaction call. The conversion history is folded
/// into a short "scene" note that then rides in front of every later request.
///
/// Same two-stage `<analysis>` / `<summary>` shape the reference compaction
/// prompts use: the scratchpad raises summary quality but is stripped before the
/// note is stored, so only the conclusion ever reaches the context.
pub const DEFAULT_CONTEXT_PROMPT: &str = "\
上面是中文输入法的转换历史：每一轮是用户键入的无调拼音，以及它被转换成的中文。\
请把这段历史压缩成一份简短的语境摘要，它会被放在后续每一次转换的最前面——\
那些请求只能看到你的摘要和最近几轮原文，看不到更早的内容。\n\
先在 <analysis> 标签中逐条梳理，再在 <summary> 中给出结论。摘要需包含：\n\
1. 主题与领域：这段文字在谈什么，属于什么专业领域。\n\
2. 术语表：出现过的专业术语、专有名词、人名、地名、产品名及其固定写法，逐条列出。\n\
3. 文风：书面还是口语、正式程度、中英混排习惯。\n\
4. 未完结的线索：最后在写什么，接下来可能写什么。\n\
只输出结论，不要罗列原文；总长不超过 400 字。";

fn default_base_url() -> String {
    DEFAULT_BASE_URL.to_string()
}
fn default_model() -> String {
    DEFAULT_MODEL.to_string()
}
fn default_system_prompt() -> String {
    DEFAULT_SYSTEM_PROMPT.to_string()
}
fn default_temperature() -> f32 {
    0.3
}
fn default_max_tokens() -> u32 {
    1024
}
fn default_timeout_ms() -> u64 {
    DEFAULT_TIMEOUT_MS
}
fn default_stream() -> bool {
    true
}
fn default_context_enabled() -> bool {
    true
}
fn default_context_window_tokens() -> u32 {
    16384
}
fn default_context_keep_recent() -> u32 {
    10
}
fn default_context_compact_ratio() -> f32 {
    0.75
}
fn default_context_max_windows() -> u32 {
    20
}
fn default_context_prompt() -> String {
    DEFAULT_CONTEXT_PROMPT.to_string()
}
fn default_queue_max_pending() -> u32 {
    8
}
fn default_reasoning_effort() -> String {
    DEFAULT_REASONING_EFFORT.to_string()
}

/// The full, serializable user configuration.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Config {
    /// OpenAI-compatible base URL, e.g. `https://api.deepseek.com/v1`.
    #[serde(default = "default_base_url")]
    pub base_url: String,
    /// API bearer key. Empty until the user sets it.
    #[serde(default)]
    pub api_key: String,
    /// Chat model id, e.g. `deepseek-v4-flash`.
    #[serde(default = "default_model")]
    pub model: String,
    /// System prompt that defines the conversion behaviour.
    #[serde(default = "default_system_prompt")]
    pub system_prompt: String,
    /// Sampling temperature. Low = more deterministic conversions. Providers
    /// ignore this while their thinking mode is on (DeepSeek documents that it
    /// neither errors nor takes effect).
    #[serde(default = "default_temperature")]
    pub temperature: f32,
    /// Upper bound on generated tokens. This budget must also cover the
    /// provider's *reasoning* tokens: a reasoning model spends it on its chain
    /// of thought before emitting any answer, and an exhausted budget comes back
    /// as empty content rather than an error.
    #[serde(default = "default_max_tokens")]
    pub max_tokens: u32,
    /// Reasoning-effort hint, sent as `reasoning_effort`. DeepSeek maps
    /// low→low, medium/high→high, max→max. Empty omits the field, for endpoints
    /// that do not accept it.
    #[serde(default = "default_reasoning_effort")]
    pub reasoning_effort: String,
    /// Thinking-mode switch, sent as `{"thinking":{"type":"…"}}`. Empty (the
    /// default) omits the field entirely; "enabled" / "disabled" set it
    /// explicitly.
    #[serde(default)]
    pub thinking: String,
    /// Per-request network timeout. See [`DEFAULT_TIMEOUT_MS`] for what it has
    /// to cover — this is not the timeout of a chat request that answers in
    /// milliseconds, and a conversion that is still being thought about is not a
    /// failed one.
    #[serde(default = "default_timeout_ms")]
    pub timeout_ms: u64,
    /// Use the streaming API (SSE) instead of a single response. No frontend
    /// streams today -- the conversion queue is non-streaming by design -- so the
    /// CLI and the integration tests are what keep this path exercised.
    #[serde(default = "default_stream")]
    pub stream: bool,
    // ---- Conversation context -------------------------------------------------
    /// Carry a per-process typing history into every request, so the model sees
    /// the domain, terminology and style of what is being written. Off makes
    /// every request the bare `[system, user]` pair it always was.
    #[serde(default = "default_context_enabled")]
    pub context_enabled: bool,
    /// Token budget for that history. The history grows append-only until it
    /// reaches `context_window_tokens × context_compact_ratio`, where it is
    /// summarised. Once full this is the steady-state size of every request's
    /// prompt — so it trades context quality against per-request cost.
    #[serde(default = "default_context_window_tokens")]
    pub context_window_tokens: u32,
    /// Turns kept verbatim after a compaction. The most recent turns are the
    /// ones that matter while typing, so summarising *everything* would hurt.
    #[serde(default = "default_context_keep_recent")]
    pub context_keep_recent: u32,
    /// Fraction of `context_window_tokens` at which compaction fires.
    #[serde(default = "default_context_compact_ratio")]
    pub context_compact_ratio: f32,
    /// Upper bound on remembered contexts -- an in-memory LRU, one per process.
    /// Nothing here is persisted; the name is left over from the version that
    /// kept a JSON file per window.
    #[serde(default = "default_context_max_windows")]
    pub context_max_windows: u32,
    /// Prompt for the compaction call. See [`DEFAULT_CONTEXT_PROMPT`].
    #[serde(default = "default_context_prompt")]
    pub context_prompt: String,
    // ---- Frontend queue -------------------------------------------------------
    /// How many conversions a frontend may have queued before it stops accepting
    /// more (back-pressure). Read by the frontends; the core never queues.
    #[serde(default = "default_queue_max_pending")]
    pub queue_max_pending: u32,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            base_url: default_base_url(),
            api_key: String::new(),
            model: default_model(),
            system_prompt: default_system_prompt(),
            temperature: default_temperature(),
            max_tokens: default_max_tokens(),
            reasoning_effort: default_reasoning_effort(),
            thinking: String::new(),
            timeout_ms: default_timeout_ms(),
            stream: default_stream(),
            context_enabled: default_context_enabled(),
            context_window_tokens: default_context_window_tokens(),
            context_keep_recent: default_context_keep_recent(),
            context_compact_ratio: default_context_compact_ratio(),
            context_max_windows: default_context_max_windows(),
            context_prompt: default_context_prompt(),
            queue_max_pending: default_queue_max_pending(),
        }
    }
}

impl Config {
    /// Per-user default config path —
    /// Windows `%APPDATA%/DSPinyinIME/DSPinyinIME/config/config.json`.
    pub fn default_path() -> PathBuf {
        if let Some(dirs) = directories::ProjectDirs::from("io", "DSPinyinIME", "DSPinyinIME") {
            dirs.config_dir().join("config.json")
        } else {
            PathBuf::from("config.json")
        }
    }

    /// Load from `path`, falling back to defaults (and creating the file) if it
    /// does not yet exist. Missing fields are filled from defaults.
    pub fn load_or_create(path: &Path) -> std::io::Result<Config> {
        match std::fs::read_to_string(path) {
            Ok(text) => {
                let mut cfg: Config = serde_json::from_str(&text).map_err(|e| {
                    std::io::Error::new(std::io::ErrorKind::InvalidData, e.to_string())
                })?;
                if cfg.upgrade_defaults() {
                    // Best effort. A read-only config directory must not stop the
                    // IME from running -- it just means the user gets the new
                    // prompt again next launch, and it is upgraded in memory
                    // either way.
                    let _ = cfg.save(path);
                }
                Ok(cfg)
            }
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                let cfg = Config::default();
                cfg.save(path)?;
                Ok(cfg)
            }
            Err(e) => Err(e),
        }
    }

    /// Move a stock system prompt forward to the current default.
    ///
    /// The prompt is stored in the config file, so changing
    /// [`DEFAULT_SYSTEM_PROMPT`] in code reaches an existing install only
    /// through here. Only an exact match against a frozen
    /// [`LEGACY_SYSTEM_PROMPTS`] entry is rewritten -- that is what "the user
    /// never touched it" looks like. Anything else is their own text and is left
    /// exactly as written.
    ///
    /// Returns true when the prompt changed and the config wants saving.
    fn upgrade_default_prompt(&mut self) -> bool {
        if !LEGACY_SYSTEM_PROMPTS.contains(&self.system_prompt.as_str()) {
            return false;
        }
        self.system_prompt = DEFAULT_SYSTEM_PROMPT.to_string();
        true
    }

    /// Move a stock `timeout_ms` forward to the current default. Same exact-match
    /// contract as [`Config::upgrade_default_prompt`] — see
    /// [`LEGACY_TIMEOUT_MS`] for why widening a timeout makes that inference
    /// safe where the prompt needed the byte-for-byte comparison.
    fn upgrade_timeout_ms(&mut self) -> bool {
        if !LEGACY_TIMEOUT_MS.contains(&self.timeout_ms) {
            return false;
        }
        self.timeout_ms = DEFAULT_TIMEOUT_MS;
        true
    }

    /// Every stock value that has moved on since this config was written.
    ///
    /// Deliberately two statements rather than `a() || b()`: `||` short-circuits,
    /// so a config that is stale in both dimensions would upgrade only the first
    /// — and that is precisely the config belonging to someone who has been
    /// running the app across both releases.
    ///
    /// Note this only runs on the load path. A frontend that saves through
    /// `ds_engine_set_config_json` writes the config directly and never comes
    /// through here, which is why the frontends' own copy of a default (the
    /// Settings dialog's blank-field fallbacks) has to be kept in step by hand.
    fn upgrade_defaults(&mut self) -> bool {
        let prompt = self.upgrade_default_prompt();
        let timeout = self.upgrade_timeout_ms();
        prompt || timeout
    }

    /// Pretty-print to `path`, creating parent directories as needed.
    pub fn save(&self, path: &Path) -> std::io::Result<()> {
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        let json = serde_json::to_string_pretty(self)
            .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e.to_string()))?;
        std::fs::write(path, json)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_are_deepseek_flash() {
        let c = Config::default();
        assert_eq!(c.base_url, "https://api.deepseek.com/v1");
        assert_eq!(c.model, "deepseek-v4-flash");
        assert!(c.api_key.is_empty());
        assert!(c.max_tokens > 0);
        // Long enough to clear the reasoning tail, not just the answer; see
        // DEFAULT_TIMEOUT_MS.
        assert_eq!(c.timeout_ms, DEFAULT_TIMEOUT_MS);
        assert!(c.timeout_ms >= 15_000);
    }

    #[test]
    fn missing_fields_fill_from_defaults() {
        // A minimal config (only api_key) must still deserialize, with every
        // other field taking its default — this is what a fresh Settings save
        // or a hand-edited file may look like. A config file written before the
        // context fields existed lands here too, and must not fail to load.
        let json = r#"{ "api_key": "sk-test" }"#;
        let c: Config = serde_json::from_str(json).unwrap();
        assert_eq!(c.api_key, "sk-test");
        assert_eq!(c.model, "deepseek-v4-flash");
        assert_eq!(c.temperature, 0.3);
        assert!(c.context_enabled);
    }

    #[test]
    fn context_defaults_leave_compaction_headroom() {
        let c = Config::default();
        assert_eq!(c.context_window_tokens, 16384);
        assert_eq!(c.context_keep_recent, 10);
        assert_eq!(c.context_compact_ratio, 0.75);
        assert!(c.context_max_windows > 0);
        assert!(c.queue_max_pending > 0);
        assert!(!c.context_prompt.is_empty());
        // Compaction has to fire strictly before the window is full, otherwise
        // the summarising request itself would be built from an over-budget
        // history.
        assert!(
            c.context_compact_ratio > 0.0 && c.context_compact_ratio < 1.0,
            "compaction ratio must leave headroom"
        );
    }

    #[test]
    fn round_trips_through_disk() {
        let dir = std::env::temp_dir().join(format!("dsime-test-{}", std::process::id()));
        let path = dir.join("config.json");
        let _ = std::fs::remove_dir_all(&dir);

        // First load creates the file with defaults.
        let c1 = Config::load_or_create(&path).unwrap();
        assert!(path.exists());
        assert_eq!(c1.model, "deepseek-v4-flash");

        // Mutate + save, then reload and confirm persistence.
        let mut c2 = c1.clone();
        c2.api_key = "sk-roundtrip".to_string();
        c2.model = "gpt-4o-mini".to_string();
        c2.save(&path).unwrap();
        let c3 = Config::load_or_create(&path).unwrap();
        assert_eq!(c3.api_key, "sk-roundtrip");
        assert_eq!(c3.model, "gpt-4o-mini");

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_legacy_default_prompt_is_upgraded() {
        // Distinct temp dir per test: these run in parallel threads of one
        // process, so a pid-only name would collide with round_trips_through_disk.
        let dir = std::env::temp_dir().join(format!("dsime-cfg-old-{}", std::process::id()));
        let path = dir.join("config.json");
        let _ = std::fs::remove_dir_all(&dir);

        // A prompt that IS a past default means the user never edited it, so it
        // is ours to move forward.
        let stale = Config {
            system_prompt: LEGACY_SYSTEM_PROMPTS[0].to_string(),
            ..Config::default()
        };
        stale.save(&path).unwrap();

        let loaded = Config::load_or_create(&path).unwrap();
        assert_eq!(loaded.system_prompt, DEFAULT_SYSTEM_PROMPT);
        assert!(
            std::fs::read_to_string(&path)
                .unwrap()
                .contains("UPPER-CASE"),
            "the upgrade has to be written back, or Settings would still show the \
             old text and a later Save would resurrect it"
        );

        // Listing the current default as a legacy value would make the upgrade a
        // no-op that looks like it works.
        assert!(!LEGACY_SYSTEM_PROMPTS.contains(&DEFAULT_SYSTEM_PROMPT));

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_customised_prompt_is_left_alone() {
        let dir = std::env::temp_dir().join(format!("dsime-cfg-mine-{}", std::process::id()));
        let path = dir.join("config.json");
        let _ = std::fs::remove_dir_all(&dir);

        // Anything that is not a byte-for-byte past default is the user's own
        // text, and a "helpful" rewrite would silently discard their work.
        let mine = Config {
            system_prompt: "CONVERT THIS MY WAY".to_string(),
            ..Config::default()
        };
        mine.save(&path).unwrap();

        let loaded = Config::load_or_create(&path).unwrap();
        assert_eq!(loaded.system_prompt, "CONVERT THIS MY WAY");
        assert!(std::fs::read_to_string(&path)
            .unwrap()
            .contains("CONVERT THIS MY WAY"));

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_legacy_timeout_is_upgraded() {
        // Same trap as the prompt: the value lives in config.json, so every
        // install that ran the 8 s build still has 8 s and would never see a
        // change to the constant.
        let dir = std::env::temp_dir().join(format!("dsime-cfg-tmo-{}", std::process::id()));
        let path = dir.join("config.json");
        let _ = std::fs::remove_dir_all(&dir);

        let stale = Config {
            timeout_ms: LEGACY_TIMEOUT_MS[0],
            ..Config::default()
        };
        stale.save(&path).unwrap();

        let loaded = Config::load_or_create(&path).unwrap();
        assert_eq!(loaded.timeout_ms, DEFAULT_TIMEOUT_MS);
        assert!(
            std::fs::read_to_string(&path)
                .unwrap()
                .contains(&DEFAULT_TIMEOUT_MS.to_string()),
            "the upgrade has to be written back, or Settings would still show the \
             old value and a later Save would resurrect it"
        );

        // Listing the current default as a legacy value would make the upgrade a
        // no-op that looks like it works.
        assert!(!LEGACY_TIMEOUT_MS.contains(&DEFAULT_TIMEOUT_MS));

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_chosen_timeout_is_left_alone() {
        let dir = std::env::temp_dir().join(format!("dsime-cfg-tmo2-{}", std::process::id()));
        let path = dir.join("config.json");
        let _ = std::fs::remove_dir_all(&dir);

        // Only the stock value is ours to move: anyone who typed a number into
        // the field — slower or faster than the default — keeps it.
        for chosen in [3000u64, 10_000, 60_000] {
            let mine = Config {
                timeout_ms: chosen,
                ..Config::default()
            };
            mine.save(&path).unwrap();
            assert_eq!(Config::load_or_create(&path).unwrap().timeout_ms, chosen);
        }

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn both_stock_upgrades_apply_in_one_load() {
        // The regression test for `upgrade_defaults` short-circuiting: a config
        // written by the oldest build in the wild is stale in both dimensions,
        // and both have to move on the same load.
        let dir = std::env::temp_dir().join(format!("dsime-cfg-both-{}", std::process::id()));
        let path = dir.join("config.json");
        let _ = std::fs::remove_dir_all(&dir);

        let stale = Config {
            system_prompt: LEGACY_SYSTEM_PROMPTS[0].to_string(),
            timeout_ms: LEGACY_TIMEOUT_MS[0],
            ..Config::default()
        };
        stale.save(&path).unwrap();

        let loaded = Config::load_or_create(&path).unwrap();
        assert_eq!(loaded.system_prompt, DEFAULT_SYSTEM_PROMPT);
        assert_eq!(loaded.timeout_ms, DEFAULT_TIMEOUT_MS);

        let on_disk = std::fs::read_to_string(&path).unwrap();
        assert!(on_disk.contains("UPPER-CASE"));
        assert!(on_disk.contains(&DEFAULT_TIMEOUT_MS.to_string()));

        let _ = std::fs::remove_dir_all(&dir);
    }
}

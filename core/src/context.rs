//! Per-window conversation context: what the user has already written in the
//! same input window, carried into the next conversion.
//!
//! The message array is **append-only** between compactions. That is the whole
//! point: DeepSeek's context cache matches complete *prefix units*, so editing or
//! reordering an earlier message invalidates every token after it. Appending a
//! `[user pinyin][assistant chinese]` pair keeps the prefix byte-identical from
//! one request to the next, so each conversion only pays for the new pair.
//!
//! Two consequences shape the code below:
//!
//! * The summary (built by a compaction call) sits *before* the turns and is
//!   rewritten only when compaction runs, so it too is a stable prefix.
//! * Token accounting is anchored on the provider's own `usage.prompt_tokens`
//!   rather than a local tokenizer — see [`WindowContext::estimated_tokens`].

use crate::api;
use crate::config::Config;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;
use std::time::{SystemTime, UNIX_EPOCH};

/// One converted sentence.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Turn {
    pub pinyin: String,
    pub chinese: String,
}

/// The remembered state of one input window — also its on-disk shape.
///
/// A request works from a *clone* of this rather than a lock guard: the store's
/// `MutexGuard` is not `Send`, and the request runs inside a spawned task that is
/// awaited.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct WindowContext {
    /// Compacted note covering everything older than `turns`. Rewritten only by
    /// compaction; `None` until the first one runs.
    #[serde(default)]
    pub summary: Option<String>,
    #[serde(default)]
    pub turns: Vec<Turn>,
    /// Prompt tokens the provider reported for the last request that carried
    /// this context — the anchor the next estimate is built from.
    #[serde(default)]
    pub last_prompt_tokens: u32,
    /// Completion tokens for that same request. The assistant's reply joins the
    /// history on the next turn, so it is part of what the next prompt costs.
    #[serde(default)]
    pub last_completion_tokens: u32,
    #[serde(default)]
    pub updated_at: u64,
}

impl WindowContext {
    /// Estimated prompt tokens for a request that sends this context plus
    /// `next_pinyin`.
    ///
    /// Anchored on the provider's own count: after a response we know exactly
    /// what that prompt cost, and the next prompt is that, plus the reply which
    /// has since joined the history, plus the new input. Only the new input needs
    /// estimating — so this neither needs a tokenizer nor accumulates drift from
    /// re-guessing the whole history every time.
    pub fn estimated_tokens(&self, cfg: &Config, next_pinyin: &str) -> u32 {
        let base = if self.last_prompt_tokens > 0 {
            self.last_prompt_tokens
                .saturating_add(self.last_completion_tokens)
        } else {
            // No server count yet (fresh window, or just compacted): fall back to
            // estimating what we would send.
            estimate_tokens(&cfg.system_prompt)
                + self.summary.as_deref().map_or(0, estimate_tokens)
                + self
                    .turns
                    .iter()
                    .map(|t| estimate_tokens(&t.pinyin) + estimate_tokens(&t.chinese))
                    .sum::<u32>()
        };
        base.saturating_add(estimate_tokens(next_pinyin))
    }

    /// True once the history has grown to `window × ratio`, i.e. it is time to
    /// summarise. The ratio leaves headroom so the compaction request itself is
    /// built from a history that still fits.
    pub fn needs_compaction(&self, cfg: &Config, next_pinyin: &str) -> bool {
        // Nothing to fold away when there are no turns yet — a bare summary is
        // already as compact as it gets.
        if !cfg.context_enabled || self.turns.is_empty() {
            return false;
        }
        let budget = (cfg.context_window_tokens as f32 * cfg.context_compact_ratio) as u32;
        self.estimated_tokens(cfg, next_pinyin) >= budget
    }

    /// Fold in a completed conversion and the usage the provider reported.
    pub fn record(&mut self, pinyin: &str, chinese: &str, usage: api::Usage) {
        self.turns.push(Turn {
            pinyin: pinyin.to_string(),
            chinese: chinese.to_string(),
        });
        self.last_prompt_tokens = usage.prompt_tokens;
        self.last_completion_tokens = usage.completion_tokens;
        self.updated_at = now_secs();
    }

    /// Replace everything older than the last `keep_recent` turns with `summary`.
    ///
    /// The recent turns stay verbatim: they are what the user is actually
    /// continuing, and summarising them loses more than the tokens save.
    pub fn apply_summary(&mut self, summary: String, keep_recent: u32) {
        let keep = keep_recent as usize;
        if self.turns.len() > keep {
            self.turns.drain(..self.turns.len() - keep);
        }
        self.summary = Some(summary);
        // The anchor described a longer history than the one we now send.
        self.last_prompt_tokens = 0;
        self.last_completion_tokens = 0;
        self.updated_at = now_secs();
    }

    /// Drop the oldest turns until the history fits the configured window.
    ///
    /// Run on load: lowering `context_window_tokens` must take effect, otherwise
    /// a context file written under a larger budget would keep sending an
    /// over-budget prompt forever.
    pub fn truncate_to_window(&mut self, cfg: &Config) {
        let budget = cfg.context_window_tokens.max(1);
        // Walk from the front; each drop invalidates the usage anchor, so
        // recompute the heuristic estimate after removing a turn.
        while !self.turns.is_empty() && self.estimated_tokens_without_input(cfg) > budget {
            self.turns.remove(0);
            self.last_prompt_tokens = 0;
            self.last_completion_tokens = 0;
        }
    }

    fn estimated_tokens_without_input(&self, cfg: &Config) -> u32 {
        self.estimated_tokens(cfg, "")
    }
}

/// Remembered windows, persisted one file per window.
pub struct ContextStore {
    /// `None` disables persistence (used by tests and by a core built without a
    /// writable config directory).
    dir: Option<PathBuf>,
    map: Mutex<HashMap<String, WindowContext>>,
    /// Directory pruning runs once, lazily, on the first access.
    pruned: AtomicBool,
}

impl ContextStore {
    pub fn new(dir: Option<PathBuf>) -> ContextStore {
        ContextStore {
            dir,
            map: Mutex::new(HashMap::new()),
            pruned: AtomicBool::new(false),
        }
    }

    /// A copy of a window's context. Unknown windows start empty; a window whose
    /// file exists is loaded (and trimmed to the current window size) on first
    /// use.
    pub fn snapshot(&self, key: &str, cfg: &Config) -> WindowContext {
        let mut map = self.map.lock().unwrap();
        if !map.contains_key(key) {
            let mut ctx = self.load(key);
            ctx.truncate_to_window(cfg);
            self.evict_if_needed(&mut map, cfg);
            map.insert(key.to_string(), ctx);
        }
        map.get(key).cloned().unwrap_or_default()
    }

    /// Apply `f` to a window's context, persist the result, and return whatever
    /// `f` produced.
    pub fn update<R>(&self, key: &str, cfg: &Config, f: impl FnOnce(&mut WindowContext) -> R) -> R {
        self.prune_dir_once(cfg);
        let mut map = self.map.lock().unwrap();
        if !map.contains_key(key) {
            let mut ctx = self.load(key);
            ctx.truncate_to_window(cfg);
            self.evict_if_needed(&mut map, cfg);
            map.insert(key.to_string(), ctx);
        }
        let ctx = map.get_mut(key).expect("just inserted");
        let out = f(ctx);
        ctx.updated_at = now_secs();
        if let Some(dir) = &self.dir {
            let path = dir.join(file_name_for(key));
            if let Err(e) = write_json(&path, ctx) {
                // A context that cannot be persisted is still perfectly usable
                // in memory — never let a disk problem break typing.
                let _ = e;
            }
        }
        out
    }

    /// Forget every window, on disk and in memory — Settings' "clear contexts".
    ///
    /// The context is a record of what the user has typed, so there has to be a
    /// way to get rid of it that does not involve hunting for files.
    pub fn clear_all(&self) {
        self.map.lock().unwrap().clear();
        let Some(dir) = &self.dir else { return };
        let Ok(entries) = std::fs::read_dir(dir) else {
            return;
        };
        for entry in entries.flatten() {
            if entry.path().extension().is_some_and(|x| x == "json") {
                let _ = std::fs::remove_file(entry.path());
            }
        }
    }

    fn load(&self, key: &str) -> WindowContext {
        let Some(dir) = &self.dir else {
            return WindowContext::default();
        };
        let path = dir.join(file_name_for(key));
        std::fs::read_to_string(&path)
            .ok()
            .and_then(|text| serde_json::from_str(&text).ok())
            .unwrap_or_default()
    }

    /// Keep the in-memory map within `context_max_windows`, oldest first.
    fn evict_if_needed(&self, map: &mut HashMap<String, WindowContext>, cfg: &Config) {
        let cap = cfg.context_max_windows.max(1) as usize;
        while map.len() >= cap {
            let Some(oldest) = map
                .iter()
                .min_by_key(|(_, c)| c.updated_at)
                .map(|(k, _)| k.clone())
            else {
                break;
            };
            map.remove(&oldest);
            if let Some(dir) = &self.dir {
                let _ = std::fs::remove_file(dir.join(file_name_for(&oldest)));
            }
        }
    }

    /// Bound the number of context files on disk. Runs once per process, before
    /// the first write, so growth is capped without a directory scan per
    /// conversion.
    fn prune_dir_once(&self, cfg: &Config) {
        if self.pruned.swap(true, Ordering::SeqCst) {
            return;
        }
        let Some(dir) = &self.dir else { return };
        let cap = cfg.context_max_windows.max(1) as usize;
        let Ok(entries) = std::fs::read_dir(dir) else {
            return;
        };
        let mut files: Vec<(SystemTime, PathBuf)> = entries
            .flatten()
            .filter(|e| e.path().extension().is_some_and(|x| x == "json"))
            .filter_map(|e| {
                let modified = e.metadata().ok()?.modified().ok()?;
                Some((modified, e.path()))
            })
            .collect();
        if files.len() <= cap {
            return;
        }
        files.sort_by_key(|(t, _)| *t);
        for (_, path) in files.drain(..files.len() - cap) {
            let _ = std::fs::remove_file(path);
        }
    }
}

fn write_json(path: &Path, ctx: &WindowContext) -> std::io::Result<()> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let json = serde_json::to_string(ctx)
        .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e.to_string()))?;
    std::fs::write(path, json)
}

/// A stable, filesystem-safe name for a window key.
///
/// The readable prefix keeps the directory browsable ("code_exe_Chrome_Widget…");
/// the hash suffix makes it collision-free and stable no matter how the prefix
/// was truncated.
fn file_name_for(key: &str) -> String {
    let mut safe: String = key
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || c == '-' || c == '_' {
                c
            } else {
                '_'
            }
        })
        .collect();
    safe.truncate(48); // ASCII-only, so this is a char-boundary-safe truncation
    format!("{safe}-{:016x}.json", fnv1a(key))
}

fn fnv1a(s: &str) -> u64 {
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    for b in s.as_bytes() {
        h ^= *b as u64;
        h = h.wrapping_mul(0x0000_0100_0000_01b3);
    }
    h
}

fn now_secs() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_secs())
}

/// Rough token estimate, used only until the provider reports a real count.
///
/// DeepSeek's guidance is ~0.6 tokens per Chinese character and ~4 ASCII
/// characters per token. The previous `chars / 2` guess applied one ratio to
/// both, which is wrong in two directions at once: it over-counts ASCII pinyin
/// by 2×, and it slightly under-counts the CJK that makes up almost every
/// assistant turn.
pub fn estimate_tokens(s: &str) -> u32 {
    let (ascii, wide) = s.chars().fold((0u32, 0u32), |(a, w), c| {
        if c.is_ascii() {
            (a + 1, w)
        } else {
            (a, w + 1)
        }
    });
    ascii.div_ceil(4) + (wide * 6).div_ceil(10)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn turn(p: &str, c: &str) -> Turn {
        Turn {
            pinyin: p.to_string(),
            chinese: c.to_string(),
        }
    }

    #[test]
    fn estimate_separates_ascii_from_cjk() {
        // 4 ASCII chars ≈ 1 token.
        assert_eq!(estimate_tokens("abcd"), 1);
        // 12 CJK chars ≈ 7.2 tokens; the old `chars / 2` guess would have said 6.
        assert_eq!(estimate_tokens("你好世界你好世界你好世界"), 8);
        assert!(estimate_tokens("你好世界你好世界你好世界") > 12 / 2);
        assert_eq!(estimate_tokens(""), 0);
    }

    #[test]
    fn usage_anchor_beats_the_heuristic() {
        let cfg = Config::default();
        let mut ctx = WindowContext {
            turns: vec![turn("nihaoshijie", "你好世界")],
            ..Default::default()
        };

        // Without a server count, the estimate is heuristic.
        let heuristic = ctx.estimated_tokens(&cfg, "woshigechengxuyuan");
        assert!(heuristic > 0);

        // With one, the next estimate is anchored on it plus the reply that has
        // since joined the history — not a re-guess of everything.
        ctx.last_prompt_tokens = 500;
        ctx.last_completion_tokens = 20;
        let anchored = ctx.estimated_tokens(&cfg, "woshigechengxuyuan");
        assert_eq!(anchored, 500 + 20 + estimate_tokens("woshigechengxuyuan"));
    }

    #[test]
    fn record_keeps_the_turn_and_the_usage() {
        let mut ctx = WindowContext::default();
        ctx.record(
            "nihaoshijie",
            "你好世界",
            api::Usage {
                prompt_tokens: 120,
                completion_tokens: 8,
            },
        );
        assert_eq!(ctx.turns.len(), 1);
        assert_eq!(ctx.turns[0].chinese, "你好世界");
        assert_eq!(ctx.last_prompt_tokens, 120);
    }

    #[test]
    fn compaction_keeps_the_recent_turns_verbatim() {
        let mut ctx = WindowContext {
            turns: (0..5)
                .map(|i| turn(&format!("p{i}"), &format!("c{i}")))
                .collect(),
            ..Default::default()
        };
        ctx.last_prompt_tokens = 9000;
        ctx.apply_summary("主题：测试".to_string(), 2);

        assert_eq!(ctx.summary.as_deref(), Some("主题：测试"));
        assert_eq!(ctx.turns.len(), 2);
        // The newest turns survive; the oldest are folded into the summary.
        assert_eq!(ctx.turns[0].pinyin, "p3");
        assert_eq!(ctx.turns[1].pinyin, "p4");
        // The anchor described the pre-compaction array, so it is dropped.
        assert_eq!(ctx.last_prompt_tokens, 0);
    }

    #[test]
    fn truncate_honours_a_lowered_window() {
        // Small enough to bite, but comfortably above the system prompt — which
        // is always sent and cannot be trimmed away.
        let cfg = Config {
            context_window_tokens: 400,
            ..Config::default()
        };
        assert!(
            WindowContext::default().estimated_tokens_without_input(&cfg) < 400,
            "the test window must leave room for at least one turn"
        );

        let mut ctx = WindowContext {
            turns: (0..200)
                .map(|_| turn("nihaoshijie", "你好世界我是一个程序员"))
                .collect(),
            ..Default::default()
        };
        assert!(ctx.estimated_tokens_without_input(&cfg) > 400);

        ctx.truncate_to_window(&cfg);
        assert!(ctx.estimated_tokens_without_input(&cfg) <= 400);
        // It drops from the front, so the newest turns are what survives.
        assert!(!ctx.turns.is_empty());
        assert!(ctx.turns.len() < 200);
    }

    #[test]
    fn needs_compaction_fires_at_the_ratio() {
        let cfg = Config {
            context_window_tokens: 1000,
            context_compact_ratio: 0.75,
            ..Config::default()
        };
        // A history is required: a bare summary has nothing left to fold away.
        let mut ctx = WindowContext {
            turns: vec![turn("nihaoshijie", "你好世界")],
            ..Default::default()
        };
        ctx.last_prompt_tokens = 740;
        assert!(!ctx.needs_compaction(&cfg, "abc")); // 741 < 750
        ctx.last_prompt_tokens = 750;
        assert!(ctx.needs_compaction(&cfg, "")); // 750 >= 750
        assert!(ctx.needs_compaction(&cfg, "abc")); // 751 >= 750
    }

    #[test]
    fn disabled_context_never_compacts() {
        let cfg = Config {
            context_enabled: false,
            ..Config::default()
        };
        let ctx = WindowContext {
            turns: vec![turn("nihaoshijie", "你好世界")],
            last_prompt_tokens: 999_999,
            ..Default::default()
        };
        assert!(!ctx.needs_compaction(&cfg, "abc"));
    }

    #[test]
    fn store_round_trips_through_disk() {
        let dir = std::env::temp_dir().join(format!("dsime-ctx-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let cfg = Config::default();
        let key = "code.exe|Chrome_WidgetWin_1";

        {
            let store = ContextStore::new(Some(dir.clone()));
            store.update(key, &cfg, |c| {
                c.record("nihaoshijie", "你好世界", api::Usage::default())
            });
        }
        // A fresh store (as after a restart) must find it again.
        let store = ContextStore::new(Some(dir.clone()));
        let snap = store.snapshot(key, &cfg);
        assert_eq!(snap.turns.len(), 1);
        assert_eq!(snap.turns[0].chinese, "你好世界");

        // An unrelated key stays empty rather than sharing the file.
        assert!(store.snapshot("other.exe|Edit", &cfg).turns.is_empty());

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn store_survives_an_unwritable_directory() {
        // Persistence is best-effort: a bad path must not lose the in-memory
        // context or panic.
        let store = ContextStore::new(Some(PathBuf::from("\0invalid")));
        let cfg = Config::default();
        store.update("k", &cfg, |c| c.record("p", "c", api::Usage::default()));
        assert_eq!(store.snapshot("k", &cfg).turns.len(), 1);
    }

    #[test]
    fn file_names_are_distinct_and_bounded() {
        let a = file_name_for("code.exe|Chrome_WidgetWin_1");
        let b = file_name_for("code.exe|Chrome_WidgetWin_2");
        assert_ne!(a, b);
        assert!(a.len() <= 48 + 1 + 16 + 5);
        assert!(a.ends_with(".json"));
    }
}

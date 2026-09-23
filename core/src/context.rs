//! Per-process conversation context: what the user has already written in this
//! program instance, carried into the next conversion.
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
//!
//! The store is **in memory only**. It used to write one JSON file per window
//! beside the config file, which had two costs that were not worth paying: the
//! history outlived the document it described (and went on being prepended to
//! every request, at token prices, however stale it was), and it left a copy of
//! the user's typed text on disk forever. A context is worth having only for the
//! session the user is actually in, and that is what this keeps.

use crate::api;
use crate::config::Config;
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::{SystemTime, UNIX_EPOCH};

/// One converted sentence.
#[derive(Debug, Clone)]
pub struct Turn {
    pub pinyin: String,
    pub chinese: String,
}

/// The remembered state of one program instance.
///
/// A request works from a *clone* of this rather than a lock guard: the store's
/// `MutexGuard` is not `Send`, and the request runs inside a spawned task that is
/// awaited.
#[derive(Debug, Clone, Default)]
pub struct WindowContext {
    /// Compacted note covering everything older than `turns`. Rewritten only by
    /// compaction; `None` until the first one runs.
    pub summary: Option<String>,
    pub turns: Vec<Turn>,
    /// Prompt tokens the provider reported for the last request that carried
    /// this context — the anchor the next estimate is built from.
    pub last_prompt_tokens: u32,
    /// Completion tokens for that same request. The assistant's reply joins the
    /// history on the next turn, so it is part of what the next prompt costs.
    pub last_completion_tokens: u32,
    /// When this window was last touched: the in-memory LRU clock that
    /// `context_max_windows` evicts on. Nothing else reads it.
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
}

/// Remembered program instances, in memory for the life of the engine.
///
/// Deliberately not persisted — see the module comment. The only thing this
/// knows about a disk is how to clean up after the version that did persist.
pub struct ContextStore {
    map: Mutex<HashMap<String, WindowContext>>,
    /// Where a previous version kept its JSON, when this core was pointed at a
    /// config file. Used *only* to delete those leftovers: nothing is ever
    /// written here again, and the field is named to keep it that way.
    legacy_dir: Option<PathBuf>,
}

impl ContextStore {
    pub fn new(legacy_dir: Option<PathBuf>) -> ContextStore {
        ContextStore {
            map: Mutex::new(HashMap::new()),
            legacy_dir,
        }
    }

    /// A copy of a window's context. Unknown windows — including every window on
    /// a fresh engine — start empty.
    pub fn snapshot(&self, key: &str, cfg: &Config) -> WindowContext {
        let mut map = self.map.lock().unwrap();
        if !map.contains_key(key) {
            self.evict_if_needed(&mut map, cfg);
            map.insert(key.to_string(), WindowContext::default());
        }
        map.get(key).cloned().unwrap_or_default()
    }

    /// Apply `f` to a window's context and return whatever `f` produced.
    pub fn update<R>(&self, key: &str, cfg: &Config, f: impl FnOnce(&mut WindowContext) -> R) -> R {
        let mut map = self.map.lock().unwrap();
        if !map.contains_key(key) {
            self.evict_if_needed(&mut map, cfg);
            map.insert(key.to_string(), WindowContext::default());
        }
        let ctx = map.get_mut(key).expect("just inserted");
        let out = f(ctx);
        ctx.updated_at = now_secs();
        out
    }

    /// Forget every window — Settings' "clear contexts".
    ///
    /// The context is a record of what the user has typed, so there has to be a
    /// way to be rid of it that does not involve hunting for files. What a
    /// previous version wrote counts as part of "it", so this deletes that too.
    pub fn clear_all(&self) {
        self.map.lock().unwrap().clear();
        if let Some(dir) = &self.legacy_dir {
            purge_legacy_files(dir);
        }
    }

    /// Delete what a previous version left on disk.
    ///
    /// The store no longer writes anything, but an install that has run an older
    /// build still has a directory of the user's typed text under `%APPDATA%`.
    /// Leaving it there would make "nothing is written to disk" false on exactly
    /// the machines that have been used. Cheap and idempotent: once the files are
    /// gone this is a failed `read_dir` on a missing path.
    pub fn purge_legacy_dir(&self) {
        let Some(dir) = &self.legacy_dir else { return };
        purge_legacy_files(dir);
    }

    /// Keep the map within `context_max_windows`, oldest first.
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
        }
    }

    /// Test-only: is this key remembered? Unlike `snapshot`, asking does not
    /// create the entry it asks about.
    #[cfg(test)]
    fn contains(&self, key: &str) -> bool {
        self.map.lock().unwrap().contains_key(key)
    }
}

/// Delete the files a previous version of this store wrote, best effort.
fn purge_legacy_files(dir: &Path) {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if is_legacy_context_file(&path) {
            let _ = std::fs::remove_file(path);
        }
    }
    // Only take the directory itself once nothing is left in it. `remove_dir`
    // refuses a non-empty one, which is the check we want.
    let _ = std::fs::remove_dir(dir);
}

/// Is this a file the previous version wrote?
///
/// The filter is load-bearing, not tidiness. The core is pointed at an arbitrary
/// config path by the CLI example and by tests, so an unfiltered `*.json` sweep
/// would delete a stranger's data from whatever directory it was handed. Only the
/// exact name shape `file_name_for` produced counts: `{prefix}-{16 hex}.json`,
/// where the hash was a 64-bit FNV-1a rendered in lower-case.
fn is_legacy_context_file(path: &Path) -> bool {
    if path.extension().is_none_or(|e| e != "json") {
        return false;
    }
    let Some(stem) = path.file_stem().and_then(|s| s.to_str()) else {
        return false;
    };
    let Some((_, hash)) = stem.rsplit_once('-') else {
        return false;
    };
    hash.len() == 16
        && hash
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
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

    /// The exact name shape the previous version wrote: a readable prefix, a
    /// '-', 16 lower-case hex, `.json`. Spelled out rather than generated,
    /// because it is a contract with a build that no longer exists here.
    const LEGACY_NAME: &str = "code_exe_Chrome_WidgetWin_1-0123456789abcdef.json";

    fn turn(p: &str, c: &str) -> Turn {
        Turn {
            pinyin: p.to_string(),
            chinese: c.to_string(),
        }
    }

    /// A temp directory unique to one test. Each test gets its own name because
    /// they run in parallel threads of a single process, so pid alone collides.
    fn temp_dir(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("dsime-ctx-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
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
    fn a_fresh_store_does_not_see_an_older_one() {
        // What a restart looks like now, and the direct assertion that history
        // is no longer carried across one.
        let cfg = Config::default();
        let first = ContextStore::new(None);
        first.update("code.exe|1234", &cfg, |c| {
            c.record("nihaoshijie", "你好世界", api::Usage::default())
        });
        assert_eq!(first.snapshot("code.exe|1234", &cfg).turns.len(), 1);

        let second = ContextStore::new(None);
        assert!(second.snapshot("code.exe|1234", &cfg).turns.is_empty());
    }

    #[test]
    fn eviction_honours_context_max_windows() {
        let cfg = Config {
            context_max_windows: 2,
            ..Config::default()
        };
        let store = ContextStore::new(None);

        // A window that was read but never recorded keeps `updated_at == 0`, so
        // it is the one eviction must pick. (`update` stamps from the clock,
        // whose one-second resolution cannot order three entries in a test.)
        store.snapshot("never-recorded", &cfg);
        store.update("fresh", &cfg, |c| c.record("p", "c", api::Usage::default()));
        store.update("newest", &cfg, |c| {
            c.record("p", "c", api::Usage::default())
        });

        assert!(store.contains("fresh"));
        assert!(store.contains("newest"));
        assert!(!store.contains("never-recorded"), "the oldest must go");
    }

    #[test]
    fn clear_all_forgets_every_window() {
        let cfg = Config::default();
        let store = ContextStore::new(None);
        store.update("a.exe|1", &cfg, |c| {
            c.record("p", "c", api::Usage::default())
        });
        store.clear_all();
        assert!(!store.contains("a.exe|1"));
    }

    #[test]
    fn clear_all_deletes_legacy_files_and_leaves_foreign_ones() {
        let dir = temp_dir("purge");
        std::fs::write(dir.join(LEGACY_NAME), b"{}").unwrap();
        // Not ours, and this is the point of the filter: the core is pointed at
        // arbitrary config directories by the CLI and by tests.
        std::fs::write(dir.join("notes.json"), b"someone else's").unwrap();
        std::fs::write(dir.join("thing-abc.json"), b"{}").unwrap();
        std::fs::write(dir.join("keep.txt"), b"not json").unwrap();

        let store = ContextStore::new(Some(dir.clone()));
        store.clear_all();

        assert!(
            !dir.join(LEGACY_NAME).exists(),
            "our own file should be gone"
        );
        assert!(dir.join("notes.json").exists());
        assert!(dir.join("thing-abc.json").exists());
        assert!(dir.join("keep.txt").exists());
        // Still occupied by someone else, so the directory itself stays.
        assert!(dir.exists());

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn the_startup_purge_takes_the_emptied_directory_too() {
        let dir = temp_dir("purge-once");
        std::fs::write(dir.join(LEGACY_NAME), b"{}").unwrap();

        ContextStore::new(Some(dir.clone())).purge_legacy_dir();
        assert!(!dir.exists());

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn the_startup_purge_is_harmless_without_a_directory() {
        // The engine is built with no config parent in some embeddings, and the
        // directory is absent after the first purge. Neither may panic.
        ContextStore::new(None).purge_legacy_dir();
        let dir = temp_dir("purge-absent");
        std::fs::remove_dir_all(&dir).unwrap();
        ContextStore::new(Some(dir.clone())).purge_legacy_dir();
        assert!(!dir.exists());
    }
}

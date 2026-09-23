//! Engine and session state: the runtime, HTTP client, config, and per-session
//! pinyin buffer with single-in-flight cancellation.

use crate::api;
use crate::config::Config;
use crate::context::{ContextStore, WindowContext};
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Condvar, Mutex, RwLock, Weak};
use std::time::{Duration, Instant};
use tokio::runtime::{Handle, Runtime};
use tokio::sync::Notify;

/// How long `ds_engine_free` waits for pending conversion callbacks to land
/// before giving up and letting the runtime be dropped. Frontends balance
/// per-request resources on that callback, so a swallowed terminal is a leak.
const SHUTDOWN_DRAIN_TIMEOUT: Duration = Duration::from_secs(2);

/// Outcome handed back to the FFI layer's callback.
pub struct ConvertOutcome {
    pub request_id: u64,
    pub status: i32,
    pub text: String,
}

/// Build the terminal outcome, honouring supersession: if a newer request has
/// claimed the session (generation moved past `my_gen`), report DS_ERR_CANCELLED
/// regardless of how this request actually finished.
fn finalize(
    active_gen: &AtomicU64,
    my_gen: u64,
    request_id: u64,
    result: Result<api::Completed, api::ConvertError>,
) -> ConvertOutcome {
    if active_gen.load(Ordering::SeqCst) != my_gen {
        let e = api::ConvertError::Cancelled;
        return ConvertOutcome {
            request_id,
            status: e.status_code(),
            text: e.message(),
        };
    }
    match result {
        Ok(completed) => ConvertOutcome {
            request_id,
            status: 0, // DS_OK
            text: completed.text,
        },
        Err(e) => ConvertOutcome {
            request_id,
            status: e.status_code(),
            text: e.message(),
        },
    }
}

/// Shared, thread-safe engine state. One per process is typical; cheap to share
/// across sessions via `Arc`.
pub struct Engine {
    /// Handle to the shared Tokio runtime, which is *owned* by [`EngineHandle`].
    /// This is a `Handle`, not the `Runtime`: dropping the last `Arc<Engine>` —
    /// which can happen on a worker thread when an in-flight task finishes after
    /// the frontend freed the engine — must NOT run the runtime's (blocking)
    /// shutdown, because dropping a `Runtime` on one of its own worker threads
    /// panics. Dropping a `Handle` is harmless on any thread.
    rt: Handle,
    client: reqwest::Client,
    config: RwLock<Arc<Config>>,
    config_path: PathBuf,
    /// Per-window conversation context, persisted next to the config.
    contexts: ContextStore,
    /// Cancel tokens of the live sessions, so shutdown can reach their in-flight
    /// work. `Weak`, so a freed session is simply skipped.
    sessions: Mutex<Vec<Weak<SessionControl>>>,
    /// Conversion tasks spawned but not yet finished delivering.
    inflight: AtomicUsize,
    idle_lock: Mutex<()>,
    idle: Condvar,
}

/// The cancel state shared between a [`Session`] and its in-flight task.
///
/// `Arc`-shared rather than living directly in the `Session` so that an in-flight
/// task keeps it alive after the session is freed — and so
/// [`EngineHandle::drop`] can reach it through the engine's registry without
/// borrowing the session, which may already be gone.
struct SessionControl {
    /// Bumping this cancels the outstanding request: `finalize` reports any
    /// result whose generation has moved as `DS_ERR_CANCELLED`.
    active_gen: AtomicU64,
    /// Wakes a parked `select!` arm so a cancelled request need not wait out its
    /// HTTP timeout before reporting.
    wake: Notify,
}

impl SessionControl {
    fn new() -> SessionControl {
        SessionControl {
            active_gen: AtomicU64::new(0),
            wake: Notify::new(),
        }
    }

    /// Cancel the outstanding request and claim the next generation for a new one.
    fn claim(&self) -> u64 {
        let gen = self.active_gen.fetch_add(1, Ordering::SeqCst) + 1;
        self.wake.notify_waiters();
        gen
    }

    /// Cancel the outstanding request, waking it if it is parked.
    fn cancel(&self) {
        self.active_gen.fetch_add(1, Ordering::SeqCst);
        self.wake.notify_waiters();
    }
}

/// Owns the Tokio [`Runtime`] plus a strong reference to the shared [`Engine`].
/// `ds_engine_new` hands a pointer to this across the FFI; `ds_engine_free` drops
/// it on the *caller's* thread, so the runtime is always shut down off its own
/// worker threads (dropping a `Runtime` on a worker thread panics). Sessions and
/// in-flight tasks only ever hold an `Arc<Engine>` — which carries a runtime
/// `Handle`, not the `Runtime` — so whichever task happens to drop the last
/// `Arc<Engine>` can never trigger the runtime's shutdown.
pub struct EngineHandle {
    // Held only to own the runtime and shut it down on drop (never read directly;
    // tasks spawn through the `Handle` in `Engine`). NB: field declaration order is
    // drop order — `_rt` is dropped first, which cancels any in-flight tasks (their
    // futures are dropped, not awaited), then the shared-engine strong ref is freed.
    _rt: Runtime,
    engine: Arc<Engine>,
}

impl EngineHandle {
    pub fn new(config_path: Option<PathBuf>) -> Result<EngineHandle, String> {
        let config_path = config_path.unwrap_or_else(Config::default_path);
        let config = Config::load_or_create(&config_path)
            .map_err(|e| format!("failed to load config at {}: {e}", config_path.display()))?;

        let rt = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .build()
            .map_err(|e| format!("failed to start runtime: {e}"))?;

        // One shared, connection-pooled client. We keep idle connections warm so
        // back-to-back conversions (one per committed sentence) reuse the same
        // TCP+TLS connection instead of reconnecting.
        let client = reqwest::Client::builder()
            .user_agent(concat!("dsime/", env!("CARGO_PKG_VERSION")))
            .tcp_keepalive(Duration::from_secs(60))
            .pool_idle_timeout(Duration::from_secs(90))
            .build()
            .map_err(|e| format!("failed to build http client: {e}"))?;

        // Contexts are in memory only, so the config directory is used for one
        // thing: finding — and deleting — what an older version of this core
        // wrote there. That is also why it is derived from `config_path` rather
        // than a fixed `%APPDATA%`: a caller that points the core at its own
        // config path (the CLI example, tests) must clean up beside *that*, not
        // in the real user profile, and the purge filters by file name so it can
        // never take something that was not ours.
        let contexts = ContextStore::new(config_path.parent().map(|p| p.join("context")));
        contexts.purge_legacy_dir();

        let engine = Arc::new(Engine {
            // Tasks spawn onto this handle; the Runtime itself stays in `rt`.
            rt: rt.handle().clone(),
            client,
            config: RwLock::new(Arc::new(config)),
            config_path,
            contexts,
            sessions: Mutex::new(Vec::new()),
            inflight: AtomicUsize::new(0),
            idle_lock: Mutex::new(()),
            idle: Condvar::new(),
        });
        Ok(EngineHandle { _rt: rt, engine })
    }

    /// Borrow the shared engine — for the read-only `ds_engine_*` FFI accessors.
    pub fn engine(&self) -> &Engine {
        &self.engine
    }

    /// A fresh strong reference to the shared engine — for `ds_session_new`, so a
    /// session keeps the engine state alive independently of this handle.
    pub fn engine_arc(&self) -> Arc<Engine> {
        Arc::clone(&self.engine)
    }
}

impl Drop for EngineHandle {
    fn drop(&mut self) {
        // Why this is a hand-written `Drop` rather than field declaration order:
        // dropping the `Runtime` cancels its spawned tasks, and a cancelled task
        // never reaches its `deliver` call. The "exactly once" contract in
        // `dsime.h` is what frontends hang per-request resources on — the TSF
        // frontend's `AddRef`, the Settings window's wait loop — so a swallowed
        // terminal is a leak, not merely a lost result.
        //
        // Cancel everything, give the deliveries a bounded moment to land, and
        // only then let the fields drop (`_rt` first, which is what actually
        // shuts the runtime down).
        self.engine.cancel_all();
        self.engine.wait_for_idle(SHUTDOWN_DRAIN_TIMEOUT);
    }
}

impl Engine {
    pub fn config_snapshot(&self) -> Arc<Config> {
        self.config.read().unwrap().clone()
    }

    pub fn config_path(&self) -> &PathBuf {
        &self.config_path
    }

    pub fn get_config_json(&self) -> Result<String, String> {
        serde_json::to_string_pretty(&*self.config_snapshot()).map_err(|e| e.to_string())
    }

    /// Replace config from JSON and persist to disk.
    pub fn set_config_json(&self, json: &str) -> Result<(), String> {
        let cfg: Config = serde_json::from_str(json).map_err(|e| format!("invalid config: {e}"))?;
        cfg.save(&self.config_path)
            .map_err(|e| format!("failed to save config: {e}"))?;
        *self.config.write().unwrap() = Arc::new(cfg);
        Ok(())
    }

    pub fn reload_config(&self) -> Result<(), String> {
        let cfg = Config::load_or_create(&self.config_path).map_err(|e| e.to_string())?;
        *self.config.write().unwrap() = Arc::new(cfg);
        Ok(())
    }

    /// Forget every window's conversation context, on disk and in memory.
    pub fn clear_contexts(&self) {
        self.contexts.clear_all();
    }

    /// Track a session's cancel token, so shutdown can reach its in-flight work.
    fn register_session(&self, control: &Arc<SessionControl>) {
        let mut list = self.sessions.lock().unwrap();
        // Drop entries whose session and task are both gone, so the list stays
        // proportional to live sessions rather than to sessions ever created.
        list.retain(|w| w.strong_count() > 0);
        list.push(Arc::downgrade(control));
    }

    /// Count a spawned conversion task in, so `wait_for_idle` knows to wait.
    fn task_started(&self) {
        self.inflight.fetch_add(1, Ordering::SeqCst);
    }

    /// Count a finished conversion task out. Called *after* `deliver`, so seeing
    /// zero means every outstanding callback has already been made.
    fn task_finished(&self) {
        if self.inflight.fetch_sub(1, Ordering::SeqCst) == 1 {
            let _guard = self.idle_lock.lock().unwrap();
            self.idle.notify_all();
        }
    }

    /// Cancel the outstanding request of every live session.
    fn cancel_all(&self) {
        let live: Vec<Arc<SessionControl>> = self
            .sessions
            .lock()
            .unwrap()
            .iter()
            .filter_map(Weak::upgrade)
            .collect();
        for control in live {
            control.cancel();
        }
    }

    /// Block until no conversion task is outstanding, or `timeout` elapses.
    ///
    /// Normally returns immediately: the callers that matter (`Deactivate`) cancel
    /// first, and a cancelled request reports without waiting out its HTTP
    /// timeout. The bound exists so a wedged task can never hang a shutdown.
    fn wait_for_idle(&self, timeout: Duration) {
        let deadline = Instant::now() + timeout;
        let mut guard = self.idle_lock.lock().unwrap();
        while self.inflight.load(Ordering::SeqCst) > 0 {
            let Some(left) = deadline.checked_duration_since(Instant::now()) else {
                return;
            };
            let (g, _) = self.idle.wait_timeout(guard, left).unwrap();
            guard = g;
        }
    }
}

/// Is context out of the picture for this request?
///
/// An empty key means the frontend could not work out which window it is typing
/// into. The ABI documents that as "no context for this request", and it has to
/// be honoured *here*: without the check, every such request files under `""` and
/// shares one history, so unrelated documents would teach the model each other's
/// vocabulary — a worse outcome than having no context at all.
fn context_off(cfg: &Config, key: &str) -> bool {
    !cfg.context_enabled || key.is_empty()
}

/// The window's context, or an empty one when the feature is switched off.
///
/// Disabling the feature stops *using* the stored history without deleting it,
/// so switching it back on resumes where the user left off.
fn load_context(engine: &Engine, cfg: &Config, key: &str) -> WindowContext {
    if context_off(cfg, key) {
        WindowContext::default()
    } else {
        engine.contexts.snapshot(key, cfg)
    }
}

/// Summarise the window's history once it has outgrown its budget.
///
/// Returns the new note, or `None` when it is not yet time or the call failed — a
/// failed compaction is not fatal, since the uncompacted history is still valid,
/// just larger.
async fn compact_if_needed(
    engine: &Engine,
    cfg: &Config,
    ctx: &WindowContext,
    pinyin: &str,
) -> Option<String> {
    if !ctx.needs_compaction(cfg, pinyin) {
        return None;
    }
    api::compact_context(&engine.client, cfg, ctx).await.ok()
}

/// The window's context as a request should see it, compacted first if it has
/// outgrown its budget.
async fn prepare_context(engine: &Engine, cfg: &Config, key: &str, pinyin: &str) -> WindowContext {
    // Before `load_context`, because the compaction path below *writes*: an
    // update under an empty key would create the entry this is meant to avoid.
    if context_off(cfg, key) {
        return WindowContext::default();
    }
    let ctx = load_context(engine, cfg, key);
    let Some(summary) = compact_if_needed(engine, cfg, &ctx, pinyin).await else {
        return ctx;
    };
    engine.contexts.update(key, cfg, |c| {
        c.apply_summary(summary, cfg.context_keep_recent)
    });
    load_context(engine, cfg, key)
}

/// Fold a completed conversion into the window's context, so the next request in
/// that window sees what has already been written.
fn remember(engine: &Engine, cfg: &Config, key: &str, pinyin: &str, completed: &api::Completed) {
    if context_off(cfg, key) {
        return;
    }
    engine.contexts.update(key, cfg, |c| {
        c.record(pinyin, &completed.text, completed.usage)
    });
}

/// Per-input-context session. Holds the raw pinyin buffer and tracks the single
/// in-flight conversion so a new keystroke supersedes the previous request.
pub struct Session {
    engine: Arc<Engine>,
    buffer: Mutex<String>,
    /// Which input window is being typed into — the key the conversation context
    /// is filed under. Set per document by the frontend, because a single
    /// text-service instance outlives any one document.
    context_key: Mutex<String>,
    req_counter: AtomicU64,
    /// Generation/cancel token for the active request, shared with the task.
    control: Arc<SessionControl>,
    /// LLM conversions of the current input the user can cycle through with
    /// up/down. Shared into the worker task so a completed conversion can record
    /// itself.
    candidates: Arc<Mutex<Candidates>>,
}

/// The alternative LLM conversions for one input, plus the currently-shown index.
/// `list[0]` is the primary conversion; later entries are regenerated on demand.
#[derive(Default)]
struct Candidates {
    /// The exact pinyin these candidates were produced for. Navigation is only
    /// valid while this matches the live buffer; once the user types more, the
    /// next conversion replaces the whole set.
    input: String,
    list: Vec<String>,
    cursor: usize,
}

impl Session {
    pub fn new(engine: Arc<Engine>) -> Session {
        let control = Arc::new(SessionControl::new());
        engine.register_session(&control);
        Session {
            engine,
            buffer: Mutex::new(String::new()),
            context_key: Mutex::new(String::new()),
            req_counter: AtomicU64::new(0),
            control,
            candidates: Arc::new(Mutex::new(Candidates::default())),
        }
    }

    /// Name the window this session is typing into.
    ///
    /// The conversation context is filed under this key, so the model keeps
    /// seeing the same domain, terminology and style for as long as the user
    /// stays in one window — and starts clean when they move to another.
    pub fn set_context_key(&self, key: &str) {
        *self.context_key.lock().unwrap() = key.to_string();
    }

    /// Move to the previous (`direction < 0`) or next (`direction >= 0`) cached
    /// candidate for the current input and return it, or `None` when there is no
    /// candidate in that direction (already at the primary going up, or none left
    /// going down — the frontend then calls [`regenerate`](Self::regenerate)).
    /// Synchronous; consults only the cache, never the network.
    pub fn cached_candidate(&self, direction: i32) -> Option<String> {
        let buffer = self.get_input();
        let mut c = self.candidates.lock().unwrap();
        if c.input != buffer || c.list.is_empty() {
            return None;
        }
        let next = if direction >= 0 {
            let n = c.cursor + 1;
            if n >= c.list.len() {
                return None;
            }
            n
        } else {
            c.cursor.checked_sub(1)?
        };
        c.cursor = next;
        Some(c.list[next].clone())
    }

    pub fn set_input(&self, pinyin: &str) {
        *self.buffer.lock().unwrap() = pinyin.to_string();
    }

    pub fn get_input(&self) -> String {
        self.buffer.lock().unwrap().clone()
    }

    pub fn reset(&self) {
        self.cancel_inflight();
        self.buffer.lock().unwrap().clear();
    }

    pub fn cancel_inflight(&self) {
        self.control.cancel();
    }

    /// Spawn an async conversion of the current buffer. `deliver` is invoked
    /// with the outcome from a worker thread, unless the request is superseded
    /// or cancelled first. Returns the request id, or 0 if the buffer is empty.
    pub fn convert<F>(&self, deliver: F) -> u64
    where
        F: FnOnce(ConvertOutcome) + Send + 'static,
    {
        let pinyin = self.get_input();
        if pinyin.trim().is_empty() {
            return 0;
        }

        // Supersede any previous request and claim this generation.
        let my_gen = self.control.claim();

        let request_id = self.req_counter.fetch_add(1, Ordering::SeqCst) + 1;
        let engine = self.engine.clone();
        let control = self.control.clone();
        let cfg = engine.config_snapshot();
        let handle = engine.rt.clone();
        let context_key = self.context_key.lock().unwrap().clone();

        let candidates = self.candidates.clone();
        engine.task_started();
        handle.spawn(async move {
            let ctx = prepare_context(&engine, &cfg, &context_key, &pinyin).await;
            let result = tokio::select! {
                biased;
                _ = control.wake.notified() => Err(api::ConvertError::Cancelled),
                r = api::convert(&engine.client, &cfg, &ctx, &pinyin, &[]) => r,
            };
            if let Ok(completed) = &result {
                record_candidate(
                    &candidates,
                    &control.active_gen,
                    my_gen,
                    &pinyin,
                    &completed.text,
                    false,
                );
                remember(&engine, &cfg, &context_key, &pinyin, completed);
            }
            // The callback is invoked exactly once for every convert() that
            // returned a non-zero id (frontends rely on this to balance the
            // resources tied to `deliver`).
            deliver(finalize(&control.active_gen, my_gen, request_id, result));
            engine.task_finished();
        });

        request_id
    }

    /// Like [`Session::convert`], but streams the conversion (SSE) when the
    /// config enables it. `on_partial` is invoked zero-or-more times with the
    /// cumulative text as it arrives, then `deliver` is invoked exactly once
    /// with the terminal outcome (final text, error, or DS_ERR_CANCELLED).
    ///
    /// `on_partial` calls are best-effort: they only fire while this request is
    /// still the active generation, and never after `deliver`. Frontends should
    /// tie per-request resource ownership to `deliver`, not to `on_partial`.
    pub fn convert_stream<P, F>(&self, on_partial: P, deliver: F) -> u64
    where
        P: Fn(u64, &str) + Send + 'static,
        F: FnOnce(ConvertOutcome) + Send + 'static,
    {
        // Normal conversion: no exclusions; the result replaces the candidate set.
        self.stream_with(Vec::new(), false, on_partial, deliver)
    }

    /// Ask the provider for a DIFFERENT conversion of the current input, avoiding
    /// every candidate already shown, and append it to the candidate list (so
    /// up/down can revisit it without another request). Same streaming contract
    /// as [`convert_stream`](Self::convert_stream). The frontend calls this when
    /// [`cached_candidate`](Self::cached_candidate) returns `None` going down —
    /// i.e. the user wants another option but none is cached yet.
    pub fn regenerate<P, F>(&self, on_partial: P, deliver: F) -> u64
    where
        P: Fn(u64, &str) + Send + 'static,
        F: FnOnce(ConvertOutcome) + Send + 'static,
    {
        let buffer = self.get_input();
        let exclude = {
            let c = self.candidates.lock().unwrap();
            if c.input == buffer {
                c.list.clone()
            } else {
                Vec::new()
            }
        };
        self.stream_with(exclude, true, on_partial, deliver)
    }

    /// Shared driver for `convert_stream` / `regenerate`. `exclude` lists the
    /// already-shown conversions to avoid; `append` adds the result to the
    /// candidate list (vs. replacing it).
    fn stream_with<P, F>(
        &self,
        exclude: Vec<String>,
        append: bool,
        on_partial: P,
        deliver: F,
    ) -> u64
    where
        P: Fn(u64, &str) + Send + 'static,
        F: FnOnce(ConvertOutcome) + Send + 'static,
    {
        let pinyin = self.get_input();
        if pinyin.trim().is_empty() {
            return 0;
        }

        let my_gen = self.control.claim();

        let request_id = self.req_counter.fetch_add(1, Ordering::SeqCst) + 1;
        let engine = self.engine.clone();
        let control = self.control.clone();
        let candidates = self.candidates.clone();
        let cfg = engine.config_snapshot();
        let handle = engine.rt.clone();
        let context_key = self.context_key.lock().unwrap().clone();

        engine.task_started();
        handle.spawn(async move {
            let ctx = prepare_context(&engine, &cfg, &context_key, &pinyin).await;
            let result = if cfg.stream {
                let control_p = control.clone();
                // `move` so the spawned future owns `on_partial` (needs only
                // Send, not Sync). Drop partials from a superseded generation so
                // a stale stream can't overwrite a newer request's pre-edit.
                let on_delta = move |cumulative: &str| {
                    if control_p.active_gen.load(Ordering::SeqCst) == my_gen {
                        on_partial(request_id, cumulative);
                    }
                };
                tokio::select! {
                    biased;
                    _ = control.wake.notified() => Err(api::ConvertError::Cancelled),
                    r = api::convert_stream(&engine.client, &cfg, &ctx, &pinyin, &exclude, on_delta) => r,
                }
            } else {
                tokio::select! {
                    biased;
                    _ = control.wake.notified() => Err(api::ConvertError::Cancelled),
                    r = api::convert(&engine.client, &cfg, &ctx, &pinyin, &exclude) => r,
                }
            };
            if let Ok(completed) = &result {
                record_candidate(
                    &candidates,
                    &control.active_gen,
                    my_gen,
                    &pinyin,
                    &completed.text,
                    append,
                );
                remember(&engine, &cfg, &context_key, &pinyin, completed);
            }
            deliver(finalize(&control.active_gen, my_gen, request_id, result));
            engine.task_finished();
        });

        request_id
    }
}

/// Record a successful LLM conversion in the candidate cache, unless a newer
/// request has already superseded this one. `append` adds `text` as another
/// option for the same input (regeneration); otherwise it replaces the set with
/// a fresh `[text]` (a new conversion). A duplicate is never appended.
fn record_candidate(
    candidates: &Mutex<Candidates>,
    active_gen: &AtomicU64,
    my_gen: u64,
    pinyin: &str,
    text: &str,
    append: bool,
) {
    if active_gen.load(Ordering::SeqCst) != my_gen {
        return;
    }
    let mut c = candidates.lock().unwrap();
    if append && c.input == pinyin {
        if !c.list.iter().any(|t| t == text) {
            c.list.push(text.to_string());
        }
        c.cursor = c.list.len().saturating_sub(1);
    } else {
        c.input = pinyin.to_string();
        c.list = vec![text.to_string()];
        c.cursor = 0;
    }
}

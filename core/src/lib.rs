//! C ABI for the DS Pinyin IME core engine. See `core/include/dsime.h` for the
//! authoritative documented interface. This file is the thin, `unsafe` FFI shell
//! over the safe `engine` / `api` / `config` modules.

mod api;
mod config;
mod context;
mod engine;
pub mod lexicon;

pub use engine::{Engine, EngineHandle, Session};
use std::cell::RefCell;
use std::ffi::{c_char, c_void, CStr, CString};
use std::ptr;

// Status codes, mirroring the `#define`s in `core/include/dsime.h`. The ones the
// conversion callback uses come from `api::ConvertError::status_code`; these are
// the synchronous entry points, which return them directly.
const DS_OK: i32 = 0;
const DS_ERR_CONFIG: i32 = 5;
const DS_ERR_INTERNAL: i32 = 6;

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::new("").unwrap());
}

fn set_last_error(msg: impl Into<String>) {
    let c = CString::new(msg.into()).unwrap_or_else(|_| CString::new("error").unwrap());
    LAST_ERROR.with(|e| *e.borrow_mut() = c);
}

/// Borrow a `&str` from a C string pointer; returns None for NULL/invalid UTF-8.
unsafe fn cstr<'a>(p: *const c_char) -> Option<&'a str> {
    if p.is_null() {
        return None;
    }
    CStr::from_ptr(p).to_str().ok()
}

/// Allocate a C string the caller must free with `ds_string_free`.
fn to_c_string(s: impl Into<Vec<u8>>) -> *mut c_char {
    match CString::new(s) {
        Ok(c) => c.into_raw(),
        Err(_) => ptr::null_mut(),
    }
}

/// Same, for a payload that legitimately contains NULs — a NUL-separated list.
///
/// `CString::new` rejects interior NULs, which is the right default but exactly
/// wrong here, so the terminator is appended by hand. Sound because the buffer is
/// a live `Vec` from the global allocator, `ManuallyDrop` stops it being freed
/// twice, and the byte we just pushed guarantees the NUL `from_raw` requires.
fn to_c_string_list(mut bytes: Vec<u8>) -> *mut c_char {
    bytes.push(0);
    let mut v = std::mem::ManuallyDrop::new(bytes);
    // SAFETY: see above — `v` owns the buffer, ends in 0, and is never used again.
    unsafe { CString::from_raw(v.as_mut_ptr() as *mut c_char) }.into_raw()
}

// ---- Engine lifecycle ------------------------------------------------------

/// # Safety
/// `config_path` is NULL or a valid NUL-terminated UTF-8 string.
#[no_mangle]
pub unsafe extern "C" fn ds_engine_new(config_path: *const c_char) -> *mut EngineHandle {
    let path = cstr(config_path).map(std::path::PathBuf::from);
    match EngineHandle::new(path) {
        Ok(handle) => Box::into_raw(Box::new(handle)),
        Err(e) => {
            set_last_error(e);
            ptr::null_mut()
        }
    }
}

/// # Safety
/// `engine` is a pointer returned by `ds_engine_new`, used at most once here.
/// All sessions created from it must already have been freed (`ds_session_free`).
#[no_mangle]
pub unsafe extern "C" fn ds_engine_free(engine: *mut EngineHandle) {
    if !engine.is_null() {
        // Drops the Tokio runtime on THIS (the caller's) thread — never on a
        // worker thread — cancelling any in-flight tasks, then releases this
        // handle's strong ref to the shared engine.
        drop(Box::from_raw(engine));
    }
}

/// Borrow the shared `Engine` behind an `EngineHandle` pointer, without taking
/// ownership.
unsafe fn engine_ref<'a>(engine: *mut EngineHandle) -> Option<&'a Engine> {
    if engine.is_null() {
        None
    } else {
        Some((*engine).engine())
    }
}

/// # Safety
/// `engine` is a valid pointer from `ds_engine_new`.
#[no_mangle]
pub unsafe extern "C" fn ds_engine_reload_config(engine: *mut EngineHandle) -> i32 {
    let Some(e) = engine_ref(engine) else {
        return 5; // DS_ERR_CONFIG
    };
    match e.reload_config() {
        Ok(()) => 0,
        Err(msg) => {
            set_last_error(msg);
            5
        }
    }
}

/// # Safety
/// `engine` is a valid pointer from `ds_engine_new`.
#[no_mangle]
pub unsafe extern "C" fn ds_engine_get_config_json(engine: *mut EngineHandle) -> *mut c_char {
    let Some(e) = engine_ref(engine) else {
        return ptr::null_mut();
    };
    match e.get_config_json() {
        Ok(json) => to_c_string(json),
        Err(msg) => {
            set_last_error(msg);
            ptr::null_mut()
        }
    }
}

/// # Safety
/// `engine` is valid; `json_utf8` is a valid NUL-terminated UTF-8 string.
#[no_mangle]
pub unsafe extern "C" fn ds_engine_set_config_json(
    engine: *mut EngineHandle,
    json_utf8: *const c_char,
) -> i32 {
    let Some(e) = engine_ref(engine) else {
        return 5;
    };
    let Some(json) = cstr(json_utf8) else {
        set_last_error("config json is NULL or not UTF-8");
        return 5;
    };
    match e.set_config_json(json) {
        Ok(()) => 0,
        Err(msg) => {
            set_last_error(msg);
            5
        }
    }
}

/// # Safety
/// `engine` is a valid pointer from `ds_engine_new`.
#[no_mangle]
pub unsafe extern "C" fn ds_engine_config_path(engine: *mut EngineHandle) -> *mut c_char {
    let Some(e) = engine_ref(engine) else {
        return ptr::null_mut();
    };
    to_c_string(e.config_path().to_string_lossy().into_owned())
}

/// Forget every remembered input window's conversation context — both the
/// in-memory copy and the files on disk. Returns `DS_OK`, or `DS_ERR_CONFIG` for
/// a NULL engine.
///
/// The context is a record of what the user has typed, so there has to be a way
/// to be rid of it that does not mean hunting for files.
///
/// # Safety
/// `engine` is a valid pointer from `ds_engine_new`.
#[no_mangle]
pub unsafe extern "C" fn ds_engine_clear_contexts(engine: *mut EngineHandle) -> i32 {
    match engine_ref(engine) {
        Some(e) => {
            e.clear_contexts();
            0
        }
        None => 5, // DS_ERR_CONFIG
    }
}

/// How many conversions the frontend should let pile up before it stops
/// accepting more (config `queue_max_pending`).
///
/// The frontend owns the queue — only it can decide on the UI thread whether to
/// take another sentence — so the bound it enforces has to be readable from
/// here. Returns 0 for a NULL engine, which the caller should read as "no
/// bound" rather than "reject everything".
///
/// # Safety
/// `engine` is a valid pointer from `ds_engine_new`.
#[no_mangle]
pub unsafe extern "C" fn ds_engine_queue_max_pending(engine: *mut EngineHandle) -> u32 {
    match engine_ref(engine) {
        Some(e) => e.config_snapshot().queue_max_pending,
        None => 0,
    }
}

// ---- Session lifecycle -----------------------------------------------------

/// # Safety
/// `engine` is a valid pointer from `ds_engine_new` and outlives the session.
#[no_mangle]
pub unsafe extern "C" fn ds_session_new(engine: *mut EngineHandle) -> *mut Session {
    if engine.is_null() {
        return ptr::null_mut();
    }
    // A session holds its own strong ref to the shared engine, so it stays valid
    // for the session's whole lifetime even though the runtime lives in the handle.
    let session = Box::new(Session::new((*engine).engine_arc()));
    Box::into_raw(session)
}

/// # Safety
/// `session` is a pointer from `ds_session_new`, used at most once here.
#[no_mangle]
pub unsafe extern "C" fn ds_session_free(session: *mut Session) {
    if !session.is_null() {
        drop(Box::from_raw(session));
    }
}

unsafe fn session_ref<'a>(session: *mut Session) -> Option<&'a Session> {
    if session.is_null() {
        None
    } else {
        Some(&*session)
    }
}

/// # Safety
/// `session` is valid; `pinyin_ascii` is a valid NUL-terminated UTF-8 string.
#[no_mangle]
pub unsafe extern "C" fn ds_session_set_input(session: *mut Session, pinyin_ascii: *const c_char) {
    if let (Some(s), Some(p)) = (session_ref(session), cstr(pinyin_ascii)) {
        s.set_input(p);
    }
}

/// Name the input window this session is typing into. The conversation context
/// is filed under this key, so the model keeps seeing the domain, terminology
/// and style of what is already written in that window — and starts clean in a
/// new one. Call it before each conversion: one session outlives any single
/// document, so this cannot be set once at creation.
///
/// An empty key (or never calling this) disables context for that request.
///
/// # Safety
/// `session` is valid; `key_utf8` is a valid NUL-terminated UTF-8 string.
#[no_mangle]
pub unsafe extern "C" fn ds_session_set_context_key(
    session: *mut Session,
    key_utf8: *const c_char,
) {
    if let (Some(s), Some(k)) = (session_ref(session), cstr(key_utf8)) {
        s.set_context_key(k);
    }
}

/// # Safety
/// `session` is a valid pointer from `ds_session_new`.
#[no_mangle]
pub unsafe extern "C" fn ds_session_get_input(session: *mut Session) -> *mut c_char {
    match session_ref(session) {
        Some(s) => to_c_string(s.get_input()),
        None => to_c_string(""),
    }
}

/// Callback type matching `DsConvertCallback` in dsime.h.
pub type DsConvertCallback =
    extern "C" fn(user_data: *mut c_void, request_id: u64, status: i32, text_utf8: *const c_char);

/// Wrapper so a raw `void*` can cross into the async task. The frontend owns the
/// pointed-to data and guarantees it stays valid until the callback fires.
struct UserData(*mut c_void);
unsafe impl Send for UserData {}

/// # Safety
/// `session` is valid; `callback` is a valid function pointer; `user_data`
/// stays valid until the callback is invoked.
#[no_mangle]
pub unsafe extern "C" fn ds_session_convert(
    session: *mut Session,
    callback: DsConvertCallback,
    user_data: *mut c_void,
) -> u64 {
    let Some(s) = session_ref(session) else {
        return 0;
    };
    let ud = UserData(user_data);
    s.convert(move |outcome| {
        let ud = ud; // move into closure; Send via wrapper
        let text = CString::new(outcome.text).unwrap_or_else(|_| CString::new("").unwrap());
        callback(ud.0, outcome.request_id, outcome.status, text.as_ptr());
    })
}

/// Callback type matching `DsStreamCallback` in dsime.h. Fires zero-or-more
/// times with `is_final = 0` (a partial, cumulative pre-edit), then exactly once
/// with `is_final = 1` (the terminal outcome — final text or DS_ERR_*).
pub type DsStreamCallback = extern "C" fn(
    user_data: *mut c_void,
    request_id: u64,
    status: i32,
    is_final: i32,
    text_utf8: *const c_char,
);

/// # Safety
/// `session` is valid; `callback` is a valid function pointer; `user_data`
/// stays valid until the terminal (`is_final = 1`) callback is invoked.
#[no_mangle]
pub unsafe extern "C" fn ds_session_convert_stream(
    session: *mut Session,
    callback: DsStreamCallback,
    user_data: *mut c_void,
) -> u64 {
    let Some(s) = session_ref(session) else {
        return 0;
    };
    let ud_partial = UserData(user_data);
    let ud_final = UserData(user_data);
    s.convert_stream(
        move |request_id, cumulative| {
            let ud = &ud_partial;
            let text = CString::new(cumulative).unwrap_or_else(|_| CString::new("").unwrap());
            callback(
                ud.0,
                request_id,
                0, /* DS_OK */
                0, /* partial */
                text.as_ptr(),
            );
        },
        move |outcome| {
            let ud = ud_final;
            let text = CString::new(outcome.text).unwrap_or_else(|_| CString::new("").unwrap());
            callback(
                ud.0,
                outcome.request_id,
                outcome.status,
                1, /* final */
                text.as_ptr(),
            );
        },
    )
}

/// Move to another already-fetched candidate for the current input and return it
/// (caller frees, never NULL). `direction > 0` goes to the NEXT candidate, `< 0`
/// to the PREVIOUS one. Returns an empty string when there is none in that
/// direction: going up past the primary conversion, or down past the last cached
/// candidate — in the down case the frontend should call `ds_session_regenerate`
/// to fetch a fresh alternative. Synchronous and cheap (cache only, no network).
///
/// # Safety
/// `session` is a valid pointer from `ds_session_new`.
#[no_mangle]
pub unsafe extern "C" fn ds_session_candidate_cached(
    session: *mut Session,
    direction: i32,
) -> *mut c_char {
    match session_ref(session) {
        Some(s) => to_c_string(s.cached_candidate(direction).unwrap_or_default()),
        None => to_c_string(""),
    }
}

/// Ask the provider for a DIFFERENT conversion of the current buffer, avoiding
/// every candidate already shown, and append it so `ds_session_candidate_cached`
/// can revisit it. Same streaming callback contract and supersession semantics as
/// `ds_session_convert_stream`; returns a request id, or 0 if the buffer is
/// empty. Call this when the user asks for another candidate (down) and the cache
/// is exhausted.
///
/// # Safety
/// `session` is valid; `callback` is a valid function pointer; `user_data` stays
/// valid until the terminal (`is_final = 1`) callback is invoked.
#[no_mangle]
pub unsafe extern "C" fn ds_session_regenerate(
    session: *mut Session,
    callback: DsStreamCallback,
    user_data: *mut c_void,
) -> u64 {
    let Some(s) = session_ref(session) else {
        return 0;
    };
    let ud_partial = UserData(user_data);
    let ud_final = UserData(user_data);
    s.regenerate(
        move |request_id, cumulative| {
            let ud = &ud_partial;
            let text = CString::new(cumulative).unwrap_or_else(|_| CString::new("").unwrap());
            callback(
                ud.0,
                request_id,
                0, /* DS_OK */
                0, /* partial */
                text.as_ptr(),
            );
        },
        move |outcome| {
            let ud = ud_final;
            let text = CString::new(outcome.text).unwrap_or_else(|_| CString::new("").unwrap());
            callback(
                ud.0,
                outcome.request_id,
                outcome.status,
                1, /* final */
                text.as_ptr(),
            );
        },
    )
}

/// # Safety
/// `session` is a valid pointer from `ds_session_new`.
#[no_mangle]
pub unsafe extern "C" fn ds_session_cancel(session: *mut Session) {
    if let Some(s) = session_ref(session) {
        s.cancel_inflight();
    }
}

/// # Safety
/// `session` is a valid pointer from `ds_session_new`.
#[no_mangle]
pub unsafe extern "C" fn ds_session_reset(session: *mut Session) {
    if let Some(s) = session_ref(session) {
        s.reset();
    }
}

// ---- Lexicon ----------------------------------------------------------------
//
// Stateless segmentation and candidate lookup over the compiled dictionary.
// Nothing here touches a `DsSession` or the conversion queue: the single-flight
// rule in `engine.rs` exists because a request is in flight, and these calls
// start none. The frontend calls them on every keystroke to draw the candidate
// row, which is why they must stay allocation-light and must never fail hard —
// a missing dictionary degrades to "one opaque segment", the behaviour the IME
// had before this existed.

use std::path::PathBuf;
use std::sync::OnceLock;

use lexicon::{Lexicon, MAX_CANDIDATES};

/// The process-wide dictionary, opened on first use.
static LEXICON: OnceLock<Option<Lexicon>> = OnceLock::new();

/// Where to map the dictionary from. Process-wide, set once, and NOT a
/// thread-local — the first version was, on the reasoning that a frontend which
/// never sets a path should not race one that does. That is not what a
/// thread-local buys; it is a path written on one thread and read on another.
///
/// The dictionary belongs to the process, not to a thread: a TSF frontend sets
/// the path on its STA thread and then asks for candidates wherever the next key
/// event lands, and a test binary asks from every thread at once. So the thread
/// that happens to reach `LEXICON.get_or_init` first reads *its own* copy of the
/// path, finds it empty, falls back to the default location, fails to open it —
/// and pins that failure for the life of the process, because a `OnceLock` is
/// never re-initialised. Every later caller then sees "no dictionary" for a
/// dictionary that is sitting on disk, named on the command line.
///
/// It surfaced as all five ABI tests failing on the Windows runner and passing
/// on Ubuntu, which is the shape of a race rather than of a bug: whichever test
/// thread won the race was the one that decided the answer for all of them.
static LEXICON_PATH: OnceLock<PathBuf> = OnceLock::new();

/// Opaque handle: one buffer's segmentation, owned by the caller.
///
/// Everything the C side can read is held here as a [`CString`], never as a
/// borrowed `&str` or a slice of the mapping. A `*const c_char` return is a
/// promise that there is a NUL after the text, and Rust's `String` does not keep
/// one: handing out `s.as_ptr()` and letting `CStr::from_ptr` run to the next
/// zero byte reads past the end of the allocation into whatever the allocator
/// left there. It usually looks fine, which is what makes it worth stating.
/// The copies are a few dozen short strings per keystroke, and they also make the
/// handle self-contained — the caller can hold one across dictionary changes.
pub struct DsSegResult {
    /// NUL-terminated pinyin per segment; empty for one with no dictionary entry,
    /// in which case it is the raw input slice instead.
    pinyin: Vec<CString>,
    /// Byte offsets of each segment within the string passed to
    /// `ds_lexicon_segment`. Kept because a code is *not* a span: "mei you" is 8
    /// bytes of code for 6 bytes of buffer, and taking a candidate has to consume
    /// the span or nothing is selected at all.
    span: Vec<(u32, u32)>,
    /// NUL-terminated best word per segment; empty when there is none.
    best: Vec<CString>,
    has_word: Vec<bool>,
    /// Candidates per segment, computed on demand. Only the segment the user can
    /// act on is ever asked for, so this stays empty for the rest of a long
    /// buffer.
    cands: RefCell<Vec<Vec<CString>>>,
}

/// The dictionary, mapping it on first use.
fn lexicon() -> Option<&'static Lexicon> {
    LEXICON
        .get_or_init(|| {
            let path = LEXICON_PATH
                .get()
                .cloned()
                .unwrap_or_else(default_lexicon_path);
            match Lexicon::open(&path) {
                Ok(l) => Some(l),
                Err(e) => {
                    // Not fatal and not worth an error dialog: the IME simply has
                    // no candidates, which is how it behaved before this feature
                    // existed. Logged so that a user reporting "no candidates"
                    // has something to correlate.
                    eprintln!("dsime: lexicon unavailable ({}): {e}", path.display());
                    None
                }
            }
        })
        .as_ref()
}

/// Where the lexicon lives when the frontend did not say: beside the config
/// file, which is the one path the core already knows how to find.
fn default_lexicon_path() -> PathBuf {
    config::Config::default_path()
        .parent()
        .map_or_else(|| PathBuf::from("dsime.lex"), |d| d.join("dsime.lex"))
}

/// # Safety
/// `utf8_path` is NULL or a valid NUL-terminated UTF-8 string.
#[no_mangle]
pub unsafe extern "C" fn ds_lexicon_set_path(utf8_path: *const c_char) -> i32 {
    let Some(s) = cstr(utf8_path) else {
        set_last_error("ds_lexicon_set_path: NULL path");
        return DS_ERR_CONFIG;
    };
    // Refuse once mapped. Silently accepting it would produce the worst kind of
    // bug: a frontend that believes it configured the dictionary, and a process
    // that quietly kept using the one it found first.
    if LEXICON.get().is_some() {
        set_last_error("ds_lexicon_set_path: the dictionary is already mapped");
        return DS_ERR_CONFIG;
    }
    let path = PathBuf::from(s);
    // Setting the SAME path again is fine, and has to be: callers that cannot
    // know whether someone else in the process got there first (every thread of
    // a test binary, say) would otherwise have to treat the second call as an
    // error it did nothing wrong about. A *different* path is the real
    // contradiction, and it is refused.
    if let Some(already) = LEXICON_PATH.get() {
        if *already == path {
            return DS_OK;
        }
        set_last_error("ds_lexicon_set_path: a different dictionary is already configured");
        return DS_ERR_CONFIG;
    }
    let _ = LEXICON_PATH.set(path);
    DS_OK
}

#[no_mangle]
pub extern "C" fn ds_lexicon_available() -> i32 {
    i32::from(lexicon().is_some())
}

/// # Safety
/// `pinyin_utf8` is NULL or valid NUL-terminated UTF-8; `out` is a valid pointer
/// to a writable `DsSegResult*`.
#[no_mangle]
pub unsafe extern "C" fn ds_lexicon_segment(
    pinyin_utf8: *const c_char,
    out: *mut *mut DsSegResult,
) -> i32 {
    if out.is_null() {
        set_last_error("ds_lexicon_segment: NULL out");
        return DS_ERR_INTERNAL;
    }
    let input = cstr(pinyin_utf8).unwrap_or("").to_owned();
    // Degrade, never fail: with no dictionary the whole buffer becomes one opaque
    // span, so the caller's "does this segment have candidates?" test simply says
    // no and the UI draws no candidate row.
    let segments = match lexicon() {
        Some(l) => l.segment(&input),
        None => vec![lexicon::Segment {
            start: 0,
            end: input.len(),
            pinyin: input,
            best: None,
        }],
    };
    let mut pinyin = Vec::with_capacity(segments.len());
    let mut best = Vec::with_capacity(segments.len());
    let mut has_word = Vec::with_capacity(segments.len());
    let mut span = Vec::with_capacity(segments.len());
    for s in &segments {
        pinyin.push(nul(s.pinyin.as_str()));
        has_word.push(s.is_selectable());
        best.push(nul(s.best.as_deref().unwrap_or("")));
        span.push((s.start as u32, s.end as u32));
    }
    *out = Box::into_raw(Box::new(DsSegResult {
        pinyin,
        span,
        best,
        has_word,
        cands: RefCell::new(Vec::new()),
    }));
    DS_OK
}

/// A NUL-terminated copy. Words and codes never contain an interior NUL, so the
/// only failure path is a caller handing us something impossible.
fn nul(s: &str) -> CString {
    CString::new(s).unwrap_or_default()
}

/// # Safety
/// `result` came from `ds_lexicon_segment` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn ds_lexicon_free(result: *mut DsSegResult) {
    if !result.is_null() {
        drop(Box::from_raw(result));
    }
}

/// Borrow the segment's pinyin at `index`, or NULL. Out-of-range and negative
/// indices are refused rather than clamped: a caller walking past the end is a
/// bug worth seeing, and a clamped index would hand back a plausible-looking
/// wrong word.
unsafe fn slot(v: &[CString], index: i32) -> Option<&CString> {
    if index < 0 {
        return None;
    }
    v.get(index as usize)
}

/// # Safety
/// `result` came from `ds_lexicon_segment` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn ds_seg_count(result: *const DsSegResult) -> i32 {
    result.as_ref().map_or(0, |r| r.pinyin.len() as i32)
}

/// # Safety
/// `result` came from `ds_lexicon_segment` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn ds_seg_pinyin(result: *const DsSegResult, index: i32) -> *const c_char {
    result
        .as_ref()
        .and_then(|r| slot(&r.pinyin, index))
        .map_or(ptr::null(), |s| s.as_ptr())
}

/// Byte offset of the segment's first letter within the string that was
/// segmented — *not* within the code. See [`ds_seg_pinyin`].
///
/// Returns -1 for an out-of-range index. An empty `end` (a valid index on a
/// zero-length segment, which cannot happen today) is 0.
///
/// # Safety
/// `result` came from `ds_lexicon_segment` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn ds_seg_start(result: *const DsSegResult, index: i32) -> i32 {
    result
        .as_ref()
        .and_then(|r| {
            if index < 0 {
                None
            } else {
                r.span.get(index as usize)
            }
        })
        .map_or(-1, |s| s.0 as i32)
}

/// Byte offset one past the segment's last letter, in the same buffer
/// [`ds_seg_start`] indexes. `end - start` is the segment's length in the
/// caller's own string.
///
/// # Safety
/// `result` came from `ds_lexicon_segment` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn ds_seg_end(result: *const DsSegResult, index: i32) -> i32 {
    result
        .as_ref()
        .and_then(|r| {
            if index < 0 {
                None
            } else {
                r.span.get(index as usize)
            }
        })
        .map_or(-1, |s| s.1 as i32)
}

/// # Safety
/// `result` came from `ds_lexicon_segment` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn ds_seg_has_word(result: *const DsSegResult, index: i32) -> i32 {
    result
        .as_ref()
        .and_then(|r| {
            if index < 0 {
                None
            } else {
                r.has_word.get(index as usize)
            }
        })
        .copied()
        .map_or(0, i32::from)
}

/// # Safety
/// `result` came from `ds_lexicon_segment` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn ds_seg_best(result: *const DsSegResult, index: i32) -> *const c_char {
    result
        .as_ref()
        .and_then(|r| slot(&r.best, index))
        .filter(|s| !s.as_bytes().is_empty())
        .map_or(ptr::null(), |s| s.as_ptr())
}

/// # Safety
/// `result` came from `ds_lexicon_segment` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn ds_seg_cand_count(result: *const DsSegResult, index: i32) -> i32 {
    if index < 0 {
        return 0;
    }
    let Some(r) = result.as_ref() else { return 0 };
    r.cache(index as usize);
    r.cands.borrow().get(index as usize).map_or(0, Vec::len) as i32
}

/// # Safety
/// `result` came from `ds_lexicon_segment` and has not been freed.
#[no_mangle]
pub unsafe extern "C" fn ds_seg_cand(
    result: *const DsSegResult,
    index: i32,
    n: i32,
) -> *const c_char {
    if index < 0 || n < 0 {
        return ptr::null();
    }
    let Some(r) = result.as_ref() else {
        return ptr::null();
    };
    r.cache(index as usize);
    let cands = r.cands.borrow();
    cands
        .get(index as usize)
        .and_then(|v| v.get(n as usize))
        .map_or(ptr::null(), |s| s.as_ptr())
}

/// # Safety
/// `code_utf8` is NULL or valid NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn ds_lexicon_candidates(code_utf8: *const c_char) -> *mut c_char {
    let Some(code) = cstr(code_utf8) else {
        return ptr::null_mut();
    };
    let Some(l) = lexicon() else {
        return ptr::null_mut();
    };
    let words = l.candidates(code);
    if words.is_empty() {
        return ptr::null_mut();
    }
    // NUL-separated and double-NUL-terminated: one allocation, and the C side
    // walks it without needing the count up front. Caller frees.
    let mut blob: Vec<u8> = Vec::new();
    for w in &words {
        blob.extend_from_slice(w.as_bytes());
        blob.push(0);
    }
    to_c_string_list(blob)
}

impl DsSegResult {
    /// Fill in segment `i`'s candidate list on first ask.
    fn cache(&self, i: usize) {
        let mut cands = self.cands.borrow_mut();
        if cands.len() < self.pinyin.len() {
            cands.resize_with(self.pinyin.len(), Vec::new);
        }
        if !cands[i].is_empty() {
            return;
        }
        // A segment with no dictionary entry has no candidates by definition;
        // asking again would only re-run the lookup to get the same nothing.
        cands[i] = match lexicon() {
            Some(l) if self.has_word[i] => l
                .candidates(&self.pinyin[i].to_string_lossy())
                .into_iter()
                .map(|w| nul(&w))
                .collect(),
            _ => Vec::new(),
        };
        debug_assert!(cands[i].len() <= MAX_CANDIDATES);
    }
}

// ---- Utilities -------------------------------------------------------------

/// # Safety
/// `s` was returned by a `ds_*` function documented as "caller frees".
#[no_mangle]
pub unsafe extern "C" fn ds_string_free(s: *mut c_char) {
    if !s.is_null() {
        drop(CString::from_raw(s));
    }
}

#[no_mangle]
pub extern "C" fn ds_last_error() -> *const c_char {
    LAST_ERROR.with(|e| e.borrow().as_ptr())
}

#[no_mangle]
pub extern "C" fn ds_version() -> *const c_char {
    concat!(env!("CARGO_PKG_VERSION"), "\0").as_ptr() as *const c_char
}

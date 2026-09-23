//! C ABI for the DS Pinyin IME core engine. See `core/include/dsime.h` for the
//! authoritative documented interface. This file is the thin, `unsafe` FFI shell
//! over the safe `engine` / `api` / `config` modules.

mod api;
mod config;
mod context;
mod engine;

pub use engine::{Engine, EngineHandle, Session};
use std::cell::RefCell;
use std::ffi::{c_char, c_void, CStr, CString};
use std::ptr;

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

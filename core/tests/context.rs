//! End-to-end tests for the per-window conversation context, across the FFI.
//!
//! The property that matters most here is **prefix stability**. DeepSeek's
//! context cache matches complete prefix units, so a request only hits the cache
//! if its leading messages are byte-identical to the previous request's. Every
//! assertion below that compares two requests' `messages` arrays is guarding
//! that — it is the difference between context costing almost nothing and
//! context costing full price on every sentence.

use std::ffi::{c_char, c_void, CStr, CString};
use std::io::{Read, Write};
use std::net::TcpListener;
use std::sync::mpsc::{sync_channel, SyncSender};
use std::sync::{Arc, Mutex};

// Multiple tokio runtimes in one process are flaky under concurrent test
// scheduling; serialize these like the other FFI integration tests.
static SERIAL: Mutex<()> = Mutex::new(());

use dsime::{
    ds_engine_free, ds_engine_new, ds_engine_set_config_json, ds_session_convert, ds_session_free,
    ds_session_new, ds_session_set_context_key, ds_session_set_input, EngineHandle, Session,
};

fn temp_config(tag: &str) -> std::path::PathBuf {
    let dir = std::env::temp_dir().join(format!("dsime-ctx-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir.join("config.json")
}

/// Serve `bodies.len()` sequential chat-completions requests, each with the next
/// canned JSON body, recording every request's raw bytes so the test can assert
/// on the `messages` that were actually sent.
fn spawn_seq_mock(bodies: Vec<&'static str>) -> (u16, Arc<Mutex<Vec<String>>>) {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    let seen = Arc::new(Mutex::new(Vec::<String>::new()));
    let seen_t = seen.clone();
    std::thread::spawn(move || {
        for body in bodies {
            let Ok((mut stream, _)) = listener.accept() else {
                return;
            };
            let mut raw = Vec::new();
            let mut tmp = [0u8; 2048];
            let mut content_len = None;
            loop {
                let n = stream.read(&mut tmp).unwrap_or(0);
                if n == 0 {
                    break;
                }
                raw.extend_from_slice(&tmp[..n]);
                if content_len.is_none() {
                    if let Ok(text) = std::str::from_utf8(&raw) {
                        if let Some(i) = text.to_ascii_lowercase().find("content-length:") {
                            let rest = &text[i + "content-length:".len()..];
                            let num: String = rest
                                .trim_start()
                                .chars()
                                .take_while(|c| c.is_ascii_digit())
                                .collect();
                            content_len = num.parse::<usize>().ok();
                        }
                    }
                }
                if let (Some(cl), Some(hdr_end)) =
                    (content_len, raw.windows(4).position(|w| w == b"\r\n\r\n"))
                {
                    if raw.len() >= hdr_end + 4 + cl {
                        break;
                    }
                }
            }
            seen_t
                .lock()
                .unwrap()
                .push(String::from_utf8_lossy(&raw).into_owned());
            let resp = format!(
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                body.len(),
                body
            );
            let _ = stream.write_all(resp.as_bytes());
            let _ = stream.flush();
        }
    });
    (port, seen)
}

/// The `messages` array of a captured request, decoded.
fn messages_of(raw: &str) -> Vec<(String, String)> {
    let body = raw
        .split("\r\n\r\n")
        .nth(1)
        .unwrap_or_else(|| panic!("no request body in:\n{raw}"));
    let json: serde_json::Value =
        serde_json::from_str(body).unwrap_or_else(|e| panic!("body is not JSON ({e}):\n{body}"));
    json["messages"]
        .as_array()
        .unwrap_or_else(|| panic!("no messages array in:\n{body}"))
        .iter()
        .map(|m| {
            (
                m["role"].as_str().unwrap_or_default().to_string(),
                m["content"].as_str().unwrap_or_default().to_string(),
            )
        })
        .collect()
}

extern "C" fn capture(user_data: *mut c_void, _req: u64, status: i32, text: *const c_char) {
    let s = if text.is_null() {
        String::new()
    } else {
        unsafe { CStr::from_ptr(text) }
            .to_string_lossy()
            .into_owned()
    };
    let tx = unsafe { &*(user_data as *const SyncSender<(i32, String)>) };
    let _ = tx.send((status, s));
}

/// Convert one input and return what came back, panicking on error.
unsafe fn convert(session: *mut Session, pinyin: &str) -> String {
    let input = CString::new(pinyin).unwrap();
    ds_session_set_input(session, input.as_ptr());
    let (tx, rx) = sync_channel::<(i32, String)>(1);
    assert!(ds_session_convert(session, capture, &tx as *const _ as *mut c_void) > 0);
    let (status, text) = rx.recv_timeout(std::time::Duration::from_secs(10)).unwrap();
    assert_eq!(status, 0, "conversion failed: {text}");
    text
}

unsafe fn open(cfg_path: &std::path::Path, cfg: &str) -> (*mut EngineHandle, *mut Session) {
    let cpath = CString::new(cfg_path.to_string_lossy().as_bytes()).unwrap();
    let engine = ds_engine_new(cpath.as_ptr());
    assert!(!engine.is_null());
    let ccfg = CString::new(cfg).unwrap();
    assert_eq!(ds_engine_set_config_json(engine, ccfg.as_ptr()), 0);
    let session = ds_session_new(engine);
    (engine, session)
}

fn base_config(port: u16, extra: &str) -> String {
    format!(
        r#"{{"base_url":"http://127.0.0.1:{port}","api_key":"sk-test","model":"mock","stream":false{extra}}}"#
    )
}

const TURN_1: &str = r#"{"choices":[{"message":{"role":"assistant","content":"你好世界"}}],
                        "usage":{"prompt_tokens":60,"completion_tokens":8}}"#;
const TURN_2: &str = r#"{"choices":[{"message":{"role":"assistant","content":"我是一个程序员"}}],
                        "usage":{"prompt_tokens":80,"completion_tokens":12}}"#;

/// [`TURN_1`] reporting a prompt large enough to trip a deliberately small
/// window. Compaction is driven off the provider's own count, so the number the
/// mock reports is what decides whether it fires.
const TURN_1_LARGE: &str = r#"{"choices":[{"message":{"role":"assistant","content":"你好世界"}}],
                        "usage":{"prompt_tokens":350,"completion_tokens":8}}"#;

#[test]
fn second_conversion_replays_the_first_as_its_prefix() {
    let _g = SERIAL.lock().unwrap();
    let (port, seen) = spawn_seq_mock(vec![TURN_1, TURN_2]);
    let cfg_path = temp_config("prefix");

    unsafe {
        let (engine, session) = open(&cfg_path, &base_config(port, ""));
        let key = CString::new("code.exe|Chrome_WidgetWin_1").unwrap();
        ds_session_set_context_key(session, key.as_ptr());

        assert_eq!(convert(session, "nihaoshijie"), "你好世界");
        assert_eq!(convert(session, "woshigechengxuyuan"), "我是一个程序员");

        ds_session_free(session);
        ds_engine_free(engine);
    }

    let requests = seen.lock().unwrap();
    assert_eq!(requests.len(), 2);
    let first = messages_of(&requests[0]);
    let second = messages_of(&requests[1]);

    // Request 1 is the bare pair it always was.
    assert_eq!(first.len(), 2);
    assert_eq!(first[0].0, "system");
    assert_eq!(first[1], ("user".to_string(), "nihaoshijie".to_string()));

    // Request 2 repeats it *verbatim* — same order, same bytes — then appends.
    // Any change to the leading messages here would miss the provider's cache.
    assert_eq!(
        &second[..first.len()],
        &first[..],
        "the earlier messages must be replayed unchanged or the prefix cache misses"
    );
    assert_eq!(
        second[first.len()],
        ("assistant".to_string(), "你好世界".to_string())
    );
    assert_eq!(
        second.last().unwrap(),
        &("user".to_string(), "woshigechengxuyuan".to_string())
    );

    drop(requests);
    let _ = std::fs::remove_dir_all(cfg_path.parent().unwrap());
}

#[test]
fn a_different_window_starts_with_a_clean_context() {
    let _g = SERIAL.lock().unwrap();
    let (port, seen) = spawn_seq_mock(vec![TURN_1, TURN_2]);
    let cfg_path = temp_config("windows");

    unsafe {
        let (engine, session) = open(&cfg_path, &base_config(port, ""));

        let a = CString::new("code.exe|Chrome_WidgetWin_1").unwrap();
        ds_session_set_context_key(session, a.as_ptr());
        assert_eq!(convert(session, "nihaoshijie"), "你好世界");

        // Same session, different window: the first window's history must not
        // leak into it.
        let b = CString::new("notepad.exe|Notepad").unwrap();
        ds_session_set_context_key(session, b.as_ptr());
        assert_eq!(convert(session, "woshigechengxuyuan"), "我是一个程序员");

        ds_session_free(session);
        ds_engine_free(engine);
    }

    let requests = seen.lock().unwrap();
    let second = messages_of(&requests[1]);
    assert_eq!(
        second.len(),
        2,
        "a window with no history should send the bare pair, got {second:?}"
    );

    drop(requests);
    let _ = std::fs::remove_dir_all(cfg_path.parent().unwrap());
}

#[test]
fn context_disabled_sends_a_bare_request() {
    let _g = SERIAL.lock().unwrap();
    let (port, seen) = spawn_seq_mock(vec![TURN_1, TURN_2]);
    let cfg_path = temp_config("off");

    unsafe {
        let (engine, session) = open(&cfg_path, &base_config(port, r#","context_enabled":false"#));
        let key = CString::new("code.exe|Chrome_WidgetWin_1").unwrap();
        ds_session_set_context_key(session, key.as_ptr());

        assert_eq!(convert(session, "nihaoshijie"), "你好世界");
        assert_eq!(convert(session, "woshigechengxuyuan"), "我是一个程序员");

        ds_session_free(session);
        ds_engine_free(engine);
    }

    let requests = seen.lock().unwrap();
    assert_eq!(messages_of(&requests[1]).len(), 2);

    drop(requests);
    let _ = std::fs::remove_dir_all(cfg_path.parent().unwrap());
}

#[test]
fn a_new_engine_starts_with_no_context() {
    let _g = SERIAL.lock().unwrap();
    let (port, seen) = spawn_seq_mock(vec![TURN_1, TURN_2]);
    let cfg_path = temp_config("restart");
    let key = CString::new("notepad3.exe|4242").unwrap();

    unsafe {
        // First run: one conversion, so there is a history to lose.
        let (engine, session) = open(&cfg_path, &base_config(port, ""));
        ds_session_set_context_key(session, key.as_ptr());
        assert_eq!(convert(session, "nihaoshijie"), "你好世界");
        ds_session_free(session);
        ds_engine_free(engine);

        // Second run, same config path and same window key. Nothing carries over:
        // the context lives in memory and the engine that held it is gone, which
        // is exactly what not writing it to disk means.
        let (engine, session) = open(&cfg_path, &base_config(port, ""));
        ds_session_set_context_key(session, key.as_ptr());
        assert_eq!(convert(session, "woshigechengxuyuan"), "我是一个程序员");
        ds_session_free(session);
        ds_engine_free(engine);
    }

    let requests = seen.lock().unwrap();
    let second = messages_of(&requests[1]);
    assert_eq!(
        second.len(),
        2,
        "history must not outlive the engine that held it, got {second:?}"
    );

    drop(requests);
    let _ = std::fs::remove_dir_all(cfg_path.parent().unwrap());
}

#[test]
fn empty_context_key_sends_a_bare_request() {
    let _g = SERIAL.lock().unwrap();
    let (port, seen) = spawn_seq_mock(vec![TURN_1, TURN_2]);
    let cfg_path = temp_config("nokey");

    unsafe {
        let (engine, session) = open(&cfg_path, &base_config(port, ""));

        // What the frontend sends when it cannot tell which window it is typing
        // into. The ABI calls that "no context", but before this was enforced the
        // empty key was just another key: every such request filed under "" and
        // shared one history with every other unidentified window.
        let empty = CString::new("").unwrap();
        ds_session_set_context_key(session, empty.as_ptr());

        assert_eq!(convert(session, "nihaoshijie"), "你好世界");
        assert_eq!(convert(session, "woshigechengxuyuan"), "我是一个程序员");

        ds_session_free(session);
        ds_engine_free(engine);
    }

    let requests = seen.lock().unwrap();
    assert_eq!(
        messages_of(&requests[1]).len(),
        2,
        "an unidentified window must not accumulate (or share) a history"
    );

    drop(requests);
    let _ = std::fs::remove_dir_all(cfg_path.parent().unwrap());
}

#[test]
fn compaction_folds_the_history_and_keeps_the_recent_turns() {
    let _g = SERIAL.lock().unwrap();
    // The first response reports a prompt of 350 tokens against a 400-token
    // window with a 0.75 ratio (threshold 300), so the *next* conversion must
    // compact first. That also exercises `usage` parsing: without it the
    // estimate would fall back to the heuristic (a few hundred tokens short) and
    // no compaction would fire.
    let (port, seen) = spawn_seq_mock(vec![
        TURN_1_LARGE,
        r#"{"choices":[{"message":{"role":"assistant","content":"<analysis>scratch pad</analysis><summary>主题：问候</summary>"}}]}"#,
        TURN_2,
    ]);
    let cfg_path = temp_config("compact");
    let extra = r#","context_window_tokens":400,"context_compact_ratio":0.75,"context_keep_recent":1,"context_prompt":"COMPACT-NOW""#;

    unsafe {
        let (engine, session) = open(&cfg_path, &base_config(port, extra));
        let key = CString::new("code.exe|Chrome_WidgetWin_1").unwrap();
        ds_session_set_context_key(session, key.as_ptr());

        assert_eq!(convert(session, "nihaoshijie"), "你好世界");
        assert_eq!(convert(session, "woshigechengxuyuan"), "我是一个程序员");

        ds_session_free(session);
        ds_engine_free(engine);
    }

    let requests = seen.lock().unwrap();
    assert_eq!(
        requests.len(),
        3,
        "expected convert, compaction, convert — got {} requests",
        requests.len()
    );

    // The middle request is the compaction: the history, then the instruction.
    // Sharing the normal requests' prefix is deliberate — it hits the same cache.
    let compact = messages_of(&requests[1]);
    assert!(compact.iter().any(|(_, c)| c == "COMPACT-NOW"));
    assert!(compact.iter().any(|(_, c)| c.contains("nihaoshijie")));

    // The third request carries the note that came back — with the scratchpad
    // stripped, since only the conclusion belongs in the context.
    let third = messages_of(&requests[2]);
    assert!(
        third
            .iter()
            .any(|(r, c)| r == "system" && c.contains("主题：问候")),
        "the compacted note should lead the request, got {third:?}"
    );
    assert!(
        third.iter().all(|(_, c)| !c.contains("scratch pad")),
        "the <analysis> scratchpad must not reach the context: {third:?}"
    );

    // context_keep_recent = 1, so the single existing turn survives verbatim.
    assert_eq!(
        third
            .iter()
            .filter(|(r, c)| r == "user" && c == "nihaoshijie")
            .count(),
        1,
        "the most recent turn should be kept verbatim: {third:?}"
    );

    drop(requests);
    let _ = std::fs::remove_dir_all(cfg_path.parent().unwrap());
}

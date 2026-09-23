//! End-to-end test of the FFI against a mock OpenAI-compatible endpoint.
//!
//! Spins up a one-shot localhost HTTP server that returns a canned
//! chat-completions response, points the engine's `base_url` at it via the JSON
//! config API, then drives a real `ds_session_convert` and asserts the async
//! callback delivers the converted sentence. No network, no API key needed.

use std::ffi::{c_char, c_void, CStr, CString};
use std::io::{Read, Write};
use std::net::TcpListener;
use std::sync::mpsc::{sync_channel, SyncSender};

use dsime::{
    ds_engine_free, ds_engine_new, ds_engine_set_config_json, ds_session_cancel,
    ds_session_convert, ds_session_convert_stream, ds_session_free, ds_session_new,
    ds_session_set_input, EngineHandle, Session,
};

mod common;

use common::{body_of, spawn_seq_mock, spawn_seq_mock_with};

/// A completion that came back with nothing in it, and the provider's own
/// explanation for why. `"length"` is what a reasoning model returns when it
/// spent the whole `max_tokens` budget thinking; the other reasons mean the
/// model simply said nothing.
fn empty_completion(finish_reason: &str) -> String {
    format!(
        r#"{{"choices":[{{"message":{{"role":"assistant","content":""}},"finish_reason":"{finish_reason}"}}]}}"#
    )
}

const CONVERTED: &str =
    r#"{"choices":[{"message":{"role":"assistant","content":"你好世界"},"finish_reason":"stop"}]}"#;

/// One conversion, driven through the FFI, waiting for its terminal callback.
/// `config_extra` is merged into the JSON config the engine is given.
fn run_one_conversion(tag: &str, port: u16, config_extra: &str) -> (i32, String, Vec<String>) {
    let tmp = std::env::temp_dir().join(format!("dsime-{tag}-{}.json", std::process::id()));
    let cpath = CString::new(tmp.to_string_lossy().as_bytes()).unwrap();

    let mut out = (0, String::new(), Vec::new());
    unsafe {
        let engine: *mut EngineHandle = ds_engine_new(cpath.as_ptr());
        assert!(!engine.is_null());

        let cfg = format!(
            r#"{{"base_url":"http://127.0.0.1:{port}","api_key":"sk-test","model":"mock","stream":false{config_extra}}}"#
        );
        let ccfg = CString::new(cfg).unwrap();
        assert_eq!(ds_engine_set_config_json(engine, ccfg.as_ptr()), 0);

        let session: *mut Session = ds_session_new(engine);
        let input = CString::new("nihaoshijie").unwrap();
        ds_session_set_input(session, input.as_ptr());

        let (tx, rx) = sync_channel::<(i32, String)>(4);
        let req = ds_session_convert(session, capture, &tx as *const _ as *mut c_void);
        assert!(req > 0);
        let (status, text) = rx
            .recv_timeout(std::time::Duration::from_secs(10))
            .expect("the terminal callback must always fire");
        out.0 = status;
        out.1 = text;

        ds_session_free(session);
        ds_engine_free(engine);
    }
    let _ = std::fs::remove_file(&tmp);
    out
}

/// `expected` bodies are served; `requests` is returned so the caller can check
/// both how many arrived and what they contained.
fn run_conversions(
    tag: &str,
    bodies: Vec<&'static str>,
    config_extra: &str,
) -> (i32, String, Vec<String>) {
    let (port, seen) = spawn_seq_mock(bodies);
    let (status, text, _) = run_one_conversion(tag, port, config_extra);
    let requests = seen.lock().unwrap().clone();
    (status, text, requests)
}

/// A reasoning model that returns an empty completion still owes the user the
/// sentence: the retry asks for thinking to be switched off, which is the
/// difference between ~10 s of empty reply and 0.6 s of answer (measured).
#[test]
fn an_empty_completion_is_rescued_with_thinking_disabled() {
    let empty = empty_completion("length");
    let bodies = vec![Box::leak(empty.into_boxed_str()) as &'static str, CONVERTED];
    let (status, text, requests) = run_conversions("rescue", bodies, "");

    assert_eq!(status, 0, "the rescue should have produced a conversion");
    assert_eq!(text, "你好世界");
    assert_eq!(requests.len(), 2, "exactly one retry");

    // The first attempt is the configured request: thinking is not mentioned at
    // all, and neither is temperature (a provider ignores it while thinking on).
    let first = body_of(&requests[0]);
    assert!(first.get("thinking").is_none());
    assert!(first.get("temperature").is_none());

    // The rescue has to be a *different* request, or it would fail the same way
    // — and it has to carry the temperature that only means something once
    // thinking is off, not just an overridden thinking field.
    let second = body_of(&requests[1]);
    assert_eq!(second["thinking"]["type"], "disabled");
    assert!(
        second.get("temperature").is_some(),
        "the rescue sends the configured temperature; thinking_off must reach \
         token_params, not just thinking_params"
    );
    assert_eq!(
        second["messages"], first["messages"],
        "the rescue reuses the same context — it is what makes the answer right"
    );
}

/// "The model returned nothing and did not say why" is not the failure the
/// rescue addresses, and guessing would mean sending a `thinking` field to a
/// provider that may not know it.
#[test]
fn a_completion_that_is_empty_for_another_reason_is_not_rescued() {
    let empty = empty_completion("stop");
    let bodies = vec![Box::leak(empty.into_boxed_str()) as &'static str];
    let (status, _, requests) = run_conversions("norescue-stop", bodies, "");

    assert_eq!(
        status, 3,
        "DS_ERR_API: the frontend then writes the raw pinyin"
    );
    assert_eq!(requests.len(), 1);
}

/// A refusal from the provider is not something a second identically-shaped
/// request fixes, and the retry is deliberately narrow enough not to try.
#[test]
fn a_provider_error_is_not_rescued() {
    // The second response is a working conversion, so this is decisive: a retry
    // would have collected it and reported DS_OK.
    let (port, seen) = spawn_seq_mock_with(vec![
        (500, r#"{"error":{"message":"upstream is unwell"}}"#),
        (200, CONVERTED),
    ]);
    let (status, _, _) = run_one_conversion("norescue-http", port, "");

    assert_eq!(status, 3, "DS_ERR_API for an HTTP error");
    assert_eq!(seen.lock().unwrap().len(), 1, "no retry for an HTTP error");
}

/// Retrying when thinking is already off would send the identical request.
#[test]
fn no_rescue_when_thinking_is_already_disabled() {
    let empty = empty_completion("length");
    let bodies = vec![Box::leak(empty.into_boxed_str()) as &'static str];
    let (status, _, requests) =
        run_conversions("norescue-flag", bodies, r#","thinking":"disabled""#);

    assert_eq!(status, 3);
    assert_eq!(requests.len(), 1, "the guard must stop the second request");
}

/// When even the rescue comes back empty the caller still gets one terminal
/// callback and one error — and a message that covers both attempts, since that
/// string is all a frontend has to show.
#[test]
fn a_rescue_that_also_fails_reports_both_attempts() {
    let a = empty_completion("length");
    let b = empty_completion("length");
    let bodies = vec![
        Box::leak(a.into_boxed_str()) as &'static str,
        Box::leak(b.into_boxed_str()) as &'static str,
    ];
    let (port, seen) = spawn_seq_mock(bodies);
    let (status, text, _) = run_one_conversion("rescue-both", port, "");

    assert_eq!(status, 3);
    assert!(
        text.contains("retried with thinking disabled"),
        "the message should say a retry happened: {text}"
    );
    assert_eq!(seen.lock().unwrap().len(), 2);
}

/// Serve exactly one request: read the (ignored) body, reply with `body_json`.
fn spawn_mock(body_json: &'static str) -> u16 {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    std::thread::spawn(move || {
        if let Ok((mut stream, _)) = listener.accept() {
            // Drain what's available; enough to let the client finish sending.
            let mut buf = [0u8; 4096];
            let _ = stream.read(&mut buf);
            let resp = format!(
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                body_json.len(),
                body_json
            );
            let _ = stream.write_all(resp.as_bytes());
            let _ = stream.flush();
        }
    });
    port
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

#[test]
fn convert_round_trips_through_mock_provider() {
    let port = spawn_mock(r#"{"choices":[{"message":{"role":"assistant","content":"你好世界"}}]}"#);

    // Isolated temp config so we never touch the user's real file.
    let tmp = std::env::temp_dir().join(format!("dsime-mock-{}.json", std::process::id()));
    let cpath = CString::new(tmp.to_string_lossy().as_bytes()).unwrap();

    unsafe {
        let engine: *mut EngineHandle = ds_engine_new(cpath.as_ptr());
        assert!(!engine.is_null());

        // Point at the mock and give a dummy key so the engine proceeds.
        let cfg = format!(
            r#"{{"base_url":"http://127.0.0.1:{port}","api_key":"sk-test","model":"mock"}}"#
        );
        let ccfg = CString::new(cfg).unwrap();
        assert_eq!(ds_engine_set_config_json(engine, ccfg.as_ptr()), 0);

        let session: *mut Session = ds_session_new(engine);
        let input = CString::new("nihaoshijie").unwrap();
        ds_session_set_input(session, input.as_ptr());

        let (tx, rx) = sync_channel::<(i32, String)>(1);
        let req = ds_session_convert(session, capture, &tx as *const _ as *mut c_void);
        assert!(req > 0, "non-empty buffer should produce a request id");

        let (status, text) = rx
            .recv_timeout(std::time::Duration::from_secs(10))
            .expect("callback should fire");
        assert_eq!(status, 0, "expected DS_OK, got status {status}: {text}");
        assert_eq!(text, "你好世界");

        ds_session_free(session);
        ds_engine_free(engine);
    }
    let _ = std::fs::remove_file(&tmp);
}

/// A server that accepts the connection but never replies, so the request stays
/// in flight until cancelled. Returns the bound port.
fn spawn_hanging_server() -> u16 {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    std::thread::spawn(move || {
        // Hold the connection open for a while without responding.
        if let Ok((stream, _)) = listener.accept() {
            std::thread::sleep(std::time::Duration::from_secs(5));
            drop(stream);
        }
    });
    port
}

#[test]
fn cancel_delivers_exactly_one_cancelled_callback() {
    // Guarantees the EXACTLY-ONCE contract: an in-flight request that is
    // cancelled still fires its callback (with DS_ERR_CANCELLED == 4) precisely
    // once. Frontends rely on this to release per-request resources.
    let port = spawn_hanging_server();
    let tmp = std::env::temp_dir().join(format!("dsime-cancel-{}.json", std::process::id()));
    let cpath = CString::new(tmp.to_string_lossy().as_bytes()).unwrap();

    unsafe {
        let engine = ds_engine_new(cpath.as_ptr());
        let cfg = format!(
            r#"{{"base_url":"http://127.0.0.1:{port}","api_key":"sk-test","model":"mock","timeout_ms":4000}}"#
        );
        let ccfg = CString::new(cfg).unwrap();
        assert_eq!(ds_engine_set_config_json(engine, ccfg.as_ptr()), 0);

        let session = ds_session_new(engine);
        let input = CString::new("nihao").unwrap();
        ds_session_set_input(session, input.as_ptr());

        let (tx, rx) = sync_channel::<(i32, String)>(4);
        let req = ds_session_convert(session, capture, &tx as *const _ as *mut c_void);
        assert!(req > 0);

        // Give the task a moment to enter its await, then cancel.
        std::thread::sleep(std::time::Duration::from_millis(50));
        ds_session_cancel(session);

        let (status, _) = rx
            .recv_timeout(std::time::Duration::from_secs(3))
            .expect("cancelled request must still fire its callback");
        assert_eq!(status, 4, "expected DS_ERR_CANCELLED");

        // And it must fire EXACTLY once — no second delivery.
        assert!(
            rx.recv_timeout(std::time::Duration::from_millis(300))
                .is_err(),
            "callback must fire exactly once"
        );

        ds_session_free(session);
        ds_engine_free(engine);
    }
    let _ = std::fs::remove_file(&tmp);
}

/// Serve one SSE chat-completions stream: a `data:` frame per delta, then
/// `data: [DONE]`, then close the connection.
fn spawn_mock_sse(deltas: &'static [&'static str]) -> u16 {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    std::thread::spawn(move || {
        if let Ok((mut stream, _)) = listener.accept() {
            let mut buf = [0u8; 4096];
            let _ = stream.read(&mut buf);
            let _ = stream.write_all(
                b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n",
            );
            for d in deltas {
                let frame =
                    format!("data: {{\"choices\":[{{\"delta\":{{\"content\":\"{d}\"}}}}]}}\n\n");
                let _ = stream.write_all(frame.as_bytes());
                let _ = stream.flush();
                std::thread::sleep(std::time::Duration::from_millis(15));
            }
            let _ = stream.write_all(b"data: [DONE]\n\n");
            let _ = stream.flush();
        }
    });
    port
}

extern "C" fn capture_stream(
    user_data: *mut c_void,
    _req: u64,
    status: i32,
    is_final: i32,
    text: *const c_char,
) {
    let s = if text.is_null() {
        String::new()
    } else {
        unsafe { CStr::from_ptr(text) }
            .to_string_lossy()
            .into_owned()
    };
    let tx = unsafe { &*(user_data as *const SyncSender<(i32, i32, String)>) };
    let _ = tx.send((status, is_final, s));
}

#[test]
fn convert_stream_delivers_partials_then_one_final() {
    let port = spawn_mock_sse(&["你", "好", "世界"]);
    let tmp = std::env::temp_dir().join(format!("dsime-stream-{}.json", std::process::id()));
    let cpath = CString::new(tmp.to_string_lossy().as_bytes()).unwrap();

    unsafe {
        let engine = ds_engine_new(cpath.as_ptr());
        assert!(!engine.is_null());
        let cfg = format!(
            r#"{{"base_url":"http://127.0.0.1:{port}","api_key":"sk-test","model":"mock","stream":true}}"#
        );
        let ccfg = CString::new(cfg).unwrap();
        assert_eq!(ds_engine_set_config_json(engine, ccfg.as_ptr()), 0);

        let session = ds_session_new(engine);
        let input = CString::new("nihaoshijie").unwrap();
        ds_session_set_input(session, input.as_ptr());

        let (tx, rx) = sync_channel::<(i32, i32, String)>(16);
        let req =
            ds_session_convert_stream(session, capture_stream, &tx as *const _ as *mut c_void);
        assert!(req > 0);

        let mut partials: Vec<String> = Vec::new();
        let final_text = loop {
            let (status, is_final, text) = rx
                .recv_timeout(std::time::Duration::from_secs(10))
                .expect("a stream event should arrive");
            assert_eq!(status, 0, "expected DS_OK, got {status}: {text}");
            if is_final == 1 {
                break text;
            }
            partials.push(text);
        };

        assert_eq!(final_text, "你好世界", "final must be the full sentence");
        assert!(!partials.is_empty(), "at least one partial must arrive");
        assert_eq!(
            partials.last().map(String::as_str),
            Some("你好世界"),
            "partials are cumulative; the last equals the full text"
        );
        // The terminal (is_final=1) callback must fire EXACTLY once.
        assert!(
            rx.recv_timeout(std::time::Duration::from_millis(300))
                .is_err(),
            "no events after the terminal callback"
        );

        ds_session_free(session);
        ds_engine_free(engine);
    }
    let _ = std::fs::remove_file(&tmp);
}

#[test]
fn empty_buffer_returns_zero_request_id() {
    let tmp = std::env::temp_dir().join(format!("dsime-empty-{}.json", std::process::id()));
    let cpath = CString::new(tmp.to_string_lossy().as_bytes()).unwrap();
    unsafe {
        let engine = ds_engine_new(cpath.as_ptr());
        let session = ds_session_new(engine);
        // No set_input → empty buffer → convert must be a no-op returning 0.
        let req = ds_session_convert(session, capture, std::ptr::null_mut());
        assert_eq!(req, 0);
        ds_session_free(session);
        ds_engine_free(engine);
    }
    let _ = std::fs::remove_file(&tmp);
}

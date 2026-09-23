//! Helpers shared by the FFI integration tests.
//!
//! Each test binary links this module separately, so anything a given test file
//! does not use looks dead to the compiler there.
#![allow(dead_code)]

use std::io::{Read, Write};
use std::net::TcpListener;
use std::sync::{Arc, Mutex};

/// Serve `bodies.len()` sequential chat-completions requests, each with the next
/// canned JSON body, recording every request's raw bytes so the test can assert
/// what was actually sent.
///
/// Requests beyond the last body are not served: the mock stops accepting, the
/// connection is refused, and a test that expected no further request sees the
/// count in the returned vector stay put.
pub fn spawn_seq_mock(bodies: Vec<&'static str>) -> (u16, Arc<Mutex<Vec<String>>>) {
    spawn_seq_mock_with(bodies.into_iter().map(|b| (200u16, b)).collect())
}

/// As [`spawn_seq_mock`], but each response carries its own status code — for
/// tests that need the provider to refuse rather than answer.
pub fn spawn_seq_mock_with(responses: Vec<(u16, &'static str)>) -> (u16, Arc<Mutex<Vec<String>>>) {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    let seen = Arc::new(Mutex::new(Vec::<String>::new()));
    let seen_t = seen.clone();
    std::thread::spawn(move || {
        for (code, body) in responses {
            let Ok((mut stream, _)) = listener.accept() else {
                return;
            };
            // Read headers, then the Content-Length body, so the whole request
            // is captured and not just whatever arrived in the first packet.
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
            let reason = if code == 200 { "OK" } else { "Status" };
            let resp = format!(
                "HTTP/1.1 {code} {reason}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                body.len(),
                body
            );
            let _ = stream.write_all(resp.as_bytes());
            let _ = stream.flush();
        }
    });
    (port, seen)
}

/// The JSON body of a captured request.
pub fn body_of(raw: &str) -> serde_json::Value {
    let body = raw
        .split("\r\n\r\n")
        .nth(1)
        .unwrap_or_else(|| panic!("no request body in:\n{raw}"));
    serde_json::from_str(body).unwrap_or_else(|e| panic!("body is not JSON ({e}):\n{body}"))
}

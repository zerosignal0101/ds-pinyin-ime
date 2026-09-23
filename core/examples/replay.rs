//! `dsime-replay` — replay a paragraph, one Space-separated sentence per line,
//! through the exact FFI the TSF frontend drives, printing each sentence's
//! status and wall time.
//!
//! Where `cli` answers "does a conversion work at all", this answers "does *this
//! paragraph* convert" — which is a different question, because the interesting
//! failures are per-sentence, depend on the conversation context, and are
//! intermittent. It is how the empty-completion failure was pinned down:
//! `manzuyixiashiyuxingshideguangyishiliangManakovfangcheng` failed 3 times out of
//! 3 with no context and 1 in 3 with it, while every neighbouring sentence was
//! fine. Run the same input a few times; a single pass proves nothing.
//!
//!   cargo run --release --example replay -- notepad3.exe|4242 < paragraph.txt
//!
//! One line of stdin = one job, sent through `ds_session_convert` (the
//! non-streaming path every frontend uses, so this exercises the rescue retry).
//! All jobs share one context key and run in order, exactly as the queue does.
//!
//! Env:
//!   DSIME_CONFIG  config.json to use (default: the real per-user one, which is
//!                 also how a stock-value migration gets exercised end to end)
//!   NO_CTX=1      send an empty context key, i.e. convert each line with no
//!                 history — the A/B for whether the context is what is helping

use dsime::{
    ds_engine_new, ds_session_convert, ds_session_free, ds_session_new, ds_session_set_context_key,
    ds_session_set_input, EngineHandle, Session,
};
use std::ffi::{c_char, c_void, CStr, CString};
use std::io::Read;
use std::sync::mpsc::sync_channel;
use std::time::{Duration, Instant};

struct Done {
    tx: std::sync::mpsc::SyncSender<(i32, String)>,
}

extern "C" fn on_done(user_data: *mut c_void, _id: u64, status: i32, text_utf8: *const c_char) {
    let text = if text_utf8.is_null() {
        String::new()
    } else {
        unsafe { CStr::from_ptr(text_utf8) }
            .to_string_lossy()
            .into_owned()
    };
    let done = unsafe { &*(user_data as *const Done) };
    let _ = done.tx.send((status, text));
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let key = args
        .first()
        .cloned()
        .unwrap_or_else(|| "notepad3.exe|4242".into());
    let key = if std::env::var("NO_CTX").is_ok() {
        String::new()
    } else {
        key
    };

    let mut raw = String::new();
    std::io::stdin().read_to_string(&mut raw).unwrap();
    let lines: Vec<String> = raw
        .lines()
        .map(|l| l.trim().to_string())
        .filter(|l| !l.is_empty())
        .collect();
    if lines.is_empty() {
        eprintln!("no input");
        std::process::exit(2);
    }

    let cfg_path = std::env::var("DSIME_CONFIG").unwrap_or_else(|_| {
        let appdata = std::env::var("APPDATA").unwrap();
        format!("{appdata}\\DSInput\\DSInput\\config\\config.json")
    });

    unsafe {
        let cpath = CString::new(cfg_path.clone()).unwrap();
        let engine: *mut EngineHandle = ds_engine_new(cpath.as_ptr());
        assert!(!engine.is_null(), "engine creation failed at {cfg_path}");
        let session: *mut Session = ds_session_new(engine);
        let ckey = CString::new(key.clone()).unwrap();
        ds_session_set_context_key(session, ckey.as_ptr());
        println!("config: {cfg_path}\ncontext key: {key:?}\n");

        for (i, line) in lines.iter().enumerate() {
            let cin = CString::new(line.as_str()).unwrap();
            ds_session_set_input(session, cin.as_ptr());

            let (tx, rx) = sync_channel::<(i32, String)>(1);
            let done = Box::new(Done { tx });
            let done_ptr = Box::into_raw(done);

            let t0 = Instant::now();
            let id = ds_session_convert(session, on_done, done_ptr as *mut c_void);
            if id == 0 {
                println!("[{i}] id=0 (empty buffer?)");
                drop(Box::from_raw(done_ptr));
                continue;
            }
            match rx.recv_timeout(Duration::from_secs(180)) {
                Ok((status, text)) => {
                    println!(
                        "[{i}] {:>6}ms status={} len={} out={:?}",
                        t0.elapsed().as_millis(),
                        status,
                        text.chars().count(),
                        if status == 0 {
                            text
                        } else {
                            String::from("<error>")
                        }
                    );
                }
                Err(_) => println!("[{i}] TIMED OUT (180s)"),
            }
            drop(Box::from_raw(done_ptr));
        }
        ds_session_free(session);
        dsime::ds_engine_free(engine);
    }
}

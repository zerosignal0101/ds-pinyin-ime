//! C ABI tests for the lexicon, against a real compiled dictionary.
//!
//! Skipped unless `DSILEX_TEST_DICT` names a `.lex`, so CI (which has no
//! dictionary) still passes.
//!
//! This is a separate test *binary* from the degradation tests on purpose: the
//! mapped dictionary is process-global behind a one-time initialiser, so a binary
//! that ever opened a missing one could never open a real one afterwards.

use std::ffi::{c_char, CStr, CString};
use std::sync::OnceLock;

use dsime::*;

/// The header's `DS_OK`, spelled out so the test fails loudly if the ABI renames it.
const DS_OK: i32 = 0;

/// Point the lexicon at the dictionary once per process. Returns false when there is
/// no dictionary, in which case the caller must return immediately: Rust has no
/// skip mechanism, so "skip" is spelled as an early return.
///
/// The set_path is once-per-process by design (it refuses once the dictionary is
/// mapped), so the three tests here cannot each make it -- whichever ran second
/// would fail for the wrong reason. The OnceLock makes the ordering irrelevant, and
/// the test that cares about the refusal forces the mapping for itself.
fn open_or_skip() -> bool {
    static ONCE: OnceLock<()> = OnceLock::new();
    ONCE.get_or_init(|| {
        if let Ok(path) = std::env::var("DSILEX_TEST_DICT") {
            let c = CString::new(path).unwrap();
            assert_eq!(
                unsafe { ds_lexicon_set_path(c.as_ptr()) },
                DS_OK,
                "first set_path is accepted"
            );
        }
    });
    if std::env::var("DSILEX_TEST_DICT").is_err() {
        return false;
    }
    assert_eq!(
        ds_lexicon_available(),
        1,
        "DSILEX_TEST_DICT is set but the dictionary did not load"
    );
    true
}

fn cstr(p: *const c_char) -> String {
    assert!(!p.is_null(), "expected a string, got NULL");
    unsafe { CStr::from_ptr(p) }.to_str().unwrap().to_owned()
}

/// Consume the NUL-separated, double-NUL-terminated list `ds_lexicon_candidates`
/// returns.
///
/// # Safety
/// `blob` must be such a list, as returned above.
unsafe fn split_list(blob: *mut c_char) -> Vec<String> {
    let mut out = Vec::new();
    let mut p = blob;
    loop {
        let s = CStr::from_ptr(p);
        if s.to_bytes().is_empty() {
            break;
        }
        out.push(s.to_str().expect("each entry is utf-8").to_owned());
        p = p.add(s.to_bytes().len() + 1);
    }
    out
}

#[test]
fn abi_round_trips_a_segmentation() {
    if !open_or_skip() {
        return;
    }

    let input = std::ffi::CString::new("nihaoshijie").unwrap();
    let mut res: *mut DsSegResult = std::ptr::null_mut();
    assert_eq!(unsafe { ds_lexicon_segment(input.as_ptr(), &mut res) }, 0);
    assert!(!res.is_null());

    unsafe {
        let n = ds_seg_count(res);
        assert_eq!(n, 2, "nihao|shijie");
        assert_eq!(ds_seg_has_word(res, 0), 1);
        assert_eq!(cstr(ds_seg_best(res, 0)), "你好");
        assert_eq!(cstr(ds_seg_best(res, 1)), "世界");
        assert_eq!(cstr(ds_seg_pinyin(res, 0)), "ni hao");
        assert_eq!(cstr(ds_seg_pinyin(res, 1)), "shi jie");

        // Candidate 0 is what the UI labels `2`; there is no label 1.
        assert!(ds_seg_cand_count(res, 0) >= 1);
        assert_eq!(cstr(ds_seg_cand(res, 0, 0)), "你好");
        // Out of range is NULL, not a clamp onto some other segment's word.
        assert!(ds_seg_cand(res, 0, 99).is_null());
        assert!(ds_seg_best(res, 99).is_null());
        assert!(ds_seg_best(res, -1).is_null());
        assert_eq!(ds_seg_count(res), 2);

        ds_lexicon_free(res);
    }
}

/// The span is a byte range in the *input*, and is not the code. A frontend that
/// sizes the deletion off `ds_seg_pinyin` refuses every multi-syllable word,
/// because "mei you" is longer than the "meiyou" the user typed.
#[test]
fn abi_reports_the_span_in_the_input() {
    if !open_or_skip() {
        return;
    }
    let input = std::ffi::CString::new("meiyou").unwrap();
    let mut res: *mut DsSegResult = std::ptr::null_mut();
    assert_eq!(unsafe { ds_lexicon_segment(input.as_ptr(), &mut res) }, 0);
    unsafe {
        assert_eq!(ds_seg_count(res), 1);
        assert_eq!(ds_seg_start(res, 0), 0);
        assert_eq!(ds_seg_end(res, 0), 6, "meiyou is 6 letters");
        assert_ne!(
            cstr(ds_seg_pinyin(res, 0)).len() as i32,
            ds_seg_end(res, 0) - ds_seg_start(res, 0),
            "if these agree, this test is vacuous"
        );
        // Out of range is -1, so a caller can tell "no such segment" from segment 0.
        assert_eq!(ds_seg_start(res, 9), -1);
        assert_eq!(ds_seg_end(res, 9), -1);
        assert_eq!(ds_seg_start(res, -1), -1);
        ds_lexicon_free(res);
    }
}

/// A segmentation that covers the whole buffer in several parts has to report
/// spans that tile it, with no gap and no overlap — that is what lets the frontend
/// erase one word from the front and re-segment what is left.
#[test]
fn abi_spans_tile_the_input() {
    if !open_or_skip() {
        return;
    }
    for input in ["nihaoshijie", "woshizhongguorenmin", "mingtianjianmianba"] {
        let cin = std::ffi::CString::new(input).unwrap();
        let mut res: *mut DsSegResult = std::ptr::null_mut();
        assert_eq!(unsafe { ds_lexicon_segment(cin.as_ptr(), &mut res) }, 0);
        let n = unsafe { ds_seg_count(res) };
        assert!(n > 0);
        let mut next = 0;
        for i in 0..n {
            let (s, e) = unsafe { (ds_seg_start(res, i), ds_seg_end(res, i)) };
            assert_eq!(s, next, "{input}: segment {i} does not continue the tiling");
            assert!(e > s, "{input}: segment {i} is empty");
            next = e;
        }
        assert_eq!(
            next as usize,
            input.len(),
            "{input}: spans do not cover the input"
        );
        unsafe { ds_lexicon_free(res) };
    }
}

#[test]
fn abi_reports_single_syllable_candidates() {
    if !open_or_skip() {
        return;
    }
    // base and ext carry no single-character entries, so this list exists only
    // because 8105 was compiled in.
    let code = std::ffi::CString::new("shi").unwrap();
    let blob = unsafe { ds_lexicon_candidates(code.as_ptr()) };
    assert!(!blob.is_null(), "candidates(shi) should exist");
    unsafe {
        // NUL-separated, double-NUL-terminated. It has to be walked by pointer:
        // CStr::from_ptr stops at the first separator, so reading the blob as one
        // string would show exactly one candidate and look like a short list.
        let list = split_list(blob);
        assert!(list.contains(&"是".to_owned()), "{list:?}");
        assert!(list.contains(&"时".to_owned()), "{list:?}");
        assert!(list.len() <= 8, "{list:?}");
        assert_eq!(
            list.first().map(String::as_str),
            Some("是"),
            "heaviest first"
        );
        ds_string_free(blob);
    }
    let unknown = std::ffi::CString::new("qqqqqqq").unwrap();
    assert!(unsafe { ds_lexicon_candidates(unknown.as_ptr()) }.is_null());
}

/// The dictionary is process-wide, so the path has to be too.
///
/// Regression: `LEXICON_PATH` used to be a `thread_local!`, which made this a
/// path *written on one thread and read on another*. Whichever of the threads
/// below reached the mapping first read its own (empty) copy, fell back to the
/// default location, failed, and pinned that failure for the whole process — so
/// every test in the binary failed, and only on the runner that happened to
/// schedule them the other way round. The bug is invisible single-threaded,
/// which is why it shipped.
#[test]
fn abi_survives_being_configured_from_another_thread() {
    if std::env::var("DSILEX_TEST_DICT").is_err() {
        return;
    }
    let path = std::ffi::CString::new(std::env::var("DSILEX_TEST_DICT").unwrap()).unwrap();

    // Race several threads to be the one that reads the dictionary. The winner is
    // arbitrary; the answer must not be.
    let handles: Vec<_> = (0..8)
        .map(|_| {
            let p = path.clone();
            std::thread::spawn(move || {
                let rc = unsafe { ds_lexicon_set_path(p.as_ptr()) };
                // Both outcomes are correct and neither is this test's business:
                // accepted if nothing has mapped a dictionary yet, refused because
                // one already has. What must never happen is a refusal that leaves
                // the process without a dictionary it was told about.
                if rc != DS_OK {
                    assert_eq!(
                        ds_lexicon_available(),
                        1,
                        "a refused set_path must mean the dictionary is already mapped"
                    );
                }
                ds_lexicon_available()
            })
        })
        .collect();

    for h in handles {
        assert_eq!(
            h.join().expect("thread panicked"),
            1,
            "a dictionary named on the command line must map whatever thread asks first"
        );
    }
}

#[test]
fn abi_rejects_a_second_set_path() {
    if !open_or_skip() {
        return;
    }
    // Force the mapping again here, so "already mapped" is true because of *this*
    // test's own actions rather than because some other test happened to run a
    // query first.
    assert_eq!(ds_lexicon_available(), 1);
    // Already mapped: a second path must be refused rather than silently appear
    // to take effect.
    let p = std::ffi::CString::new("elsewhere.lex").unwrap();
    assert_ne!(unsafe { ds_lexicon_set_path(p.as_ptr()) }, 0);
    assert!(!ds_last_error().is_null(), "a refusal should say why");
}

//! What the IME does with no dictionary.
//!
//! The contract the frontend relies on is that a missing, unreadable or
//! wrong-version lexicon is **not** an error: `ds_lexicon_available()` reports 0
//! and `ds_lexicon_segment()` returns one opaque span, so the caller draws no
//! candidate row and everything else behaves exactly as it did before the feature
//! existed. That means the frontend needs no fallback path of its own, and it is
//! worth a test that says so.
//!
//! Its own test binary: the mapped dictionary is process-global behind a one-time
//! initialiser, so this file can never be joined with a file that opens a real
//! one.

use std::ffi::{c_char, CString};

use dsime::*;

fn cstr(p: *const c_char) -> String {
    unsafe { std::ffi::CStr::from_ptr(p) }
        .to_str()
        .unwrap()
        .to_owned()
}

#[test]
fn missing_dictionary_degrades_instead_of_failing() {
    let missing = CString::new("this-file-does-not-exist.lex").unwrap();
    assert_eq!(
        unsafe { ds_lexicon_set_path(missing.as_ptr()) },
        0,
        "first set_path must be accepted"
    );
    assert_eq!(ds_lexicon_available(), 0, "and must report no dictionary");

    let input = CString::new("nihaoshijie").unwrap();
    let mut res: *mut DsSegResult = std::ptr::null_mut();
    assert_eq!(
        unsafe { ds_lexicon_segment(input.as_ptr(), &mut res) },
        0,
        "segmenting must still succeed"
    );
    assert!(!res.is_null());

    unsafe {
        // One opaque span covering the whole buffer: selectable nowhere, with no
        // candidate to draw.
        assert_eq!(ds_seg_count(res), 1);
        assert_eq!(ds_seg_has_word(res, 0), 0);
        assert!(ds_seg_best(res, 0).is_null());
        assert_eq!(ds_seg_cand_count(res, 0), 0);
        assert!(ds_seg_cand(res, 0, 0).is_null());
        // The span is still reported, so a frontend that wants to show where the
        // pinyin runs still can.
        assert_eq!(cstr(ds_seg_pinyin(res, 0)), "nihaoshijie");
        ds_lexicon_free(res);
    }

    // And the standalone candidate query simply has nothing to say.
    let code = CString::new("shi jie").unwrap();
    assert!(unsafe { ds_lexicon_candidates(code.as_ptr()) }.is_null());
}

#[test]
fn empty_and_bogus_inputs_are_handled() {
    // Nothing here may panic, however odd the input: this is on the keystroke path
    // and a panic would take the host's process with it.
    for input in [
        "",
        "nihao",
        "12345",
        "!!!",
        "AI",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    ] {
        let c = CString::new(input).unwrap();
        let mut res: *mut DsSegResult = std::ptr::null_mut();
        assert_eq!(
            unsafe { ds_lexicon_segment(c.as_ptr(), &mut res) },
            0,
            "{input:?}"
        );
        unsafe {
            assert!(!res.is_null(), "{input:?}");
            let n = ds_seg_count(res);
            for i in 0..n.max(0) {
                let _ = ds_seg_pinyin(res, i);
                let _ = ds_seg_has_word(res, i);
                let _ = ds_seg_cand_count(res, i);
                let _ = ds_seg_cand(res, i, 0);
            }
            ds_lexicon_free(res);
        }
    }

    // A NULL out-pointer is refused, not dereferenced.
    assert_ne!(
        unsafe { ds_lexicon_segment(std::ptr::null(), std::ptr::null_mut()) },
        0
    );
    // Freeing NULL is a no-op, as free(0) is.
    unsafe { ds_lexicon_free(std::ptr::null_mut()) };
}

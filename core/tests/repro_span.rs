use dsime::lexicon::Lexicon;

/// Reproduction: the candidate row was drawn correctly, but pressing the digit
/// did nothing and the digit landed in the buffer instead.
///
/// Cause: the frontend took the *code* (`ds_seg_pinyin`, space-joined: "mei
/// you", 8 bytes) to be the segment's length in the buffer, and compared the
/// buffer's prefix against it. The buffer holds "meiyou" (6 bytes), so
/// `6 < 8` and every selection was refused — for every multi-syllable word, which
/// is the common case. A one-syllable word happened to work only because its
/// code contains no space and so is byte-identical to the input.
#[test]
fn segment_span_differs_from_code() {
    let Ok(path) = std::env::var("DSILEX_TEST_DICT") else {
        return;
    };
    let lex = Lexicon::open(std::path::Path::new(&path)).unwrap();

    for (input, expect_slices, expect_codes) in [
        // The reported case.
        ("meiyou", &["meiyou"][..], &["mei you"][..]),
        // A two-syllable word, and the common `nihaoshijie`.
        ("nihao", &["nihao"][..], &["ni hao"][..]),
        ("shijie", &["shijie"][..], &["shi jie"][..]),
    ] {
        let segs = lex.segment(input);
        let slices: Vec<&str> = segs.iter().map(|s| &input[s.start..s.end]).collect();
        let codes: Vec<&str> = segs.iter().map(|s| s.pinyin.as_str()).collect();
        assert_eq!(slices, expect_slices, "byte span of {input:?}");
        assert_eq!(codes, expect_codes, "dictionary code of {input:?}");
        // The whole point: the two are different, so the span — not the code — is
        // what a selection has to consume.
        assert_ne!(
            slices, codes,
            "{input:?}: span and code agree, test is vacuous"
        );
    }
}

/// And the single-syllable case that masked the bug: here they coincide.
#[test]
fn single_syllable_span_equals_code() {
    let Ok(path) = std::env::var("DSILEX_TEST_DICT") else {
        return;
    };
    let lex = Lexicon::open(std::path::Path::new(&path)).unwrap();
    let segs = lex.segment("shi");
    assert_eq!(segs.len(), 1);
    assert_eq!(&"shi"[segs[0].start..segs[0].end], segs[0].pinyin);
}

#[test]
fn span_and_apostrophe_span() {
    let Ok(path) = std::env::var("DSILEX_TEST_DICT") else {
        return;
    };
    let lex = Lexicon::open(std::path::Path::new(&path)).unwrap();
    // "xi'an" is read as two syllables, so the code is "xi an" (西安) while the
    // span keeps the apostrophe. Selecting the word has to consume the span, or an
    // apostrophe is left stranded in the buffer with nothing in front of it.
    let segs = lex.segment("xi'an");
    assert_eq!(segs.len(), 1);
    assert_eq!(segs[0].pinyin, "xi an");
    assert_eq!(&"xi'an"[segs[0].start..segs[0].end], "xi'an");
}

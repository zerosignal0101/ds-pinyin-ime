//! Lexicon tests: the segmenter, the compiled-binary round trip, and the
//! degradation path.
//!
//! The fixture is embedded rather than read from rime-frost so that CI — which
//! has no dictionary — still gates the algorithm. Its weights are the real ones
//! from the shipped dictionaries, because the scoring constants are calibrated
//! against exactly these numbers (see `lexicon`'s module docs): changing a
//! fixture weight is a scoring change, not a cosmetic edit.
//!
//! `DSILEX_TEST_DICT` points the last group of tests at a compiled `.lex`; they
//! skip when it is unset, so `cargo test --all` passes with no dictionary.

use std::path::{Path, PathBuf};

use dsime::lexicon::build::{self, Entry};
use dsime::lexicon::{LexError, Lexicon, MAX_CANDIDATES};

const FIXTURE: &str = "\
# Rime dictionary
# encoding: utf-8
name: fixture
...
是\tshi\t1799848
时\tshi\t287440
事\tshi\t35940
世\tshi\t959
先\txian\t75337
现\txian\t4526
线\txian\t24039
你\tni\t492791
我\two\t1234
们\tmen\t5678
一\tyi\t999
个\tge\t888
中\tzhong\t497871
国\tguo\t7173
好\thao\t124546
# duplicate rows: the highest weight must win
你好\tni hao\t5328
你好\tni hao\t1
西安\txi an\t6091
世界\tshi jie\t68347
使用\tshi yong\t900
中国\tzhong guo\t139769
程序员\tcheng xu yuan\t14589
界\tjie\t7232
程\tcheng\t4321
序\txu\t1111
员\tyuan\t2222
";

/// Write `entries` to a private temp file and memory-map it.
fn lex_from(entries: &[Entry]) -> (Lexicon, PathBuf) {
    let blob = build::compile(entries);
    let path = temp_path("lex");
    std::fs::write(&path, &blob).expect("write fixture binary");
    let lex = Lexicon::open(&path).expect("open fixture binary");
    (lex, path)
}

fn fixture() -> (Lexicon, PathBuf) {
    let parsed = build::parse_dict("fixture", FIXTURE).expect("parse fixture");
    let prepared = build::prepare(parsed);
    lex_from(&prepared)
}

/// A path nobody else is using. Tests run in parallel and the pid alone is not
/// enough — two of them would race on one name, and Windows refuses to rewrite a
/// file that is still mapped (error 1224), so the collision shows up as a
/// confusing write error rather than a name clash. The file is intentionally not
/// removed: the mapping stays valid for as long as the `Lexicon` does, and
/// Windows will not let us delete an open mapping anyway.
fn temp_path(tag: &str) -> PathBuf {
    static SEQ: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
    let n = SEQ.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    let mut p = std::env::temp_dir();
    p.push(format!("dsime-test-{}-{n}-{tag}.lex", std::process::id()));
    p
}

/// `["ni hao", "shi jie"]`-style rendering of a segmentation, so a failure
/// names the words rather than printing a struct.
fn shapes(lex: &Lexicon, input: &str) -> Vec<String> {
    lex.segment(input)
        .iter()
        .map(|s| s.pinyin.clone())
        .collect()
}

fn bests(lex: &Lexicon, input: &str) -> Vec<Option<String>> {
    lex.segment(input).into_iter().map(|s| s.best).collect()
}

// ---- parsing ----------------------------------------------------------------

#[test]
fn parses_rime_front_matter_and_comments() {
    let entries = build::parse_dict("fixture", FIXTURE).expect("parse");
    // The front matter's own `name:` line must not become an entry.
    assert!(
        !entries
            .iter()
            .any(|e| e.word == "name" || e.word == "encoding"),
        "front matter leaked into entries: {entries:?}"
    );
    assert!(entries
        .iter()
        .any(|e| e.code == "ni hao" && e.word == "你好"));
}

#[test]
fn accepts_crlf_and_trailing_tab_comments() {
    let text = "世界\tshi jie\t68347\t# rime-moran\r\n你好\tni hao\t5328\r\n";
    let entries = build::parse_dict("t", text).expect("parse");
    assert_eq!(entries.len(), 2);
    assert_eq!(entries[0].word, "世界");
    assert_eq!(entries[0].weight, 68347);
}

#[test]
fn rejects_initial_only_codes() {
    // `aabq` is structurally indistinguishable from a genuine five-letter
    // syllable such as `zhuang`, so no per-line check can reject it — the corpus
    // has to check itself, because a dictionary of initial-only codes compiles
    // without complaint and then silently never matches anything.
    //
    // (The real chengyu.dict.yaml is caught one step earlier, by the column
    // check in `rejects_missing_columns`: it has only two columns per row. This
    // test is the backstop for a three-column initial-only dictionary.)
    let chengyu = build::parse_dict(
        "chengyu",
        "---\nname: chengyu\nsort: by_weight\n...\n傲岸不群\taabq\t1\n皑皑白雪\taabx\t1\n",
    )
    .expect("per-line parse cannot know; the corpus check must");
    let err = build::validate(&chengyu).expect_err("must reject");
    assert!(
        err.contains("chengyu"),
        "error should name the culprit: {err}"
    );

    // A corpus whose only rows are spaced codes establishes the real syllables
    // and passes.
    let good = build::prepare(
        build::parse_dict(
            "base",
            "---\nname: base\n...\n世界\tshi jie\t1\n西安\txi an\t1\n",
        )
        .expect("parse"),
    );
    build::validate(&good).expect("spaced corpus is fine");

    // A rare syllable that occurs *only* as a whole word is a straggler, not a
    // failure: `biang` is real pinyin and the shipped dictionaries contain it only
    // as the code of biángbiáng面. The judgement is a ratio, so the corpus has to
    // be big enough for one straggler to be a small fraction of it — which is
    // exactly why this is a ratio and not a per-token rule.
    let mut big: Vec<Entry> = Vec::new();
    for i in 0..40 {
        let syl = format!("s{i}");
        big.push(Entry {
            code: format!("{syl} hao"),
            word: format!("词{i}"),
            weight: 10,
        });
        big.push(Entry {
            code: syl,
            word: format!("字{i}"),
            weight: 10,
        });
    }
    big.push(Entry {
        code: "biang".into(),
        word: "biangbiang面".into(),
        weight: 1,
    });
    build::validate(&big).expect("one straggler among 40 is fine");

    // An initial-only code, though, is the failure this check exists for — and it
    // fails the ratio just as loudly as it fails the empty case.
    let contaminated: Vec<Entry> = big
        .iter()
        .cloned()
        .chain((0..40).map(|i| Entry {
            code: format!("a{i}q"),
            word: format!("词{i}"),
            weight: 1,
        }))
        .collect();
    let err = build::validate(&contaminated).expect_err("must reject the mixed corpus");
    assert!(err.contains("a0q"), "error should name a bad token: {err}");
}

#[test]
fn rejects_missing_columns() {
    let err = build::parse_dict("bad", "你好\tni hao\n").expect_err("must reject");
    assert!(
        err.contains("weight"),
        "error should mention the column: {err}"
    );
}

#[test]
fn dedup_collapses_by_code_and_word_keeping_max_weight() {
    // 姓名 pairs: 同码 different words MUST all survive (they are the candidate
    // list), and identical (code, word) rows MUST collapse to the max weight.
    let parsed = build::parse_dict("d", "是\tshi\t10\n时\tshi\t20\n是\tshi\t99\n").expect("parse");
    let prepared = build::prepare(parsed);
    assert_eq!(
        prepared.len(),
        2,
        "homophones must not be merged away: {prepared:?}"
    );
    let shi = prepared.iter().find(|e| e.word == "是").expect("是");
    assert_eq!(shi.weight, 99, "max weight must win");
    let shi = prepared.iter().find(|e| e.word == "时").expect("时");
    assert_eq!(shi.weight, 20);
    // And within one code, heavier first — that is the candidate order.
    assert_eq!(prepared[0].weight, 99);
}

// ---- binary round trip ------------------------------------------------------

#[test]
fn compiled_binary_round_trips() {
    let (lex, _p) = fixture();
    let (n_syll, n_entries) = lex.counts();
    assert!(n_syll > 0 && n_entries > 0);
    assert!(!lex.is_empty());
    assert!(lex.has("ni hao"));
    assert!(!lex.has("ni hao x"), "a longer code must not match");
    assert!(lex.has("shi"));
    let words = lex.candidates("shi");
    assert_eq!(
        words,
        vec!["是", "时", "事", "世"],
        "candidates are weight-descending"
    );
}

#[test]
fn rejects_bad_magic() {
    let entries = build::prepare(build::parse_dict("f", FIXTURE).unwrap());
    let mut blob = build::compile(&entries);
    blob[0] = b'X';
    let path = temp_path("badmagic");
    std::fs::write(&path, &blob).unwrap();
    match Lexicon::open(&path) {
        Err(LexError::BadFormat) => {}
        other => panic!("expected BadFormat, got {other:?}"),
    }
}

#[test]
fn rejects_truncated_file() {
    let entries = build::prepare(build::parse_dict("f", FIXTURE).unwrap());
    let blob = build::compile(&entries);
    for cut in [0usize, 64, 100, 200] {
        let path = temp_path(&format!("trunc{cut}"));
        std::fs::write(&path, &blob[..cut.min(blob.len())]).unwrap();
        // Must be an Err, never a panic and never a half-usable mapping.
        assert!(
            Lexicon::open(&path).is_err(),
            "truncation at {cut} must be rejected"
        );
    }
}

#[test]
fn rejects_absent_file() {
    let p = temp_path("does-not-exist");
    let _ = std::fs::remove_file(&p);
    assert!(matches!(Lexicon::open(&p), Err(LexError::Io(_))));
}

#[test]
fn empty_dictionary_is_legal_and_serves_nothing() {
    // A corpus with no usable entries must not be a load failure; it is simply
    // an IME with no candidates.
    let lex = lex_from(&[]).0;
    assert!(lex.is_empty());
    assert!(lex.segment("nihao").iter().all(|s| s.best.is_none()));
    assert!(lex.candidates("ni hao").is_empty());
}

// ---- segmentation -----------------------------------------------------------

#[test]
fn splits_a_sentence_into_words() {
    let (lex, _p) = fixture();
    assert_eq!(shapes(&lex, "nihaoshijie"), vec!["ni hao", "shi jie"]);
    assert_eq!(
        bests(&lex, "nihaoshijie"),
        vec![Some("你好".into()), Some("世界".into())]
    );
}

#[test]
fn prefers_a_multi_syllable_word_over_its_characters() {
    let (lex, _p) = fixture();
    // 世界 (weight 68347) must beat 世+界 (959 + 7232), which the word penalty
    // is what buys: without it the two single characters, both frequent, would
    // out-score a 68k word.
    assert_eq!(bests(&lex, "shijie"), vec![Some("世界".into())]);
    assert_eq!(bests(&lex, "zhongguo"), vec![Some("中国".into())]);
    assert_eq!(bests(&lex, "nihao"), vec![Some("你好".into())]);
}

#[test]
fn one_syllable_can_also_be_two() {
    let (lex, _p) = fixture();
    // `xian` is one syllable (先, 75337) and also `xi`+`an` (西安, 6091). Only the
    // dictionary knows which was meant, and the dictionary says 西安 — which is
    // why the syllabifier builds a DAG instead of taking the longest match.
    assert_eq!(bests(&lex, "xian"), vec![Some("西安".into())]);
    // The single-syllable reading is still reachable, as a candidate.
    assert_eq!(lex.candidates("xian"), vec!["先", "线", "现"]);
}

#[test]
fn long_sentence_splits_into_dictionary_words() {
    let (lex, _p) = fixture();
    let segs = lex.segment("woshiyigechengxuyuan");
    let words: Vec<_> = segs.iter().filter_map(|s| s.best.clone()).collect();
    assert!(
        words.contains(&"程序员".to_string()),
        "程序员 must be found in a full sentence: {segs:?}"
    );
    // Every byte is covered exactly once, in order.
    let mut at = 0;
    for s in &segs {
        assert_eq!(s.start, at, "gap or overlap at {at}: {segs:?}");
        at = s.end;
    }
    assert_eq!(at, "woshiyigechengxuyuan".len());
}

#[test]
fn single_syllable_candidates_come_from_the_character_dictionary() {
    let (lex, _p) = fixture();
    // This is the whole reason 8105.dict.yaml is compiled: base and ext carry no
    // single-character entries, so without it `shi` would have no candidates.
    let c = lex.candidates("shi");
    assert!(c.contains(&"是".to_string()), "got {c:?}");
    assert!(c.contains(&"时".to_string()), "got {c:?}");
    assert!(c.contains(&"事".to_string()), "got {c:?}");
}

#[test]
fn apostrophe_is_transparent_to_the_syllabifier() {
    let (lex, _p) = fixture();
    assert_eq!(bests(&lex, "xi'an"), bests(&lex, "xian"));
    // But the apostrophe is still inside the segment's span, so selecting the
    // segment deletes it too.
    let seg = &lex.segment("xi'an")[0];
    assert_eq!((seg.start, seg.end), (0, "xi'an".len()));
}

#[test]
fn capitals_and_digits_stay_opaque() {
    let (lex, _p) = fixture();
    let segs = lex.segment("shiyongAI");
    assert_eq!(segs.len(), 2, "{segs:?}");
    assert_eq!(segs[0].best.as_deref(), Some("使用"));
    assert_eq!(segs[1].best, None, "AI must be opaque");
    assert!(!segs[1].is_selectable());
    assert_eq!(segs[1].pinyin, "AI");

    let segs = lex.segment("ni3hao");
    assert_eq!(
        segs.iter().filter(|s| s.best.is_none()).count(),
        1,
        "the digit: {segs:?}"
    );
    assert!(segs.iter().any(|s| s.best.as_deref() == Some("好")));
}

#[test]
fn punctuation_does_not_break_the_words_around_it() {
    let (lex, _p) = fixture();
    let segs = lex.segment("ni,hao");
    assert_eq!(segs.len(), 3, "{segs:?}");
    assert_eq!(segs[1].pinyin, ",");
    assert_eq!(segs[1].best, None);
    assert_eq!(segs[0].best.as_deref(), Some("你"));
    assert_eq!(segs[2].best.as_deref(), Some("好"));
}

#[test]
fn a_known_run_beats_being_opaque() {
    let (lex, _p) = fixture();
    // Regression: an unmatched span used to be priced as a weight-1 *word*, which
    // let it collect SYLLABUS_BONUS for free. A 4-syllable opaque run then scored
    // +2 and beat the correct split of the same buffer (你好 + 世界 = -0.29), so
    // the whole of `nihaoshijie` came back as one opaque span.
    let words: Vec<Option<String>> = bests(&lex, "nihaoshijie");
    assert_eq!(words, vec![Some("你好".into()), Some("世界".into())]);

    // And the same when an unknown name trails a known sentence, or leads it.
    for input in ["nihaoshijiezhangwei", "zhangweinishijie"] {
        let words: Vec<Option<String>> = bests(&lex, input);
        let found: Vec<&str> = words.iter().filter_map(|w| w.as_deref()).collect();
        assert!(found.contains(&"世界"), "{input} -> {words:?}");
        assert!(
            words.iter().any(|w| w.is_none()),
            "{input} should still carry the unmatched run: {words:?}"
        );
    }
}

#[test]
fn unknown_name_survives_as_one_opaque_run() {
    let (lex, _p) = fixture();
    let segs = lex.segment("zhangwei");
    assert_eq!(segs.len(), 1, "unmatched syllables should merge: {segs:?}");
    assert_eq!(segs[0].best, None);
    assert!(!segs[0].is_selectable());
}

#[test]
fn empty_input_yields_no_segments() {
    let (lex, _p) = fixture();
    assert!(lex.segment("").is_empty());
}

#[test]
fn segmentation_is_deterministic() {
    let (lex, _p) = fixture();
    // The candidate row must not flicker between keystrokes that produce the
    // same buffer, so equal scores have to resolve the same way every time.
    for input in [
        "nihaoshijie",
        "xian",
        "woshiyigechengxuyuan",
        "shiyongAI",
        "ni,hao",
    ] {
        let a = lex.segment(input);
        let b = lex.segment(input);
        assert_eq!(a, b, "non-deterministic segmentation for {input}");
    }
}

#[test]
fn candidates_never_exceed_the_label_range() {
    let (lex, _p) = fixture();
    for code in ["shi", "xian", "ni hao", "shi jie"] {
        assert!(lex.candidates(code).len() <= MAX_CANDIDATES);
    }
}

// ---- against a real compiled dictionary -------------------------------------

fn real_lex() -> Option<Lexicon> {
    let path = std::env::var_os("DSILEX_TEST_DICT")?;
    Lexicon::open(Path::new(&path)).ok()
}

#[test]
fn real_dictionary_segments_a_sentence() {
    let Some(lex) = real_lex() else { return };
    let words = |input: &str| -> Vec<String> {
        lex.segment(input)
            .into_iter()
            .filter_map(|s| s.best)
            .collect()
    };

    // Spot-checked against the shipped dictionary. These are the cases a plain
    // longest-match or a per-syllable-greedy segmenter gets wrong, which is why
    // they are pinned here rather than left to inspection.
    for (input, want) in [
        ("nihaoshijie", "你好|世界"),
        ("woshiyigechengxuyuan", "我是|一个|程序员"),
        ("zhongwenshurufa", "中文|输入法"),
        ("mingtianjianmianba", "明天|见面|吧"),
        ("xian", "西安"),  // one syllable or two, dictionary decides
        ("xi'an", "西安"), // the apostrophe is transparent
        ("qingtian", "晴天"),
        ("shiyongAI", "使用"), // trailing capitals stay opaque
    ] {
        assert_eq!(words(input).join("|"), want, "{input}");
    }

    // The single-syllable case only works because 8105 was compiled in: base and
    // ext carry no single-character entries at all.
    let c = lex.candidates("shi");
    assert!(c.contains(&"是".to_string()), "candidates(shi) -> {c:?}");
    assert_eq!(
        lex.candidates("zhong guo").first().map(String::as_str),
        Some("中国")
    );
}

/// A buffer that is not pure ASCII must segment, not panic.
///
/// The opaque step used to advance one byte, which lands a segment end in the
/// middle of a multi-byte character; the caller then slices its own input with
/// that offset, and a slice that does not start and end on a character boundary
/// panics. `ds_lexicon_segment` is `extern "C"`, so that panic cannot unwind
/// back into the caller — it aborts the host process, and this DLL is loaded
/// *into* notepad.exe. The frontend only ever hands over ASCII today, which is
/// exactly what makes this worth a test: nothing in the algorithm should be
/// relying on that.
#[test]
fn non_ascii_input_segments_instead_of_panicking() {
    let (lex, _p) = fixture();
    for input in [
        "你好",          // whole input multi-byte
        "nihao，shijie", // full-width comma between two words
        "，",            // lone multi-byte character
        "ni好hao",       // multi-byte in the middle
        "ni\u{0301}hao", // combining mark: two code points, one grapheme
    ] {
        let segs = lex.segment(input);
        // The real requirement: every span is a slice of the input, and the spans
        // cover it without gaps. Both panic if a boundary is wrong.
        let mut next = 0;
        for s in &segs {
            assert_eq!(s.start, next, "{input:?}: spans do not tile");
            assert!(s.end > s.start, "{input:?}: empty span");
            let _ = &input[s.start..s.end];
            next = s.end;
        }
        assert_eq!(next, input.len(), "{input:?}: spans do not cover the input");
    }
}

/// Punctuation is its own opaque segment, never part of a word's span.
///
/// This is what lets the frontend keep it: the reported bug was a comma typed
/// between two words disappearing when the second word was chosen, because the
/// active segment did not start at offset 0 and everything in front of it was
/// deleted along with it.
#[test]
fn punctuation_never_ends_up_inside_a_word_span() {
    let Some(lex) = real_lex() else { return };
    for input in [
        "nihao,shijie",
        "nihao，shijie",
        "woxiangquerenyixiexinxi.",
        ",nihaoshijie",
        "shi4hao",
    ] {
        let segs = lex.segment(input);
        let mut next = 0;
        for s in &segs {
            assert_eq!(s.start, next, "{input:?}: spans do not tile");
            assert!(s.end > s.start, "{input:?}: empty span");
            let text = &input[s.start..s.end];
            if let Some(word) = &s.best {
                assert!(
                    text.chars().all(|c| c.is_ascii_lowercase()),
                    "{input:?}: word {word:?} spans {text:?}, which is not pinyin"
                );
            }
            next = s.end;
        }
        assert_eq!(next, input.len(), "{input:?}: spans do not cover the input");
    }
    // And the specific case from the report: the comma is a segment of its own,
    // sitting between the two words, with the second word still selectable.
    let segs = lex.segment("nihao,shijie");
    let comma = segs
        .iter()
        .find(|s| input_slice(s, "nihao,shijie").contains(','));
    assert!(
        comma.is_some_and(|s| s.best.is_none()),
        "the comma is opaque"
    );
}

fn input_slice<'a>(s: &dsime::lexicon::Segment, input: &'a str) -> &'a str {
    &input[s.start..s.end]
}

#[test]
fn real_dictionary_stays_fast_on_a_long_buffer() {
    let Some(lex) = real_lex() else { return };
    // The candidate row is recomputed on every keystroke, so this has to stay
    // far below the frame budget even at the size of a long sentence.
    let input = "woshiyizhongguochengxuyuandejizhiyonghulianxiepeizhichi";
    let t0 = std::time::Instant::now();
    for _ in 0..100 {
        let _ = lex.segment(input);
    }
    let per = t0.elapsed() / 100;
    assert!(per.as_millis() < 20, "segment took {per:?} for {input:?}");
}

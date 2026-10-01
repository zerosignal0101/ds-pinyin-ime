//! Pinyin segmentation and dictionary candidate lookup.
//!
//! Two things live here, and neither of them touches `DsSession`:
//!
//!   * the compiled dictionary (`.lex`) — a flat binary, mmap'd read-only, with
//!     entries sorted by code so that both query shapes (exact, and prefix for
//!     segmentation) are a single binary search over one compact array; and
//!   * the segmenter — a per-syllable DAG plus a backwards DP over it.
//!
//! Everything is a pure function of its input string. Which words the user has
//! already picked is deliberately *not* here: that is interaction state, and the
//! frontend owns it. In practice the frontend only ever hands us the unselected
//! tail, so every call sees a buffer that starts at a word boundary.
//!
//! ## Why the scoring looks the way it does
//!
//! rime-frost ships no bigram weights (`essay.txt` has a weight per line, not
//! counts), so segmentation is maximum-probability over unigram weights alone:
//!
//! ```text
//! score(word)   = ln(weight) + SYLLABUS_BONUS*(syllables-1) - WORD_PENALTY
//! score(opaque) = 0
//! ```
//!
//! Both constants are load-bearing, and the reason is a scale clash between the
//! two dictionaries we compile. The 8105 single-character weights are character
//! frequencies and reach 1,799,848 (`是`), while base's word weights top out
//! around 660k. So `ln(w)` for one character is ~14.5 and for two ~27 — more than
//! any bonus for being a longer word could offset without a word penalty of the
//! same order. Measured behaviour of `SYLLABUS_BONUS=6, WORD_PENALTY=16`:
//!
//! | input   | chosen    | rejected   | scores |
//! |---------|-----------|------------|--------|
//! | `nihao` | 你好      | 你 + 好    | `-1.42` vs `-2.89 + -4.27` |
//! | `xian`  | 西安 (`xi an`) | 先    | `-1.29` vs `-4.77` |
//! | `shijie`| 世界      | 世 + 界    | `1.13` vs `-9.14 + -7.11` |
//!
//! `xian` is the case that forces the whole design: it is one syllable *and* two
//! (`xi` + `an`), and only the dictionary can say which was meant. That is why
//! the syllabifier builds a DAG of *all* matches rather than taking the longest
//! greedily — see [`Lexicon::syllable_ends`].

use std::cell::RefCell;
use std::fs::File;
use std::path::Path;
use std::str;

use memmap2::Mmap;

/// File magic. Bump [`FORMAT_VERSION`] if the layout ever changes; there is no
/// migration path, so a mismatch is a hard load failure — and therefore a
/// silent degradation to "no candidates", which is the intended failure mode.
pub const MAGIC: [u8; 8] = *b"DSLEXv1\0";
pub const FORMAT_VERSION: u32 = 1;

/// Longest syllable the compiled dictionary can contain. Measured from the
/// dictionaries themselves: 413 distinct syllables, max 7 letters.
const MAX_SYLLABLE_LEN: usize = 7;
/// Longest word we will look for, in syllables.
const MAX_SYLL_PER_WORD: usize = 6;
/// How many alternative readings of one position we explore. Real positions
/// offer one or two; the cap only stops a pathological buffer from making the
/// per-offset branching product explode.
const MAX_SYLL_OPTIONS: usize = 6;
/// Hard ceiling on dictionary lookups per `segment` call. With the prefix
/// pruning below this is not reached in practice; it exists so a hostile buffer
/// costs a bounded, predictable amount of work on the keystroke path.
const LOOKUP_BUDGET: usize = 40_000;

/// Candidates per segment, and therefore the digit labels the UI can offer:
/// `2`..`= MAX_CANDIDATES + 1`. There is deliberately no `1`.
pub const MAX_CANDIDATES: usize = 8;

const SYLLABUS_BONUS: f64 = 6.0;
const WORD_PENALTY: f64 = 16.0;
/// What a span the dictionary cannot match costs. Flat, and just under
/// `WORD_PENALTY`, so that no opaque span is ever preferable to a single known
/// character. See `explore`.
const OPAQUE_COST: f64 = -16.0;

// ---- on-disk layout (little-endian) ------------------------------------------
//
// The header occupies exactly HEADER_SIZE bytes and every section is 8-byte
// aligned. Offsets are read by byte index rather than by casting the mapping to
// a struct, so a truncated or hostile file yields `None` instead of UB.

const H_MAGIC: usize = 0;
const H_VERSION: usize = 8;
const H_N_SYLL: usize = 12;
const H_N_ENTRIES: usize = 16;
const H_SYLL_OFFS: usize = 20; // u64 -> u32[n_syll] offsets into syll_blob
const H_SYLL_BLOB: usize = 28; // u64
const H_SYLL_BLOB_LEN: usize = 36;
const H_ENTRIES: usize = 44; // u64, base of the five SoA arrays
const H_CODE_BLOB: usize = 52;
const H_CODE_BLOB_LEN: usize = 60;
const H_WORD_BLOB: usize = 68;
const H_WORD_BLOB_LEN: usize = 76;
const HEADER_SIZE: usize = 128;

// The entry region is struct-of-arrays. Each array occupies a contiguous
// *byte* range, so the four offsets below are multiplied by `n_entries` to become
// section bases — they are array positions, not per-element field offsets.
//   code_off  u32[n]   weight  u32[n]   word_off u32[n]   code_len u8[n]   word_len u8[n]
// The three u32 arrays come first so they stay 4-byte aligned.
const SOA_U32_COUNT: usize = 3;
const SOA_CODE_OFF: usize = 0;
const SOA_WEIGHT: usize = 1;
const SOA_WORD_OFF: usize = 2;
const SOA_BYTE_COUNT: usize = 2;
const SOA_CODE_LEN: usize = SOA_U32_COUNT * 4;
const SOA_WORD_LEN: usize = SOA_CODE_LEN + 1;
const SOA_STRIDE: usize = SOA_CODE_LEN + SOA_BYTE_COUNT;

/// Byte bases of the five SoA arrays within the entry region.
fn soa_bases(n: usize) -> [usize; 5] {
    [
        SOA_CODE_OFF * 4 * n,
        SOA_WEIGHT * 4 * n,
        SOA_WORD_OFF * 4 * n,
        SOA_CODE_LEN * n,
        SOA_WORD_LEN * n,
    ]
}

/// A contiguous run of input, as produced by the segmenter.
///
/// `best` is the highest-weighted word for this span, which is also candidate
/// #1. It is `None` for an opaque span — anything the dictionary could not
/// match: a digit, a comma, the capital letters of `shiyongAI`, or a name the
/// dictionaries do not carry.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Segment {
    /// Byte offset of the first byte in the input.
    pub start: usize,
    /// Byte offset one past the last byte.
    pub end: usize,
    /// Space-joined code for a word segment — the key to pass back to
    /// [`Lexicon::candidates`]. For an opaque segment, the raw input slice.
    pub pinyin: String,
    /// Highest-weighted word, or `None` for an opaque segment.
    pub best: Option<String>,
}

impl Segment {
    /// Whether the dictionary matched this span and it can be selected.
    pub fn is_selectable(&self) -> bool {
        self.best.is_some()
    }
}

/// The DP's choice at one byte offset. `entry` indexes the dictionary, which is
/// what makes the winning code recoverable without threading strings through
/// the table (the code of entry `i` *is* the code that matched).
#[derive(Debug, Clone, Copy)]
struct Best {
    end: usize,
    score: f64,
    entry: Option<usize>,
    n_syll: usize,
}

/// A read-only view of a compiled dictionary.
pub struct Lexicon {
    map: Mmap,
    n_syll: usize,
    n_entries: usize,
    syll_offs: usize,
    syll_blob: usize,
    /// Byte bases of the five SoA arrays, absolute into the mapping.
    soa: [usize; 5],
    code_blob: usize,
    word_blob: usize,
}

impl std::fmt::Debug for Lexicon {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Lexicon")
            .field("n_syll", &self.n_syll)
            .field("n_entries", &self.n_entries)
            .field("bytes", &self.map.len())
            .finish()
    }
}

/// Why a dictionary could not be opened. Every variant is recoverable: the
/// caller degrades to "no segmentation, no candidates", which is exactly how the
/// IME behaved before this feature existed.
#[derive(Debug)]
pub enum LexError {
    Io(std::io::Error),
    /// Shorter than the fixed header, or a section escapes the file.
    Truncated,
    /// Magic mismatch, or a `version` this build does not understand.
    BadFormat,
    /// A UTF-8 string inside the file is not valid UTF-8.
    BadUtf8,
}

impl std::fmt::Display for LexError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            LexError::Io(e) => write!(f, "io: {e}"),
            LexError::Truncated => write!(f, "truncated or out-of-range section"),
            LexError::BadFormat => write!(f, "bad magic or unsupported version"),
            LexError::BadUtf8 => write!(f, "invalid utf-8 in dictionary"),
        }
    }
}

impl From<std::io::Error> for LexError {
    fn from(e: std::io::Error) -> Self {
        LexError::Io(e)
    }
}

impl Lexicon {
    /// Map a compiled dictionary read-only.
    pub fn open(path: &Path) -> Result<Lexicon, LexError> {
        let file = File::open(path)?;
        // SAFETY: read-only private mapping. Nothing here writes through it, and
        // the file handle outlives the mapping, so the bytes cannot go away.
        let map = unsafe { Mmap::map(&file)? };

        let get_u32 = |off: usize| -> Option<u32> {
            map.get(off..off + 4)
                .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
        };
        let get_u64 = |off: usize| -> Option<u64> {
            map.get(off..off + 8)
                .map(|b| u64::from_le_bytes([b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]]))
        };

        if map.len() < HEADER_SIZE {
            return Err(LexError::Truncated);
        }
        if map.get(H_MAGIC..H_MAGIC + 8) != Some(MAGIC.as_slice()) {
            return Err(LexError::BadFormat);
        }
        let (version, n_syll, n_entries) =
            match (get_u32(H_VERSION), get_u32(H_N_SYLL), get_u32(H_N_ENTRIES)) {
                (Some(v), Some(s), Some(n)) => (v, s as usize, n as usize),
                _ => return Err(LexError::Truncated),
            };
        if version != FORMAT_VERSION {
            return Err(LexError::BadFormat);
        }
        let (
            syll_offs,
            syll_blob,
            syll_blob_len,
            entries,
            code_blob,
            code_blob_len,
            word_blob,
            word_blob_len,
        ) = match (
            get_u64(H_SYLL_OFFS),
            get_u64(H_SYLL_BLOB),
            get_u64(H_SYLL_BLOB_LEN),
            get_u64(H_ENTRIES),
            get_u64(H_CODE_BLOB),
            get_u64(H_CODE_BLOB_LEN),
            get_u64(H_WORD_BLOB),
            get_u64(H_WORD_BLOB_LEN),
        ) {
            (Some(a), Some(b), Some(c), Some(d), Some(e), Some(f), Some(g), Some(h)) => (
                a as usize, b as usize, c as usize, d as usize, e as usize, f as usize, g as usize,
                h as usize,
            ),
            _ => return Err(LexError::Truncated),
        };

        // Bounds-check every section once, here, so the accessors below can index
        // without re-checking. A zero-length dictionary is legal — that is what a
        // corpus with no usable entries compiles to — so the tests are against
        // the declared lengths rather than a minimum size.
        let fits =
            |base: usize, len: usize| base.checked_add(len).is_some_and(|end| end <= map.len());
        if !fits(syll_offs, n_syll.saturating_mul(4))
            || !fits(syll_blob, syll_blob_len)
            || !fits(code_blob, code_blob_len)
            || !fits(word_blob, word_blob_len)
            || !fits(entries, n_entries.saturating_mul(SOA_STRIDE))
        {
            return Err(LexError::Truncated);
        }

        Ok(Lexicon {
            map,
            n_syll,
            n_entries,
            syll_offs,
            syll_blob,
            soa: soa_bases(n_entries).map(|o| entries + o),
            code_blob,
            word_blob,
        })
    }

    /// `(distinct syllables, entries)`.
    pub fn counts(&self) -> (usize, usize) {
        (self.n_syll, self.n_entries)
    }

    /// Total mapped bytes.
    pub fn len(&self) -> usize {
        self.map.len()
    }

    /// Whether the dictionary holds no entries at all.
    pub fn is_empty(&self) -> bool {
        self.n_entries == 0
    }

    // ---- primitive accessors -------------------------------------------------

    fn u32_at(&self, off: usize) -> u32 {
        u32::from_le_bytes([
            self.map[off],
            self.map[off + 1],
            self.map[off + 2],
            self.map[off + 3],
        ])
    }

    fn code_bytes(&self, i: usize) -> &[u8] {
        let off = self.u32_at(self.soa[0] + i * 4) as usize;
        let len = self.map[self.soa[3] + i] as usize;
        &self.map[self.code_blob + off..self.code_blob + off + len]
    }

    fn word_at(&self, i: usize) -> Result<&str, LexError> {
        let off = self.u32_at(self.soa[2] + i * 4) as usize;
        let len = self.map[self.soa[4] + i] as usize;
        str::from_utf8(&self.map[self.word_blob + off..self.word_blob + off + len])
            .map_err(|_| LexError::BadUtf8)
    }

    fn weight_at(&self, i: usize) -> u32 {
        self.u32_at(self.soa[1] + i * 4)
    }

    fn syllable_at(&self, i: usize) -> &[u8] {
        let off = self.u32_at(self.syll_offs + i * 4) as usize;
        let base = self.syll_blob + off;
        let end = self.map[base..]
            .iter()
            .position(|&b| b == 0)
            .map(|n| base + n)
            .unwrap_or(self.map.len());
        &self.map[base..end]
    }

    fn is_syllable(&self, s: &[u8]) -> bool {
        let (mut lo, mut hi) = (0usize, self.n_syll);
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            match self.syllable_at(mid) {
                v if v < s => lo = mid + 1,
                v if v > s => hi = mid,
                _ => return true,
            }
        }
        false
    }

    /// First index whose code is >= `key`.
    fn lower_bound(&self, key: &[u8]) -> usize {
        let (mut lo, mut hi) = (0usize, self.n_entries);
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            if self.code_bytes(mid) < key {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        lo
    }

    /// Half-open range of entries whose code is exactly `code`.
    fn exact_range(&self, code: &[u8]) -> std::ops::Range<usize> {
        let lo = self.lower_bound(code);
        let mut hi = lo;
        while hi < self.n_entries && self.code_bytes(hi) == code {
            hi += 1;
        }
        lo..hi
    }

    /// Whether any code starts with `prefix`. This is the segmenter's pruning
    /// test: if no entry begins with the syllables chosen so far, no longer word
    /// can either, so the DFS stops rather than extending.
    fn prefix_exists(&self, prefix: &[u8]) -> bool {
        let lo = self.lower_bound(prefix);
        lo < self.n_entries && self.code_bytes(lo).starts_with(prefix)
    }

    fn max_weight_entry(&self, range: &std::ops::Range<usize>) -> (usize, u32) {
        let mut best = (range.start, 0u32);
        for i in range.clone() {
            let w = self.weight_at(i);
            if w > best.1 {
                best = (i, w);
            }
        }
        best
    }

    /// Whether the dictionary carries at least one word for this code.
    pub fn has(&self, code: &str) -> bool {
        let key = code.as_bytes();
        let lo = self.lower_bound(key);
        lo < self.n_entries && self.code_bytes(lo) == key
    }

    /// Candidate words for a code, highest weight first, at most
    /// [`MAX_CANDIDATES`]. Index 0 is what the UI labels `2`.
    pub fn candidates(&self, code: &str) -> Vec<String> {
        let range = self.exact_range(code.as_bytes());
        if range.is_empty() {
            return Vec::new();
        }
        let mut idx: Vec<usize> = range.collect();
        // Ties break on entry index, not on sort stability, so the order is a
        // property of the file rather than of the allocator.
        idx.sort_unstable_by(|&a, &b| {
            self.weight_at(b)
                .cmp(&self.weight_at(a))
                .then_with(|| a.cmp(&b))
        });
        idx.truncate(MAX_CANDIDATES);
        idx.into_iter()
            .filter_map(|i| self.word_at(i).ok().map(str::to_owned))
            .collect()
    }

    // ---- syllabification ----------------------------------------------------

    /// Every offset at which the bytes from `at` can end a syllable, shortest
    /// first.
    ///
    /// Returning *all* matches rather than the longest is the whole point — see
    /// the module docs for `xian`. Apostrophes are transparent: `xi'an` reads
    /// as the syllable `xian`, because the dictionary spells that code without
    /// one. The apostrophe is still inside the input span, so selecting the
    /// segment deletes it along with the rest.
    fn syllable_ends(&self, input: &[u8], at: usize, out: &mut Vec<usize>) {
        out.clear();
        let mut letters: Vec<u8> = Vec::with_capacity(MAX_SYLLABLE_LEN);
        for (j, &b) in input[at..].iter().enumerate() {
            if b == b'\'' {
                continue;
            }
            if !b.is_ascii_lowercase() || letters.len() == MAX_SYLLABLE_LEN {
                break;
            }
            letters.push(b);
            if self.is_syllable(&letters) {
                out.push(at + j + 1);
            }
        }
    }

    /// Append `input[from..to]` to `code`, dropping apostrophes. Code bytes are
    /// ASCII lowercase by construction.
    fn push_syllable(code: &mut String, input: &[u8], from: usize, to: usize) {
        for &b in &input[from..to] {
            if b != b'\'' {
                code.push(b as char);
            }
        }
    }

    /// Score contributed by one word.
    ///
    /// `SYLLABUS_BONUS` **saturates at two syllables**, and that is not a
    /// simplification — it is the only shape that works. A linear per-syllable
    /// bonus has to satisfy two constraints at once, and they are contradictory
    /// with unigram weights alone:
    ///
    /// * 西安 (`xi an`, w=6091) must beat 先 (`xian`, w=75337), so a 2-syllable
    ///   word needs at least `ln(75337) - ln(6091) ≈ 2.5` more than a 1-syllable
    ///   one.
    /// * 你好世界 (`ni hao shi jie`) must *lose* to 你好 + 世界. Its own weight is
    ///   a real number, and paying it 3 extra bonus units costs more than the 16
    ///   that splitting saves. With `ln(w) ≈ 8.5` that needs the bonus to go
    ///   *negative*.
    ///
    /// So the bonus pays for exactly the thing it is there for — crossing from
    /// one syllable to two, which is what `xian` needs — and stops. From three
    /// syllables on, a word competes on its own weight, which is the honest
    /// signal: a long word's frequency already says how likely it is as a unit.
    fn word_score(weight: u32, n_syll: usize) -> f64 {
        let bonus = if n_syll >= 2 { SYLLABUS_BONUS } else { 0.0 };
        (weight.max(1) as f64).ln() + bonus - WORD_PENALTY
    }

    /// Total order over competing edges, so the segmenter is a pure function of
    /// the dictionary: equal scores must not resolve by iteration order, or the
    /// candidate row would flicker between keystrokes that should be equivalent.
    fn better(&self, a: &Best, b: &Best) -> bool {
        if a.score != b.score {
            return a.score > b.score;
        }
        if a.n_syll != b.n_syll {
            return a.n_syll < b.n_syll;
        }
        let (wa, wb) = (
            a.entry.map_or(0, |i| self.weight_at(i)),
            b.entry.map_or(0, |i| self.weight_at(i)),
        );
        if wa != wb {
            return wa > wb;
        }
        a.end < b.end
    }

    /// Extend the current word at `scan.at`, trying every reading, recording
    /// into `scan.best` every edge that reaches a dictionary entry.
    fn explore(&self, code: &mut String, n_syll: usize, scan: &mut Scan<'_>) {
        let at = scan.at;
        if at >= scan.input.len() {
            return;
        }
        let mut ends = Vec::with_capacity(MAX_SYLL_OPTIONS);
        self.syllable_ends(scan.input, at, &mut ends);
        for &end in ends.iter().take(MAX_SYLL_OPTIONS) {
            if scan.budget == 0 {
                return;
            }
            let mark = code.len();
            if n_syll > 0 {
                code.push(' ');
            }
            Self::push_syllable(code, scan.input, at, end);
            scan.budget -= 1;

            let k = n_syll + 1;
            let range = self.exact_range(code.as_bytes());
            let Some(tail) = scan.tail(end) else {
                code.truncate(mark);
                continue;
            };
            if !range.is_empty() {
                let (entry, weight) = self.max_weight_entry(&range);
                let cand = Best {
                    end,
                    score: tail + Self::word_score(weight, k),
                    entry: Some(entry),
                    n_syll: k,
                };
                if scan.better(self, &cand) {
                    scan.best = Some(cand);
                }
            } else {
                // No entry for this span: the dictionary has nothing to say about
                // it — a name it does not carry, or syllables we could not read.
                //
                // Priced as a flat unknown word, and deliberately *not* scaled by
                // the span's syllable count. Scaling it would hand the span a
                // bonus it has not earned, and a long unmatched run would then
                // beat a correct multi-word split of the same buffer: the
                // segmenter would prefer "I know nothing" to a segmentation it
                // had just found. A flat cost below that of any known character
                // makes an opaque span strictly the last resort, which is what it
                // is.
                let cand = Best {
                    end,
                    score: tail + OPAQUE_COST,
                    entry: None,
                    n_syll: k,
                };
                if scan.better(self, &cand) {
                    scan.best = Some(cand);
                }
            }

            // Keep going only if some entry could still start with what we have.
            if k < MAX_SYLL_PER_WORD && self.prefix_exists(code.as_bytes()) {
                scan.at = end;
                self.explore(code, k, scan);
                scan.at = at;
            }
            code.truncate(mark);
        }
    }

    fn best_at(&self, scan: &mut Scan<'_>) -> Option<Best> {
        let i = scan.at;
        if i >= scan.input.len() {
            return None;
        }
        // Not a lowercase letter: opaque, one CHARACTER. This is how a digit, a
        // comma, and the capital letters of `shiyongAI` pass through untouched.
        // Such a byte carries no pinyin and no ambiguity, so it costs nothing —
        // and it is the same cost for every segmentation that covers it, so it
        // cannot bias the choice between them.
        //
        // The step is a character, not a byte, and that is a correctness
        // requirement rather than tidiness: the caller slices its own input with
        // the offsets returned here, and a slice that does not start and end on a
        // character boundary panics. A panic inside an `extern "C"` function
        // cannot unwind into the caller, so it takes the whole host process down —
        // this DLL is loaded *into* notepad.exe. The frontend only ever hands over
        // ASCII, but `ds_lexicon_segment` is a public ABI function and a caller
        // that does not owe us that must not be able to kill the process.
        if !scan.input[i].is_ascii_lowercase() {
            let end = i + char_len(scan.input, i);
            let tail = scan.tail(end)?;
            return Some(Best {
                end,
                score: tail,
                entry: None,
                n_syll: 0,
            });
        }

        let mut code = String::new();
        // `Scan` is reused across positions, so the previous position's winner has
        // to be cleared or it would be carried into this one.
        scan.best = None;
        self.explore(&mut code, 0, scan);
        // Ultimate fallback: no reading of this byte is a syllable, so consume
        // one byte opaquely rather than stalling.
        scan.best.or_else(|| {
            let tail = scan.tail(i + 1)?;
            Some(Best {
                end: i + 1,
                score: tail,
                entry: None,
                n_syll: 0,
            })
        })
    }

    /// Segment a pinyin buffer.
    ///
    /// Adjacent opaque spans are merged, so `shi,hao` and `zhang-wei` come back
    /// as `["shi", ",", "hao"]` and `["zhang", "-wei"]` rather than one segment
    /// per byte. The result is a pure function of `input` and the dictionary.
    pub fn segment(&self, input: &str) -> Vec<Segment> {
        let bytes = input.as_bytes();
        let n = bytes.len();
        if n == 0 {
            return Vec::new();
        }
        let mut table = vec![None; n + 1];
        table[n] = Some(Best {
            end: n,
            score: 0.0,
            entry: None,
            n_syll: 0,
        });
        let mut scan = Scan {
            input: bytes,
            dp: RefCell::new(table),
            at: 0,
            budget: LOOKUP_BUDGET,
            best: None,
        };
        for i in (0..n).rev() {
            // Only character starts are resolved. A byte offset inside a
            // multi-byte character can never be reached by any edge — a word
            // spells itself in ASCII, and the opaque step below consumes a whole
            // character — so resolving one would only produce a `tail` that
            // nothing asks for, at an offset nothing may slice.
            if !input.is_char_boundary(i) {
                continue;
            }
            scan.at = i;
            let v = self.best_at(&mut scan);
            scan.dp.borrow_mut()[i] = v;
        }
        let table = scan.into_table();

        let mut segs: Vec<Segment> = Vec::new();
        let mut i = 0usize;
        while i < n {
            // `best_at` always yields something, so this cannot panic on a
            // corrupt dictionary; the fallback keeps the loop making progress.
            // A whole character, for the same reason: a one-byte step from the
            // middle of a multi-byte character is not a slice at all.
            let Some(choice) = table[i] else {
                let end = i + char_len(bytes, i);
                segs.push(opaque(&input[i..end], i, end));
                i = end;
                continue;
            };
            let (pinyin, best) = match choice.entry {
                // The code of the winning entry *is* the code that matched, so
                // the space-joined key comes back without threading a string
                // through the DP table.
                Some(e) => (
                    str::from_utf8(self.code_bytes(e))
                        .unwrap_or_default()
                        .to_owned(),
                    self.word_at(e).ok().map(str::to_owned),
                ),
                None => (input[i..choice.end].to_owned(), None),
            };
            segs.push(Segment {
                start: i,
                end: choice.end,
                pinyin,
                best,
            });
            i = if choice.end > i { choice.end } else { i + 1 };
        }

        merge_opaque(segs)
    }
}

/// The mutable state of one backwards pass over the buffer.
///
/// `dp` is the awkward field. The driver writes `dp[i]` while `explore` reads
/// `dp[j]` for `j > i` — the same table, live at the same time — so a plain
/// `&`/`&mut` split cannot express it. [`RefCell`] is the honest tool: the borrows
/// are short and never overlap (`tail` releases before any candidate is built), so
/// there is no re-entrancy to defend against.
///
/// It is a struct rather than loose parameters mostly so the budget stays visible
/// and travels with the table it bounds: `budget` is the ceiling on the keystroke
/// path, and threading it separately is how a future code path ends up forgetting
/// to decrement it.
struct Scan<'a> {
    input: &'a [u8],
    /// `dp[pos]` is the best edge for `input[pos..]`, resolved from the end
    /// backwards.
    dp: RefCell<Vec<Option<Best>>>,
    /// The byte offset being resolved. `explore` walks it forward and restores it.
    at: usize,
    /// Dictionary lookups still allowed. See [`LOOKUP_BUDGET`].
    budget: usize,
    /// Best edge found at the offset currently being resolved.
    best: Option<Best>,
}

impl Scan<'_> {
    /// Remaining score from `pos` onward, if that position is already resolved.
    fn tail(&self, pos: usize) -> Option<f64> {
        self.dp.borrow().get(pos).and_then(|o| *o).map(|b| b.score)
    }

    fn better(&self, lex: &Lexicon, cand: &Best) -> bool {
        self.best.is_none_or(|b| lex.better(cand, &b))
    }

    fn into_table(self) -> Vec<Option<Best>> {
        self.dp.into_inner()
    }
}

/// Byte length of the UTF-8 character starting at `i`.
///
/// The UTF-8 lead-byte table, without decoding: a sequence that is not valid
/// UTF-8 cannot be sliced by the caller either, and one byte is the only step
/// that always advances. `str::len_utf8` would need a `char`, which needs a
/// decode, which is a panic on exactly the input this exists to survive.
fn char_len(bytes: &[u8], i: usize) -> usize {
    let width = match bytes[i] {
        0x00..=0x7F => 1,
        0xC0..=0xDF => 2,
        0xE0..=0xEF => 3,
        0xF0..=0xF7 => 4,
        // A continuation byte, or anything invalid: not a character start at all.
        _ => 1,
    };
    width.min(bytes.len() - i).max(1)
}

fn opaque(text: &str, start: usize, end: usize) -> Segment {
    Segment {
        start,
        end,
        pinyin: text.to_owned(),
        best: None,
    }
}

/// Fuse neighbouring opaque spans. They are never selectable, so their internal
/// boundaries carry no meaning to the caller; merging keeps the segment list the
/// size the user would describe.
fn merge_opaque(segs: Vec<Segment>) -> Vec<Segment> {
    let mut out: Vec<Segment> = Vec::with_capacity(segs.len());
    for s in segs {
        match out.last_mut() {
            Some(prev) if prev.best.is_none() && s.best.is_none() => {
                prev.pinyin.push_str(&s.pinyin);
                prev.end = s.end;
            }
            _ => out.push(s),
        }
    }
    out
}

/// The compiler half of the format: parsing rime dictionaries and writing the
/// `.lex` binary. It lives beside the reader so there is exactly one definition
/// of the layout, and it is unreachable from any exported symbol, so the linker
/// drops it from `dsime.dll` rather than shipping a parser to every host process.
pub mod build {
    use super::*;
    use std::collections::BTreeMap;

    /// The dictionaries this project compiles, in priority order. Later files
    /// fill in what earlier ones miss; `8105` is not optional — it is the only
    /// source of single-character entries, and without it a one-syllable buffer
    /// has no candidates at all.
    pub const SOURCES: &[&str] = &["base", "ext", "8105"];

    /// One dictionary line.
    #[derive(Debug, Clone, PartialEq, Eq)]
    pub struct Entry {
        pub code: String,
        pub word: String,
        pub weight: u32,
    }

    /// Parse a rime `.dict.yaml` into entries.
    ///
    /// Handles the YAML front matter rime requires, `#` comments, CRLF, and an
    /// optional `columns:` declaration (defaulting to rime's own
    /// `text code weight`). Codes must be lowercase ASCII syllables separated by
    /// spaces; anything else is an error rather than a silent skip, because a
    /// dictionary of initial-only codes — `word.dict.yaml`, `chengyu.dict.yaml`
    /// — compiles to something that can never match a full-pinyin query.
    pub fn parse_dict(name: &str, text: &str) -> Result<Vec<Entry>, String> {
        let mut columns: Vec<String> =
            vec!["text".to_owned(), "code".to_owned(), "weight".to_owned()];
        let mut out = Vec::new();
        let mut in_body = false;
        let mut in_front = false;

        for (lineno, raw) in text.lines().enumerate() {
            let line = raw.trim_end_matches('\r');
            let trimmed = line.trim();

            // `columns:` is honoured wherever it appears. rime puts it in the
            // front matter, but the front matter is optional in practice, and a
            // corpus that declares columns without `---` markers still has to work.
            if let Some(rest) = trimmed.strip_prefix("columns:") {
                columns = rest
                    .split(',')
                    .map(|c| c.trim().trim_matches('"').to_owned())
                    .collect();
                continue;
            }

            if !in_body {
                if in_front {
                    if trimmed == "..." || trimmed == "---" {
                        in_front = false;
                        in_body = true;
                    }
                    continue;
                }
                if trimmed.is_empty() || trimmed.starts_with('#') {
                    continue;
                }
                if trimmed == "---" {
                    in_front = true;
                    continue;
                }
                // A bare `...` closes front matter whose opening `---` was left
                // out. Everything before it was front matter, everything after
                // is the TSV body.
                if trimmed == "..." {
                    in_body = true;
                    continue;
                }
                // The opening `---` is optional in the wild. Before the first
                // real row, a `key: value` line carrying no tab is front matter
                // rather than data — otherwise `name: fixture` is read as a word.
                if !line.contains('\t') && looks_like_yaml_key(trimmed) {
                    continue;
                }
                in_body = true;
            }

            if trimmed.is_empty() || trimmed.starts_with('#') {
                continue;
            }
            // A trailing comment glued on with a tab (`... \t# rime-moran`).
            let line = match line.find("\t#") {
                Some(at) => &line[..at],
                None => line,
            };

            let fields: Vec<&str> = line.split('\t').collect();
            if fields.len() < columns.len() {
                return Err(format!(
                    "{name}:{lineno}: expected {} tab-separated columns ({}), found {} in {line:?}",
                    columns.len(),
                    columns.join(","),
                    fields.len()
                ));
            }
            let get = |want: &str| -> Option<&str> {
                columns
                    .iter()
                    .position(|c| c == want)
                    .and_then(|i| fields.get(i).copied())
            };
            let (Some(word), Some(code)) = (get("text"), get("code")) else {
                return Err(format!(
                    "{name}:{lineno}: no text/code column (have {:?}, columns: {})",
                    fields,
                    columns.join(",")
                ));
            };
            let weight = match get("weight") {
                None | Some("") => 0,
                Some(w) => w
                    .trim()
                    .parse::<u32>()
                    .map_err(|e| format!("{name}:{lineno}: bad weight {w:?}: {e}"))?,
            };

            let code = code.trim();
            if code.is_empty() || word.is_empty() {
                continue;
            }
            if !code
                .split(' ')
                .all(|s| !s.is_empty() && s.bytes().all(|b| b.is_ascii_lowercase()))
            {
                return Err(format!(
                    "{name}:{lineno}: code {code:?} is not space-separated lowercase pinyin \
                     (an initial-only dictionary such as cn_dicts_common/chengyu.dict.yaml \
                     cannot be used with full pinyin)"
                ));
            }
            out.push(Entry {
                code: code.to_owned(),
                word: word.to_owned(),
                weight,
            });
        }
        Ok(out)
    }

    fn looks_like_yaml_key(s: &str) -> bool {
        match s.split_once(':') {
            Some((k, _)) => {
                !k.is_empty()
                    && k.chars()
                        .all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-')
            }
            None => false,
        }
    }

    /// Reject a corpus whose codes are not full pinyin.
    ///
    /// `word.dict.yaml` and `chengyu.dict.yaml` spell 傲岸不群 as `aabq`, which
    /// parses without complaint and compiles to a dictionary that can never
    /// match a full-pinyin query — a completely silent failure that would leave
    /// the IME with no candidates and nothing to point at. A hardcoded syllable
    /// table cannot be the test: `aabq` is structurally indistinguishable from a
    /// genuine five-letter syllable such as `zhuang`.
    ///
    /// So the corpus validates itself. A space-separated code is unambiguous —
    /// a space *is* rime's syllable delimiter — so those rows yield a
    /// trustworthy syllable set. A single-token code must then appear in it, with
    /// one honest exception: a rare syllable can be the *only* place it occurs.
    /// `biang` is real pinyin, and the shipped dictionaries contain it solely as
    /// the single-syllable code of biángbiáng面, never inside a longer word. So a
    /// handful of strays is normal, and the judgement is a ratio: a corpus that
    /// is genuinely initial-only fails this overwhelmingly, while a real
    /// dictionary has a few tenths of a percent. Warns and returns the strays;
    /// errors past the threshold.
    pub fn validate(entries: &[Entry]) -> Result<(), String> {
        /// Fraction of single-token codes that must be explainable as pinyin
        /// before we refuse to compile.
        const MAX_STRAY_RATIO: f64 = 0.05;

        let from_spaced: BTreeMap<&str, ()> = entries
            .iter()
            .filter(|e| e.code.contains(' '))
            .flat_map(|e| e.code.split(' '))
            .filter(|s| !s.is_empty())
            .map(|s| (s, ()))
            .collect();
        if from_spaced.is_empty() {
            return Err(
                "no space-separated code in any input, so none of them can be full pinyin. \
                 cn_dicts_common/chengyu.dict.yaml and cn_dicts_common/word.dict.yaml spell \
                 their codes as initial-only abbreviations (aabq) and are unusable here; use \
                 cn_dicts/base.dict.yaml, cn_dicts/ext.dict.yaml and cn_dicts/8105.dict.yaml."
                    .to_owned(),
            );
        }

        let mut single: Vec<&str> = entries
            .iter()
            .map(|e| e.code.as_str())
            .filter(|c| !c.contains(' '))
            .collect();
        single.sort_unstable();
        single.dedup();
        let strays: Vec<&str> = single
            .iter()
            .copied()
            .filter(|s| !from_spaced.contains_key(*s))
            .collect();
        if strays.is_empty() {
            return Ok(());
        }
        let ratio = strays.len() as f64 / single.len() as f64;
        let sample: Vec<&str> = strays.iter().take(12).copied().collect();
        if ratio > MAX_STRAY_RATIO {
            return Err(format!(
                "{:.1}% of this corpus's single-token codes ({}/{}) are not pinyin syllables \
                 found anywhere else in it, e.g. {sample:?} — an initial-only dictionary such \
                 as cn_dicts_common/chengyu.dict.yaml or cn_dicts_common/word.dict.yaml",
                ratio * 100.0,
                strays.len(),
                single.len()
            ));
        }
        eprintln!(
            "dslex: note: {} single-token code(s) occur only as whole words and are not in the \
             syllable set derived from space-separated codes, e.g. {sample:?}",
            strays.len()
        );
        Ok(())
    }

    /// Deduplicate and order entries into the layout the reader binary-searches.
    ///
    /// The deduplication key is the **(code, word) pair**, not the code. Merging
    /// by code would be catastrophic: `shi` legitimately maps to 是, 时, 事, 世,
    /// 式 … and those are the candidate list. Only genuinely repeated
    /// `(code, word)` rows — the 姓X surnames that appear in both `base` and
    /// `ext` — are collapsed, keeping the highest weight.
    pub fn prepare(mut entries: Vec<Entry>) -> Vec<Entry> {
        entries.sort_by(|a, b| a.code.cmp(&b.code).then_with(|| a.word.cmp(&b.word)));
        entries.dedup_by(|later, kept| {
            // `dedup_by` hands the arguments in *opposite* order from the slice
            // and removes the first one, so `later` is the row on its way out and
            // `kept` is the one that survives. The max weight has to land on
            // `kept`.
            if later.code == kept.code && later.word == kept.word {
                kept.weight = kept.weight.max(later.weight);
                true
            } else {
                false
            }
        });
        // Within one code, order by weight: that is exactly the candidate order
        // the reader will hand back, so the compiled file is self-consistent.
        entries.sort_by(|a, b| {
            a.code
                .cmp(&b.code)
                .then_with(|| b.weight.cmp(&a.weight))
                .then_with(|| a.word.cmp(&b.word))
        });
        entries
    }

    /// The syllable table: every distinct syllable across all codes, sorted, so
    /// the reader can membership-test it by binary search. Returns the syllable
    /// list, its NUL-terminated blob, and the u32 offset array.
    fn syllable_table(entries: &[Entry]) -> (Vec<String>, Vec<u8>, Vec<u8>) {
        let mut set: BTreeMap<&str, ()> = BTreeMap::new();
        for e in entries {
            for s in e.code.split(' ') {
                if !s.is_empty() {
                    set.insert(s, ());
                }
            }
        }
        let syllables: Vec<String> = set.into_keys().map(str::to_owned).collect();
        let mut blob = Vec::new();
        let mut offs = Vec::with_capacity(syllables.len() * 4);
        for s in &syllables {
            offs.extend_from_slice(&(blob.len() as u32).to_le_bytes());
            blob.extend_from_slice(s.as_bytes());
            blob.push(0);
        }
        (syllables, blob, offs)
    }

    /// Serialize entries to the v1 binary. See the module-level layout comment.
    pub fn compile(entries: &[Entry]) -> Vec<u8> {
        assert!(
            entries.windows(2).all(|w| w[0].code <= w[1].code),
            "entries must be sorted by code (see `prepare`)"
        );

        let (syllables, syll_blob, syll_offs) = syllable_table(entries);
        let n = entries.len();
        let mut code_blob: Vec<u8> = Vec::new();
        let mut word_blob: Vec<u8> = Vec::new();
        let mut code_offs = Vec::with_capacity(n);
        let mut word_offs = Vec::with_capacity(n);

        for e in entries {
            assert!(
                e.code.len() <= u8::MAX as usize && e.word.len() <= u8::MAX as usize,
                "entry too long for the u8 length fields: {}",
                e.code
            );
            code_offs.push(code_blob.len() as u32);
            code_blob.extend_from_slice(e.code.as_bytes());
            word_offs.push(word_blob.len() as u32);
            word_blob.extend_from_slice(e.word.as_bytes());
        }

        // Struct-of-arrays in the same layout the reader indexes: the three u32
        // arrays first (so they stay 4-byte aligned), then the two byte arrays.
        // The hot array during a binary search is `code_off`, and keeping it
        // separate from the 12-byte-wide rest is what lets the upper levels of
        // the search stay in cache.
        let b = soa_bases(n);
        let mut soa = vec![0u8; n * SOA_STRIDE];
        for i in 0..n {
            w32(&mut soa, b[0] + i * 4, code_offs[i]);
            w32(&mut soa, b[1] + i * 4, entries[i].weight);
            w32(&mut soa, b[2] + i * 4, word_offs[i]);
            soa[b[3] + i] = entries[i].code.len() as u8;
            soa[b[4] + i] = entries[i].word.len() as u8;
        }

        // Lay sections out 8-byte aligned, in the order the reader expects.
        let align8 = |n: usize| (n + 7) & !7;
        let syll_offs_at = HEADER_SIZE;
        let syll_blob_at = align8(syll_offs_at + syll_offs.len());
        let entries_at = align8(syll_blob_at + syll_blob.len());
        let code_blob_at = align8(entries_at + soa.len());
        let word_blob_at = align8(code_blob_at + code_blob.len());

        let mut out = vec![0u8; word_blob_at + word_blob.len()];
        out[syll_offs_at..syll_offs_at + syll_offs.len()].copy_from_slice(&syll_offs);
        out[syll_blob_at..syll_blob_at + syll_blob.len()].copy_from_slice(&syll_blob);
        out[entries_at..entries_at + soa.len()].copy_from_slice(&soa);
        out[code_blob_at..code_blob_at + code_blob.len()].copy_from_slice(&code_blob);
        out[word_blob_at..word_blob_at + word_blob.len()].copy_from_slice(&word_blob);

        out[H_MAGIC..H_MAGIC + 8].copy_from_slice(&MAGIC);
        w32(&mut out, H_VERSION, FORMAT_VERSION);
        w32(&mut out, H_N_SYLL, syllables.len() as u32);
        w32(&mut out, H_N_ENTRIES, entries.len() as u32);
        w64(&mut out, H_SYLL_OFFS, syll_offs_at as u64);
        w64(&mut out, H_SYLL_BLOB, syll_blob_at as u64);
        w64(&mut out, H_SYLL_BLOB_LEN, syll_blob.len() as u64);
        w64(&mut out, H_ENTRIES, entries_at as u64);
        w64(&mut out, H_CODE_BLOB, code_blob_at as u64);
        w64(&mut out, H_CODE_BLOB_LEN, code_blob.len() as u64);
        w64(&mut out, H_WORD_BLOB, word_blob_at as u64);
        w64(&mut out, H_WORD_BLOB_LEN, word_blob.len() as u64);
        out
    }

    fn w32(buf: &mut [u8], at: usize, n: u32) {
        buf[at..at + 4].copy_from_slice(&n.to_le_bytes());
    }

    fn w64(buf: &mut [u8], at: usize, n: u64) {
        buf[at..at + 8].copy_from_slice(&n.to_le_bytes());
    }
}

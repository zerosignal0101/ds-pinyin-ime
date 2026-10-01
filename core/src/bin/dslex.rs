//! `dslex` — compile rime-frost dictionaries into the flat `.lex` binary that
//! [`dsime::lexicon`] memory-maps at runtime.
//!
//! This is a build-time tool. It is never linked into `dsime.dll`; the format it
//! writes is defined in `lexicon::build` so that the writer and the reader cannot
//! drift apart.
//!
//! ```text
//! dslex <out.dslex> <in1.dict.yaml> [in2.dict.yaml ...] [--min-weight N]
//! ```
//!
//! Inputs are read in order and merged, so put the general dictionary first and
//! the specialised ones after. Duplicate `(code, word)` rows collapse to the
//! highest weight seen.

use std::io::Write;
use std::path::PathBuf;
use std::process::ExitCode;

use dsime::lexicon::build::{self, Entry};

const USAGE: &str = "\
usage: dslex <out.dslex> <in1.dict.yaml> [in2.dict.yaml ...] [--min-weight N]

Compiles rime .dict.yaml files into the dsime lexicon binary.
  --min-weight N   drop entries weighted below N (default 0; rime-frost's
                   weight is a frequency rank, not a quality flag, so this
                   removes very little and exists mostly for experiments).";

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("dslex: {e}");
            ExitCode::FAILURE
        }
    }
}

fn run() -> Result<(), String> {
    let mut out_path: Option<PathBuf> = None;
    let mut inputs: Vec<PathBuf> = Vec::new();
    let mut min_weight: u32 = 0;

    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        let arg = arg.as_str().to_owned();
        let mut value_for = |name: &str| -> Result<String, String> {
            args.next().ok_or_else(|| format!("{name} needs a value"))
        };
        match arg.as_str() {
            "-h" | "--help" => {
                println!("{USAGE}");
                return Ok(());
            }
            _ if arg == "--min-weight" => {
                min_weight = value_for("--min-weight")?
                    .parse()
                    .map_err(|_| "--min-weight needs a number".to_owned())?;
            }
            _ if let Some(v) = arg.strip_prefix("--min-weight=") => {
                min_weight = v.parse().map_err(|_| format!("bad --min-weight {v:?}"))?;
            }
            _ if arg.starts_with('-') => return Err(format!("unknown option {arg:?}\n{USAGE}")),
            _ => {
                if out_path.is_none() {
                    out_path = Some(PathBuf::from(&arg));
                } else {
                    inputs.push(PathBuf::from(arg));
                }
            }
        }
    }

    let out_path = out_path.ok_or_else(|| USAGE.to_string())?;
    if inputs.is_empty() {
        return Err(USAGE.to_string());
    }

    let mut all: Vec<Entry> = Vec::new();
    for path in &inputs {
        let name = path
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_else(|| path.display().to_string());
        let text = std::fs::read_to_string(path).map_err(|e| format!("{name}: {e}"))?;
        let entries = build::parse_dict(&name, &text)?;
        eprintln!("dslex: {name}: {} entries", entries.len());
        all.extend(entries);
    }

    if min_weight > 0 {
        let before = all.len();
        all.retain(|e| e.weight >= min_weight);
        eprintln!(
            "dslex: --min-weight {min_weight}: dropped {} of {before}",
            before - all.len()
        );
    }

    // Before anything is written: is this corpus even full pinyin? A dictionary
    // of initial-only codes compiles without complaint and then never matches.
    build::validate(&all)?;

    let all = build::prepare(all);
    let syllables = all
        .iter()
        .flat_map(|e| e.code.split(' '))
        .collect::<std::collections::BTreeSet<_>>()
        .len();
    let blob = build::compile(&all);

    if let Some(dir) = out_path.parent() {
        if !dir.as_os_str().is_empty() {
            std::fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
        }
    }
    let mut f =
        std::fs::File::create(&out_path).map_err(|e| format!("{}: {e}", out_path.display()))?;
    f.write_all(&blob)
        .map_err(|e| format!("{}: {e}", out_path.display()))?;

    eprintln!(
        "dslex: wrote {} — {} entries, {} syllables, {:.1} MB",
        out_path.display(),
        all.len(),
        syllables,
        blob.len() as f64 / (1024.0 * 1024.0)
    );
    Ok(())
}

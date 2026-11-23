//! Bounded JSON-lines diagnostic log.
//!
//! Disabled unless built with the `diagnostics` cargo feature (meson
//! `-Ddiagnostic_logging=true`). It records no audio, API keys or recognized
//! text: callers pass lengths and truncated SHA-256 hashes instead.

use std::fs::{File, OpenOptions};
use std::io::Write;
use std::path::PathBuf;
use std::sync::{Mutex, OnceLock};
use std::time::{SystemTime, UNIX_EPOCH};

use sha2::{Digest, Sha256};

pub const K_MAX_DIAGNOSTIC_LOG_BYTES: u64 = 1 << 30;

struct State {
    file: Option<File>,
    written: u64,
    stopped: bool,
}

static STATE: OnceLock<Mutex<State>> = OnceLock::new();

pub fn enabled() -> bool {
    cfg!(feature = "diagnostics")
}

/// Minimal lifecycle trace for the fcitx5 journal. No request or speech data.
pub fn trace(id: u64, stage: &str) {
    let _ = writeln!(std::io::stderr().lock(), "Ainput: id={id} stage={stage}");
    if enabled() {
        event("engine", stage, &[("recognition_id", id.to_string())]);
    }
}

/// A dictation's failure for the fcitx5 journal: the category plus the core's
/// own description, so a friendlier panel sentence never hides the raw cause
/// (a vendor error code, a transport message, a file path). Still no request
/// or speech data.
pub fn trace_failure(id: u64, error: &crate::error::Error) {
    let _ = writeln!(
        std::io::stderr().lock(),
        "Ainput: id={id} stage={} {}",
        error.category.as_str(),
        one_line(&error.detail, 200)
    );
    if enabled() {
        event(
            "engine",
            error.category.as_str(),
            &[("recognition_id", id.to_string())],
        );
    }
}

/// Folds line breaks onto one line and bounds the length at a character
/// boundary, so one event is always one journal line.
fn one_line(text: &str, limit: usize) -> String {
    let mut out: String = text
        .chars()
        .map(|ch| {
            if matches!(ch, '\n' | '\r' | '\t') {
                ' '
            } else {
                ch
            }
        })
        .collect();
    if out.chars().count() > limit {
        out = out.chars().take(limit).collect::<String>() + "…";
    }
    out
}

fn log_path() -> PathBuf {
    if let Ok(path) = std::env::var("AINPUT_DIAGNOSTIC_LOG") {
        if !path.is_empty() {
            return PathBuf::from(path);
        }
    }
    match std::env::var("HOME") {
        Ok(home) if !home.is_empty() => PathBuf::from(home)
            .join(".local/share/ainput")
            .join("diagnostic.log"),
        _ => PathBuf::from("/tmp/ainput-diagnostic.log"),
    }
}

fn state() -> &'static Mutex<State> {
    STATE.get_or_init(|| {
        if !enabled() {
            return Mutex::new(State {
                file: None,
                written: 0,
                stopped: true,
            });
        }
        let path = log_path();
        if let Some(parent) = path.parent() {
            if let Err(err) = std::fs::create_dir_all(parent) {
                eprintln!("Ainput diagnostic: cannot create {parent:?}: {err}");
                return Mutex::new(State {
                    file: None,
                    written: 0,
                    stopped: true,
                });
            }
        }
        let existing = std::fs::metadata(&path).map(|m| m.len()).unwrap_or(0);
        if existing >= K_MAX_DIAGNOSTIC_LOG_BYTES {
            return Mutex::new(State {
                file: None,
                written: existing,
                stopped: true,
            });
        }
        let file = OpenOptions::new()
            .create(true)
            .append(true)
            .open(&path)
            .ok();
        if file.is_some() {
            // The log can contain hashed identifiers; keep it private.
            use std::os::unix::fs::PermissionsExt;
            let _ = std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600));
        }
        Mutex::new(State {
            file,
            written: existing,
            stopped: false,
        })
    })
}

fn timestamp() -> String {
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default();
    let secs = now.as_secs();
    let millis = now.subsec_millis();

    // Civil time from the Unix epoch, UTC. Avoids a chrono dependency.
    let days = (secs / 86_400) as i64;
    let rem = secs % 86_400;
    let (year, month, day) = civil_from_days(days);
    format!(
        "{year:04}-{month:02}-{day:02}T{:02}:{:02}:{:02}.{millis:03}Z",
        rem / 3600,
        (rem % 3600) / 60,
        rem % 60
    )
}

/// Howard Hinnant's days-to-civil algorithm.
fn civil_from_days(z: i64) -> (i64, u32, u32) {
    let z = z + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = (z - era * 146_097) as u64;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe as i64 + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    (if m <= 2 { y + 1 } else { y }, m, d)
}

fn json_escape(value: &str, out: &mut String) {
    for ch in value.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => {
                out.push_str(&format!("\\u{:04x}", c as u32));
            }
            c => out.push(c),
        }
    }
}

/// Appends one event. Safe to call from any thread; a full log silently stops
/// accepting events (the caller sees `enabled()` true but nothing is written).
pub fn event(component: &str, name: &str, fields: &[(&str, String)]) {
    if !enabled() {
        return;
    }
    let mut line = String::with_capacity(160);
    line.push_str("{\"ts\":\"");
    line.push_str(&timestamp());
    line.push_str("\",\"pid\":");
    line.push_str(&std::process::id().to_string());
    line.push_str(",\"tid\":");
    line.push_str(&thread_tag());
    line.push_str(",\"component\":\"");
    json_escape(component, &mut line);
    line.push_str("\",\"event\":\"");
    json_escape(name, &mut line);
    line.push('"');
    for (key, value) in fields {
        line.push_str(",\"");
        json_escape(key, &mut line);
        line.push_str("\":\"");
        json_escape(value, &mut line);
        line.push('"');
    }
    line.push_str("}\n");

    let mut state = match state().lock() {
        Ok(guard) => guard,
        Err(poisoned) => poisoned.into_inner(),
    };
    if state.stopped {
        return;
    }
    if state.written + line.len() as u64 > K_MAX_DIAGNOSTIC_LOG_BYTES {
        state.stopped = true;
        state.file = None;
        eprintln!("Ainput diagnostic: log reached the 1 GiB limit, stopping");
        return;
    }
    if let Some(file) = state.file.as_mut() {
        if file.write_all(line.as_bytes()).is_ok() {
            state.written += line.len() as u64;
        } else {
            state.stopped = true;
            state.file = None;
        }
    }
}

fn thread_tag() -> String {
    use std::hash::{Hash, Hasher};
    let mut hasher = std::collections::hash_map::DefaultHasher::new();
    std::thread::current().id().hash(&mut hasher);
    hasher.finish().to_string()
}

/// Truncated hash used for paths, text and identifiers.
pub fn hash_value(value: &str) -> String {
    let digest = Sha256::digest(value.as_bytes());
    let mut out = String::with_capacity(64);
    for byte in digest.iter() {
        out.push_str(&format!("{byte:02x}"));
    }
    out[..16].to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hash_is_stable_and_short() {
        assert_eq!(hash_value("abc").len(), 16);
        assert_eq!(hash_value("abc"), hash_value("abc"));
        assert_ne!(hash_value("abc"), hash_value("abd"));
    }

    #[test]
    fn civil_from_days_matches_known_dates() {
        assert_eq!(civil_from_days(0), (1970, 1, 1));
        assert_eq!(civil_from_days(19_000), (2022, 1, 8));
    }

    #[test]
    fn failure_detail_is_one_bounded_line() {
        // Every one of \n, \r and \t becomes exactly one space.
        assert_eq!(one_line("a\nb\r\n\tc", 200), "a b   c");
        // Cut after 200 whole characters, never inside a multi-byte one.
        let long = "中".repeat(300);
        let cut = one_line(&long, 200);
        assert_eq!(cut.chars().count(), 201);
        assert!(cut.chars().take(200).all(|ch| ch == '中'));
    }
}

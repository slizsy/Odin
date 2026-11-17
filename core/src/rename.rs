// filename scrubber. IMG_20261116_104233.jpg tells people when you took it,
// John_Resume_final.pdf tells them who you are

use crate::kind::Kind;
use std::path::{Path, PathBuf};

#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum RenameMode {
    #[default]
    Keep,
    Random,  // a3f91c0b72de.jpg
    Generic, // image-001.jpg
}

fn ext_of(src: &Path) -> String {
    src.extension().map(|e| format!(".{}", e.to_string_lossy().to_ascii_lowercase())).unwrap_or_default()
}

pub(crate) fn random_stem() -> String {
    let mut b = [0u8; 6];
    if getrandom::fill(&mut b).is_err() {
        // os rng missing is basically impossible, fall back to the clock so we still get a name
        let t = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_nanos()).unwrap_or(0);
        b.copy_from_slice(&t.to_le_bytes()[..6]);
    }
    b.iter().map(|c| format!("{c:02x}")).collect()
}

// picks the output path inside `dir`. never returns something that already exists
pub fn target(src: &Path, dir: &Path, kind: Kind, mode: RenameMode) -> PathBuf {
    let ext = ext_of(src);
    match mode {
        RenameMode::Random => loop {
            let p = dir.join(format!("{}{ext}", random_stem()));
            if !p.exists() {
                return p;
            }
        },
        RenameMode::Generic => (1..).map(|i| dir.join(format!("{}-{i:03}{ext}", kind.noun()))).find(|p| !p.exists()).unwrap(),
        RenameMode::Keep => {
            let stem = src.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_else(|| "file".into());
            let orig = src.extension().map(|e| format!(".{}", e.to_string_lossy())).unwrap_or_default();
            let first = dir.join(format!("{stem}{orig}"));
            if !first.exists() {
                return first;
            }
            (2..).map(|i| dir.join(format!("{stem} ({i}){orig}"))).find(|p| !p.exists()).unwrap()
        }
    }
}

// Some(why) if the name itself gives something away
pub fn leak_in_name(name: &str) -> Option<&'static str> {
    let b = name.as_bytes();
    let digits = |s: &[u8]| s.iter().all(|c| c.is_ascii_digit());
    for i in 0..b.len() {
        // 20261116 or 2026-11-16 / 2026_11_16
        if i + 8 <= b.len() && digits(&b[i..i + 8]) && (i == 0 || !b[i - 1].is_ascii_digit()) {
            let (y, m, d) = (num(&b[i..i + 4]), num(&b[i + 4..i + 6]), num(&b[i + 6..i + 8]));
            if (1990..=2099).contains(&y) && (1..=12).contains(&m) && (1..=31).contains(&d) {
                return Some("has a date in it");
            }
        }
        if i + 10 <= b.len() && digits(&b[i..i + 4]) && matches!(b[i + 4], b'-' | b'_' | b'.') && digits(&b[i + 5..i + 7]) && b[i + 7] == b[i + 4] && digits(&b[i + 8..i + 10]) {
            let y = num(&b[i..i + 4]);
            if (1990..=2099).contains(&y) {
                return Some("has a date in it");
            }
        }
    }
    let low = name.to_ascii_lowercase();
    for (pat, why) in [
        ("whatsapp", "says which app it came from"),
        ("screenshot", "says it's a screenshot"),
        ("signal-", "says which app it came from"),
        ("telegram", "says which app it came from"),
        ("fb_img", "says which app it came from"),
        ("resume", "probably has your name next to it"),
        ("_cv", "probably has your name next to it"),
    ] {
        if low.contains(pat) {
            return Some(why);
        }
    }
    None
}

fn num(b: &[u8]) -> u32 {
    b.iter().fold(0, |a, c| a * 10 + (c - b'0') as u32)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn name_leaks() {
        assert!(leak_in_name("IMG_20261116_104233.jpg").is_some());
        assert!(leak_in_name("Screenshot 2026-11-16 at 10.42.33.png").is_some());
        assert!(leak_in_name("PXL_20250101_120000000.mp4").is_some());
        assert!(leak_in_name("cat.jpg").is_none());
        assert!(leak_in_name("report-12345678901.pdf").is_none());
    }

    #[test]
    fn targets() {
        let dir = std::env::temp_dir().join(format!("odin-rename-{}", random_stem()));
        std::fs::create_dir_all(&dir).unwrap();
        let src = Path::new("/x/IMG_20261116_104233.JPG");
        let a = target(src, &dir, Kind::Jpeg, RenameMode::Generic);
        assert_eq!(a.file_name().unwrap(), "image-001.jpg");
        std::fs::write(&a, b"x").unwrap();
        assert_eq!(target(src, &dir, Kind::Jpeg, RenameMode::Generic).file_name().unwrap(), "image-002.jpg");
        let r = target(src, &dir, Kind::Jpeg, RenameMode::Random);
        let name = r.file_name().unwrap().to_string_lossy().into_owned();
        assert!(name.len() == 16 && name.ends_with(".jpg") && !name.contains("2026"));
        assert_eq!(target(src, &dir, Kind::Jpeg, RenameMode::Keep).file_name().unwrap(), "IMG_20261116_104233.JPG");
        std::fs::remove_dir_all(&dir).unwrap();
    }
}

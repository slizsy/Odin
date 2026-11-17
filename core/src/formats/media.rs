// video + audio get cleaned by ffmpeg (stream copy, so no quality loss and it's quick).
// odin doesn't bundle ffmpeg, it just looks for one on the machine

use crate::clean::{CleanError, CleanOptions};
use crate::report::Report;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::OnceLock;

fn quiet(cmd: &mut Command) -> &mut Command {
    // don't flash a console window on windows
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x0800_0000);
    }
    cmd.stdin(Stdio::null())
}

// ODIN_FFMPEG wins, then PATH, then a copy sitting next to odin itself
pub fn ffmpeg() -> Option<&'static Path> {
    static FOUND: OnceLock<Option<PathBuf>> = OnceLock::new();
    FOUND
        .get_or_init(|| {
            let exe = if cfg!(windows) { "ffmpeg.exe" } else { "ffmpeg" };
            let mut tries: Vec<PathBuf> = vec![];
            if let Some(p) = std::env::var_os("ODIN_FFMPEG") {
                tries.push(p.into());
            }
            tries.push(exe.into());
            if let Some(dir) = std::env::current_exe().ok().and_then(|p| p.parent().map(|d| d.to_path_buf())) {
                tries.push(dir.join(exe));
            }
            tries.into_iter().find(|p| quiet(Command::new(p).arg("-version")).stdout(Stdio::null()).stderr(Stdio::null()).status().is_ok_and(|s| s.success()))
        })
        .as_deref()
}

// global tags for the formats we don't parse ourselves (mp3, flac, mkv, ...)
pub fn read_tags(rep: &mut Report, path: &Path) {
    let Some(ff) = ffmpeg() else {
        rep.note("install ffmpeg to see and clean the tags in this kind of file");
        return;
    };
    // bitexact stops ffmpeg adding its own "encoder" line to the dump
    let out = quiet(Command::new(ff).args(["-nostdin", "-v", "error", "-i"]).arg(path).args(["-fflags", "+bitexact", "-f", "ffmetadata", "-"])).stderr(Stdio::null()).output();
    let Ok(out) = out else { return };
    let text = String::from_utf8_lossy(&out.stdout);
    let mut chapters = 0;
    for line in text.lines() {
        if line.starts_with("[CHAPTER]") {
            chapters += 1;
        }
        if chapters > 0 || line.starts_with(';') || line.starts_with('[') {
            continue;
        }
        if let Some((k, v)) = line.split_once('=') {
            rep.add("tags", k, &v.replace(r"\=", "=").replace(r"\;", ";").replace(r"\#", "#").replace(r"\\", r"\"));
        }
    }
    if chapters > 0 {
        rep.add("tags", "Chapters", &format!("{chapters} chapter markers"));
    }
}

// keeps the video / audio / subtitle streams exactly as they are and drops everything
// else: container tags, per-stream tags, chapters, gps, data tracks
pub fn clean(src: &Path, dst: &Path, _o: &CleanOptions) -> Result<(), CleanError> {
    let ff = ffmpeg().ok_or(CleanError::NeedsFfmpeg)?;
    let mut cmd = Command::new(ff);
    quiet(&mut cmd).args(["-nostdin", "-hide_banner", "-v", "error", "-y", "-i"]).arg(src);
    cmd.args(["-map", "0:v?", "-map", "0:a?", "-map", "0:s?", "-c", "copy"]);
    cmd.args(["-map_metadata", "-1", "-map_metadata:s", "-1", "-map_chapters", "-1"]);
    cmd.args(["-fflags", "+bitexact", "-flags:v", "+bitexact", "-flags:a", "+bitexact"]);
    cmd.arg(dst);
    let out = cmd.stdout(Stdio::null()).output().map_err(|e| CleanError::Failed(format!("couldn't run ffmpeg: {e}")))?;
    if !out.status.success() {
        let err = String::from_utf8_lossy(&out.stderr);
        let last = err.lines().last().unwrap_or("no details").trim().to_string();
        return Err(CleanError::Failed(format!("ffmpeg gave up: {last}")));
    }
    // stream copy barely changes the size. if most of the file vanished, something
    // didn't get copied (broken input, no real streams) and the result can't be trusted
    let size = |p: &Path| std::fs::metadata(p).map(|m| m.len()).unwrap_or(0);
    if size(dst) * 2 < size(src) {
        return Err(CleanError::Failed("the cleaned copy came out way smaller than the original, so odin threw it away".into()));
    }
    Ok(())
}

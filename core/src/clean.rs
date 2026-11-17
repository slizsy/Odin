// front door for cleaning. originals are never touched: the cleaned copy is written
// to a temp file in the output folder and only renamed into place once it's done

use crate::formats::{gif, isobmff, jpeg, media, office, pdf, png, webp};
use crate::inspect::inspect;
use crate::kind::Kind;
use crate::rename::{self, RenameMode};
use crate::report::Report;
use std::fmt;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::{Path, PathBuf};

#[derive(Clone, Debug)]
pub struct CleanOptions {
    pub keep_icc: bool,         // colour profile, off = strip it too
    pub keep_orientation: bool, // leave the one exif tag that stops photos going sideways
    pub remove_comments: bool,  // comments in docs + pdfs
}

impl Default for CleanOptions {
    fn default() -> Self {
        CleanOptions { keep_icc: false, keep_orientation: true, remove_comments: true }
    }
}

#[derive(Clone, Debug)]
pub enum CleanError {
    Unsupported(String),
    NeedsFfmpeg,
    Failed(String),
}

impl fmt::Display for CleanError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            CleanError::Unsupported(s) | CleanError::Failed(s) => f.write_str(s),
            CleanError::NeedsFfmpeg => f.write_str("video and audio need ffmpeg installed"),
        }
    }
}

pub struct Outcome {
    pub out: PathBuf,
    pub before: Report,
    pub after: Report, // the cleaned file scanned again, so you can see what's left
}

impl Outcome {
    pub fn removed(&self) -> usize {
        self.before.fields.len().saturating_sub(self.after.fields.len())
    }
}

pub fn clean_bytes(d: &[u8], kind: Kind, o: &CleanOptions) -> Result<Vec<u8>, String> {
    match kind {
        Kind::Jpeg => jpeg::clean(d, o),
        Kind::Png => png::clean(d, o),
        Kind::Webp => webp::clean(d, o),
        Kind::Gif => gif::clean(d, o),
        Kind::Heif => isobmff::clean_heif(d),
        Kind::Pdf => pdf::clean(d, o),
        Kind::Docx | Kind::Xlsx | Kind::Pptx | Kind::Odf => office::clean(d, kind, o),
        k => Err(format!("odin can't rewrite {} files yet", k.label())),
    }
}

pub fn clean_file(src: &Path, out_dir: &Path, mode: RenameMode, o: &CleanOptions) -> Result<Outcome, CleanError> {
    let before = inspect(src);
    let kind = before.kind;
    if !kind.can_clean() {
        return Err(CleanError::Unsupported(format!("odin can't rewrite {} files yet", kind.label())));
    }
    if kind.is_media() && media::ffmpeg().is_none() {
        return Err(CleanError::NeedsFfmpeg);
    }
    std::fs::create_dir_all(out_dir).map_err(|e| CleanError::Failed(format!("couldn't make the output folder: {e}")))?;
    let dst = rename::target(src, out_dir, kind, mode);
    // ffmpeg picks the container from the extension, so the temp file keeps it
    let ext = dst.extension().map(|e| format!(".{}", e.to_string_lossy())).unwrap_or_default();
    let tmp = out_dir.join(format!(".odin-{}{ext}", rename::random_stem()));

    let work = || -> Result<(), CleanError> {
        if kind.is_media() {
            return media::clean(src, &tmp, o);
        }
        let data = std::fs::read(src).map_err(|e| CleanError::Failed(format!("couldn't read it: {e}")))?;
        let clean = clean_bytes(&data, kind, o).map_err(CleanError::Failed)?;
        std::fs::write(&tmp, clean).map_err(|e| CleanError::Failed(format!("couldn't write the cleaned copy: {e}")))
    };
    let res = catch_unwind(AssertUnwindSafe(work)).unwrap_or_else(|_| Err(CleanError::Failed("this file crashed the cleaner, it's probably corrupt".into())));
    let res = res.and_then(|_| std::fs::rename(&tmp, &dst).map_err(|e| CleanError::Failed(format!("couldn't save the cleaned copy: {e}"))));
    if let Err(e) = res {
        let _ = std::fs::remove_file(&tmp);
        return Err(e);
    }
    let after = inspect(&dst);
    Ok(Outcome { out: dst, before, after })
}

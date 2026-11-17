// front door for reading: open a file, work out what it is, hand it to the right parser

use crate::formats::{exif, gif, isobmff, jpeg, media, office, pdf, png, webp, xmp};
use crate::kind::Kind;
use crate::report::{Report, Risk};
use crate::{classify, rename};
use std::fs::File;
use std::io::Read;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::Path;

// anything that isn't video/audio gets read whole, so cap it
const MAX_IN_MEMORY: u64 = 1 << 30;

pub fn inspect(path: &Path) -> Report {
    // a parser blowing up on a mangled file shouldn't take the app down with it
    catch_unwind(AssertUnwindSafe(|| inspect_inner(path))).unwrap_or_else(|_| {
        let mut r = Report::new(Kind::Unknown, 0);
        r.note("this file crashed the parser, it's probably corrupt");
        r
    })
}

fn inspect_inner(path: &Path) -> Report {
    let mut f = match File::open(path) {
        Ok(f) => f,
        Err(e) => {
            let mut r = Report::new(Kind::Unknown, 0);
            r.note(format!("couldn't open it: {e}"));
            return r;
        }
    };
    let len = f.metadata().map(|m| m.len()).unwrap_or(0);
    let mut head = Vec::with_capacity(4096);
    let _ = (&mut f).take(4096).read_to_end(&mut head);
    let mut kind = Kind::sniff(&head);

    let mut rep = if kind.is_media() {
        let mut rep = Report::new(kind, len);
        if kind == Kind::Mp4 {
            isobmff::read_mp4_file(&mut rep, &mut f, len);
        } else {
            media::read_tags(&mut rep, path);
        }
        rep
    } else if len > MAX_IN_MEMORY {
        let mut rep = Report::new(kind, len);
        rep.note("file is over 1 gb, too big to scan");
        rep
    } else {
        let mut data = head;
        let _ = f.read_to_end(&mut data);
        if data.starts_with(b"PK\x03\x04") {
            kind = Kind::sniff_zip(&data);
        }
        parse(&data, kind)
    };
    rep.size = len;
    if let Some(name) = path.file_name().map(|n| n.to_string_lossy().into_owned()) {
        if let Some(why) = rename::leak_in_name(&name) {
            rep.find(Risk::Low, "filename", format!("\"{name}\" {why}"));
        }
    }
    classify::finish(&mut rep);
    rep
}

// same thing for bytes already in memory (pictures inside office docs, tests)
pub fn inspect_bytes(d: &[u8], kind: Kind) -> Report {
    let mut rep = parse(d, kind);
    classify::finish(&mut rep);
    rep
}

fn parse(d: &[u8], kind: Kind) -> Report {
    let mut rep = Report::new(kind, d.len() as u64);
    match kind {
        Kind::Jpeg => jpeg::read(&mut rep, d),
        Kind::Png => png::read(&mut rep, d),
        Kind::Webp => webp::read(&mut rep, d),
        Kind::Gif => gif::read(&mut rep, d),
        Kind::Heif => isobmff::read_heif(&mut rep, d),
        Kind::Pdf => pdf::read(&mut rep, d),
        Kind::Docx | Kind::Xlsx | Kind::Pptx | Kind::Odf => office::read(&mut rep, d, kind),
        Kind::Tiff => {
            exif::read_tiff(&mut rep, d, "EXIF");
            loose_xmp(&mut rep, d);
        }
        Kind::Unknown => loose_xmp(&mut rep, d),
        _ => {}
    }
    rep
}

// xmp packets are plain text, so they can be fished out of formats we don't know (psd, svg, ai...)
fn loose_xmp(rep: &mut Report, d: &[u8]) {
    let needle = b"<x:xmpmeta";
    if let Some(at) = d.windows(needle.len()).position(|w| w == needle) {
        let end = (at + (4 << 20)).min(d.len());
        xmp::read(rep, &d[at..end], "XMP");
    }
}

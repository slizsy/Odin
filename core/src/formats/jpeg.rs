// jpeg = a row of marker segments, then the compressed scan data. metadata all
// lives in APPn / COM segments, so cleaning is just picking which ones to keep

use super::{be16, be32, exif, icc, iptc, jumbf, xmp};
use crate::clean::CleanOptions;
use crate::report::{text_of, Group, Report, Risk, Thumb};

#[derive(Clone, Copy, Debug)]
pub enum Piece {
    Seg { marker: u8, start: usize, end: usize }, // start is the 0xFF, end is one past the payload
    Scan { start: usize, end: usize },            // entropy coded image data
}

pub struct Layout {
    pub pieces: Vec<Piece>,
    pub trailer_at: usize, // first byte after the end-of-image marker
}

pub fn layout(d: &[u8]) -> Option<Layout> {
    if !d.starts_with(&[0xFF, 0xD8]) {
        return None;
    }
    let mut pieces = vec![];
    let mut p = 2;
    loop {
        if p >= d.len() {
            return Some(Layout { pieces, trailer_at: d.len() });
        }
        if d[p] != 0xFF {
            // junk between segments, treat the rest as image data and stop
            pieces.push(Piece::Scan { start: p, end: d.len() });
            return Some(Layout { pieces, trailer_at: d.len() });
        }
        let mut q = p;
        while q < d.len() && d[q] == 0xFF {
            q += 1; // fill bytes
        }
        let Some(&m) = d.get(q) else { return Some(Layout { pieces, trailer_at: d.len() }) };
        match m {
            0xD9 => return Some(Layout { pieces, trailer_at: q + 1 }),
            0x01 | 0xD0..=0xD7 => {
                pieces.push(Piece::Seg { marker: m, start: q - 1, end: q + 1 });
                p = q + 1;
            }
            _ => {
                let len = be16(d, q + 1).unwrap_or(0) as usize;
                if len < 2 {
                    return Some(Layout { pieces, trailer_at: d.len() });
                }
                let end = (q + 1 + len).min(d.len());
                pieces.push(Piece::Seg { marker: m, start: q - 1, end });
                p = end;
                if m == 0xDA {
                    // scan data runs until a marker that isn't a restart or a stuffed 0xFF00
                    let mut i = p;
                    while i < d.len() {
                        match d[i..].iter().position(|&b| b == 0xFF) {
                            None => i = d.len(),
                            Some(k) => {
                                i += k;
                                match d.get(i + 1) {
                                    Some(0x00) | Some(0xD0..=0xD7) => i += 2,
                                    Some(0xFF) => i += 1,
                                    Some(_) => break,
                                    None => i = d.len(),
                                }
                            }
                        }
                    }
                    pieces.push(Piece::Scan { start: p, end: i });
                    p = i;
                }
            }
        }
    }
}

fn body(d: &[u8], start: usize, end: usize) -> &[u8] {
    &d[(start + 4).min(end)..end]
}

const XMP_NS: &[u8] = b"http://ns.adobe.com/xap/1.0/\0";
const XMP_EXT_NS: &[u8] = b"http://ns.adobe.com/xmp/extension/\0";

pub fn read(rep: &mut Report, d: &[u8]) {
    let Some(l) = layout(d) else {
        rep.note("jpeg looks damaged");
        return;
    };
    let mut ext_xmp: Vec<(u32, &[u8])> = vec![];
    let mut icc_parts: Vec<(u8, &[u8])> = vec![];
    let mut jumb: Vec<u8> = vec![];
    for piece in &l.pieces {
        let Piece::Seg { marker, start, end } = *piece else { continue };
        let b = body(d, start, end);
        match marker {
            0xE0 => {
                if b.starts_with(b"JFXX\0") || (b.starts_with(b"JFIF\0") && b.len() >= 14 && b[12] != 0 && b[13] != 0) {
                    rep.add_as(Group::Other, Risk::Low, "JFIF", "Embedded thumbnail", "small preview image in the jfif header");
                }
            }
            0xE1 => {
                if let Some(t) = b.strip_prefix(b"Exif\0\0") {
                    exif::read_tiff(rep, t, "EXIF");
                } else if let Some(x) = b.strip_prefix(XMP_NS) {
                    xmp::read(rep, x, "XMP");
                } else if let Some(x) = b.strip_prefix(XMP_EXT_NS) {
                    // 32 char guid, total length, offset of this piece
                    if x.len() > 40 {
                        ext_xmp.push((be32(x, 36).unwrap_or(0), &x[40..]));
                    }
                } else {
                    other(rep, marker, b);
                }
            }
            0xE2 => {
                if let Some(i) = b.strip_prefix(b"ICC_PROFILE\0") {
                    if i.len() > 2 {
                        icc_parts.push((i[0], &i[2..]));
                    }
                } else if b.starts_with(b"MPF\0") {
                    rep.add_as(Group::Other, Risk::Low, "MPF", "Extra images", "index of more images stored after the main one (previews, depth or hdr maps)");
                } else {
                    other(rep, marker, b);
                }
            }
            0xEB => jumb.extend_from_slice(b),
            0xED => match b.strip_prefix(b"Photoshop 3.0\0") {
                Some(i) => iptc::read_irb(rep, i),
                None => other(rep, marker, b),
            },
            0xEC if b.starts_with(b"Ducky") => rep.add("JPEG", "Software", "photoshop save for web (ducky tag)"),
            0xEE if b.starts_with(b"Adobe") => {} // colour transform flag, the decoder needs it
            0xFE => rep.add("JPEG", "Comment", &text_of(b)),
            0xE3..=0xEF => other(rep, marker, b),
            _ => {}
        }
    }
    if !ext_xmp.is_empty() {
        ext_xmp.sort_by_key(|p| p.0);
        let all: Vec<u8> = ext_xmp.iter().flat_map(|p| p.1.iter().copied()).collect();
        xmp::read(rep, &all, "XMP (extended)");
    }
    if !icc_parts.is_empty() {
        icc_parts.sort_by_key(|p| p.0);
        let all: Vec<u8> = icc_parts.iter().flat_map(|p| p.1.iter().copied()).collect();
        rep.add("ICC", "Color profile", &icc::name(&all).unwrap_or_else(|| "unnamed profile".into()));
    }
    if !jumb.is_empty() {
        if jumbf::looks_like_c2pa(&jumb) {
            jumbf::describe(rep, &jumb, "jpeg app11 segments");
        } else {
            rep.add_as(Group::Other, Risk::Low, "JUMBF", "Embedded data box", &format!("{} bytes", jumb.len()));
        }
    }

    // whatever is glued on after the image ends
    let tail = &d[l.trailer_at..];
    if tail.iter().any(|&b| b != 0 && !b.is_ascii_whitespace()) {
        let n = tail.len();
        if tail.windows(4).take(4096).any(|w| w == b"ftyp") || tail.windows(11).any(|w| w == b"MotionPhoto") {
            rep.add_as(Group::Other, Risk::High, "JPEG", "Hidden data", &format!("{n} bytes after the image: looks like a video clip (motion / live photo)"));
        } else if let Some(at) = tail.windows(3).position(|w| w == [0xFF, 0xD8, 0xFF]) {
            rep.add_as(Group::Other, Risk::Low, "JPEG", "Hidden data", &format!("{n} bytes after the image: more jpegs (previews, depth or hdr maps)"));
            let extra = &tail[at..];
            let end = layout(extra).map(|l| l.trailer_at).unwrap_or(extra.len());
            if end < 32 << 20 {
                rep.thumbs.push(Thumb { label: "image stored after the main one".into(), data: extra[..end].to_vec() });
            }
        } else {
            rep.add_as(Group::Other, Risk::Low, "JPEG", "Hidden data", &format!("{n} bytes tacked on after the image"));
        }
    }
}

fn other(rep: &mut Report, marker: u8, b: &[u8]) {
    let id: String = b.iter().take(24).take_while(|c| c.is_ascii_graphic() || **c == b' ').map(|&c| c as char).collect();
    let id = if id.is_empty() { "unlabeled".to_string() } else { id };
    rep.add_as(Group::Other, Risk::Low, "JPEG", &format!("APP{} segment", marker - 0xE0), &format!("{id} ({} bytes)", b.len()));
}

fn push_seg(out: &mut Vec<u8>, marker: u8, payload: &[u8]) {
    out.extend_from_slice(&[0xFF, marker]);
    out.extend_from_slice(&((payload.len() + 2) as u16).to_be_bytes());
    out.extend_from_slice(payload);
}

pub fn clean(d: &[u8], o: &CleanOptions) -> Result<Vec<u8>, String> {
    let l = layout(d).ok_or("not a jpeg")?;
    let segs = || l.pieces.iter().filter_map(|p| if let Piece::Seg { marker, start, end } = *p { Some((marker, start, end)) } else { None });

    let mut out = Vec::with_capacity(d.len());
    out.extend_from_slice(&[0xFF, 0xD8]);

    // jfif header goes first if the file had one (minus any thumbnail packed inside it)
    if let Some((_, s, e)) = segs().find(|&(m, s, e)| m == 0xE0 && body(d, s, e).starts_with(b"JFIF\0")) {
        let b = body(d, s, e);
        if b.len() >= 14 {
            let mut head = b[..12].to_vec();
            head.extend_from_slice(&[0, 0]);
            push_seg(&mut out, 0xE0, &head);
        }
    }
    // without the orientation tag phone photos come out sideways, so that one tag stays
    if o.keep_orientation {
        let orient = segs()
            .filter(|&(m, _, _)| m == 0xE1)
            .find_map(|(_, s, e)| body(d, s, e).strip_prefix(b"Exif\0\0").and_then(exif::orientation))
            .filter(|&v| v != 1);
        if orient.is_some() {
            let mut p = b"Exif\0\0".to_vec();
            p.extend_from_slice(&exif::minimal_tiff(orient));
            push_seg(&mut out, 0xE1, &p);
        }
    }
    for piece in &l.pieces {
        match *piece {
            Piece::Scan { start, end } => out.extend_from_slice(&d[start..end]),
            Piece::Seg { marker, start, end } => {
                let b = body(d, start, end);
                let keep = match marker {
                    0xE2 => o.keep_icc && b.starts_with(b"ICC_PROFILE\0"),
                    0xEE => b.starts_with(b"Adobe"),
                    0xE0..=0xEF | 0xFE => false,
                    _ => true,
                };
                if keep {
                    out.extend_from_slice(&d[start..end]);
                }
            }
        }
    }
    out.extend_from_slice(&[0xFF, 0xD9]);
    Ok(out)
}

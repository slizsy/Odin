// png is a list of chunks. pixels live in the critical ones (IHDR, PLTE, IDAT, IEND),
// everything personal sits in text / exif / time chunks that are safe to drop

use super::{be16, be32, exif, icc, jumbf, xmp};
use crate::clean::CleanOptions;
use crate::report::{text_of, Group, Report, Risk};
use flate2::read::ZlibDecoder;
use flate2::Crc;
use std::io::Read;

pub const SIG: &[u8] = b"\x89PNG\r\n\x1a\n";

pub struct Chunk<'a> {
    pub ty: [u8; 4],
    pub data: &'a [u8],
    pub raw: &'a [u8], // length + type + data + crc
}

// returns the chunks and where the file stops being png
pub fn chunks(d: &[u8]) -> Option<(Vec<Chunk<'_>>, usize)> {
    if !d.starts_with(SIG) {
        return None;
    }
    let mut out = vec![];
    let mut p = SIG.len();
    while let Some(len) = be32(d, p) {
        let len = len as usize;
        let end = p.checked_add(12)?.checked_add(len)?;
        if end > d.len() {
            break;
        }
        let ty: [u8; 4] = d[p + 4..p + 8].try_into().ok()?;
        out.push(Chunk { ty, data: &d[p + 8..p + 8 + len], raw: &d[p..end] });
        p = end;
        if &ty == b"IEND" {
            break;
        }
    }
    Some((out, p))
}

// ancillary chunks that only affect how the picture renders
const RENDER: &[&[u8; 4]] = &[
    b"tRNS", b"gAMA", b"cHRM", b"sRGB", b"sBIT", b"bKGD", b"pHYs", b"hIST", b"sPLT", b"acTL", b"fcTL", b"fdAT", b"cICP",
    b"mDCV", b"cLLI",
];

fn inflate(z: &[u8]) -> Vec<u8> {
    let mut out = vec![];
    let _ = ZlibDecoder::new(z).take(16 << 20).read_to_end(&mut out);
    out
}

fn split0(b: &[u8]) -> (&[u8], &[u8]) {
    match b.iter().position(|&c| c == 0) {
        Some(i) => (&b[..i], &b[i + 1..]),
        None => (b, &[]),
    }
}

pub fn read(rep: &mut Report, d: &[u8]) {
    let Some((list, end)) = chunks(d) else {
        rep.note("png looks damaged");
        return;
    };
    for c in &list {
        match &c.ty {
            b"tEXt" => {
                let (k, v) = split0(c.data);
                text(rep, &text_of(k), v);
            }
            b"zTXt" => {
                let (k, v) = split0(c.data);
                text(rep, &text_of(k), &inflate(v.get(1..).unwrap_or(&[])));
            }
            b"iTXt" => {
                let (k, rest) = split0(c.data);
                if rest.len() >= 2 {
                    let packed = rest[0] == 1;
                    let (_lang, rest) = split0(&rest[2..]);
                    let (_tkey, v) = split0(rest);
                    if packed {
                        text(rep, &text_of(k), &inflate(v));
                    } else {
                        text(rep, &text_of(k), v);
                    }
                }
            }
            b"eXIf" => exif::read_tiff(rep, c.data, "EXIF"),
            b"tIME" if c.data.len() >= 7 => {
                let t = c.data;
                let stamp = format!("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", be16(t, 0).unwrap_or(0), t[2], t[3], t[4], t[5], t[6]);
                rep.add("PNG", "Modify date", &stamp);
            }
            b"caBX" => jumbf::describe(rep, c.data, "png caBX chunk"),
            b"iCCP" => {
                let (k, v) = split0(c.data);
                let name = icc::name(&inflate(v.get(1..).unwrap_or(&[]))).unwrap_or_else(|| text_of(k));
                rep.add("ICC", "Color profile", &name);
            }
            ty => {
                let critical = ty[0].is_ascii_uppercase();
                if !critical && !RENDER.contains(&ty) {
                    rep.add_as(Group::Other, Risk::Low, "PNG", "Private chunk", &format!("{} ({} bytes)", text_of(ty), c.data.len()));
                }
            }
        }
    }
    let tail = d.len() - end;
    if d[end..].iter().any(|&b| b != 0 && !b.is_ascii_whitespace()) {
        rep.add_as(Group::Other, Risk::Low, "PNG", "Hidden data", &format!("{tail} bytes tacked on after the image"));
    }
}

fn text(rep: &mut Report, key: &str, v: &[u8]) {
    if key == "XML:com.adobe.xmp" {
        return xmp::read(rep, v, "XMP");
    }
    // imagemagick stores exif as hex text: "\nexif\n   1234\n4578696600..."
    if key.starts_with("Raw profile type") {
        let body = text_of(v);
        let hex: String = body.lines().skip(3).flat_map(|l| l.chars()).filter(|c| c.is_ascii_hexdigit()).collect();
        let bytes: Vec<u8> = (0..hex.len() / 2).filter_map(|i| u8::from_str_radix(&hex[i * 2..i * 2 + 2], 16).ok()).collect();
        if let Some(t) = bytes.strip_prefix(b"Exif\0\0") {
            return exif::read_tiff(rep, t, "EXIF (raw profile)");
        }
        return rep.add_as(Group::Other, Risk::Low, "PNG text", key, &format!("embedded profile ({} bytes)", bytes.len()));
    }
    rep.add("PNG text", key, &text_of(v));
}

pub fn make_chunk(ty: &[u8; 4], data: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(data.len() + 12);
    out.extend_from_slice(&(data.len() as u32).to_be_bytes());
    out.extend_from_slice(ty);
    out.extend_from_slice(data);
    let mut crc = Crc::new();
    crc.update(ty);
    crc.update(data);
    out.extend_from_slice(&crc.sum().to_be_bytes());
    out
}

pub fn clean(d: &[u8], o: &CleanOptions) -> Result<Vec<u8>, String> {
    let (list, _) = chunks(d).ok_or("not a png")?;
    if !list.iter().any(|c| &c.ty == b"IEND") {
        return Err("png is cut short".into());
    }
    let orient = if o.keep_orientation {
        list.iter().find(|c| &c.ty == b"eXIf").and_then(|c| exif::orientation(c.data)).filter(|&v| v != 1)
    } else {
        None
    };
    let mut out = Vec::with_capacity(d.len());
    out.extend_from_slice(SIG);
    let mut exif_done = orient.is_none();
    for c in &list {
        let critical = c.ty[0].is_ascii_uppercase();
        let keep = critical || RENDER.contains(&&c.ty) || (o.keep_icc && &c.ty == b"iCCP");
        if &c.ty == b"IDAT" && !exif_done {
            out.extend_from_slice(&make_chunk(b"eXIf", &exif::minimal_tiff(orient)));
            exif_done = true;
        }
        if keep {
            out.extend_from_slice(c.raw);
        }
    }
    Ok(out)
}

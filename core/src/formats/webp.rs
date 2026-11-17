// webp is a riff file: tagged chunks, each padded to an even size

use super::{exif, icc, jumbf, le32, xmp};
use crate::clean::CleanOptions;
use crate::report::{text_of, Group, Report, Risk};

pub fn chunks(d: &[u8]) -> Option<Vec<([u8; 4], &[u8])>> {
    if d.len() < 12 || &d[..4] != b"RIFF" || &d[8..12] != b"WEBP" {
        return None;
    }
    let mut out = vec![];
    let mut p = 12;
    while let Some(size) = le32(d, p + 4) {
        let size = size as usize;
        let end = p.checked_add(8)?.checked_add(size)?;
        if end > d.len() {
            break;
        }
        out.push((d[p..p + 4].try_into().ok()?, &d[p + 8..end]));
        p = end + (size & 1);
    }
    Some(out)
}

fn tiff_of(b: &[u8]) -> &[u8] {
    b.strip_prefix(b"Exif\0\0").unwrap_or(b)
}

const PIXELS: &[&[u8; 4]] = &[b"VP8 ", b"VP8L", b"VP8X", b"ALPH", b"ANIM", b"ANMF"];

pub fn read(rep: &mut Report, d: &[u8]) {
    let Some(list) = chunks(d) else {
        rep.note("webp looks damaged");
        return;
    };
    for (ty, data) in list {
        match &ty {
            b"EXIF" => exif::read_tiff(rep, tiff_of(data), "EXIF"),
            b"XMP " => xmp::read(rep, data, "XMP"),
            b"ICCP" => rep.add("ICC", "Color profile", &icc::name(data).unwrap_or_else(|| "unnamed profile".into())),
            b"C2PA" => jumbf::describe(rep, data, "webp C2PA chunk"),
            t if PIXELS.contains(&t) => {}
            t => rep.add_as(Group::Other, Risk::Low, "WebP", "Private chunk", &format!("{} ({} bytes)", text_of(t), data.len())),
        }
    }
}

pub fn clean(d: &[u8], o: &CleanOptions) -> Result<Vec<u8>, String> {
    let list = chunks(d).ok_or("not a webp")?;
    let orient = if o.keep_orientation {
        list.iter().find(|(t, _)| t == b"EXIF").and_then(|(_, b)| exif::orientation(tiff_of(b))).filter(|&v| v != 1)
    } else {
        None
    };
    let has_vp8x = list.iter().any(|(t, _)| t == b"VP8X");
    let tiff = exif::minimal_tiff(orient);
    let mut body = b"WEBP".to_vec();
    let mut put = |ty: &[u8; 4], data: &[u8]| {
        body.extend_from_slice(ty);
        body.extend_from_slice(&(data.len() as u32).to_le_bytes());
        body.extend_from_slice(data);
        if data.len() & 1 == 1 {
            body.push(0);
        }
    };
    for (ty, data) in &list {
        match ty {
            b"VP8X" if !data.is_empty() => {
                // flag bits: 0x20 icc, 0x08 exif, 0x04 xmp
                let mut v = data.to_vec();
                v[0] &= !0x0C;
                if !o.keep_icc {
                    v[0] &= !0x20;
                }
                if orient.is_some() {
                    v[0] |= 0x08;
                }
                put(ty, &v);
            }
            b"ICCP" if !o.keep_icc => {}
            t if PIXELS.contains(&t) || t == b"ICCP" => put(ty, data),
            _ => {}
        }
    }
    // exif can only exist in the extended layout, and it goes after the pixels
    if orient.is_some() && has_vp8x {
        put(b"EXIF", &tiff);
    }
    let mut out = b"RIFF".to_vec();
    out.extend_from_slice(&(body.len() as u32).to_le_bytes());
    out.extend_from_slice(&body);
    Ok(out)
}

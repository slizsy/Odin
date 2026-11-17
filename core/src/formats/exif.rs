// exif lives in a tiff block. kamadak-exif does the tag decoding for the viewer,
// the bits the cleaners need (orientation, a blank block) are done by hand below

use crate::report::{text_of, Gps, Report, Thumb};
use exif::{In, Reader, Tag, Value};

pub fn read_tiff(rep: &mut Report, tiff: &[u8], source: &str) {
    let mut reader = Reader::new();
    reader.continue_on_error(true);
    let ex = match reader.read_raw(tiff.to_vec()).or_else(|e| e.distill_partial_result(|_| {})) {
        Ok(ex) => ex,
        Err(_) => {
            rep.note(format!("{source} block is damaged, couldn't read it"));
            return;
        }
    };
    for f in ex.fields() {
        if matches!(f.tag, Tag::JPEGInterchangeFormat | Tag::JPEGInterchangeFormatLength | Tag::StripOffsets | Tag::StripByteCounts) {
            continue;
        }
        let num = f.tag.number();
        let mut key = match num {
            0x9c9b => "XPTitle".to_string(),
            0x9c9c => "XPComment".to_string(),
            0x9c9d => "XPAuthor".to_string(),
            0x9c9e => "XPKeywords".to_string(),
            0x9c9f => "XPSubject".to_string(),
            0xc62f => "CameraSerialNumber".to_string(),
            _ if f.tag == Tag::MakerNote => "Maker notes".to_string(),
            _ if f.tag.description().is_some() => f.tag.to_string(),
            _ => format!("tag 0x{num:04x}"),
        };
        if f.ifd_num == In::THUMBNAIL {
            key = format!("Thumbnail {key}");
        }
        let value = match &f.value {
            _ if f.tag == Tag::MakerNote => format!("{} bytes of camera-specific data (often hides serial numbers)", raw_len(&f.value)),
            Value::Byte(b) if (0x9c9b..=0x9c9f).contains(&num) => utf16le(b),
            Value::Undefined(b, _) if f.tag == Tag::UserComment => user_comment(b),
            Value::Ascii(v) => v.iter().map(|s| text_of(s)).filter(|s| !s.trim().is_empty()).collect::<Vec<_>>().join(", "),
            Value::Undefined(b, _) | Value::Byte(b) if b.len() > 48 => format!("({} bytes)", b.len()),
            _ => f.display_value().with_unit(&ex).to_string(),
        };
        rep.add(source, &key, &value);
    }

    // gps as plain decimal degrees
    if let (Some(lat), Some(lon)) = (coord(&ex, Tag::GPSLatitude, Tag::GPSLatitudeRef), coord(&ex, Tag::GPSLongitude, Tag::GPSLongitudeRef)) {
        if lat != 0.0 || lon != 0.0 {
            let num = |t| ex.get_field(t, In::PRIMARY).and_then(|f| rational(&f.value));
            let below = ex.get_field(Tag::GPSAltitudeRef, In::PRIMARY).and_then(|f| f.value.get_uint(0)) == Some(1);
            rep.gps.push(Gps {
                lat,
                lon,
                alt: num(Tag::GPSAltitude).map(|a| if below { -a } else { a }),
                accuracy_m: num(Tag::GPSHPositioningError),
                source: source.to_string(),
            });
        }
    }

    // the little preview jpeg cameras tuck into ifd1
    let uint = |t| ex.get_field(t, In::THUMBNAIL).and_then(|f| f.value.get_uint(0)).map(|v| v as usize);
    if let (Some(off), Some(len)) = (uint(Tag::JPEGInterchangeFormat), uint(Tag::JPEGInterchangeFormatLength)) {
        if let Some(jpg) = ex.buf().get(off..off.saturating_add(len)) {
            if jpg.starts_with(&[0xFF, 0xD8]) {
                rep.thumbs.push(Thumb { label: format!("{source} thumbnail"), data: jpg.to_vec() });
            }
        }
    }
}

fn raw_len(v: &Value) -> usize {
    match v {
        Value::Undefined(b, _) | Value::Byte(b) => b.len(),
        _ => 0,
    }
}

fn rational(v: &Value) -> Option<f64> {
    match v {
        Value::Rational(r) => r.first().map(|x| x.to_f64()).filter(|x| x.is_finite()),
        _ => None,
    }
}

fn coord(ex: &exif::Exif, tag: Tag, reftag: Tag) -> Option<f64> {
    let Value::Rational(v) = &ex.get_field(tag, In::PRIMARY)?.value else { return None };
    if v.len() < 3 {
        return None;
    }
    let d = v[0].to_f64() + v[1].to_f64() / 60.0 + v[2].to_f64() / 3600.0;
    let r = match &ex.get_field(reftag, In::PRIMARY)?.value {
        Value::Ascii(a) => a.first().and_then(|s| s.first().copied()),
        _ => None,
    };
    let d = if matches!(r, Some(b'S' | b'W' | b's' | b'w')) { -d } else { d };
    (d.is_finite() && d.abs() <= 180.0).then_some(d)
}

fn utf16le(b: &[u8]) -> String {
    let u: Vec<u16> = b.chunks_exact(2).map(|c| u16::from_le_bytes([c[0], c[1]])).take_while(|&c| c != 0).collect();
    String::from_utf16_lossy(&u)
}

// 8 byte charset marker, then the text
fn user_comment(b: &[u8]) -> String {
    if b.len() < 8 {
        return String::new();
    }
    let (code, body) = b.split_at(8);
    if code.starts_with(b"UNICODE") {
        // spec says utf16, plenty of writers do it big endian, some little. pick whichever has fewer weird chars
        let dec = |le: bool| -> String {
            let u: Vec<u16> = body.chunks_exact(2).map(|c| if le { u16::from_le_bytes([c[0], c[1]]) } else { u16::from_be_bytes([c[0], c[1]]) }).collect();
            String::from_utf16_lossy(&u)
        };
        let (a, z) = (dec(false), dec(true));
        let ascii = |s: &str| s.chars().filter(|c| c.is_ascii()).count();
        return if ascii(&a) >= ascii(&z) { a } else { z };
    }
    text_of(body)
}

// ---- hand rolled bits for the cleaners ----

// orientation tag (0x0112) out of ifd0, so cleaned photos don't end up sideways
pub fn orientation(tiff: &[u8]) -> Option<u16> {
    let le = match tiff.get(..4)? {
        b"II*\0" => true,
        b"MM\0*" => false,
        _ => return None,
    };
    let u16at = |i: usize| -> Option<u16> {
        let b: [u8; 2] = tiff.get(i..i + 2)?.try_into().ok()?;
        Some(if le { u16::from_le_bytes(b) } else { u16::from_be_bytes(b) })
    };
    let u32at = |i: usize| -> Option<u32> {
        let b: [u8; 4] = tiff.get(i..i + 4)?.try_into().ok()?;
        Some(if le { u32::from_le_bytes(b) } else { u32::from_be_bytes(b) })
    };
    let ifd = u32at(4)? as usize;
    let n = u16at(ifd)? as usize;
    for i in 0..n {
        let e = ifd + 2 + i * 12;
        if u16at(e)? == 0x0112 && u16at(e + 2)? == 3 {
            let o = u16at(e + 8)?;
            return (1..=8).contains(&o).then_some(o);
        }
    }
    None
}

// a tiff block holding just the orientation tag (or nothing at all when o is None)
pub fn minimal_tiff(o: Option<u16>) -> Vec<u8> {
    let mut t = b"II*\0\x08\0\0\0".to_vec();
    match o {
        Some(o) => {
            t.extend_from_slice(&1u16.to_le_bytes());
            t.extend_from_slice(&[0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00]);
            t.extend_from_slice(&o.to_le_bytes());
            t.extend_from_slice(&[0, 0]);
        }
        None => t.extend_from_slice(&0u16.to_le_bytes()),
    }
    t.extend_from_slice(&0u32.to_le_bytes());
    t
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn orientation_roundtrip() {
        assert_eq!(orientation(&minimal_tiff(Some(6))), Some(6));
        assert_eq!(orientation(&minimal_tiff(None)), None);
        assert_eq!(orientation(b"garbage"), None);
    }
}

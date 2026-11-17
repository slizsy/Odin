// the box format behind heic / avif (stills) and mp4 / mov / m4a (video + audio)

use super::{be16, be32, be64, exif, jumbf, xmp};
use crate::report::{text_of, Gps, Group, Report, Risk};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};

const XMP_UUID: [u8; 16] = [0xbe, 0x7a, 0xcf, 0xcb, 0x97, 0xa9, 0x42, 0xe8, 0x9c, 0x71, 0x99, 0x94, 0x91, 0xe3, 0xaf, 0xac];

#[derive(Clone, Copy)]
pub struct Bx {
    pub ty: [u8; 4],
    pub start: usize,
    pub body: usize,
    pub end: usize,
}

pub fn boxes(d: &[u8], from: usize, to: usize) -> Vec<Bx> {
    let mut out = vec![];
    let mut p = from;
    while p + 8 <= to {
        let Some(size32) = be32(d, p) else { break };
        let ty: [u8; 4] = d[p + 4..p + 8].try_into().unwrap();
        let (hdr, size) = match size32 {
            1 => (16, be64(d, p + 8).unwrap_or(0)),
            0 => (8, (to - p) as u64),
            n => (8, n as u64),
        };
        let Some(end) = p.checked_add(size as usize) else { break };
        if size < hdr as u64 || end > to {
            break;
        }
        out.push(Bx { ty, start: p, body: p + hdr, end });
        p = end;
    }
    out
}

fn cstr(d: &[u8], p: usize) -> (String, usize) {
    let end = d[p.min(d.len())..].iter().position(|&c| c == 0).map(|i| p + i).unwrap_or(d.len());
    (text_of(&d[p.min(d.len())..end]), end + 1)
}

// ---------- heic / avif ----------

struct Item {
    id: u32,
    ty: [u8; 4],
    mime: String,
    ranges: Vec<(usize, usize)>, // absolute byte ranges in the file
}

fn uint(d: &[u8], p: usize, size: usize) -> Option<u64> {
    match size {
        0 => Some(0),
        2 => be16(d, p).map(u64::from),
        4 => be32(d, p).map(u64::from),
        8 => be64(d, p),
        _ => None,
    }
}

fn heif_items(d: &[u8]) -> Option<Vec<Item>> {
    let meta = boxes(d, 0, d.len()).into_iter().find(|b| &b.ty == b"meta")?;
    let kids = boxes(d, meta.body + 4, meta.end);
    let mut items: Vec<Item> = vec![];
    let idat = kids.iter().find(|b| &b.ty == b"idat").map(|b| b.body);

    let iinf = kids.iter().find(|b| &b.ty == b"iinf")?;
    let ver = *d.get(iinf.body)?;
    let first = iinf.body + if ver == 0 { 6 } else { 8 };
    for e in boxes(d, first, iinf.end).into_iter().filter(|b| &b.ty == b"infe") {
        let v = *d.get(e.body)?;
        if v < 2 {
            continue;
        }
        let mut p = e.body + 4;
        let id = if v == 2 { be16(d, p)? as u32 } else { be32(d, p)? };
        p += if v == 2 { 2 } else { 4 };
        p += 2; // protection index
        let ty: [u8; 4] = d.get(p..p + 4)?.try_into().ok()?;
        let (_name, after) = cstr(d, p + 4);
        let mime = if &ty == b"mime" { cstr(d, after).0 } else { String::new() };
        items.push(Item { id, ty, mime, ranges: vec![] });
    }

    let iloc = kids.iter().find(|b| &b.ty == b"iloc")?;
    let v = *d.get(iloc.body)?;
    let sizes = be16(d, iloc.body + 4)?;
    let (osz, lsz, bsz, isz) = ((sizes >> 12) as usize, (sizes >> 8 & 15) as usize, (sizes >> 4 & 15) as usize, (sizes & 15) as usize);
    let mut p = iloc.body + 6;
    let count = if v < 2 { be16(d, p)? as u32 } else { be32(d, p)? };
    p += if v < 2 { 2 } else { 4 };
    for _ in 0..count {
        let id = if v < 2 { be16(d, p)? as u32 } else { be32(d, p)? };
        p += if v < 2 { 2 } else { 4 };
        let mut method = 0;
        if v == 1 || v == 2 {
            method = be16(d, p)? & 15;
            p += 2;
        }
        p += 2; // data reference index
        let base = uint(d, p, bsz)?;
        p += bsz;
        let extents = be16(d, p)?;
        p += 2;
        let mut ranges = vec![];
        for _ in 0..extents {
            if (v == 1 || v == 2) && isz > 0 {
                p += isz;
            }
            let off = uint(d, p, osz)?;
            p += osz;
            let len = uint(d, p, lsz)?;
            p += lsz;
            // method 0 = offsets into the file, 1 = offsets into the idat box
            let start = match method {
                0 => Some(base + off),
                1 => idat.map(|i| i as u64 + base + off),
                _ => None,
            };
            if let Some(s) = start {
                let (s, e) = (s as usize, (s + len) as usize);
                if s <= e && e <= d.len() {
                    ranges.push((s, e));
                }
            }
        }
        if let Some(it) = items.iter_mut().find(|i| i.id == id) {
            it.ranges = ranges;
        }
    }
    Some(items)
}

fn is_xmp(it: &Item) -> bool {
    &it.ty == b"mime" && (it.mime.contains("rdf") || it.mime.contains("xml"))
}

pub fn read_heif(rep: &mut Report, d: &[u8]) {
    for b in boxes(d, 0, d.len()) {
        if &b.ty == b"uuid" {
            uuid_box(rep, &d[b.body..b.end]);
        }
    }
    let Some(items) = heif_items(d) else {
        rep.note("couldn't walk this file's item table");
        return;
    };
    for it in &items {
        let bytes: Vec<u8> = it.ranges.iter().flat_map(|&(s, e)| d[s..e].iter().copied()).collect();
        if &it.ty == b"Exif" {
            // 4 byte offset to the tiff header comes first
            let skip = 4 + be32(&bytes, 0).unwrap_or(0) as usize;
            if let Some(t) = bytes.get(skip..) {
                exif::read_tiff(rep, t, "EXIF");
            }
        } else if is_xmp(it) {
            xmp::read(rep, &bytes, "XMP");
        }
    }
}

fn uuid_box(rep: &mut Report, body: &[u8]) {
    let Some(id) = body.get(..16) else { return };
    if id == jumbf::C2PA_UUID {
        jumbf::describe(rep, &body[16..], "a uuid box");
    } else if id == XMP_UUID {
        xmp::read(rep, &body[16..], "XMP");
    } else {
        rep.add_as(Group::Other, Risk::Low, "MP4", "Private uuid box", &format!("{} bytes", body.len()));
    }
}

// blanks metadata in place so no offsets move: exif gets an empty tiff block,
// xmp gets spaces, uuid boxes become free space
pub fn clean_heif(d: &[u8]) -> Result<Vec<u8>, String> {
    let items = heif_items(d).ok_or("couldn't walk this file's item table")?;
    let mut out = d.to_vec();
    for it in &items {
        let exif = &it.ty == b"Exif";
        if !exif && !is_xmp(it) {
            continue;
        }
        let mut blank = if exif { [vec![0u8; 4], exif::minimal_tiff(None)].concat() } else { vec![] };
        let total: usize = it.ranges.iter().map(|&(s, e)| e - s).sum();
        if blank.len() > total {
            blank.clear();
        }
        let mut src = blank.into_iter();
        for &(s, e) in &it.ranges {
            for b in &mut out[s..e] {
                *b = src.next().unwrap_or(if exif { 0 } else { b' ' });
            }
        }
    }
    for b in boxes(d, 0, d.len()) {
        if &b.ty == b"uuid" {
            out[b.start + 4..b.start + 8].copy_from_slice(b"free");
            out[b.body..b.end].fill(0);
        }
    }
    Ok(out)
}

// ---------- mp4 / mov ----------

// only pulls the small metadata boxes into memory, never the video itself
pub fn read_mp4_file(rep: &mut Report, f: &mut File, len: u64) {
    let mut pos = 0u64;
    let mut head = [0u8; 16];
    while pos + 8 <= len {
        if f.seek(SeekFrom::Start(pos)).is_err() || f.read_exact(&mut head[..8]).is_err() {
            break;
        }
        let ty: [u8; 4] = head[4..8].try_into().unwrap();
        let (hdr, size) = match be32(&head, 0).unwrap_or(0) {
            1 => {
                if f.read_exact(&mut head[8..16]).is_err() {
                    break;
                }
                (16u64, be64(&head, 8).unwrap_or(0))
            }
            0 => (8, len - pos),
            n => (8, n as u64),
        };
        if size < hdr || pos + size > len {
            break;
        }
        if matches!(&ty, b"moov" | b"uuid" | b"meta" | b"udta") && size - hdr <= 256 << 20 {
            let mut body = vec![0u8; (size - hdr) as usize];
            if f.read_exact(&mut body).is_ok() {
                match &ty {
                    b"moov" => moov(rep, &body),
                    b"uuid" => uuid_box(rep, &body),
                    b"meta" => meta(rep, &body),
                    _ => udta(rep, &body),
                }
            }
        }
        pos += size;
    }
}

fn times(rep: &mut Report, d: &[u8], what: &str) {
    // version 1 boxes use 64 bit times
    let (c, m) = if d.first() == Some(&1) { (be64(d, 4), be64(d, 12)) } else { (be32(d, 4).map(u64::from), be32(d, 8).map(u64::from)) };
    for (label, v) in [("create date", c), ("modify date", m)] {
        if let Some(secs) = v.filter(|&s| s > 0) {
            // counted from 1904, because apple
            rep.add("MP4", &format!("{what} {label}"), &fmt_epoch(secs as i64 - 2_082_844_800));
        }
    }
}

fn moov(rep: &mut Report, d: &[u8]) {
    for b in boxes(d, 0, d.len()) {
        let body = &d[b.body..b.end];
        match &b.ty {
            b"mvhd" => times(rep, body, "Movie"),
            b"udta" => udta(rep, body),
            b"meta" => meta(rep, body),
            b"trak" => {
                for t in boxes(body, 0, body.len()) {
                    let tb = &body[t.body..t.end];
                    match &t.ty {
                        b"tkhd" => times(rep, tb, "Track"),
                        b"udta" => udta(rep, tb),
                        b"meta" => meta(rep, tb),
                        b"mdia" => {
                            for m in boxes(tb, 0, tb.len()) {
                                let mb = &tb[m.body..m.end];
                                if &m.ty == b"mdhd" {
                                    times(rep, mb, "Media");
                                } else if &m.ty == b"hdlr" {
                                    handler(rep, mb);
                                }
                            }
                        }
                        _ => {}
                    }
                }
            }
            _ => {}
        }
    }
}

fn handler(rep: &mut Report, d: &[u8]) {
    let Some(name) = d.get(24..) else { return };
    // quicktime uses a pascal string here, mp4 a c string
    let name = if name.first().is_some_and(|&n| n as usize == name.len() - 1) { &name[1..] } else { name };
    let name = text_of(name);
    let kind = d.get(8..12).map(text_of).unwrap_or_default();
    if !matches!(kind.as_str(), "vide" | "soun") && !name.trim().is_empty() {
        rep.add_as(Group::Other, Risk::Low, "MP4", "Data track", &format!("{} ({kind})", name.trim()));
    } else if !matches!(name.trim(), "" | "VideoHandler" | "SoundHandler" | "VideoHandle" | "SoundHandle") {
        // the default names say nothing, anything else usually names the camera app
        rep.add("MP4", "Handler", name.trim());
    }
}

fn udta(rep: &mut Report, d: &[u8]) {
    for b in boxes(d, 0, d.len()) {
        let body = &d[b.body..b.end];
        match &b.ty {
            b"meta" => meta(rep, body),
            b"XMP_" => xmp::read(rep, body, "XMP"),
            // 3gp location box: lang, name, role, then lon / lat / alt as 16.16 fixed point
            b"loci" => {
                let (_, p) = cstr(body, 6);
                let fx = |i: usize| be32(body, p + 1 + i * 4).map(|v| v as i32 as f64 / 65536.0);
                if let (Some(lon), Some(lat)) = (fx(0), fx(1)) {
                    rep.add("MP4", "GPS coordinates", &format!("{lat:.6}, {lon:.6}"));
                    push_gps(rep, lat, lon, fx(2));
                }
            }
            t if t[0] == 0xA9 => {
                // either "len, lang, text" or an itunes style nested data box
                let text = match boxes(body, 0, body.len()).into_iter().find(|x| &x.ty == b"data") {
                    Some(x) => text_of(body.get(x.body + 8..x.end).unwrap_or(&[])),
                    None => {
                        let n = be16(body, 0).unwrap_or(0) as usize;
                        text_of(body.get(4..4 + n).unwrap_or(&[]))
                    }
                };
                tag(rep, &copyright_name(t), &text);
            }
            b"cprt" | b"auth" | b"titl" | b"perf" | b"dscp" | b"albm" | b"gnre" => {
                let key = match &b.ty {
                    b"cprt" => "Copyright",
                    b"auth" => "Author",
                    b"titl" => "Title",
                    b"perf" => "Artist",
                    b"dscp" => "Description",
                    b"albm" => "Album",
                    _ => "Genre",
                };
                rep.add("MP4", key, &text_of(body.get(6..).unwrap_or(&[])));
            }
            b"free" | b"skip" | b"hnti" | b"hinf" => {}
            t => rep.add_as(Group::Device, Risk::Low, "MP4", "Maker data", &format!("{} ({} bytes)", text_of(t).trim(), body.len())),
        }
    }
}

fn copyright_name(t: &[u8; 4]) -> String {
    match &t[1..] {
        b"xyz" => "GPS coordinates",
        b"mak" => "Make",
        b"mod" => "Model",
        b"swr" => "Software",
        b"too" => "Encoder",
        b"day" => "Creation date",
        b"nam" => "Title",
        b"cmt" => "Comment",
        b"ART" => "Artist",
        b"aut" => "Author",
        b"cpy" => "Copyright",
        b"alb" => "Album",
        b"wrt" => "Writer",
        b"des" => "Description",
        b"inf" => "Information",
        rest => return format!("©{}", text_of(rest)),
    }
    .to_string()
}

fn tag(rep: &mut Report, key: &str, value: &str) {
    rep.add("MP4", key, value);
    if key.to_ascii_lowercase().contains("iso6709") || key == "GPS coordinates" {
        if let Some((lat, lon, alt)) = iso6709(value) {
            push_gps(rep, lat, lon, alt);
        }
    }
}

// the same spot often shows up in two or three boxes, only list it once
fn push_gps(rep: &mut Report, lat: f64, lon: f64, alt: Option<f64>) {
    if !rep.gps.iter().any(|g| (g.lat - lat).abs() < 1e-4 && (g.lon - lon).abs() < 1e-4) {
        rep.gps.push(Gps { lat, lon, alt, accuracy_m: None, source: "MP4".into() });
    }
}

fn meta(rep: &mut Report, d: &[u8]) {
    // mp4 puts a version word before the children, quicktime doesn't
    let from = if d.get(4..8) == Some(b"hdlr") { 0 } else { 4 };
    let kids = boxes(d, from.min(d.len()), d.len());
    let mut keys: Vec<String> = vec![];
    if let Some(k) = kids.iter().find(|b| &b.ty == b"keys") {
        let mut p = k.body + 8;
        while let Some(size) = be32(d, p).map(|s| s as usize) {
            if size < 8 || p + size > k.end {
                break;
            }
            keys.push(text_of(&d[p + 8..p + size]));
            p += size;
        }
    }
    let Some(ilst) = kids.iter().find(|b| &b.ty == b"ilst") else { return };
    for it in boxes(d, ilst.body, ilst.end) {
        let inner = boxes(d, it.body, it.end);
        let Some(data) = inner.iter().find(|x| &x.ty == b"data") else { continue };
        let kind = be32(d, data.body).unwrap_or(0) & 0xFF_FFFF;
        let raw = d.get(data.body + 8..data.end).unwrap_or(&[]);
        let value = match kind {
            1 => text_of(raw),
            13 | 14 | 27 => format!("picture ({} bytes)", raw.len()),
            21 | 22 if raw.len() <= 8 => raw.iter().fold(0u64, |a, &b| a << 8 | b as u64).to_string(),
            23 if raw.len() == 4 => f32::from_be_bytes(raw.try_into().unwrap()).to_string(),
            _ if raw.len() <= 64 && raw.iter().all(|c| c.is_ascii_graphic() || *c == b' ') => text_of(raw),
            _ => format!("({} bytes)", raw.len()),
        };
        let idx = u32::from_be_bytes(it.ty) as usize;
        let key = if !keys.is_empty() && idx >= 1 && idx <= keys.len() {
            let k = keys[idx - 1].clone();
            match k.strip_prefix("com.apple.quicktime.") {
                Some(short) => short.to_string(),
                None if k == "com.android.version" => "Android version".to_string(),
                None => k,
            }
        } else if &it.ty == b"----" {
            inner.iter().find(|x| &x.ty == b"name").map(|x| text_of(d.get(x.body + 4..x.end).unwrap_or(&[]))).unwrap_or_else(|| "freeform tag".into())
        } else if it.ty[0] == 0xA9 {
            copyright_name(&it.ty)
        } else {
            match &it.ty {
                b"cprt" => "Copyright".to_string(),
                b"aART" => "Album artist".to_string(),
                b"covr" => "Cover art".to_string(),
                b"desc" | b"ldes" => "Description".to_string(),
                t => text_of(t),
            }
        };
        tag(rep, &key, &value);
    }
    // iphones store how good the gps fix was as its own key
    let acc = rep.fields.iter().find(|f| f.key.ends_with("accuracy.horizontal")).and_then(|f| f.value.trim().parse::<f64>().ok());
    if let Some(a) = acc {
        for g in rep.gps.iter_mut().filter(|g| g.accuracy_m.is_none()) {
            g.accuracy_m = Some(a);
        }
    }
}

// "+45.5152-122.6784+035.000/"
pub fn iso6709(s: &str) -> Option<(f64, f64, Option<f64>)> {
    let s = s.trim().trim_end_matches('/');
    let mut nums = vec![];
    let mut cur = String::new();
    for c in s.chars() {
        if (c == '+' || c == '-') && !cur.is_empty() {
            nums.push(cur.parse::<f64>().ok()?);
            cur.clear();
        }
        cur.push(c);
    }
    if !cur.is_empty() {
        nums.push(cur.parse::<f64>().ok()?);
    }
    (nums.len() >= 2 && nums[0].abs() <= 90.0 && nums[1].abs() <= 180.0).then(|| (nums[0], nums[1], nums.get(2).copied()))
}

// unix seconds -> "2026-11-16 10:42:33 UTC"
pub fn fmt_epoch(secs: i64) -> String {
    let (days, rem) = (secs.div_euclid(86400), secs.rem_euclid(86400));
    // civil-from-days, the howard hinnant one
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + if m <= 2 { 1 } else { 0 };
    format!("{y:04}-{m:02}-{d:02} {:02}:{:02}:{:02} UTC", rem / 3600, rem % 3600 / 60, rem % 60)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn helpers() {
        assert_eq!(iso6709("+45.5152-122.6784+035.000/"), Some((45.5152, -122.6784, Some(35.0))));
        assert_eq!(iso6709("+45.5152-122.6784/"), Some((45.5152, -122.6784, None)));
        assert_eq!(iso6709("nope"), None);
        assert_eq!(fmt_epoch(0), "1970-01-01 00:00:00 UTC");
        assert_eq!(fmt_epoch(1_794_825_753), "2026-11-16 10:42:33 UTC");
    }
}

// small zip reader/writer (stored + deflate). office files are zips and we want full
// control over what goes back in: no timestamps, no extra fields, no comments

use flate2::read::DeflateDecoder;
use flate2::write::DeflateEncoder;
use flate2::{Compression, Crc};
use std::io::{Read, Write};

pub struct Entry {
    pub name: String,
    pub stored: bool, // written without compression (odf wants its mimetype like that)
    pub data: Vec<u8>,
}

// don't let a zip bomb eat all the ram
const MAX_TOTAL: u64 = 2 << 30;

struct Central {
    name: String,
    method: u16,
    flags: u16,
    csize: u64,
    usize_: u64,
    offset: u64,
}

fn u16le(b: &[u8], i: usize) -> u16 {
    u16::from_le_bytes([b[i], b[i + 1]])
}
fn u32le(b: &[u8], i: usize) -> u32 {
    u32::from_le_bytes([b[i], b[i + 1], b[i + 2], b[i + 3]])
}

fn central(data: &[u8]) -> Result<Vec<Central>, String> {
    if data.len() < 22 {
        return Err("not a zip".into());
    }
    // end-of-central-directory record sits at the very end, after an optional comment
    let floor = data.len().saturating_sub(22 + 65535);
    let eocd = (floor..=data.len() - 22).rev().find(|&i| &data[i..i + 4] == b"PK\x05\x06").ok_or("no zip directory")?;
    let count = u16le(data, eocd + 10) as usize;
    let cd_size = u32le(data, eocd + 12) as u64;
    let cd_off = u32le(data, eocd + 16) as u64;
    if count == 0xFFFF || cd_off == 0xFFFF_FFFF || cd_size == 0xFFFF_FFFF {
        return Err("zip64 files aren't supported yet".into());
    }
    let mut out = Vec::with_capacity(count);
    let mut p = cd_off as usize;
    for _ in 0..count {
        if p + 46 > data.len() || &data[p..p + 4] != b"PK\x01\x02" {
            return Err("broken zip directory".into());
        }
        let nlen = u16le(data, p + 28) as usize;
        let elen = u16le(data, p + 30) as usize;
        let clen = u16le(data, p + 32) as usize;
        if p + 46 + nlen > data.len() {
            return Err("broken zip directory".into());
        }
        out.push(Central {
            name: String::from_utf8_lossy(&data[p + 46..p + 46 + nlen]).into_owned(),
            method: u16le(data, p + 10),
            flags: u16le(data, p + 8),
            csize: u32le(data, p + 20) as u64,
            usize_: u32le(data, p + 24) as u64,
            offset: u32le(data, p + 42) as u64,
        });
        p += 46 + nlen + elen + clen;
    }
    Ok(out)
}

pub fn names(data: &[u8]) -> Result<Vec<String>, String> {
    Ok(central(data)?.into_iter().map(|c| c.name).collect())
}

pub fn read(data: &[u8]) -> Result<Vec<Entry>, String> {
    let mut out = vec![];
    let mut total = 0u64;
    for c in central(data)? {
        if c.flags & 1 != 0 {
            return Err("zip is password protected".into());
        }
        total += c.usize_;
        if total > MAX_TOTAL {
            return Err("zip unpacks to something huge, not touching it".into());
        }
        let o = c.offset as usize;
        if o + 30 > data.len() || &data[o..o + 4] != b"PK\x03\x04" {
            return Err(format!("broken zip entry {}", c.name));
        }
        let start = o + 30 + u16le(data, o + 26) as usize + u16le(data, o + 28) as usize;
        let end = start.checked_add(c.csize as usize).filter(|&e| e <= data.len()).ok_or("zip entry runs off the end")?;
        let raw = &data[start..end];
        let bytes = match c.method {
            0 => raw.to_vec(),
            8 => {
                let mut v = Vec::with_capacity(c.usize_.min(64 << 20) as usize);
                DeflateDecoder::new(raw).take(MAX_TOTAL).read_to_end(&mut v).map_err(|_| format!("can't unpack {}", c.name))?;
                v
            }
            m => return Err(format!("zip compression method {m} isn't supported")),
        };
        out.push(Entry { name: c.name, stored: c.method == 0, data: bytes });
    }
    Ok(out)
}

pub fn write(entries: &[Entry]) -> Vec<u8> {
    let mut out = vec![];
    let mut dir = vec![];
    for e in entries {
        let mut crc = Crc::new();
        crc.update(&e.data);
        let (method, body) = if e.stored || e.data.is_empty() {
            (0u16, e.data.clone())
        } else {
            let mut enc = DeflateEncoder::new(Vec::new(), Compression::new(6));
            enc.write_all(&e.data).unwrap();
            (8u16, enc.finish().unwrap())
        };
        let offset = out.len() as u32;
        let name = e.name.as_bytes();
        let flags: u16 = if name.is_ascii() { 0 } else { 0x0800 }; // utf8 names
        // local header then central header share most fields. dos date 0x21 = 1980-01-01
        let mut common = vec![];
        common.extend_from_slice(&20u16.to_le_bytes());
        common.extend_from_slice(&flags.to_le_bytes());
        common.extend_from_slice(&method.to_le_bytes());
        common.extend_from_slice(&0u16.to_le_bytes());
        common.extend_from_slice(&0x21u16.to_le_bytes());
        common.extend_from_slice(&crc.sum().to_le_bytes());
        common.extend_from_slice(&(body.len() as u32).to_le_bytes());
        common.extend_from_slice(&(e.data.len() as u32).to_le_bytes());
        common.extend_from_slice(&(name.len() as u16).to_le_bytes());
        common.extend_from_slice(&0u16.to_le_bytes());

        out.extend_from_slice(b"PK\x03\x04");
        out.extend_from_slice(&common);
        out.extend_from_slice(name);
        out.extend_from_slice(&body);

        dir.extend_from_slice(b"PK\x01\x02");
        dir.extend_from_slice(&20u16.to_le_bytes());
        dir.extend_from_slice(&common);
        dir.extend_from_slice(&[0u8; 10]); // comment len, disk, internal + external attrs
        dir.extend_from_slice(&offset.to_le_bytes());
        dir.extend_from_slice(name);
    }
    let dir_off = out.len() as u32;
    out.extend_from_slice(&dir);
    out.extend_from_slice(b"PK\x05\x06");
    out.extend_from_slice(&[0u8; 4]);
    out.extend_from_slice(&(entries.len() as u16).to_le_bytes());
    out.extend_from_slice(&(entries.len() as u16).to_le_bytes());
    out.extend_from_slice(&(dir.len() as u32).to_le_bytes());
    out.extend_from_slice(&dir_off.to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes());
    out
}

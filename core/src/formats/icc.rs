// just enough icc parsing to pull the profile's name. custom monitor profiles
// have names like "DELL U2720Q" which is a nice little fingerprint

use super::be32;

pub fn name(p: &[u8]) -> Option<String> {
    let count = be32(p, 128)? as usize;
    for i in 0..count.min(64) {
        let e = 132 + i * 12;
        if p.get(e..e + 4)? != b"desc" {
            continue;
        }
        let off = be32(p, e + 4)? as usize;
        let size = be32(p, e + 8)? as usize;
        let tag = p.get(off..off.checked_add(size)?)?;
        return match tag.get(..4)? {
            // v2: ascii with a length prefix
            b"desc" => {
                let n = be32(tag, 8)? as usize;
                let s = tag.get(12..12 + n)?;
                Some(String::from_utf8_lossy(s).trim_end_matches('\0').to_string())
            }
            // v4: utf16be records, take the first
            b"mluc" => {
                let len = be32(tag, 20)? as usize;
                let o = be32(tag, 24)? as usize;
                let s = tag.get(o..o.checked_add(len)?)?;
                let u: Vec<u16> = s.chunks_exact(2).map(|c| u16::from_be_bytes([c[0], c[1]])).collect();
                Some(String::from_utf16_lossy(&u))
            }
            _ => None,
        };
    }
    None
}

// gif: header, then blocks. comments and app extensions (xmp) are the only places
// metadata hides

use super::xmp;
use crate::clean::CleanOptions;
use crate::report::{text_of, Group, Report, Risk};

enum Block {
    Image,
    Ext { label: u8, app: [u8; 11] },
}

fn table(packed: u8) -> usize {
    if packed & 0x80 != 0 {
        3 << ((packed & 7) + 1)
    } else {
        0
    }
}

// skips a run of length-prefixed sub blocks, returns the offset after the terminator
fn sub_blocks(d: &[u8], mut p: usize) -> Option<usize> {
    loop {
        let n = *d.get(p)? as usize;
        p += 1;
        if n == 0 {
            return Some(p);
        }
        p += n;
    }
}

// (block, start, end) for everything between the header and the trailer
fn walk(d: &[u8]) -> Option<(usize, Vec<(Block, usize, usize)>)> {
    if d.len() < 13 || !(d.starts_with(b"GIF87a") || d.starts_with(b"GIF89a")) {
        return None;
    }
    let head = 13 + table(d[10]);
    if head > d.len() {
        return None;
    }
    let mut p = head;
    let mut out = vec![];
    while p < d.len() {
        let start = p;
        match d[p] {
            0x2C => {
                let packed = *d.get(p + 9)?;
                p = sub_blocks(d, p + 10 + table(packed) + 1)?;
                out.push((Block::Image, start, p));
            }
            0x21 => {
                let label = *d.get(p + 1)?;
                let mut app = [0u8; 11];
                if label == 0xFF && d.get(p + 2) == Some(&11) {
                    app.copy_from_slice(d.get(p + 3..p + 14)?);
                }
                p = sub_blocks(d, p + 2)?;
                out.push((Block::Ext { label, app }, start, p));
            }
            _ => break, // 0x3B trailer, or junk
        }
    }
    Some((head, out))
}

const LOOP_APPS: &[&[u8; 11]] = &[b"NETSCAPE2.0", b"ANIMEXTS1.0"];

pub fn read(rep: &mut Report, d: &[u8]) {
    let Some((_, blocks)) = walk(d) else {
        rep.note("gif looks damaged");
        return;
    };
    for (b, start, end) in blocks {
        match b {
            Block::Ext { label: 0xFE, .. } => {
                // glue the sub blocks back together
                let mut text = vec![];
                let mut p = start + 2;
                while let Some(&n) = d.get(p) {
                    if n == 0 {
                        break;
                    }
                    text.extend_from_slice(d.get(p + 1..p + 1 + n as usize).unwrap_or(&[]));
                    p += 1 + n as usize;
                }
                rep.add("GIF", "Comment", &text_of(&text));
            }
            Block::Ext { label: 0xFF, app } if &app == b"XMP DataXMP" => xmp::read(rep, &d[start..end], "XMP"),
            Block::Ext { label: 0xFF, app } if !LOOP_APPS.contains(&&app) => {
                rep.add_as(Group::Other, Risk::Low, "GIF", "Application data", &format!("{} ({} bytes)", text_of(&app), end - start));
            }
            _ => {}
        }
    }
}

pub fn clean(d: &[u8], o: &CleanOptions) -> Result<Vec<u8>, String> {
    let (head, blocks) = walk(d).ok_or("not a gif")?;
    let mut out = d[..head].to_vec();
    for (b, start, end) in blocks {
        let keep = match b {
            Block::Image => true,
            Block::Ext { label: 0xF9 | 0x01, .. } => true, // frame timing, plain text frames
            Block::Ext { label: 0xFF, app } => LOOP_APPS.contains(&&app) || (o.keep_icc && &app == b"ICCRGBG1012"),
            Block::Ext { .. } => false,
        };
        if keep {
            out.extend_from_slice(&d[start..end]);
        }
    }
    out.push(0x3B);
    Ok(out)
}

// pdfs: the info dictionary, the xmp packet, comments, and old versions of the
// document that pile up every time someone hits save. lopdf does the parsing and
// writing, cleaning rebuilds the file from scratch so the old saves are really gone

use super::xmp;
use crate::clean::CleanOptions;
use crate::report::{text_of, Group, Report, Risk};
use lopdf::{Dictionary, Document, Object, ObjectId};
use std::collections::BTreeSet;

fn show(doc: &Document, o: &Object) -> String {
    match o {
        Object::Reference(id) => doc.get_object(*id).map(|o| show(doc, o)).unwrap_or_default(),
        Object::String(b, _) => {
            if b.starts_with(&[0xFE, 0xFF]) {
                let u: Vec<u16> = b[2..].chunks_exact(2).map(|c| u16::from_be_bytes([c[0], c[1]])).collect();
                String::from_utf16_lossy(&u)
            } else {
                text_of(b.strip_prefix(&[0xEF, 0xBB, 0xBF]).unwrap_or(b))
            }
        }
        Object::Name(n) => text_of(n),
        Object::Integer(i) => i.to_string(),
        Object::Real(r) => r.to_string(),
        Object::Boolean(b) => b.to_string(),
        _ => String::new(),
    }
}

fn dict_of<'a>(doc: &'a Document, o: &'a Object) -> Option<&'a Dictionary> {
    match o {
        Object::Reference(id) => doc.get_dictionary(*id).ok(),
        Object::Dictionary(d) => Some(d),
        _ => None,
    }
}

// annotation types that are part of the page itself, everything else is a comment / markup
const NOT_COMMENTS: &[&[u8]] = &[b"Link", b"Widget", b"Screen", b"Movie", b"3D", b"RichMedia", b"PrinterMark", b"TrapNet", b"Watermark"];

fn is_comment(doc: &Document, o: &Object) -> bool {
    let Some(d) = dict_of(doc, o) else { return false };
    match d.get(b"Subtype") {
        Ok(Object::Name(n)) => !NOT_COMMENTS.contains(&n.as_slice()),
        _ => false,
    }
}

fn annots(doc: &Document, page: ObjectId) -> Vec<Object> {
    let Ok(p) = doc.get_dictionary(page) else { return vec![] };
    match p.get(b"Annots") {
        Ok(Object::Array(a)) => a.clone(),
        Ok(Object::Reference(r)) => match doc.get_object(*r) {
            Ok(Object::Array(a)) => a.clone(),
            _ => vec![],
        },
        _ => vec![],
    }
}

pub fn read(rep: &mut Report, d: &[u8]) {
    let saves = d.windows(5).filter(|w| w == b"%%EOF").count();
    let doc = match Document::load_mem(d) {
        Ok(doc) => doc,
        Err(e) => {
            rep.note(format!("couldn't parse this pdf ({e})"));
            return;
        }
    };
    rep.add("PDF", "PDF version", &doc.version);
    if doc.is_encrypted() {
        rep.note("this pdf is password protected");
    }
    if let Some(info) = doc.trailer.get(b"Info").ok().and_then(|o| dict_of(&doc, o)) {
        for (k, v) in info.iter() {
            rep.add("PDF info", &text_of(k), &show(&doc, v));
        }
    }
    // xmp packet hangs off the catalog
    if let Ok(Object::Reference(id)) = doc.catalog().and_then(|c| c.get(b"Metadata")) {
        if let Ok(Object::Stream(s)) = doc.get_object(*id) {
            let bytes = s.decompressed_content().unwrap_or_else(|_| s.content.clone());
            xmp::read(rep, &bytes, "XMP");
        }
    }
    if saves > 1 {
        rep.add_as(Group::Software, Risk::Low, "PDF", "Saves", &format!("saved {saves} times, earlier versions of the content can still be inside the file"));
    }
    if let Ok(Object::Array(id)) = doc.trailer.get(b"ID") {
        if let Some(Object::String(b, _)) = id.first() {
            let hex: String = b.iter().map(|c| format!("{c:02x}")).collect();
            rep.add("PDF", "File ID", &hex);
        }
    }
    let mut count = 0;
    let mut authors = BTreeSet::new();
    for page in doc.get_pages().into_values() {
        for a in annots(&doc, page).iter().filter(|a| is_comment(&doc, a)) {
            count += 1;
            if let Some(t) = dict_of(&doc, a).and_then(|d| d.get(b"T").ok()) {
                let name = show(&doc, t);
                if !name.trim().is_empty() {
                    authors.insert(name);
                }
            }
        }
    }
    if count > 0 {
        rep.add_as(Group::Author, Risk::Low, "PDF", "Comments", &format!("{count} comment / markup annotation(s)"));
    }
    if !authors.is_empty() {
        rep.add_as(Group::Author, Risk::High, "PDF", "Comment authors", &authors.into_iter().take(10).collect::<Vec<_>>().join(", "));
    }
    let attachments = doc.objects.values().filter(|o| matches!(o, Object::Dictionary(d) if matches!(d.get(b"Type"), Ok(Object::Name(n)) if n == b"Filespec"))).count();
    if attachments > 0 {
        rep.add_as(Group::Other, Risk::Low, "PDF", "Attachments", &format!("{attachments} embedded file(s), odin leaves these alone"));
    }
}

pub fn clean(d: &[u8], o: &CleanOptions) -> Result<Vec<u8>, String> {
    let mut doc = Document::load_mem(d).map_err(|e| format!("couldn't parse this pdf ({e})"))?;
    if doc.is_encrypted() {
        return Err("this pdf is password protected, odin can't clean those yet".into());
    }
    doc.trailer.remove(b"Info");
    doc.trailer.remove(b"ID");

    if o.remove_comments {
        let pages: Vec<ObjectId> = doc.get_pages().into_values().collect();
        for page in pages {
            let all = annots(&doc, page);
            if all.is_empty() {
                continue;
            }
            let kept: Vec<Object> = all.iter().filter(|a| !is_comment(&doc, a)).cloned().collect();
            if kept.len() == all.len() {
                continue;
            }
            // the array is either inline on the page or its own object
            let indirect = match doc.get_dictionary(page).ok().and_then(|p| p.get(b"Annots").ok()) {
                Some(Object::Reference(r)) => Some(*r),
                _ => None,
            };
            match indirect {
                Some(r) => {
                    if let Ok(obj) = doc.get_object_mut(r) {
                        *obj = Object::Array(kept);
                    }
                }
                None => {
                    if let Ok(Object::Dictionary(p)) = doc.get_object_mut(page) {
                        if kept.is_empty() {
                            p.remove(b"Annots");
                        } else {
                            p.set("Annots", Object::Array(kept));
                        }
                    }
                }
            }
        }
    }

    // xmp + app-private data can hang off any object (pages, images, the catalog)
    for obj in doc.objects.values_mut() {
        let dict = match obj {
            Object::Dictionary(d) => d,
            Object::Stream(s) => &mut s.dict,
            _ => continue,
        };
        for key in [&b"Metadata"[..], b"PieceInfo", b"LastModified"] {
            dict.remove(key);
        }
    }
    // only objects the document still points at survive, so old saves fall away here
    doc.prune_objects();
    doc.renumber_objects();
    let mut out = vec![];
    doc.save_to(&mut out).map_err(|e| format!("couldn't write the cleaned pdf ({e})"))?;
    Ok(out)
}

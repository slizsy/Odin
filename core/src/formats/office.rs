// docx / xlsx / pptx (and odf) are zips full of xml. the leaks: docProps (author,
// company, app), comments, tracked changes, template paths, and exif inside pasted photos

use crate::clean::CleanOptions;
use crate::kind::Kind;
use crate::report::{Group, Report, Risk, Thumb};
use crate::xml::{self, attr, local, tokens, Act, Tok};
use crate::zip::{self, Entry};
use std::collections::{BTreeSet, HashMap, HashSet};

const CORE_BLANK: &str = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\" xmlns:dcmitype=\"http://purl.org/dc/dcmitype/\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"/>";
const APP_BLANK: &str = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\" xmlns:vt=\"http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes\"/>";

// tracked-change wrappers. dropping these = the old text / old formatting is gone
const CHANGE_DROP: &[&str] = &[
    "w:del", "w:moveFrom", "w:rPrChange", "w:pPrChange", "w:sectPrChange", "w:tblPrChange", "w:tcPrChange",
    "w:trPrChange", "w:tblGridChange", "w:tblPrExChange", "w:numberingChange", "w:cellIns", "w:cellDel",
    "w:cellMerge", "w:moveFromRangeStart", "w:moveFromRangeEnd", "w:moveToRangeStart", "w:moveToRangeEnd",
];
// and these keep their contents, the inserted text just becomes normal text
const CHANGE_UNWRAP: &[&str] = &["w:ins", "w:moveTo"];
const COMMENT_MARKS: &[&str] = &["w:commentRangeStart", "w:commentRangeEnd", "w:commentReference"];

fn text(e: &Entry) -> Option<&str> {
    std::str::from_utf8(&e.data).ok()
}

fn is_media(name: &str) -> bool {
    name.contains("/media/") || name.starts_with("Pictures/") || name.starts_with("media/")
}

// simple "<x>text</x>" children one level under the root
fn flat_children(xml_text: &str) -> Vec<(String, String)> {
    let mut out = vec![];
    let mut depth = 0;
    let mut cur: Option<(String, String)> = None;
    for t in tokens(xml_text) {
        match t {
            Tok::Open { name, empty, .. } => {
                if !empty {
                    depth += 1;
                    if depth == 2 {
                        cur = Some((name.to_string(), String::new()));
                    }
                }
            }
            Tok::Close { .. } => {
                if depth == 2 {
                    if let Some((k, v)) = cur.take() {
                        out.push((k, xml::unescape(v.trim())));
                    }
                }
                depth -= 1;
                if depth < 0 {
                    break;
                }
            }
            Tok::Text(s) if depth == 2 => {
                if let Some(c) = cur.as_mut() {
                    c.1.push_str(s);
                }
            }
            _ => {}
        }
    }
    out
}

fn names_list(set: &BTreeSet<String>) -> String {
    set.iter().take(10).cloned().collect::<Vec<_>>().join(", ")
}

pub fn read(rep: &mut Report, d: &[u8], kind: Kind) {
    let entries = match zip::read(d) {
        Ok(e) => e,
        Err(e) => {
            rep.note(format!("couldn't open this file's zip container: {e}"));
            return;
        }
    };
    let mut comment_authors = BTreeSet::new();
    let mut change_authors = BTreeSet::new();
    let (mut comments, mut changes, mut rsids) = (0usize, 0usize, false);

    for e in &entries {
        let name = e.name.as_str();
        if name.starts_with("docProps/thumbnail") || name.starts_with("Thumbnails/") {
            rep.add_as(Group::Other, Risk::Low, "Office", "Embedded thumbnail", "preview picture of the first page");
            if Kind::sniff(&e.data) == Kind::Jpeg {
                rep.thumbs.push(Thumb { label: "document thumbnail".into(), data: e.data.clone() });
            }
            continue;
        }
        if is_media(name) {
            media(rep, e);
            continue;
        }
        let Some(x) = text(e) else { continue };
        match name {
            "docProps/core.xml" => {
                for (k, v) in flat_children(x) {
                    rep.add("Office", local(&k), &v);
                }
            }
            "docProps/app.xml" => {
                for (k, v) in flat_children(x) {
                    let k = local(&k);
                    // page / word counts and friends are just noise
                    if matches!(k, "Application" | "AppVersion" | "Company" | "Manager" | "Template" | "TotalTime" | "HyperlinkBase") {
                        rep.add("Office", k, &v);
                    }
                }
            }
            "docProps/custom.xml" => {
                let mut prop = String::new();
                for t in tokens(x) {
                    match t {
                        Tok::Open { raw, name, .. } if local(name) == "property" => prop = attr(raw, "name").unwrap_or_default(),
                        Tok::Text(s) if !s.trim().is_empty() && !prop.is_empty() => rep.add("Office custom", &prop, &xml::unescape(s.trim())),
                        _ => {}
                    }
                }
            }
            "meta.xml" => {
                // odf: everything under office:meta
                for t in tokens(x) {
                    if let Tok::Open { raw, name, .. } = t {
                        if local(name) == "template" {
                            rep.add("ODF", "template", &attr(raw, "href").unwrap_or_default());
                        }
                    }
                }
                for key in ["initial-creator", "creator", "creation-date", "date", "generator", "editing-cycles", "editing-duration", "printed-by", "print-date", "title", "subject", "description", "keyword"] {
                    if let Some(v) = xml::text_of(x, key) {
                        rep.add("ODF", key, &v);
                    }
                }
            }
            "xl/workbook.xml" => {
                for t in tokens(x) {
                    if let Tok::Open { raw, name, .. } = t {
                        match local(name) {
                            "absPath" => rep.add_as(Group::Author, Risk::Low, "Excel", "Saved from path", &attr(raw, "url").unwrap_or_default()),
                            "fileSharing" => rep.add("Excel", "LastModifiedBy", &attr(raw, "userName").unwrap_or_default()),
                            _ => {}
                        }
                    }
                }
            }
            _ => {}
        }
        // comments + tracked changes + who made them
        let commentish = name.contains("comment") || name.contains("Comment") || name.ends_with("people.xml") || name.contains("/persons/") || name.ends_with("authors.xml");
        let body = name.starts_with("word/") && name.ends_with(".xml");
        if !commentish && !body {
            continue;
        }
        let mut in_author = false;
        for t in tokens(x) {
            match t {
                Tok::Open { raw, name: n, .. } => {
                    let who = || attr(raw, "author").or_else(|| attr(raw, "name")).or_else(|| attr(raw, "displayName")).filter(|a| !a.is_empty());
                    match local(n) {
                        "comment" | "cm" | "threadedComment" if commentish => {
                            comments += 1;
                            if let Some(a) = attr(raw, "author").filter(|a| !a.is_empty()) {
                                comment_authors.insert(a);
                            }
                        }
                        "cmAuthor" | "person" | "author" if commentish => {
                            match who() {
                                Some(a) => drop(comment_authors.insert(a)),
                                None => in_author = local(n) == "author",
                            }
                        }
                        _ if body && (CHANGE_DROP.contains(&n) || CHANGE_UNWRAP.contains(&n)) => {
                            if let Some(a) = attr(raw, "author") {
                                changes += 1;
                                if !a.is_empty() {
                                    change_authors.insert(a);
                                }
                            }
                        }
                        _ => {}
                    }
                    if body && !rsids && raw.contains(":rsid") {
                        rsids = true;
                    }
                }
                Tok::Text(s) if in_author => {
                    if !s.trim().is_empty() {
                        comment_authors.insert(xml::unescape(s.trim()));
                    }
                }
                Tok::Close { .. } => in_author = false,
                _ => {}
            }
        }
    }
    // file links that point at somebody's disk or a network share
    for e in entries.iter().filter(|e| e.name.ends_with(".rels")) {
        let Some(x) = text(e) else { continue };
        for t in tokens(x) {
            let Tok::Open { raw, name, .. } = t else { continue };
            if local(name) != "Relationship" || attr(raw, "TargetMode").as_deref() != Some("External") {
                continue;
            }
            let target = attr(raw, "Target").unwrap_or_default();
            let ty = attr(raw, "Type").unwrap_or_default();
            if ty.ends_with("/attachedTemplate") {
                rep.add("Office", "Attached template", &target);
            } else if target.starts_with("file:") || target.starts_with("\\\\") || target.get(1..3) == Some(":\\") {
                rep.add("Office", "Linked file", &target);
            }
        }
    }
    if comments > 0 {
        rep.add_as(Group::Author, Risk::Low, "Office", "Comments", &format!("{comments} comment(s)"));
    }
    if !comment_authors.is_empty() {
        rep.add_as(Group::Author, Risk::High, "Office", "Comment authors", &names_list(&comment_authors));
    }
    if changes > 0 {
        rep.add_as(Group::Software, Risk::Low, "Office", "Tracked changes", &format!("{changes} tracked edit(s), deleted text is still in the file"));
    }
    if !change_authors.is_empty() {
        rep.add_as(Group::Author, Risk::High, "Office", "Change authors", &names_list(&change_authors));
    }
    if rsids {
        rep.add_as(Group::Software, Risk::Low, "Office", "Revision ids", "editing-session ids on paragraphs (can link documents edited together)");
    }
    let _ = kind;
}

// photos pasted into a doc keep their own exif (gps included)
fn media(rep: &mut Report, e: &Entry) {
    let kind = Kind::sniff(&e.data);
    if !matches!(kind, Kind::Jpeg | Kind::Png | Kind::Webp | Kind::Gif | Kind::Tiff) {
        return;
    }
    let sub = crate::inspect::inspect_bytes(&e.data, kind);
    let short = e.name.rsplit('/').next().unwrap_or(&e.name);
    for f in sub.fields.iter().filter(|f| f.risk != Risk::None).take(40) {
        rep.add_as(f.group, f.risk, &format!("{short} > {}", f.source), &f.key, &f.value);
    }
    for mut g in sub.gps {
        g.source = format!("{short} > {}", g.source);
        rep.gps.push(g);
    }
}

// "xl/worksheets/" + "../comments1.xml" -> "xl/comments1.xml"
fn resolve(base: &str, target: &str) -> String {
    if let Some(abs) = target.strip_prefix('/') {
        return abs.to_string();
    }
    let mut parts: Vec<&str> = base.split('/').filter(|p| !p.is_empty()).collect();
    for p in target.split('/') {
        match p {
            "" | "." => {}
            ".." => drop(parts.pop()),
            p => parts.push(p),
        }
    }
    parts.join("/")
}

// "xl/worksheets/_rels/sheet1.xml.rels" -> ("xl/worksheets/", "xl/worksheets/sheet1.xml")
fn rels_owner(rels: &str) -> (String, String) {
    let (dir, file) = match rels.rfind("_rels/") {
        Some(i) => (&rels[..i], &rels[i + 6..]),
        None => ("", rels),
    };
    (dir.to_string(), format!("{dir}{}", file.trim_end_matches(".rels")))
}

// cuts whole <name>..</name> subtrees when `kill` says so
fn drop_where(x: &str, want: &str, kill: impl Fn(&str) -> bool) -> String {
    let base = x.as_ptr() as usize;
    let mut out = String::with_capacity(x.len());
    let (mut depth, mut start, mut copied) = (0usize, 0usize, 0usize);
    for t in tokens(x) {
        match t {
            Tok::Open { raw, name, empty } if local(name) == want => {
                let at = raw.as_ptr() as usize - base;
                if depth == 0 {
                    start = at;
                }
                if empty {
                    if depth == 0 && kill(raw) {
                        out.push_str(&x[copied..at]);
                        copied = at + raw.len();
                    }
                } else {
                    depth += 1;
                }
            }
            Tok::Close { raw, name } if local(name) == want && depth > 0 => {
                depth -= 1;
                let end = raw.as_ptr() as usize - base + raw.len();
                if depth == 0 && kill(&x[start..end]) {
                    out.push_str(&x[copied..start]);
                    copied = end;
                }
            }
            _ => {}
        }
    }
    out.push_str(&x[copied..]);
    out
}

pub fn clean(d: &[u8], kind: Kind, o: &CleanOptions) -> Result<Vec<u8>, String> {
    let mut entries = zip::read(d)?;
    if kind == Kind::Odf {
        return Ok(clean_odf(entries, o));
    }

    // 1. parts that go away completely
    let mut gone: HashSet<String> = HashSet::new();
    for e in &entries {
        let n = e.name.as_str();
        let comment_part = (n.starts_with("xl/comments") && n.ends_with(".xml"))
            || n.starts_with("xl/threadedComments/")
            || n.starts_with("xl/persons/")
            || n.starts_with("ppt/comments/")
            || n == "ppt/commentAuthors.xml"
            || n == "ppt/authors.xml";
        let history = n.starts_with("xl/revisions/") || n == "ppt/revisionInfo.xml" || n.starts_with("ppt/changesInfos/");
        if n.starts_with("docProps/thumbnail") || history || (o.remove_comments && comment_part) {
            gone.insert(n.to_string());
        }
    }

    // 2. excel keeps the comment bubbles as vml shapes in a separate drawing part
    let mut legacy: HashMap<String, String> = HashMap::new(); // sheet part -> rId of a vml part we're dropping
    let mut vml_fix: HashMap<String, String> = HashMap::new();
    if o.remove_comments && kind == Kind::Xlsx {
        for e in entries.iter().filter(|e| e.name.ends_with(".rels")) {
            let Some(x) = text(e) else { continue };
            let (base, owner) = rels_owner(&e.name);
            for t in tokens(x) {
                let Tok::Open { raw, name, .. } = t else { continue };
                if local(name) != "Relationship" || !attr(raw, "Type").unwrap_or_default().ends_with("/vmlDrawing") {
                    continue;
                }
                let part = resolve(&base, &attr(raw, "Target").unwrap_or_default());
                let Some(vml) = entries.iter().find(|v| v.name == part).and_then(text) else { continue };
                let shapes = vml.matches("<v:shape ").count();
                let notes = vml.matches("ObjectType=\"Note\"").count();
                if notes > 0 && notes >= shapes {
                    gone.insert(part);
                    legacy.insert(owner.clone(), attr(raw, "Id").unwrap_or_default());
                } else if notes > 0 {
                    vml_fix.insert(part, drop_where(vml, "shape", |s| s.contains("ObjectType=\"Note\"")));
                }
            }
        }
    }
    // rels files that belonged to dropped parts
    let orphan_rels: Vec<String> = entries.iter().filter(|e| e.name.ends_with(".rels") && gone.contains(&rels_owner(&e.name).1)).map(|e| e.name.clone()).collect();
    gone.extend(orphan_rels);

    // 3. rewrite what stays
    for e in entries.iter_mut() {
        if gone.contains(&e.name) {
            continue;
        }
        let name = e.name.clone();
        if is_media(&name) {
            // keep the colour profile so pictures inside the doc don't shift
            let k = Kind::sniff(&e.data);
            let opts = CleanOptions { keep_icc: true, ..o.clone() };
            if let Ok(clean) = crate::clean::clean_bytes(&e.data, k, &opts) {
                e.data = clean;
            }
            continue;
        }
        if let Some(fixed) = vml_fix.remove(&name) {
            e.data = fixed.into_bytes();
            continue;
        }
        let new = match name.as_str() {
            "docProps/core.xml" => Some(CORE_BLANK.to_string()),
            "docProps/app.xml" => Some(APP_BLANK.to_string()),
            _ => text(e).and_then(|x| rewrite_part(&name, x, kind, o, &gone, &legacy)),
        };
        if let Some(new) = new {
            e.data = new.into_bytes();
        }
    }
    entries.retain(|e| !gone.contains(&e.name));
    Ok(zip::write(&entries))
}

fn rewrite_part(name: &str, x: &str, kind: Kind, o: &CleanOptions, gone: &HashSet<String>, legacy: &HashMap<String, String>) -> Option<String> {
    let rsid = |a: &str| local(a).starts_with("rsid");
    let none = |_: &str| false;

    if name.ends_with(".rels") {
        let (base, _) = rels_owner(name);
        return Some(xml::rewrite(
            x,
            |n, raw| {
                if local(n) != "Relationship" {
                    return Act::Keep;
                }
                let ty = attr(raw, "Type").unwrap_or_default();
                let external = attr(raw, "TargetMode").as_deref() == Some("External");
                let dead = if external {
                    // template + mail merge sources are paths on the author's machine
                    ty.ends_with("/attachedTemplate") || ty.contains("mailMerge")
                } else {
                    gone.contains(&resolve(&base, &attr(raw, "Target").unwrap_or_default()))
                };
                if dead { Act::Drop } else { Act::Keep }
            },
            none,
        ));
    }
    if name == "[Content_Types].xml" {
        return Some(xml::rewrite(
            x,
            |n, raw| {
                let part = attr(raw, "PartName").unwrap_or_default();
                if local(n) == "Override" && gone.contains(part.trim_start_matches('/')) { Act::Drop } else { Act::Keep }
            },
            none,
        ));
    }
    if name == "docProps/custom.xml" {
        return xml::empty_root(x);
    }
    match kind {
        Kind::Docx if name.starts_with("word/") && name.ends_with(".xml") => {
            let file = name.rsplit('/').next().unwrap_or(name);
            if file == "people.xml" || (o.remove_comments && file.starts_with("comments")) {
                return xml::empty_root(x);
            }
            if file == "settings.xml" {
                return Some(xml::rewrite(
                    x,
                    |n, _| if matches!(n, "w:rsids" | "w:attachedTemplate" | "w:mailMerge" | "w:docVars") || local(n) == "docId" { Act::Drop } else { Act::Keep },
                    rsid,
                ));
            }
            Some(xml::rewrite(
                x,
                |n, _| {
                    // w:rsid shows up as its own element inside style definitions
                    if CHANGE_DROP.contains(&n) || n == "w:rsid" || (o.remove_comments && COMMENT_MARKS.contains(&n)) {
                        Act::Drop
                    } else if CHANGE_UNWRAP.contains(&n) {
                        Act::Unwrap
                    } else {
                        Act::Keep
                    }
                },
                rsid,
            ))
        }
        Kind::Xlsx if name == "xl/workbook.xml" => {
            // absPath is the full folder the workbook was last saved in
            let x = drop_where(x, "AlternateContent", |s| s.contains("absPath"));
            Some(xml::rewrite(&x, |n, _| if matches!(local(n), "fileVersion" | "fileSharing" | "absPath") { Act::Drop } else { Act::Keep }, none))
        }
        // modern powerpoint comments leave a pointer in the slide's extension list
        Kind::Pptx if o.remove_comments && name.starts_with("ppt/slides/") && x.contains("commentRel") => {
            Some(drop_where(x, "ext", |s| s.contains("commentRel")))
        }
        Kind::Xlsx => {
            let rid = legacy.get(name)?;
            Some(xml::rewrite(x, |n, raw| if local(n) == "legacyDrawing" && attr(raw, "id").as_deref() == Some(rid.as_str()) { Act::Drop } else { Act::Keep }, none))
        }
        _ => None,
    }
}

fn clean_odf(mut entries: Vec<Entry>, o: &CleanOptions) -> Vec<u8> {
    entries.retain(|e| !e.name.starts_with("Thumbnails/"));
    for e in entries.iter_mut() {
        if is_media(&e.name) {
            let opts = CleanOptions { keep_icc: true, ..o.clone() };
            if let Ok(clean) = crate::clean::clean_bytes(&e.data, Kind::sniff(&e.data), &opts) {
                e.data = clean;
            }
            continue;
        }
        let Some(x) = text(e) else { continue };
        let new = match e.name.as_str() {
            "meta.xml" => xml::rewrite(x, |n, _| if n.starts_with("meta:") || n.starts_with("dc:") { Act::Drop } else { Act::Keep }, |_| false),
            "META-INF/manifest.xml" => xml::rewrite(
                x,
                |n, raw| if local(n) == "file-entry" && attr(raw, "full-path").unwrap_or_default().starts_with("Thumbnails/") { Act::Drop } else { Act::Keep },
                |_| false,
            ),
            _ => continue,
        };
        e.data = new.into_bytes();
    }
    zip::write(&entries)
}

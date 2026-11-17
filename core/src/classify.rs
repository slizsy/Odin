// sorts raw fields into groups and decides how sensitive each one is

use crate::ai;
use crate::report::{Group, Report, Risk};

const LOCATION: &[&str] = &[
    "city", "sub-location", "sublocation", "province-state", "state", "country", "country-code", "countrycode",
    "country-primarylocationname", "country-primarylocationcode", "location", "locationname", "worldregion",
    "countryname", "provincestate",
];
const AUTHOR: &[&str] = &[
    "artist", "xpauthor", "author", "authors", "cameraownername", "ownername", "owner", "lastmodifiedby", "company",
    "manager", "initial-creator", "copyright", "rights", "by-line", "by-linetitle", "writer-editor", "credit",
    "printed-by", "copyrightnotice", "captionwriter", "contact", "authorsposition", "creditline", "usageterms",
    "webstatement", "publisher", "contributor", "comment authors", "change authors", "lastauthor", "source",
    "copyrightownername", "licensorname", "imagecreatorname", "encoded_by", "encodedby", "album_artist", "composer", "title", "subject", "description", "keywords",
    "imagedescription", "usercomment", "xpcomment", "xptitle", "xpsubject", "xpkeywords", "comment", "headline",
    "caption-abstract", "objectname", "category", "dc:title",
];
const DEVICE: &[&str] = &[
    "make", "model", "lensmake", "lensmodel", "lensspecification", "lensinfo", "lens", "hostcomputer",
    "uniquecameramodel", "localizedcameramodel", "devicemanufacturer", "devicemodel", "android version",
    "os version",
];
const SOFTWARE: &[&str] = &[
    "software", "creatortool", "producer", "application", "appversion", "generator", "softwareagent",
    "originatingprogram", "programversion", "encoder", "encodingtool", "encoded by", "tool", "processingsoftware",
    "pdf version", "docsecurity", "writingapplication", "muxingapplication", "handler",
];
// ids + trails that tie files together or show how something was edited
const TRAIL: &[&str] = &[
    "documentid", "instanceid", "originaldocumentid", "template", "revision", "revisionnumber", "totaltime",
    "editing-duration", "editing-cycles", "saves", "history", "derivedfrom", "preservedfilename", "rawfilename",
    "originalfilename", "ingredients", "pantry", "manifest", "tracked changes", "comments", "file id",
    "attached template", "linked file", "embedded thumbnail", "hidden data",
];

pub fn local_key(key: &str) -> String {
    // "xmpMM:History/stEvt:softwareAgent" -> "softwareagent"
    let k = key.rsplit('/').next().unwrap_or(key);
    let k = k.rsplit(':').next().unwrap_or(k);
    k.trim().to_ascii_lowercase()
}

fn is_date(k: &str) -> bool {
    if k.contains("exposure") || k.contains("shutter") || k.contains("timescale") || k == "totaltime" {
        return false;
    }
    k.contains("date") || k.contains("time") || k.contains("subsec")
        || matches!(k, "created" | "modified" | "lastprinted" | "lastsaved" | "when" | "timezone" | "day")
}

pub fn classify(source: &str, key: &str) -> (Group, Risk) {
    let k = local_key(key);
    let k = k.as_str();
    // pdf "Creator" is the app that made it, everywhere else it's a person
    let pdf = source.starts_with("PDF");

    if k.starts_with("gps") || k.contains("location") || k.contains("iso6709") || k.contains("coordinates") || LOCATION.contains(&k) {
        return (Group::Location, Risk::High);
    }
    if k.contains("serial") || matches!(k, "imageuniqueid" | "cameraid" | "lensid" | "deviceid" | "uniqueid") {
        return (Group::Device, Risk::High);
    }
    if DEVICE.contains(&k) || k == "maker notes" {
        return (Group::Device, Risk::Low);
    }
    if k == "creator" {
        return if pdf { (Group::Software, Risk::None) } else { (Group::Author, Risk::High) };
    }
    if AUTHOR.contains(&k) {
        let name = !matches!(
            k,
            "title" | "subject" | "description" | "keywords" | "imagedescription" | "usercomment" | "xpcomment" | "xptitle"
                | "xpsubject" | "xpkeywords" | "comment" | "headline" | "caption-abstract" | "objectname" | "category"
        );
        return (Group::Author, if name { Risk::High } else { Risk::Low });
    }
    if SOFTWARE.contains(&k) {
        return (Group::Software, Risk::None);
    }
    if TRAIL.contains(&k) {
        return (Group::Software, Risk::Low);
    }
    if is_date(k) {
        return (Group::Dates, Risk::Low);
    }
    (Group::Other, Risk::None)
}

// runs after a parser is done: groups everything, hunts for leaks inside values, builds findings
pub fn finish(r: &mut Report) {
    let mut extra: Vec<(Risk, &'static str, String)> = vec![];
    for f in r.fields.iter_mut() {
        if f.group == Group::Other && f.risk == Risk::None {
            (f.group, f.risk) = classify(&f.source, &f.key);
        }
        if f.group != Group::Ai {
            if let Some(what) = ai::hint(&f.source, &f.key, &f.value) {
                f.group = Group::Ai;
                f.risk = Risk::High;
                extra.push((Risk::High, "ai tag", format!("{}: {}", f.key, what)));
            }
        }
        for user in usernames_in(&f.value) {
            f.risk = Risk::High;
            extra.push((Risk::High, "path leak", format!("username \"{}\" in {}", user, f.key)));
        }
        for mail in emails_in(&f.value) {
            f.risk = Risk::High;
            extra.push((Risk::High, "email", format!("{} in {}", mail, f.key)));
        }
    }
    for (risk, kind, detail) in extra {
        r.find(risk, kind, detail);
    }
    let gps: Vec<_> = r.gps.iter().map(|g| format!("{:.6}, {:.6} (from {})", g.lat, g.lon, g.source)).collect();
    for g in gps {
        r.find(Risk::High, "location", g);
    }
    if r.c2pa {
        r.find(Risk::High, "content credentials", "c2pa manifest embedded (says where the file came from and what made it)".to_string());
    }
    if !r.thumbs.is_empty() {
        r.find(Risk::Low, "hidden preview", format!("{} embedded preview image(s), can show the original before a crop", r.thumbs.len()));
    }
    let serial = r.fields.iter().any(|f| f.group == Group::Device && f.risk == Risk::High);
    if serial {
        r.find(Risk::High, "device id", "serial number or unique device id (links files made by the same device)".to_string());
    }
    let names: Vec<_> = r.fields.iter().filter(|f| f.group == Group::Author && f.risk == Risk::High).map(|f| f.key.clone()).collect();
    if !names.is_empty() {
        r.find(Risk::High, "names", format!("owner / author fields: {}", dedup_join(names)));
    }
    r.findings.sort_by(|a, b| b.risk.cmp(&a.risk));
    // most sensitive first inside each group, the ui shows them in this order
    r.fields.sort_by(|a, b| b.risk.cmp(&a.risk));
}

fn dedup_join(mut v: Vec<String>) -> String {
    v.sort();
    v.dedup();
    v.truncate(8);
    v.join(", ")
}

// C:\Users\bob\..., /home/bob/..., /Users/bob/...
pub fn usernames_in(v: &str) -> Vec<String> {
    let mut out = vec![];
    let lower = v.to_ascii_lowercase();
    for (marker, seps) in [("\\users\\", "\\/"), ("/users/", "/\\"), ("/home/", "/"), ("\\documents and settings\\", "\\/")] {
        let mut from = 0;
        while let Some(i) = lower[from..].find(marker) {
            let start = from + i + marker.len();
            // "/home/" inside a url (https://x.com/home/..) isn't a path leak
            let before = &lower[..from + i];
            let urlish = before.rsplit(|c: char| c.is_whitespace() || c == '"').next().is_some_and(|s| s.contains("://") && !s.starts_with("file:"));
            let name: String = v[start..].chars().take_while(|c| !seps.contains(*c) && !"\"<>|:*?\r\n".contains(*c)).collect();
            let name = name.trim();
            let boring = matches!(name.to_ascii_lowercase().as_str(), "" | "public" | "shared" | "default" | "all users");
            if !urlish && !boring && name.len() <= 64 && !out.iter().any(|n| n == name) {
                out.push(name.to_string());
            }
            from = start;
        }
    }
    out
}

pub fn emails_in(v: &str) -> Vec<String> {
    let mut out = vec![];
    let b = v.as_bytes();
    let ok = |c: u8| c.is_ascii_alphanumeric() || matches!(c, b'.' | b'_' | b'+' | b'-');
    for (at, _) in v.match_indices('@') {
        let mut s = at;
        while s > 0 && ok(b[s - 1]) {
            s -= 1;
        }
        let mut e = at + 1;
        while e < b.len() && (b[e].is_ascii_alphanumeric() || b[e] == b'-' || b[e] == b'.') {
            e += 1;
        }
        let host = v[at + 1..e].trim_end_matches('.');
        let tld = host.rsplit('.').next().unwrap_or("");
        if s < at && host.contains('.') && tld.len() >= 2 && tld.bytes().all(|c| c.is_ascii_alphabetic()) {
            let mail = format!("{}@{}", &v[s..at], host);
            if !out.contains(&mail) {
                out.push(mail);
            }
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn groups() {
        assert_eq!(classify("EXIF", "GPSLatitude"), (Group::Location, Risk::High));
        assert_eq!(classify("EXIF", "BodySerialNumber"), (Group::Device, Risk::High));
        assert_eq!(classify("EXIF", "Model"), (Group::Device, Risk::Low));
        assert_eq!(classify("XMP", "dc:creator"), (Group::Author, Risk::High));
        assert_eq!(classify("PDF info", "Creator"), (Group::Software, Risk::None));
        assert_eq!(classify("EXIF", "DateTimeOriginal"), (Group::Dates, Risk::Low));
        assert_eq!(classify("EXIF", "ExposureTime"), (Group::Other, Risk::None));
        assert_eq!(classify("XMP", "xmpMM:History/stEvt:softwareAgent").0, Group::Software);
    }

    #[test]
    fn leaks() {
        assert_eq!(usernames_in(r"C:\Users\bob\Documents\Normal.dotm"), vec!["bob"]);
        assert_eq!(usernames_in("file:///home/alice/report.odt"), vec!["alice"]);
        assert_eq!(usernames_in("/Users/Mike Smith/Desktop/a.psd"), vec!["Mike Smith"]);
        assert!(usernames_in("https://example.com/home/page").is_empty());
        assert_eq!(emails_in("made by bob.smith+x@mail.example.com."), vec!["bob.smith+x@mail.example.com"]);
        assert!(emails_in("2x @ 300dpi").is_empty());
    }
}

// xmp is rdf/xml stuffed inside other files. this flattens it into key/value fields

use crate::report::{Gps, Report};
use crate::xml::{attrs, tokens, unescape, Tok};

pub fn read(rep: &mut Report, data: &[u8], source: &str) {
    let text = decode(data);
    let Some(start) = text.find("<x:xmpmeta").or_else(|| text.find("<rdf:RDF")) else { return };
    let mut path: Vec<&str> = vec![];
    let mut lat = None;
    let mut lon = None;
    let mut add = |rep: &mut Report, key: String, val: String| {
        let low = key.to_ascii_lowercase();
        if low.ends_with("gpslatitude") {
            lat = coord(&val);
        } else if low.ends_with("gpslongitude") {
            lon = coord(&val);
        }
        rep.add(source, &key, &val);
    };
    for t in tokens(&text[start..]) {
        match t {
            Tok::Open { raw, name, empty } => {
                let props = prop_path(&path);
                for (an, av, _) in attrs(raw) {
                    if an.starts_with("xmlns") || matches!(an, "rdf:about" | "rdf:parseType" | "xml:lang" | "x:xmptk" | "rdf:ID" | "rdf:nodeID") {
                        continue;
                    }
                    // on a property element the attribute hangs off that property
                    let mut key = props.clone();
                    if !name.starts_with("rdf:") && !name.starts_with("x:") {
                        push(&mut key, name);
                    }
                    if an != "rdf:resource" {
                        push(&mut key, an);
                    }
                    if !key.is_empty() {
                        add(rep, key, unescape(av));
                    }
                }
                if !empty {
                    path.push(name);
                }
            }
            Tok::Close { name, .. } => {
                path.pop();
                if name == "x:xmpmeta" {
                    break;
                }
            }
            Tok::Text(s) => {
                let v = s.trim();
                let key = prop_path(&path);
                if !v.is_empty() && !key.is_empty() {
                    add(rep, key, unescape(v));
                }
            }
            Tok::Other(_) => {}
        }
    }
    if let (Some(lat), Some(lon)) = (lat, lon) {
        if !rep.gps.iter().any(|g| (g.lat - lat).abs() < 1e-4 && (g.lon - lon).abs() < 1e-4) {
            rep.gps.push(Gps { lat, lon, alt: None, accuracy_m: None, source: source.to_string() });
        }
    }
}

fn push(key: &mut String, part: &str) {
    if !key.is_empty() {
        key.push('/');
    }
    key.push_str(part);
}

// the property names on the way down, minus the rdf plumbing
fn prop_path(path: &[&str]) -> String {
    let mut key = String::new();
    for p in path.iter().filter(|p| !p.starts_with("rdf:") && !p.starts_with("x:")) {
        push(&mut key, p);
    }
    key
}

// some packets are utf16
fn decode(data: &[u8]) -> String {
    let zeros = data.iter().take(200).filter(|&&b| b == 0).count();
    if zeros > 40 {
        let le = data.get(1) == Some(&0) || data.starts_with(&[0xFF, 0xFE]);
        let u: Vec<u16> = data.chunks_exact(2).map(|c| if le { u16::from_le_bytes([c[0], c[1]]) } else { u16::from_be_bytes([c[0], c[1]]) }).collect();
        return String::from_utf16_lossy(&u);
    }
    String::from_utf8_lossy(data).into_owned()
}

// xmp writes gps as "45,30.912N" or "45,30,54.7N", sometimes just a decimal
fn coord(v: &str) -> Option<f64> {
    let v = v.trim();
    let (num, r) = match v.chars().last()? {
        c @ ('N' | 'S' | 'E' | 'W') => (&v[..v.len() - 1], c),
        _ => (v, 'N'),
    };
    let mut parts = num.split(',').map(|p| p.trim().parse::<f64>());
    let mut d = parts.next()?.ok()?;
    if let Some(m) = parts.next() {
        d += m.ok()? / 60.0;
    }
    if let Some(s) = parts.next() {
        d += s.ok()? / 3600.0;
    }
    Some(if r == 'S' || r == 'W' { -d } else { d })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::kind::Kind;

    #[test]
    fn flattens() {
        let x = br#"<?xpacket begin=""?><x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="r">
          <rdf:Description rdf:about="" xmlns:xmp="x" xmp:CreatorTool="Photoshop &amp; co" exif:GPSLatitude="45,30.912N" exif:GPSLongitude="122,40.5W">
            <dc:creator><rdf:Seq><rdf:li>Bob</rdf:li></rdf:Seq></dc:creator>
            <xmpMM:History><rdf:Seq><rdf:li stEvt:action="saved" stEvt:softwareAgent="GIMP"/></rdf:Seq></xmpMM:History>
          </rdf:Description></rdf:RDF></x:xmpmeta>"#;
        let mut r = Report::new(Kind::Jpeg, 0);
        read(&mut r, x, "XMP");
        let get = |k: &str| r.fields.iter().find(|f| f.key == k).map(|f| f.value.as_str());
        assert_eq!(get("xmp:CreatorTool"), Some("Photoshop & co"));
        assert_eq!(get("dc:creator"), Some("Bob"));
        assert_eq!(get("xmpMM:History/stEvt:softwareAgent"), Some("GIMP"));
        assert!((r.gps[0].lat - 45.5152).abs() < 1e-3 && (r.gps[0].lon + 122.675).abs() < 1e-3);
    }
}

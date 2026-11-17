// what inspect() hands back: every field we found, grouped and risk-tagged

use crate::kind::Kind;

#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug, PartialOrd, Ord)]
pub enum Group {
    Location,
    Device,
    Dates,
    Software,
    Author,
    Ai,
    Other,
}

impl Group {
    pub const ALL: [Group; 7] =
        [Group::Location, Group::Author, Group::Device, Group::Dates, Group::Software, Group::Ai, Group::Other];

    pub fn label(self) -> &'static str {
        match self {
            Group::Location => "location",
            Group::Device => "device",
            Group::Dates => "dates",
            Group::Software => "software",
            Group::Author => "author",
            Group::Ai => "ai / content credentials",
            Group::Other => "other",
        }
    }
}

// how bad it is if this leaks. high = can point at a person or a place
#[derive(Clone, Copy, PartialEq, Eq, Debug, PartialOrd, Ord)]
pub enum Risk {
    None,
    Low,
    High,
}

#[derive(Clone, Debug)]
pub struct Field {
    pub group: Group,
    pub source: String, // where it came from: "EXIF", "XMP", "PNG text", ...
    pub key: String,
    pub value: String,
    pub risk: Risk,
}

#[derive(Clone, Debug)]
pub struct Finding {
    pub risk: Risk,
    pub kind: &'static str,
    pub detail: String,
}

#[derive(Clone, Debug)]
pub struct Gps {
    pub lat: f64,
    pub lon: f64,
    pub alt: Option<f64>,
    pub accuracy_m: Option<f64>,
    pub source: String,
}

// an embedded preview image (jpeg bytes), these can show the uncropped original
#[derive(Clone, Debug)]
pub struct Thumb {
    pub label: String,
    pub data: Vec<u8>,
}

#[derive(Clone, Debug)]
pub struct Report {
    pub kind: Kind,
    pub size: u64,
    pub fields: Vec<Field>,
    pub findings: Vec<Finding>,
    pub gps: Vec<Gps>,
    pub thumbs: Vec<Thumb>,
    pub notes: Vec<String>, // stuff that went wrong or couldn't be read
    pub c2pa: bool,
}

const MAX_FIELDS: usize = 2000;
const MAX_VALUE: usize = 600;

impl Report {
    pub fn new(kind: Kind, size: u64) -> Self {
        Report { kind, size, fields: vec![], findings: vec![], gps: vec![], thumbs: vec![], notes: vec![], c2pa: false }
    }

    // group + risk get filled in later by classify. add_as pins them yourself
    pub fn add(&mut self, source: &str, key: &str, value: &str) {
        self.push(Group::Other, Risk::None, source, key, value);
    }

    pub fn add_as(&mut self, group: Group, risk: Risk, source: &str, key: &str, value: &str) {
        self.push(group, risk, source, key, value);
    }

    fn push(&mut self, group: Group, risk: Risk, source: &str, key: &str, value: &str) {
        if self.fields.len() >= MAX_FIELDS {
            return;
        }
        let value = tidy(value);
        if value.is_empty() || key.is_empty() {
            return;
        }
        if self.fields.iter().any(|f| f.source == source && f.key == key && f.value == value) {
            return;
        }
        self.fields.push(Field { group, source: source.to_string(), key: key.to_string(), value, risk });
    }

    pub fn note(&mut self, msg: impl Into<String>) {
        let msg = msg.into();
        if !self.notes.contains(&msg) {
            self.notes.push(msg);
        }
    }

    pub fn find(&mut self, risk: Risk, kind: &'static str, detail: impl Into<String>) {
        let detail = detail.into();
        if !self.findings.iter().any(|f| f.kind == kind && f.detail == detail) {
            self.findings.push(Finding { risk, kind, detail });
        }
    }

    pub fn count(&self, risk: Risk) -> usize {
        self.fields.iter().filter(|f| f.risk == risk).count()
    }

    pub fn group(&self, g: Group) -> impl Iterator<Item = &Field> {
        self.fields.iter().filter(move |f| f.group == g)
    }

    pub fn worst(&self) -> Risk {
        self.fields.iter().map(|f| f.risk).max().unwrap_or(Risk::None)
    }
}

// squash control chars + whitespace runs, cap the length
pub fn tidy(v: &str) -> String {
    let mut out = String::with_capacity(v.len().min(MAX_VALUE));
    let mut space = true;
    for c in v.chars() {
        if out.len() >= MAX_VALUE {
            out.push('…');
            break;
        }
        if c.is_whitespace() || c.is_control() {
            if !space {
                out.push(' ');
            }
            space = true;
        } else {
            out.push(c);
            space = false;
        }
    }
    out.trim_end().to_string()
}

// bytes that are probably text: utf8 if it parses, latin1 otherwise
pub fn text_of(b: &[u8]) -> String {
    let b = match b.iter().position(|&c| c == 0) {
        Some(i) if b[i..].iter().all(|&c| c == 0) => &b[..i],
        _ => b,
    };
    match std::str::from_utf8(b) {
        Ok(s) => s.to_string(),
        Err(_) => b.iter().map(|&c| c as char).collect(),
    }
}

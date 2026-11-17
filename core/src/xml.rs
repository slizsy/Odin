// tiny xml tokenizer. no dom, no validation, just enough to walk office xml and xmp
// and write it back out byte for byte except the bits we cut

#[derive(Clone, Copy, Debug)]
pub enum Tok<'a> {
    Open { raw: &'a str, name: &'a str, empty: bool },
    Close { raw: &'a str, name: &'a str },
    Text(&'a str),
    Other(&'a str), // comments, <?pi?>, cdata, doctype
}

pub struct Tokens<'a> {
    s: &'a str,
    pos: usize,
}

pub fn tokens(s: &str) -> Tokens<'_> {
    Tokens { s, pos: 0 }
}

impl<'a> Iterator for Tokens<'a> {
    type Item = Tok<'a>;

    fn next(&mut self) -> Option<Tok<'a>> {
        let s = self.s;
        if self.pos >= s.len() {
            return None;
        }
        let rest = &s[self.pos..];
        if !rest.starts_with('<') {
            let end = rest.find('<').unwrap_or(rest.len());
            self.pos += end;
            return Some(Tok::Text(&rest[..end]));
        }
        // the odd ones first
        for (open, close) in [("<!--", "-->"), ("<![CDATA[", "]]>"), ("<?", "?>"), ("<!", ">")] {
            if rest.starts_with(open) {
                let end = rest[open.len()..].find(close).map(|i| i + open.len() + close.len()).unwrap_or(rest.len());
                self.pos += end;
                return Some(Tok::Other(&rest[..end]));
            }
        }
        // a real tag. > can show up inside quoted attribute values so track quotes
        let mut quote = 0u8;
        let mut end = rest.len();
        for (i, &b) in rest.as_bytes().iter().enumerate().skip(1) {
            if quote != 0 {
                if b == quote {
                    quote = 0;
                }
            } else if b == b'"' || b == b'\'' {
                quote = b;
            } else if b == b'>' {
                end = i + 1;
                break;
            }
        }
        let raw = &rest[..end];
        self.pos += end;
        let inner = raw.trim_start_matches('<').trim_end_matches('>');
        if let Some(n) = inner.strip_prefix('/') {
            return Some(Tok::Close { raw, name: n.trim() });
        }
        let empty = inner.ends_with('/');
        let name_end = inner.find(|c: char| c.is_whitespace() || c == '/').unwrap_or(inner.len());
        Some(Tok::Open { raw, name: &inner[..name_end], empty })
    }
}

// "w:ins" -> "ins"
pub fn local(name: &str) -> &str {
    name.rsplit(':').next().unwrap_or(name)
}

// every attribute on an open tag as (name, raw value still escaped, byte range in raw incl. leading space)
pub fn attrs(raw: &str) -> Vec<(&str, &str, std::ops::Range<usize>)> {
    let b = raw.as_bytes();
    let mut out = vec![];
    // skip "<name"
    let mut i = 1;
    while i < b.len() && !b[i].is_ascii_whitespace() && b[i] != b'>' && b[i] != b'/' {
        i += 1;
    }
    loop {
        let start = i;
        while i < b.len() && b[i].is_ascii_whitespace() {
            i += 1;
        }
        let n0 = i;
        while i < b.len() && !b[i].is_ascii_whitespace() && b[i] != b'=' && b[i] != b'>' && b[i] != b'/' {
            i += 1;
        }
        if i == n0 {
            break;
        }
        let name = &raw[n0..i];
        while i < b.len() && b[i].is_ascii_whitespace() {
            i += 1;
        }
        if i >= b.len() || b[i] != b'=' {
            break;
        }
        i += 1;
        while i < b.len() && b[i].is_ascii_whitespace() {
            i += 1;
        }
        if i >= b.len() || (b[i] != b'"' && b[i] != b'\'') {
            break;
        }
        let q = b[i];
        let v0 = i + 1;
        let Some(len) = b[v0..].iter().position(|&c| c == q) else { break };
        i = v0 + len + 1;
        out.push((name, &raw[v0..v0 + len], start..i));
    }
    out
}

pub fn attr(raw: &str, name: &str) -> Option<String> {
    attrs(raw).into_iter().find(|(n, _, _)| *n == name || local(n) == name).map(|(_, v, _)| unescape(v))
}

pub fn unescape(s: &str) -> String {
    if !s.contains('&') {
        return s.to_string();
    }
    let mut out = String::with_capacity(s.len());
    let mut rest = s;
    while let Some(i) = rest.find('&') {
        out.push_str(&rest[..i]);
        rest = &rest[i..];
        let Some(end) = rest.find(';').filter(|&e| e <= 10) else {
            out.push('&');
            rest = &rest[1..];
            continue;
        };
        let ent = &rest[1..end];
        let ch = match ent {
            "lt" => Some('<'),
            "gt" => Some('>'),
            "amp" => Some('&'),
            "quot" => Some('"'),
            "apos" => Some('\''),
            _ if ent.starts_with("#x") || ent.starts_with("#X") => u32::from_str_radix(&ent[2..], 16).ok().and_then(char::from_u32),
            _ if ent.starts_with('#') => ent[1..].parse().ok().and_then(char::from_u32),
            _ => None,
        };
        match ch {
            Some(c) => {
                out.push(c);
                rest = &rest[end + 1..];
            }
            None => {
                out.push('&');
                rest = &rest[1..];
            }
        }
    }
    out.push_str(rest);
    out
}

// what to do with an element while rewriting
#[derive(Clone, Copy, PartialEq)]
pub enum Act {
    Keep,
    Drop,   // the element and everything inside it
    Unwrap, // lose the tags, keep what's inside
}

// streams xml through, asking `decide` about every open tag. `strip_attr` says which
// attributes to cut from tags that stay. everything else comes out untouched
pub fn rewrite(xml: &str, mut decide: impl FnMut(&str, &str) -> Act, strip_attr: impl Fn(&str) -> bool) -> String {
    let mut out = String::with_capacity(xml.len());
    let mut stack: Vec<Act> = vec![];
    let mut dropping = 0usize;
    for t in tokens(xml) {
        match t {
            Tok::Open { raw, name, empty } => {
                let act = if dropping > 0 { Act::Drop } else { decide(name, raw) };
                if !empty {
                    stack.push(act);
                    if act == Act::Drop {
                        dropping += 1;
                    }
                }
                if act == Act::Keep && dropping == 0 {
                    push_tag(&mut out, raw, &strip_attr);
                }
            }
            Tok::Close { raw, .. } => {
                let act = stack.pop().unwrap_or(Act::Keep);
                if act == Act::Drop {
                    dropping = dropping.saturating_sub(1);
                } else if act == Act::Keep && dropping == 0 {
                    out.push_str(raw);
                }
            }
            Tok::Text(s) | Tok::Other(s) => {
                if dropping == 0 {
                    out.push_str(s);
                }
            }
        }
    }
    out
}

fn push_tag(out: &mut String, raw: &str, strip_attr: &impl Fn(&str) -> bool) {
    let cut: Vec<_> = attrs(raw).into_iter().filter(|(n, _, _)| strip_attr(n)).map(|(_, _, r)| r).collect();
    if cut.is_empty() {
        out.push_str(raw);
        return;
    }
    let mut at = 0;
    for r in cut {
        out.push_str(&raw[at..r.start]);
        at = r.end;
    }
    out.push_str(&raw[at..]);
}

// keeps the prolog and the root tag (with its namespaces), throws away everything inside
pub fn empty_root(xml: &str) -> Option<String> {
    let mut out = String::new();
    for t in tokens(xml) {
        match t {
            Tok::Other(s) if s.starts_with("<?") => out.push_str(s),
            Tok::Open { raw, name, empty } => {
                if empty {
                    out.push_str(raw);
                } else {
                    out.push_str(raw);
                    out.push_str("</");
                    out.push_str(name);
                    out.push('>');
                }
                return Some(out);
            }
            _ => {}
        }
    }
    None
}

// all the text inside the first element with this local name
pub fn text_of(xml: &str, want: &str) -> Option<String> {
    let mut depth = 0usize;
    let mut out = String::new();
    for t in tokens(xml) {
        match t {
            Tok::Open { name, empty, .. } => {
                if depth > 0 && !empty {
                    depth += 1;
                } else if depth == 0 && local(name) == want {
                    if empty {
                        return Some(String::new());
                    }
                    depth = 1;
                }
            }
            Tok::Close { .. } if depth > 0 => {
                depth -= 1;
                if depth == 0 {
                    return Some(unescape(&out));
                }
            }
            Tok::Text(s) if depth > 0 => out.push_str(s),
            Tok::Other(s) if depth > 0 && s.starts_with("<![CDATA[") => out.push_str(&s[9..s.len().saturating_sub(3)]),
            _ => {}
        }
    }
    None
}

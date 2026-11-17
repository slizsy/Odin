// photoshop "8BIM" resource blocks (jpeg app13) and the old iptc records inside them

use super::{be16, be32};
use crate::report::{text_of, Report, Thumb};

pub fn read_irb(rep: &mut Report, data: &[u8]) {
    let mut p = 0;
    while data.get(p..p + 4) == Some(b"8BIM") {
        let Some(id) = be16(data, p + 4) else { break };
        // pascal string name, padded so len byte + name is even
        let nlen = *data.get(p + 6).unwrap_or(&0) as usize;
        let mut q = p + 7 + nlen;
        if (1 + nlen) % 2 == 1 {
            q += 1;
        }
        let Some(size) = be32(data, q).map(|s| s as usize) else { break };
        let Some(body) = data.get(q + 4..q + 4 + size) else { break };
        match id {
            0x0404 => read_iim(rep, body),
            // thumbnail resource: 28 byte header then a jpeg
            0x040C => {
                if let Some(jpg) = body.get(28..).filter(|j| j.starts_with(&[0xFF, 0xD8])) {
                    rep.thumbs.push(Thumb { label: "photoshop thumbnail".into(), data: jpg.to_vec() });
                }
            }
            0x0409 => rep.add("Photoshop", "Embedded thumbnail", "old style raw thumbnail"),
            _ => {}
        }
        p = q + 4 + size + (size % 2);
    }
}

pub fn read_iim(rep: &mut Report, data: &[u8]) {
    let mut p = 0;
    while p + 5 <= data.len() && data[p] == 0x1C {
        let (rec, ds) = (data[p + 1], data[p + 2]);
        let len = be16(data, p + 3).unwrap_or(0) as usize;
        if len & 0x8000 != 0 {
            break; // extended length datasets, nobody writes these
        }
        let Some(body) = data.get(p + 5..p + 5 + len) else { break };
        if rec == 2 {
            let name = match ds {
                5 => "ObjectName",
                25 => "Keywords",
                40 => "SpecialInstructions",
                55 => "DateCreated",
                60 => "TimeCreated",
                62 => "DigitalCreationDate",
                63 => "DigitalCreationTime",
                65 => "OriginatingProgram",
                70 => "ProgramVersion",
                80 => "By-line",
                85 => "By-lineTitle",
                90 => "City",
                92 => "Sub-location",
                95 => "Province-State",
                100 => "Country-Code",
                101 => "Country",
                103 => "OriginalTransmissionReference",
                105 => "Headline",
                110 => "Credit",
                115 => "Source",
                116 => "CopyrightNotice",
                118 => "Contact",
                120 => "Caption-Abstract",
                122 => "Writer-Editor",
                _ => "",
            };
            if ds != 0 {
                let key = if name.is_empty() { format!("record 2:{ds}") } else { name.to_string() };
                rep.add("IPTC", &key, &text_of(body));
            }
        }
        p += 5 + len;
    }
}

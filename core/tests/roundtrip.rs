// builds little files with known leaks baked in, checks inspect() sees them,
// cleans them, and checks they're gone and the file still opens

use odin_core::formats::png::make_chunk;
use odin_core::zip::{self, Entry};
use odin_core::{clean::clean_bytes, inspect_bytes, CleanOptions, Group, Kind, Report, Risk};

// ---------- fixture helpers ----------

fn rat(n: u32, d: u32) -> Vec<u8> {
    [n.to_le_bytes(), d.to_le_bytes()].concat()
}

fn ascii(s: &str) -> (u16, u32, Vec<u8>) {
    let mut b = s.as_bytes().to_vec();
    b.push(0);
    (2, b.len() as u32, b)
}

// appends an ifd, returns where each entry's value field landed (for patching pointers)
fn ifd(buf: &mut Vec<u8>, entries: &[(u16, (u16, u32, Vec<u8>))]) -> Vec<usize> {
    let mut data_at = buf.len() + 2 + entries.len() * 12 + 4;
    let (mut tail, mut pos) = (vec![], vec![]);
    buf.extend_from_slice(&(entries.len() as u16).to_le_bytes());
    for (tag, (ty, count, data)) in entries {
        buf.extend_from_slice(&tag.to_le_bytes());
        buf.extend_from_slice(&ty.to_le_bytes());
        buf.extend_from_slice(&count.to_le_bytes());
        pos.push(buf.len());
        if data.len() <= 4 {
            let mut v = data.clone();
            v.resize(4, 0);
            buf.extend_from_slice(&v);
        } else {
            buf.extend_from_slice(&(data_at as u32).to_le_bytes());
            data_at += data.len() + (data.len() & 1);
            tail.extend_from_slice(data);
            if data.len() & 1 == 1 {
                tail.push(0);
            }
        }
    }
    buf.extend_from_slice(&0u32.to_le_bytes());
    buf.extend_from_slice(&tail);
    pos
}

// exif block: camera, owner, serial, orientation 6, gps in portland
fn tiff() -> Vec<u8> {
    let mut t = b"II*\0\x08\0\0\0".to_vec();
    let long0 = (4u16, 1u32, vec![0u8; 4]);
    let pos = ifd(
        &mut t,
        &[
            (0x010F, ascii("TestCam")),
            (0x0110, ascii("Model X1")),
            (0x0112, (3, 1, 6u16.to_le_bytes().to_vec())),
            (0x0132, ascii("2026:11:16 10:42:33")),
            (0x013B, ascii("Bob Owner")),
            (0x8769, long0.clone()),
            (0x8825, long0),
        ],
    );
    let exif_at = t.len() as u32;
    t[pos[5]..pos[5] + 4].copy_from_slice(&exif_at.to_le_bytes());
    ifd(&mut t, &[(0x9003, ascii("2026:11:16 10:42:33")), (0xA431, ascii("SN-0012345"))]);
    let gps_at = t.len() as u32;
    t[pos[6]..pos[6] + 4].copy_from_slice(&gps_at.to_le_bytes());
    ifd(
        &mut t,
        &[
            (1, ascii("N")),
            (2, (5, 3, [rat(45, 1), rat(30, 1), rat(5472, 100)].concat())),
            (3, ascii("W")),
            (4, (5, 3, [rat(122, 1), rat(40, 1), rat(4224, 100)].concat())),
        ],
    );
    t
}

const XMP: &str = r#"<?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?><x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"><rdf:Description rdf:about="" xmlns:xmp="http://ns.adobe.com/xap/1.0/" xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:Iptc4xmpExt="http://iptc.org/std/Iptc4xmpExt/2008-02-29/" xmp:CreatorTool="Some Editor 3.1" Iptc4xmpExt:DigitalSourceType="http://cv.iptc.org/newscodes/digitalsourcetype/trainedAlgorithmicMedia"><dc:creator><rdf:Seq><rdf:li>Bob Owner</rdf:li></rdf:Seq></dc:creator></rdf:Description></rdf:RDF></x:xmpmeta><?xpacket end="w"?>"#;

// not a real signed manifest, just shaped like one
fn fake_c2pa() -> Vec<u8> {
    let mut b = vec![];
    b.extend_from_slice(b"\0\0\0\x40jumb\0\0\0\x20jumdc2pa\0\x11\0\x10\x80\0\0\xaa\0\x38\x9b\x71\x03c2pa\0");
    b.extend_from_slice(b"c2pa.actions digitalSourceType trainedAlgorithmicMedia claim_generator Adobe Firefly");
    b
}

fn seg(marker: u8, payload: &[u8]) -> Vec<u8> {
    let mut s = vec![0xFF, marker];
    s.extend_from_slice(&((payload.len() + 2) as u16).to_be_bytes());
    s.extend_from_slice(payload);
    s
}

fn pixels() -> image::RgbImage {
    image::RgbImage::from_fn(16, 12, |x, y| image::Rgb([(x * 16) as u8, (y * 20) as u8, 128]))
}

fn encode(fmt: image::ImageFormat) -> Vec<u8> {
    let mut out = std::io::Cursor::new(vec![]);
    image::DynamicImage::ImageRgb8(pixels()).write_to(&mut out, fmt).unwrap();
    out.into_inner()
}

fn decode(d: &[u8]) -> image::RgbImage {
    image::load_from_memory(d).expect("cleaned image should still decode").to_rgb8()
}

fn leaky_jpeg() -> Vec<u8> {
    let plain = encode(image::ImageFormat::Jpeg);
    let mut out = plain[..2].to_vec();
    out.extend(seg(0xE1, &[b"Exif\0\0".as_slice(), &tiff()].concat()));
    out.extend(seg(0xE1, &[b"http://ns.adobe.com/xap/1.0/\0".as_slice(), XMP.as_bytes()].concat()));
    out.extend(seg(0xEB, &[b"JP\0\x01\0\0\0\x01".as_slice(), &fake_c2pa()].concat()));
    let iptc = b"\x1c\x02\x50\x00\x09Bob Owner\x1c\x02\x5a\x00\x08Portland";
    let mut irb = b"Photoshop 3.0\x008BIM\x04\x04\0\0".to_vec();
    irb.extend_from_slice(&(iptc.len() as u32).to_be_bytes());
    irb.extend_from_slice(iptc);
    out.extend(seg(0xED, &irb));
    out.extend(seg(0xFE, b"edited on bob's laptop"));
    out.extend_from_slice(&plain[2..]);
    out.extend_from_slice(b"SECRET-TRAILER-DATA");
    out
}

fn has(r: &Report, key: &str) -> bool {
    r.fields.iter().any(|f| f.key == key)
}

fn sensitive(r: &Report) -> Vec<String> {
    r.fields.iter().filter(|f| f.risk == Risk::High).map(|f| format!("{}:{}={}", f.source, f.key, f.value)).collect()
}

// ---------- images ----------

#[test]
fn jpeg_roundtrip() {
    let dirty = leaky_jpeg();
    let r = inspect_bytes(&dirty, Kind::Jpeg);
    assert!((r.gps[0].lat - 45.5152).abs() < 1e-4 && (r.gps[0].lon + 122.6784).abs() < 1e-4, "{:?}", r.gps);
    for key in ["Make", "Artist", "BodySerialNumber", "DateTimeOriginal", "xmp:CreatorTool", "dc:creator", "By-line", "City", "Comment", "Hidden data"] {
        assert!(has(&r, key), "missing {key}");
    }
    assert!(r.c2pa && r.group(Group::Ai).count() >= 3);
    assert!(r.fields.iter().any(|f| f.key == "BodySerialNumber" && f.group == Group::Device && f.risk == Risk::High));
    assert!(r.fields.iter().any(|f| f.key == "Artist" && f.group == Group::Author && f.risk == Risk::High));
    assert!(r.findings.iter().any(|f| f.kind == "location"));

    let clean = clean_bytes(&dirty, Kind::Jpeg, &CleanOptions::default()).unwrap();
    let after = inspect_bytes(&clean, Kind::Jpeg);
    // the only thing left is the orientation tag so it doesn't flip sideways
    assert_eq!(after.fields.iter().map(|f| f.key.as_str()).collect::<Vec<_>>(), vec!["Orientation"]);
    assert!(after.gps.is_empty() && !after.c2pa && after.findings.is_empty());
    assert!(!clean.windows(6).any(|w| w == b"SECRET") && !clean.windows(3).any(|w| w == b"Bob"));
    assert_eq!(decode(&clean), decode(&dirty));

    let bare = clean_bytes(&dirty, Kind::Jpeg, &CleanOptions { keep_orientation: false, ..Default::default() }).unwrap();
    assert!(inspect_bytes(&bare, Kind::Jpeg).fields.is_empty());
}

#[test]
fn png_roundtrip() {
    let plain = encode(image::ImageFormat::Png);
    let ihdr_end = 8 + 25;
    let mut dirty = plain[..ihdr_end].to_vec();
    dirty.extend(make_chunk(b"tEXt", b"parameters\0a cat on a couch\nSteps: 20, Sampler: Euler a, Seed: 12345"));
    dirty.extend(make_chunk(b"tEXt", b"Author\0Bob Owner"));
    dirty.extend(make_chunk(b"iTXt", &[b"XML:com.adobe.xmp\0\0\0\0\0".as_slice(), XMP.as_bytes()].concat()));
    dirty.extend(make_chunk(b"eXIf", &tiff()));
    dirty.extend(make_chunk(b"tIME", &[0x07, 0xEA, 11, 16, 10, 42, 33]));
    dirty.extend(make_chunk(b"caBX", &fake_c2pa()));
    dirty.extend_from_slice(&plain[ihdr_end..]);
    dirty.extend_from_slice(b"junk after iend");

    let r = inspect_bytes(&dirty, Kind::Png);
    assert!(r.fields.iter().any(|f| f.key == "parameters" && f.group == Group::Ai), "{:?}", r.fields);
    assert!(r.c2pa && has(&r, "Author") && has(&r, "Modify date") && has(&r, "Hidden data") && !r.gps.is_empty());

    let clean = clean_bytes(&dirty, Kind::Png, &CleanOptions { keep_orientation: false, ..Default::default() }).unwrap();
    let after = inspect_bytes(&clean, Kind::Png);
    assert!(after.fields.is_empty() && after.gps.is_empty() && !after.c2pa, "{:?}", after.fields);
    // lossless format, so the pixels have to match exactly and the file should be the plain one again
    assert_eq!(decode(&clean), pixels());
    assert_eq!(clean, plain);
}

#[test]
fn webp_roundtrip() {
    let plain = encode(image::ImageFormat::WebP);
    let vp8l = &plain[12..];
    let chunk = |ty: &[u8; 4], d: &[u8]| {
        let mut c = ty.to_vec();
        c.extend_from_slice(&(d.len() as u32).to_le_bytes());
        c.extend_from_slice(d);
        if d.len() & 1 == 1 {
            c.push(0);
        }
        c
    };
    // extended layout: flags say exif + xmp, canvas is 16x12
    let mut body = b"WEBP".to_vec();
    body.extend(chunk(b"VP8X", &[0x0C, 0, 0, 0, 15, 0, 0, 11, 0, 0]));
    body.extend_from_slice(vp8l);
    body.extend(chunk(b"EXIF", &tiff()));
    body.extend(chunk(b"XMP ", XMP.as_bytes()));
    let mut dirty = b"RIFF".to_vec();
    dirty.extend_from_slice(&(body.len() as u32).to_le_bytes());
    dirty.extend(body);

    let r = inspect_bytes(&dirty, Kind::Webp);
    assert!(!r.gps.is_empty() && has(&r, "Artist") && has(&r, "dc:creator"));
    let clean = clean_bytes(&dirty, Kind::Webp, &CleanOptions::default()).unwrap();
    let after = inspect_bytes(&clean, Kind::Webp);
    assert_eq!(after.fields.iter().map(|f| f.key.as_str()).collect::<Vec<_>>(), vec!["Orientation"]);
    assert_eq!(decode(&clean), pixels());
}

#[test]
fn gif_roundtrip() {
    let plain = encode(image::ImageFormat::Gif);
    // slip a comment block in right before the first image / control block
    let head = 13 + if plain[10] & 0x80 != 0 { 3usize << ((plain[10] & 7) + 1) } else { 0 };
    let mut dirty = plain[..head].to_vec();
    dirty.extend_from_slice(b"\x21\xFE\x0Emade by bob :)\0");
    dirty.extend_from_slice(&plain[head..]);
    let r = inspect_bytes(&dirty, Kind::Gif);
    assert!(has(&r, "Comment"));
    let clean = clean_bytes(&dirty, Kind::Gif, &CleanOptions::default()).unwrap();
    assert!(inspect_bytes(&clean, Kind::Gif).fields.is_empty());
    assert_eq!(decode(&clean).dimensions(), (16, 12));
}

// a heic skeleton: real item tables, pretend pixel data
#[test]
fn heic_scrub_in_place() {
    fn bx(ty: &[u8; 4], body: &[u8]) -> Vec<u8> {
        let mut b = ((body.len() + 8) as u32).to_be_bytes().to_vec();
        b.extend_from_slice(ty);
        b.extend_from_slice(body);
        b
    }
    let exif_item = [&[0u8, 0, 0, 0][..], &tiff()].concat();
    let xmp_item = XMP.as_bytes().to_vec();
    let ftyp = bx(b"ftyp", b"heic\0\0\0\0mif1heic");
    let infe = |id: u16, ty: &[u8; 4], extra: &[u8]| bx(b"infe", &[&[2, 0, 0, 0][..], &id.to_be_bytes(), &[0, 0], ty, b"\0", extra].concat());
    let iinf = bx(b"iinf", &[&[0, 0, 0, 0, 0, 2][..], &infe(1, b"Exif", b""), &infe(2, b"mime", b"application/rdf+xml\0")].concat());
    // iloc v0, 4 byte offsets + lengths, no base offset. offsets get patched once we know where mdat lands
    let loc = |id: u16, off: u32, len: u32| [&id.to_be_bytes()[..], &[0, 0, 0, 1], &off.to_be_bytes(), &len.to_be_bytes()].concat();
    let build = |mdat_at: u32| {
        let iloc = bx(b"iloc", &[&[0, 0, 0, 0, 0x44, 0x00, 0, 2][..], &loc(1, mdat_at, exif_item.len() as u32), &loc(2, mdat_at + exif_item.len() as u32, xmp_item.len() as u32)].concat());
        let hdlr = bx(b"hdlr", &[&[0u8; 8][..], b"pict", &[0u8; 13]].concat());
        bx(b"meta", &[&[0u8, 0, 0, 0][..], &hdlr, &iinf, &iloc].concat())
    };
    let meta_len = build(0).len();
    let mdat_at = (ftyp.len() + meta_len + 8) as u32;
    let dirty = [ftyp.clone(), build(mdat_at), bx(b"mdat", &[exif_item.clone(), xmp_item.clone(), b"PIXELS".to_vec()].concat())].concat();

    assert_eq!(Kind::sniff(&dirty), Kind::Heif);
    let r = inspect_bytes(&dirty, Kind::Heif);
    assert!(!r.gps.is_empty() && has(&r, "Artist") && has(&r, "dc:creator"), "{:?}", r.fields);
    let clean = clean_bytes(&dirty, Kind::Heif, &CleanOptions::default()).unwrap();
    assert_eq!(clean.len(), dirty.len());
    let after = inspect_bytes(&clean, Kind::Heif);
    assert!(after.fields.is_empty() && after.gps.is_empty(), "{:?}", after.fields);
    assert!(clean.ends_with(b"PIXELS") && !clean.windows(3).any(|w| w == b"Bob"));
}

// ---------- pdf ----------

#[test]
fn pdf_roundtrip() {
    use lopdf::{dictionary, Document, Object, Stream};
    let mut doc = Document::with_version("1.5");
    let pages_id = doc.new_object_id();
    let note = doc.add_object(dictionary! { "Type" => "Annot", "Subtype" => "Text", "T" => Object::string_literal("Carol Reviewer"), "Contents" => Object::string_literal("fix this"), "Rect" => vec![0.into(), 0.into(), 10.into(), 10.into()] });
    let link = doc.add_object(dictionary! { "Type" => "Annot", "Subtype" => "Link", "Rect" => vec![0.into(), 0.into(), 10.into(), 10.into()] });
    let content = doc.add_object(Stream::new(dictionary! {}, b"BT ET".to_vec()));
    let page = doc.add_object(dictionary! { "Type" => "Page", "Parent" => pages_id, "Contents" => content, "Annots" => vec![note.into(), link.into()], "MediaBox" => vec![0.into(), 0.into(), 200.into(), 200.into()] });
    doc.objects.insert(pages_id, Object::Dictionary(dictionary! { "Type" => "Pages", "Kids" => vec![page.into()], "Count" => 1 }));
    let xmp = doc.add_object(Stream::new(dictionary! { "Type" => "Metadata", "Subtype" => "XML" }, XMP.as_bytes().to_vec()));
    let catalog = doc.add_object(dictionary! { "Type" => "Catalog", "Pages" => pages_id, "Metadata" => xmp });
    let info = doc.add_object(dictionary! { "Author" => Object::string_literal("Bob Owner"), "Creator" => Object::string_literal("Microsoft Word"), "Producer" => Object::string_literal("Some PDF Lib 1.2"), "CreationDate" => Object::string_literal("D:20261116104233-08'00'") });
    doc.trailer.set("Root", catalog);
    doc.trailer.set("Info", info);
    let mut dirty = vec![];
    doc.save_to(&mut dirty).unwrap();

    let r = inspect_bytes(&dirty, Kind::Pdf);
    assert!(r.fields.iter().any(|f| f.key == "Author" && f.value == "Bob Owner" && f.risk == Risk::High));
    assert!(r.fields.iter().any(|f| f.key == "Creator" && f.group == Group::Software));
    assert!(has(&r, "Comment authors") && has(&r, "dc:creator") && has(&r, "CreationDate"));

    let clean = clean_bytes(&dirty, Kind::Pdf, &CleanOptions::default()).unwrap();
    let after = inspect_bytes(&clean, Kind::Pdf);
    assert_eq!(after.fields.iter().map(|f| f.key.as_str()).collect::<Vec<_>>(), vec!["PDF version"], "{:?}", after.fields);
    assert!(!clean.windows(3).any(|w| w == b"Bob") && !clean.windows(5).any(|w| w == b"Carol"));
    // still one page, the link survived, the note didn't
    let back = Document::load_mem(&clean).unwrap();
    let pages = back.get_pages();
    assert_eq!(pages.len(), 1);
    assert_eq!(back.get_page_annotations(*pages.values().next().unwrap()).unwrap().len(), 1);
}

// ---------- office ----------

fn e(name: &str, data: &[u8]) -> Entry {
    Entry { name: name.into(), stored: false, data: data.to_vec() }
}

fn part<'a>(entries: &'a [Entry], name: &str) -> Option<&'a str> {
    entries.iter().find(|e| e.name == name).map(|e| std::str::from_utf8(&e.data).unwrap())
}

const W: &str = "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"";
const REL: &str = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
const CORE: &str = "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\"><dc:creator>Bob Owner</dc:creator><cp:lastModifiedBy>Alice Editor</cp:lastModifiedBy><cp:revision>7</cp:revision><dcterms:created>2026-11-16T10:42:33Z</dcterms:created></cp:coreProperties>";
const APP: &str = "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\"><Template>Normal.dotm</Template><TotalTime>42</TotalTime><Application>Microsoft Office Word</Application><Company>ACME Corp</Company><AppVersion>16.0000</AppVersion></Properties>";

#[test]
fn docx_roundtrip() {
    let rels = format!("<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"{REL}/officeDocument\" Target=\"word/document.xml\"/><Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/><Relationship Id=\"rId3\" Type=\"{REL}/extended-properties\" Target=\"docProps/app.xml\"/><Relationship Id=\"rId4\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/thumbnail\" Target=\"docProps/thumbnail.jpeg\"/></Relationships>");
    let doc = format!("<w:document {W}><w:body><w:p w:rsidR=\"00AB12CD\" w:rsidRDefault=\"00AB12CD\"><w:commentRangeStart w:id=\"0\"/><w:r><w:t>Hello </w:t></w:r><w:ins w:id=\"1\" w:author=\"Alice Editor\" w:date=\"2026-11-16T10:00:00Z\"><w:r><w:t>inserted words</w:t></w:r></w:ins><w:del w:id=\"2\" w:author=\"Alice Editor\" w:date=\"2026-11-16T10:00:00Z\"><w:r><w:delText>secret old text</w:delText></w:r></w:del><w:commentRangeEnd w:id=\"0\"/><w:r><w:commentReference w:id=\"0\"/></w:r></w:p><w:p><w:r><w:drawing><pic r:embed=\"rId9\"/></w:drawing></w:r></w:p></w:body></w:document>");
    let comments = format!("<w:comments {W}><w:comment w:id=\"0\" w:author=\"Carol Reviewer\" w:initials=\"CR\"><w:p><w:r><w:t>this number is wrong</w:t></w:r></w:p></w:comment></w:comments>");
    let settings = format!("<w:settings {W}><w:attachedTemplate r:id=\"rId1\"/><w:rsids><w:rsidRoot w:val=\"00AB12CD\"/></w:rsids><w:compat/></w:settings>");
    let settings_rels = format!("<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"{REL}/attachedTemplate\" Target=\"file:///C:\\Users\\bob\\AppData\\Roaming\\Microsoft\\Templates\\Normal.dotm\" TargetMode=\"External\"/></Relationships>");
    let doc_rels = format!("<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"{REL}/comments\" Target=\"comments.xml\"/><Relationship Id=\"rId2\" Type=\"{REL}/settings\" Target=\"settings.xml\"/><Relationship Id=\"rId9\" Type=\"{REL}/image\" Target=\"media/image1.jpeg\"/></Relationships>");
    let types = "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"><Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/><Default Extension=\"xml\" ContentType=\"application/xml\"/><Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/><Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/></Types>";
    let dirty = zip::write(&[
        e("[Content_Types].xml", types.as_bytes()),
        e("_rels/.rels", rels.as_bytes()),
        e("docProps/core.xml", CORE.as_bytes()),
        e("docProps/app.xml", APP.as_bytes()),
        e("docProps/thumbnail.jpeg", &encode(image::ImageFormat::Jpeg)),
        e("word/document.xml", doc.as_bytes()),
        e("word/comments.xml", comments.as_bytes()),
        e("word/settings.xml", settings.as_bytes()),
        e("word/_rels/settings.xml.rels", settings_rels.as_bytes()),
        e("word/_rels/document.xml.rels", doc_rels.as_bytes()),
        e("word/media/image1.jpeg", &leaky_jpeg()),
    ]);
    assert_eq!(Kind::sniff_zip(&dirty), Kind::Docx);

    let r = inspect_bytes(&dirty, Kind::Docx);
    for key in ["creator", "lastModifiedBy", "Company", "Comment authors", "Change authors", "Tracked changes", "Attached template", "Revision ids", "Embedded thumbnail"] {
        assert!(has(&r, key), "missing {key}: {:?}", r.fields);
    }
    assert!(r.findings.iter().any(|f| f.kind == "path leak" && f.detail.contains("bob")), "{:?}", r.findings);
    assert!(!r.gps.is_empty(), "gps from the embedded photo");
    assert_eq!(r.thumbs.len(), 1);

    let clean = clean_bytes(&dirty, Kind::Docx, &CleanOptions::default()).unwrap();
    let after = inspect_bytes(&clean, Kind::Docx);
    assert!(sensitive(&after).is_empty() && after.gps.is_empty(), "{:?}", after.fields);
    let out = zip::read(&clean).unwrap();
    let body = part(&out, "word/document.xml").unwrap();
    assert!(body.contains("Hello ") && body.contains("inserted words") && body.contains("r:embed=\"rId9\""));
    for gone in ["secret old text", "rsid", "commentRange", "commentReference", "w:ins", "w:del", "Alice"] {
        assert!(!body.contains(gone), "{gone} still in the document");
    }
    assert!(!part(&out, "word/comments.xml").unwrap().contains("Carol"));
    assert!(!part(&out, "docProps/core.xml").unwrap().contains("Bob") && !part(&out, "docProps/app.xml").unwrap().contains("ACME"));
    assert!(part(&out, "docProps/thumbnail.jpeg").is_none() && !part(&out, "_rels/.rels").unwrap().contains("thumbnail"));
    assert!(!part(&out, "word/_rels/settings.xml.rels").unwrap().contains("bob"));
    let s = part(&out, "word/settings.xml").unwrap();
    assert!(!s.contains("attachedTemplate") && !s.contains("rsid") && s.contains("w:compat"));
    for x in out.iter().filter(|e| e.name.ends_with(".xml") || e.name.ends_with(".rels")) {
        assert_balanced(std::str::from_utf8(&x.data).unwrap(), &x.name);
    }
}

// every open tag has its close, in order. catches a rewrite that tore an element in half
fn assert_balanced(x: &str, name: &str) {
    use odin_core::xml::{tokens, Tok};
    let mut stack = vec![];
    for t in tokens(x) {
        match t {
            Tok::Open { name: n, empty: false, .. } => stack.push(n),
            Tok::Close { name: n, .. } => assert_eq!(stack.pop(), Some(n), "unbalanced xml in {name}"),
            _ => {}
        }
    }
    assert!(stack.is_empty(), "unclosed tags in {name}");
}

#[test]
fn xlsx_comments_and_paths() {
    let x = "xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"";
    let workbook = format!("<workbook {x} xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\"><fileVersion appName=\"xl\" lastEdited=\"7\" rupBuild=\"24729\"/><mc:AlternateContent><mc:Choice Requires=\"x15\"><x15ac:absPath url=\"C:\\Users\\bob\\Documents\\budget\\\" xmlns:x15ac=\"http://schemas.microsoft.com/office/spreadsheetml/2010/11/ac\"/></mc:Choice></mc:AlternateContent><sheets><sheet name=\"Sheet1\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>");
    let sheet = format!("<worksheet {x}><sheetData><row r=\"1\"><c r=\"A1\"><v>42</v></c></row></sheetData><legacyDrawing r:id=\"rId2\"/></worksheet>");
    let sheet_rels = format!("<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"{REL}/comments\" Target=\"../comments1.xml\"/><Relationship Id=\"rId2\" Type=\"{REL}/vmlDrawing\" Target=\"../drawings/vmlDrawing1.vml\"/></Relationships>");
    let comments = format!("<comments {x}><authors><author>Carol Reviewer</author></authors><commentList><comment ref=\"A1\" authorId=\"0\"><text><t>where did this come from</t></text></comment></commentList></comments>");
    let vml = "<xml xmlns:v=\"urn:schemas-microsoft-com:vml\" xmlns:x=\"urn:schemas-microsoft-com:office:excel\"><v:shape id=\"_x0000_s1025\" type=\"#_x0000_t202\"><x:ClientData ObjectType=\"Note\"><x:Row>0</x:Row></x:ClientData></v:shape></xml>";
    let types = "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"><Default Extension=\"xml\" ContentType=\"application/xml\"/><Override PartName=\"/xl/workbook.xml\" ContentType=\"a\"/><Override PartName=\"/xl/comments1.xml\" ContentType=\"b\"/></Types>";
    let dirty = zip::write(&[
        e("[Content_Types].xml", types.as_bytes()),
        e("docProps/core.xml", CORE.as_bytes()),
        e("docProps/app.xml", APP.as_bytes()),
        e("xl/workbook.xml", workbook.as_bytes()),
        e("xl/worksheets/sheet1.xml", sheet.as_bytes()),
        e("xl/worksheets/_rels/sheet1.xml.rels", sheet_rels.as_bytes()),
        e("xl/comments1.xml", comments.as_bytes()),
        e("xl/drawings/vmlDrawing1.vml", vml.as_bytes()),
    ]);
    assert_eq!(Kind::sniff_zip(&dirty), Kind::Xlsx);
    let r = inspect_bytes(&dirty, Kind::Xlsx);
    assert!(has(&r, "Comment authors") && has(&r, "Saved from path") && has(&r, "creator"), "{:?}", r.fields);
    assert!(r.findings.iter().any(|f| f.kind == "path leak"));

    let clean = clean_bytes(&dirty, Kind::Xlsx, &CleanOptions::default()).unwrap();
    assert!(sensitive(&inspect_bytes(&clean, Kind::Xlsx)).is_empty());
    let out = zip::read(&clean).unwrap();
    assert!(part(&out, "xl/comments1.xml").is_none() && part(&out, "xl/drawings/vmlDrawing1.vml").is_none());
    let sheet = part(&out, "xl/worksheets/sheet1.xml").unwrap();
    assert!(!sheet.contains("legacyDrawing") && sheet.contains("<v>42</v>"));
    assert!(!part(&out, "xl/worksheets/_rels/sheet1.xml.rels").unwrap().contains("Relationship "));
    assert!(!part(&out, "[Content_Types].xml").unwrap().contains("comments1"));
    let wb = part(&out, "xl/workbook.xml").unwrap();
    assert!(!wb.contains("bob") && !wb.contains("fileVersion") && wb.contains("Sheet1"));
    for x in out.iter().filter(|e| e.name.ends_with("ml") || e.name.ends_with(".rels")) {
        assert_balanced(std::str::from_utf8(&x.data).unwrap(), &x.name);
    }

    // comments can stay if you ask
    let kept = clean_bytes(&dirty, Kind::Xlsx, &CleanOptions { remove_comments: false, ..Default::default() }).unwrap();
    assert!(part(&zip::read(&kept).unwrap(), "xl/comments1.xml").is_some());
}

#[test]
fn pptx_comments() {
    let pres_rels = format!("<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"{REL}/slide\" Target=\"slides/slide1.xml\"/><Relationship Id=\"rId2\" Type=\"{REL}/commentAuthors\" Target=\"commentAuthors.xml\"/></Relationships>");
    let slide_rels = format!("<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"{REL}/comments\" Target=\"../comments/comment1.xml\"/></Relationships>");
    let dirty = zip::write(&[
        e("[Content_Types].xml", b"<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"><Override PartName=\"/ppt/commentAuthors.xml\" ContentType=\"a\"/><Override PartName=\"/ppt/presentation.xml\" ContentType=\"b\"/></Types>"),
        e("docProps/core.xml", CORE.as_bytes()),
        e("ppt/presentation.xml", b"<p:presentation xmlns:p=\"p\"/>"),
        e("ppt/_rels/presentation.xml.rels", pres_rels.as_bytes()),
        e("ppt/slides/slide1.xml", b"<p:sld xmlns:p=\"p\"><p:cSld/></p:sld>"),
        e("ppt/slides/_rels/slide1.xml.rels", slide_rels.as_bytes()),
        e("ppt/commentAuthors.xml", b"<p:cmAuthorLst xmlns:p=\"p\"><p:cmAuthor id=\"0\" name=\"Carol Reviewer\" initials=\"CR\"/></p:cmAuthorLst>"),
        e("ppt/comments/comment1.xml", b"<p:cmLst xmlns:p=\"p\"><p:cm authorId=\"0\"><p:text>cut this slide</p:text></p:cm></p:cmLst>"),
    ]);
    assert_eq!(Kind::sniff_zip(&dirty), Kind::Pptx);
    let r = inspect_bytes(&dirty, Kind::Pptx);
    assert!(r.fields.iter().any(|f| f.key == "Comment authors" && f.value.contains("Carol")), "{:?}", r.fields);
    let clean = clean_bytes(&dirty, Kind::Pptx, &CleanOptions::default()).unwrap();
    let out = zip::read(&clean).unwrap();
    assert!(part(&out, "ppt/commentAuthors.xml").is_none() && part(&out, "ppt/comments/comment1.xml").is_none());
    assert!(!part(&out, "ppt/_rels/presentation.xml.rels").unwrap().contains("commentAuthors"));
    assert!(part(&out, "ppt/_rels/presentation.xml.rels").unwrap().contains("slides/slide1.xml"));
    assert!(!part(&out, "[Content_Types].xml").unwrap().contains("commentAuthors"));
    assert!(sensitive(&inspect_bytes(&clean, Kind::Pptx)).is_empty());
}

// ---------- garbage in ----------

#[test]
fn junk_doesnt_crash() {
    let samples: Vec<Vec<u8>> = vec![leaky_jpeg(), encode(image::ImageFormat::Png), encode(image::ImageFormat::Gif), encode(image::ImageFormat::WebP)];
    for (i, s) in samples.iter().enumerate() {
        let kind = Kind::sniff(s);
        // chop it at every length and flip bytes around, nothing should panic
        for cut in (0..s.len()).step_by(7) {
            let _ = inspect_bytes(&s[..cut], kind);
            let _ = clean_bytes(&s[..cut], kind, &CleanOptions::default());
        }
        for at in (0..s.len()).step_by(11) {
            let mut m = s.clone();
            m[at] ^= 0xFF;
            let _ = inspect_bytes(&m, kind);
            let _ = clean_bytes(&m, kind, &CleanOptions::default());
        }
        assert!(i < 4);
    }
    for kind in [Kind::Pdf, Kind::Docx, Kind::Heif, Kind::Tiff, Kind::Unknown] {
        let _ = inspect_bytes(b"PK\x03\x04 definitely not a real file \xFF\xD8\xFF", kind);
        let _ = clean_bytes(b"%PDF-1.4 nope", kind, &CleanOptions::default());
    }
}

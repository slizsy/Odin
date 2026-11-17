// figures out what a file really is from its first bytes, extensions lie

use crate::zip;

#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash)]
pub enum Kind {
    Jpeg,
    Png,
    Webp,
    Gif,
    Tiff,
    Heif,
    Mp4,
    Mkv,
    Avi,
    Mp3,
    Flac,
    Wav,
    Ogg,
    Pdf,
    Docx,
    Xlsx,
    Pptx,
    Odf,
    Unknown,
}

impl Kind {
    pub fn label(self) -> &'static str {
        match self {
            Kind::Jpeg => "jpeg",
            Kind::Png => "png",
            Kind::Webp => "webp",
            Kind::Gif => "gif",
            Kind::Tiff => "tiff / raw",
            Kind::Heif => "heic / avif",
            Kind::Mp4 => "mp4 / mov",
            Kind::Mkv => "mkv / webm",
            Kind::Avi => "avi",
            Kind::Mp3 => "mp3",
            Kind::Flac => "flac",
            Kind::Wav => "wav",
            Kind::Ogg => "ogg",
            Kind::Pdf => "pdf",
            Kind::Docx => "word doc",
            Kind::Xlsx => "excel sheet",
            Kind::Pptx => "powerpoint",
            Kind::Odf => "opendocument",
            Kind::Unknown => "unknown",
        }
    }

    // used by the generic filename scrubber
    pub fn noun(self) -> &'static str {
        match self {
            Kind::Jpeg | Kind::Png | Kind::Webp | Kind::Gif | Kind::Tiff | Kind::Heif => "image",
            Kind::Mp4 | Kind::Mkv | Kind::Avi => "video",
            Kind::Mp3 | Kind::Flac | Kind::Wav | Kind::Ogg => "audio",
            Kind::Pdf | Kind::Docx | Kind::Xlsx | Kind::Pptx | Kind::Odf => "document",
            Kind::Unknown => "file",
        }
    }

    // video + audio go through ffmpeg
    pub fn is_media(self) -> bool {
        matches!(self, Kind::Mp4 | Kind::Mkv | Kind::Avi | Kind::Mp3 | Kind::Flac | Kind::Wav | Kind::Ogg)
    }

    pub fn can_clean(self) -> bool {
        !matches!(self, Kind::Tiff | Kind::Unknown)
    }

    // head = the first chunk of the file (64 bytes is enough, more is fine)
    pub fn sniff(head: &[u8]) -> Kind {
        let h = head;
        if h.starts_with(&[0xFF, 0xD8, 0xFF]) {
            return Kind::Jpeg;
        }
        if h.starts_with(b"\x89PNG\r\n\x1a\n") {
            return Kind::Png;
        }
        if h.len() >= 12 && &h[..4] == b"RIFF" {
            return match &h[8..12] {
                b"WEBP" => Kind::Webp,
                b"AVI " => Kind::Avi,
                b"WAVE" => Kind::Wav,
                _ => Kind::Unknown,
            };
        }
        if h.starts_with(b"GIF87a") || h.starts_with(b"GIF89a") {
            return Kind::Gif;
        }
        if h.starts_with(b"II*\0") || h.starts_with(b"MM\0*") {
            return Kind::Tiff;
        }
        if h.len() >= 12 && &h[4..8] == b"ftyp" {
            let still = [&b"heic"[..], b"heix", b"hevc", b"hevx", b"heim", b"heis", b"mif1", b"msf1", b"avif", b"avis"];
            return if still.contains(&&h[8..12]) { Kind::Heif } else { Kind::Mp4 };
        }
        // old quicktime files skip the ftyp box
        if h.len() >= 8 && [&b"moov"[..], b"mdat", b"wide", b"free", b"skip"].contains(&&h[4..8]) {
            return Kind::Mp4;
        }
        if h.starts_with(&[0x1A, 0x45, 0xDF, 0xA3]) {
            return Kind::Mkv;
        }
        if h.starts_with(b"fLaC") {
            return Kind::Flac;
        }
        if h.starts_with(b"OggS") {
            return Kind::Ogg;
        }
        if h.starts_with(b"ID3") || (h.len() >= 2 && h[0] == 0xFF && matches!(h[1], 0xFB | 0xF3 | 0xF2 | 0xFA)) {
            return Kind::Mp3;
        }
        if h.starts_with(b"PK\x03\x04") {
            return Kind::Odf; // placeholder, sniff_zip sorts out which kind of zip
        }
        let n = h.len().min(1024);
        if h[..n].windows(5).any(|w| w == b"%PDF-") {
            return Kind::Pdf;
        }
        Kind::Unknown
    }

    // docx / xlsx / pptx / odf are all zips, the folder names inside tell them apart
    pub fn sniff_zip(data: &[u8]) -> Kind {
        let Ok(names) = zip::names(data) else { return Kind::Unknown };
        let has = |p: &str| names.iter().any(|n| n.starts_with(p));
        if has("word/") {
            Kind::Docx
        } else if has("xl/") {
            Kind::Xlsx
        } else if has("ppt/") {
            Kind::Pptx
        } else if names.iter().any(|n| n == "meta.xml") && names.iter().any(|n| n == "mimetype") {
            Kind::Odf
        } else {
            Kind::Unknown
        }
    }
}

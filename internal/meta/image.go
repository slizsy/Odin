package meta

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"fmt"
	"io"
)

var (
	pngSig  = []byte("\x89PNG\r\n\x1a\n")
	xmpNS   = []byte("http://ns.adobe.com/xap/1.0/\x00")
	exifHdr = []byte("Exif\x00\x00")
)

func parseJPEG(r *Result, b []byte, pfx string) {
	i := 2
	for i+4 <= len(b) {
		if b[i] != 0xFF {
			r.errf("jpeg: lost sync at byte %d", i)
			return
		}
		m := b[i+1]
		switch {
		case m == 0xFF: // fill byte
			i++
			continue
		case m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7):
			i += 2
			continue
		case m == 0xDA || m == 0xD9:
			return // start of scan, all the metadata is before this
		}
		l := int(binary.BigEndian.Uint16(b[i+2:]))
		if l < 2 || i+2+l > len(b) {
			r.errf("jpeg: truncated segment at byte %d", i)
			return
		}
		seg := b[i+4 : i+2+l]
		switch m {
		case 0xE1:
			if bytes.HasPrefix(seg, exifHdr) {
				parseEXIF(r, seg[len(exifHdr):], pfx+"EXIF")
			} else if bytes.HasPrefix(seg, xmpNS) {
				parseXMP(r, seg[len(xmpNS):], pfx+"XMP")
			}
		case 0xED:
			if bytes.HasPrefix(seg, []byte("Photoshop 3.0\x00")) {
				parse8BIM(r, seg[14:], pfx+"IPTC")
			}
		case 0xFE:
			r.add(pfx+"JPEG", "Comment", string(seg))
		}
		i += 2 + l
	}
}

func parsePNG(r *Result, b []byte, pfx string) {
	i := len(pngSig)
	g := pfx + "PNG"
	for i+12 <= len(b) {
		n := int(binary.BigEndian.Uint32(b[i:]))
		typ := string(b[i+4 : i+8])
		if n < 0 || i+12+n > len(b) {
			r.errf("png: truncated %q chunk", typ)
			return
		}
		d := b[i+8 : i+8+n]
		switch typ {
		case "tEXt":
			if k, v, ok := bytes.Cut(d, []byte{0}); ok {
				pngText(r, string(k), latin1(v), pfx)
			}
		case "zTXt":
			if k, rest, ok := bytes.Cut(d, []byte{0}); ok && len(rest) > 1 {
				if v, err := inflate(rest[1:]); err == nil {
					pngText(r, string(k), latin1(v), pfx)
				}
			}
		case "iTXt":
			k, rest, ok := bytes.Cut(d, []byte{0})
			if !ok || len(rest) < 2 {
				break
			}
			compressed := rest[0] == 1
			rest = rest[2:]
			_, rest, _ = bytes.Cut(rest, []byte{0}) // language tag
			_, rest, _ = bytes.Cut(rest, []byte{0}) // translated keyword
			if compressed {
				v, err := inflate(rest)
				if err != nil {
					break
				}
				rest = v
			}
			pngText(r, string(k), string(rest), pfx)
		case "eXIf":
			parseEXIF(r, bytes.TrimPrefix(d, exifHdr), pfx+"EXIF")
		case "tIME":
			if n == 7 {
				r.add(g, "ModifyDate", fmt.Sprintf("%04d-%02d-%02d %02d:%02d:%02d UTC",
					binary.BigEndian.Uint16(d), d[2], d[3], d[4], d[5], d[6]))
			}
		case "IEND":
			return
		}
		i += 12 + n
	}
}

func pngText(r *Result, k, v, pfx string) {
	if k == "XML:com.adobe.xmp" {
		parseXMP(r, []byte(v), pfx+"XMP")
		return
	}
	r.add(pfx+"PNG", k, v)
}

func parseWebP(r *Result, b []byte, pfx string) {
	i := 12
	for i+8 <= len(b) {
		id := string(b[i : i+4])
		n := int(binary.LittleEndian.Uint32(b[i+4:]))
		if n < 0 || i+8+n > len(b) {
			r.errf("webp: truncated %q chunk", id)
			return
		}
		d := b[i+8 : i+8+n]
		switch id {
		case "EXIF":
			parseEXIF(r, bytes.TrimPrefix(d, exifHdr), pfx+"EXIF")
		case "XMP ":
			parseXMP(r, d, pfx+"XMP")
		}
		i += 8 + n + n%2 // chunks are padded to even
	}
}

// IPTC lives inside photoshop's 8BIM resource blocks
var iptcTags = map[byte]string{
	5:   "ObjectName",
	25:  "Keywords",
	55:  "DateCreated",
	60:  "TimeCreated",
	65:  "OriginatingProgram",
	80:  "By-line",
	85:  "By-lineTitle",
	90:  "City",
	92:  "Sub-location",
	95:  "Province-State",
	101: "Country",
	110: "Credit",
	115: "Source",
	116: "CopyrightNotice",
	118: "Contact",
	120: "Caption-Abstract",
	122: "Writer-Editor",
}

func parse8BIM(r *Result, b []byte, group string) {
	i := 0
	for i+12 <= len(b) && string(b[i:i+4]) == "8BIM" {
		id := binary.BigEndian.Uint16(b[i+4:])
		nameLen := int(b[i+6])
		p := i + 7 + nameLen
		if (nameLen+1)%2 == 1 {
			p++ // pascal string padded to even
		}
		if p+4 > len(b) {
			return
		}
		size := int(binary.BigEndian.Uint32(b[p:]))
		p += 4
		if size < 0 || p+size > len(b) {
			return
		}
		if id == 0x0404 {
			parseIPTC(r, b[p:p+size], group)
		}
		i = p + size + size%2
	}
}

func parseIPTC(r *Result, b []byte, group string) {
	i := 0
	for i+5 <= len(b) && b[i] == 0x1C {
		rec, ds := b[i+1], b[i+2]
		n := int(binary.BigEndian.Uint16(b[i+3:]))
		if n&0x8000 != 0 || i+5+n > len(b) {
			return // extended length, basically never used for text so bail
		}
		if rec == 2 {
			if name, ok := iptcTags[ds]; ok {
				r.add(group, name, string(b[i+5:i+5+n]))
			}
		}
		i += 5 + n
	}
}

func inflate(b []byte) ([]byte, error) {
	zr, err := zlib.NewReader(bytes.NewReader(b))
	if err != nil {
		return nil, err
	}
	defer zr.Close()
	return io.ReadAll(io.LimitReader(zr, 16<<20))
}

func latin1(b []byte) string {
	rs := make([]rune, len(b))
	for i, c := range b {
		rs[i] = rune(c)
	}
	return string(rs)
}

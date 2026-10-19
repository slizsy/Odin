package meta

import (
	"encoding/binary"
	"fmt"
	"strconv"
	"strings"
	"unicode/utf16"
	"unicode/utf8"
)

// only the tags worth showing, exiftool mode gets you everything else
var ifd0Tags = map[uint16]string{
	0x010E: "ImageDescription",
	0x010F: "Make",
	0x0110: "Model",
	0x0112: "Orientation",
	0x0131: "Software",
	0x0132: "ModifyDate",
	0x013B: "Artist",
	0x013C: "HostComputer",
	0x8298: "Copyright",
	0x9C9B: "XPTitle",
	0x9C9C: "XPComment",
	0x9C9D: "XPAuthor",
	0x9C9E: "XPKeywords",
	0x9C9F: "XPSubject",
}

var exifTags = map[uint16]string{
	0x829A: "ExposureTime",
	0x829D: "FNumber",
	0x8827: "ISO",
	0x9003: "DateTimeOriginal",
	0x9004: "CreateDate",
	0x9010: "OffsetTime",
	0x9011: "OffsetTimeOriginal",
	0x920A: "FocalLength",
	0x9286: "UserComment",
	0xA002: "ImageWidth",
	0xA003: "ImageHeight",
	0xA420: "ImageUniqueID",
	0xA430: "CameraOwnerName",
	0xA431: "BodySerialNumber",
	0xA432: "LensSpecification",
	0xA433: "LensMake",
	0xA434: "LensModel",
	0xA435: "LensSerialNumber",
}

const (
	tagExifIFD = 0x8769
	tagGPSIFD  = 0x8825
)

// bytes per tiff type
var typeSize = map[uint16]int{1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 6: 1, 7: 1, 8: 2, 9: 4, 10: 8, 11: 4, 12: 8, 13: 4}

type ifdEntry struct {
	tag   uint16
	typ   uint16
	count uint32
	data  []byte
}

type tiff struct {
	b  []byte
	bo binary.ByteOrder
}

func parseEXIF(r *Result, b []byte, group string) {
	if len(b) < 8 {
		r.errf("%s: too short", group)
		return
	}
	t := &tiff{b: b}
	switch string(b[:2]) {
	case "II":
		t.bo = binary.LittleEndian
	case "MM":
		t.bo = binary.BigEndian
	default:
		r.errf("%s: bad tiff header", group)
		return
	}
	if t.bo.Uint16(b[2:]) != 42 {
		r.errf("%s: bad tiff magic", group)
		return
	}

	ifd0, err := t.readIFD(t.bo.Uint32(b[4:]))
	if err != nil {
		r.errf("%s: %v", group, err)
		return
	}
	var exifOff, gpsOff uint32
	for _, e := range ifd0 {
		switch e.tag {
		case tagExifIFD:
			exifOff = t.u32(e)
		case tagGPSIFD:
			gpsOff = t.u32(e)
		default:
			if name, ok := ifd0Tags[e.tag]; ok {
				r.add(group, name, t.format(e))
			}
		}
	}
	if exifOff != 0 {
		if ents, err := t.readIFD(exifOff); err == nil {
			for _, e := range ents {
				if name, ok := exifTags[e.tag]; ok {
					r.add(group, name, t.format(e))
				}
			}
		}
	}
	if gpsOff != 0 {
		if ents, err := t.readIFD(gpsOff); err == nil {
			t.gps(r, ents, group)
		}
	}
}

func (t *tiff) readIFD(off uint32) ([]ifdEntry, error) {
	b := t.b
	if int(off)+2 > len(b) || off < 8 {
		return nil, fmt.Errorf("ifd offset %d out of range", off)
	}
	n := int(t.bo.Uint16(b[off:]))
	start := int(off) + 2
	if start+n*12 > len(b) {
		return nil, fmt.Errorf("ifd at %d is truncated", off)
	}
	out := make([]ifdEntry, 0, n)
	for i := 0; i < n; i++ {
		e := b[start+i*12 : start+i*12+12]
		tag, typ, count := t.bo.Uint16(e), t.bo.Uint16(e[2:]), t.bo.Uint32(e[4:])
		sz, ok := typeSize[typ]
		if !ok {
			continue
		}
		total := uint64(sz) * uint64(count)
		if total > uint64(len(b)) {
			continue // garbage count, skip it
		}
		var data []byte
		if total <= 4 {
			data = e[8 : 8+total]
		} else {
			o := uint64(t.bo.Uint32(e[8:]))
			if o+total > uint64(len(b)) {
				continue
			}
			data = b[o : o+total]
		}
		out = append(out, ifdEntry{tag, typ, count, data})
	}
	return out, nil
}

func (t *tiff) u32(e ifdEntry) uint32 {
	switch {
	case len(e.data) >= 4:
		return t.bo.Uint32(e.data)
	case len(e.data) >= 2:
		return uint32(t.bo.Uint16(e.data))
	}
	return 0
}

func (t *tiff) rats(e ifdEntry) []float64 {
	var out []float64
	for i := 0; i+8 <= len(e.data); i += 8 {
		num, den := t.bo.Uint32(e.data[i:]), t.bo.Uint32(e.data[i+4:])
		if e.typ == 10 {
			n, d := int32(num), int32(den)
			if d == 0 {
				out = append(out, 0)
			} else {
				out = append(out, float64(n)/float64(d))
			}
			continue
		}
		if den == 0 {
			out = append(out, 0)
		} else {
			out = append(out, float64(num)/float64(den))
		}
	}
	return out
}

func (t *tiff) format(e ifdEntry) string {
	d := e.data
	switch e.typ {
	case 2:
		return cleanASCII(d)
	case 1, 7:
		if e.tag >= 0x9C9B && e.tag <= 0x9C9F {
			return utf16le(d) // windows XP* tags
		}
		if e.tag == 0x9286 {
			return userComment(d, t.bo)
		}
		if s := cleanASCII(d); s != "" && utf8.ValidString(s) && isPrintable(s) {
			return s
		}
		return fmt.Sprintf("% x", head(d, 16))
	case 3, 8:
		var parts []string
		for i := 0; i+2 <= len(d); i += 2 {
			v := t.bo.Uint16(d[i:])
			if e.typ == 8 {
				parts = append(parts, strconv.Itoa(int(int16(v))))
			} else {
				parts = append(parts, strconv.Itoa(int(v)))
			}
		}
		return strings.Join(parts, " ")
	case 4, 9, 13:
		var parts []string
		for i := 0; i+4 <= len(d); i += 4 {
			v := t.bo.Uint32(d[i:])
			if e.typ == 9 {
				parts = append(parts, strconv.Itoa(int(int32(v))))
			} else {
				parts = append(parts, strconv.FormatUint(uint64(v), 10))
			}
		}
		return strings.Join(parts, " ")
	case 5, 10:
		var parts []string
		for _, f := range t.rats(e) {
			parts = append(parts, strconv.FormatFloat(f, 'f', -1, 64))
		}
		return strings.Join(parts, " ")
	}
	return ""
}

func (t *tiff) gps(r *Result, ents []ifdEntry, group string) {
	var latRef, lonRef, date string
	var lat, lon, alt, ts []float64
	var altBelow bool
	for _, e := range ents {
		switch e.tag {
		case 1:
			latRef = cleanASCII(e.data)
		case 2:
			lat = t.rats(e)
		case 3:
			lonRef = cleanASCII(e.data)
		case 4:
			lon = t.rats(e)
		case 5:
			altBelow = len(e.data) > 0 && e.data[0] == 1
		case 6:
			alt = t.rats(e)
		case 7:
			ts = t.rats(e)
		case 29:
			date = cleanASCII(e.data)
		}
	}
	if len(lat) == 3 && len(lon) == 3 {
		la := lat[0] + lat[1]/60 + lat[2]/3600
		lo := lon[0] + lon[1]/60 + lon[2]/3600
		if strings.EqualFold(latRef, "S") {
			la = -la
		}
		if strings.EqualFold(lonRef, "W") {
			lo = -lo
		}
		r.add(group, "GPSLatitude", strconv.FormatFloat(la, 'f', 6, 64))
		r.add(group, "GPSLongitude", strconv.FormatFloat(lo, 'f', 6, 64))
		r.addGPS(la, lo, group)
	}
	if len(alt) == 1 {
		a := alt[0]
		if altBelow {
			a = -a
		}
		r.add(group, "GPSAltitude", fmt.Sprintf("%.1f m", a))
	}
	if date != "" {
		if len(ts) == 3 {
			date += fmt.Sprintf(" %02d:%02d:%02d", int(ts[0]), int(ts[1]), int(ts[2]))
		}
		r.add(group, "GPSDateTime", date+" UTC")
	}
}

func cleanASCII(b []byte) string {
	return strings.TrimSpace(strings.TrimRight(string(b), "\x00"))
}

func utf16le(b []byte) string {
	u := make([]uint16, 0, len(b)/2)
	for i := 0; i+1 < len(b); i += 2 {
		u = append(u, uint16(b[i])|uint16(b[i+1])<<8)
	}
	return strings.TrimRight(string(utf16.Decode(u)), "\x00")
}

func utf16be(b []byte) string {
	u := make([]uint16, 0, len(b)/2)
	for i := 0; i+1 < len(b); i += 2 {
		u = append(u, uint16(b[i])<<8|uint16(b[i+1]))
	}
	return strings.TrimRight(string(utf16.Decode(u)), "\x00")
}

// first 8 bytes say what charset the rest is in
func userComment(b []byte, bo binary.ByteOrder) string {
	if len(b) < 8 {
		return ""
	}
	body := b[8:]
	switch strings.TrimRight(string(b[:8]), "\x00 ") {
	case "UNICODE":
		if bo == binary.BigEndian {
			return utf16be(body)
		}
		return utf16le(body)
	default:
		return cleanASCII(body)
	}
}

func isPrintable(s string) bool {
	for _, c := range s {
		if c < 0x20 && c != '\n' && c != '\r' && c != '\t' {
			return false
		}
	}
	return true
}

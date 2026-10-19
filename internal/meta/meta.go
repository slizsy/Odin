// Package meta pulls metadata out of files without needing any outside tools.
// exiftool is optional, if it's installed we can use it for a deeper dump.
package meta

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"math"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"
	"unicode"
)

type Field struct {
	Group string `json:"group"`
	Key   string `json:"key"`
	Value string `json:"value"`
}

type GeoPoint struct {
	Lat    float64 `json:"lat"`
	Lon    float64 `json:"lon"`
	Source string  `json:"source"`
}

func (g GeoPoint) MapsURL() string {
	return fmt.Sprintf("https://www.google.com/maps?q=%.6f,%.6f", g.Lat, g.Lon)
}

const (
	High   = "high"
	Medium = "medium"
	Info   = "info"
)

type Finding struct {
	Level  string `json:"level"`
	Kind   string `json:"kind"`
	Detail string `json:"detail"`
	Source string `json:"source,omitempty"`
}

type Result struct {
	Path     string     `json:"path"`
	Name     string     `json:"name"`
	Type     string     `json:"type"`
	MIME     string     `json:"mime"`
	Size     int64      `json:"size"`
	SHA256   string     `json:"sha256,omitempty"`
	ModTime  time.Time  `json:"modified"`
	Engine   string     `json:"engine"`
	GPS      []GeoPoint `json:"gps,omitempty"`
	Findings []Finding  `json:"findings"`
	Fields   []Field    `json:"fields"`
	Errors   []string   `json:"errors,omitempty"`

	seen map[string]bool
}

type Options struct {
	ExifTool bool // use exiftool for the field dump if it's on PATH
}

// anything bigger than this we don't slurp into memory
const maxRead = 512 << 20

func Extract(path string, opt Options) *Result {
	r := &Result{Path: path, Name: filepath.Base(path), Engine: "native", Type: "unknown", seen: map[string]bool{}}
	st, err := os.Stat(path)
	if err != nil {
		r.errf("%v", err)
		return r
	}
	r.Size = st.Size()
	r.ModTime = st.ModTime()

	if st.Size() > maxRead {
		r.errf("file is %s, too big for the native parser", HumanSize(st.Size()))
	} else {
		b, err := os.ReadFile(path)
		if err != nil {
			r.errf("%v", err)
			return r
		}
		sum := sha256.Sum256(b)
		r.SHA256 = hex.EncodeToString(sum[:])
		r.MIME = http.DetectContentType(b)
		parseBytes(r, b, "")
	}

	if opt.ExifTool {
		if err := runExifTool(r); err != nil {
			r.errf("exiftool: %v", err)
		} else {
			r.Engine = "exiftool"
		}
	}

	r.Findings = analyze(r)
	if r.Fields == nil {
		r.Fields = []Field{}
	}
	if r.Findings == nil {
		r.Findings = []Finding{}
	}
	return r
}

// pfx is non-empty when we're parsing something embedded inside another file
// (like a photo inside a docx)
func parseBytes(r *Result, b []byte, pfx string) {
	setType := func(t string) {
		if pfx == "" {
			r.Type = t
		}
	}
	switch {
	case bytes.HasPrefix(b, []byte{0xFF, 0xD8, 0xFF}):
		setType("JPEG image")
		parseJPEG(r, b, pfx)
	case bytes.HasPrefix(b, pngSig):
		setType("PNG image")
		parsePNG(r, b, pfx)
	case bytes.HasPrefix(b, []byte("II*\x00")), bytes.HasPrefix(b, []byte("MM\x00*")):
		// plain tiff and most camera raws (cr2, nef, dng, arw) are tiff under the hood
		setType("TIFF / camera raw")
		parseEXIF(r, b, pfx+"EXIF")
	case len(b) >= 12 && string(b[:4]) == "RIFF" && string(b[8:12]) == "WEBP":
		setType("WebP image")
		parseWebP(r, b, pfx)
	case bytes.Contains(head(b, 1024), []byte("%PDF-")):
		setType("PDF document")
		parsePDF(r, b, pfx)
	case bytes.HasPrefix(b, []byte("PK\x03\x04")):
		setType("ZIP archive")
		parseZip(r, b, pfx, setType)
	case len(b) >= 12 && string(b[4:8]) == "ftyp":
		setType("ISO media (" + strings.TrimSpace(string(b[8:12])) + ")")
		scanXMP(r, b, pfx+"XMP")
	default:
		// last resort, lots of formats (psd, ai, mp4, svg...) stash an xmp packet somewhere
		scanXMP(r, b, pfx+"XMP")
	}
}

func head(b []byte, n int) []byte {
	if len(b) < n {
		return b
	}
	return b[:n]
}

func (r *Result) add(group, key, val string) {
	val = clean(val)
	if val == "" || key == "" {
		return
	}
	id := group + "\x00" + key + "\x00" + val
	if r.seen[id] {
		return
	}
	r.seen[id] = true
	r.Fields = append(r.Fields, Field{group, key, val})
}

func (r *Result) addGPS(lat, lon float64, src string) {
	if math.IsNaN(lat) || math.IsNaN(lon) || math.Abs(lat) > 90 || math.Abs(lon) > 180 {
		return
	}
	// 0,0 is almost always a camera writing junk, not a photo off the coast of africa
	if lat == 0 && lon == 0 {
		return
	}
	for _, g := range r.GPS {
		if math.Abs(g.Lat-lat) < 1e-6 && math.Abs(g.Lon-lon) < 1e-6 {
			return
		}
	}
	r.GPS = append(r.GPS, GeoPoint{lat, lon, src})
}

func (r *Result) errf(format string, a ...any) {
	r.Errors = append(r.Errors, fmt.Sprintf(format, a...))
}

// strip control chars + nulls, squash whitespace, cap the length
func clean(s string) string {
	s = strings.Map(func(c rune) rune {
		if c == '\n' || c == '\r' || c == '\t' {
			return ' '
		}
		if unicode.IsControl(c) || c == unicode.ReplacementChar {
			return -1
		}
		return c
	}, s)
	s = strings.Join(strings.Fields(s), " ")
	if len([]rune(s)) > 500 {
		s = string([]rune(s)[:500]) + "…"
	}
	return s
}

func HumanSize(n int64) string {
	const unit = 1024
	if n < unit {
		return fmt.Sprintf("%d B", n)
	}
	div, exp := int64(unit), 0
	for m := n / unit; m >= unit; m /= unit {
		div *= unit
		exp++
	}
	return fmt.Sprintf("%.1f %cB", float64(n)/float64(div), "KMGTPE"[exp])
}

package meta

import (
	"bytes"
	"fmt"
	"regexp"
	"strconv"
	"strings"
)

var (
	pdfInfoKey = regexp.MustCompile(`/(Title|Author|Subject|Keywords|Creator|Producer|CreationDate|ModDate|Company|SourceModified|Trapped)\s*[(<]`)
	pdfVersion = regexp.MustCompile(`%PDF-(\d\.\d)`)
)

func parsePDF(r *Result, b []byte, pfx string) {
	g := pfx + "PDF"
	if m := pdfVersion.FindSubmatch(head(b, 1024)); m != nil {
		r.add(g, "PDFVersion", string(m[1]))
	}
	encrypted := bytes.Contains(b, []byte("/Encrypt"))
	if encrypted {
		r.add(g, "Encrypted", "yes (info strings may come out scrambled)")
	}

	// every incremental save appends a new info dict, so older values can still
	// be sitting in the file. we grab all of them, findings flags when they differ
	for _, loc := range pdfInfoKey.FindAllSubmatchIndex(b, -1) {
		key := string(b[loc[2]:loc[3]])
		raw, ok := pdfString(b, loc[1]-1)
		if !ok {
			continue
		}
		v := decodePDFText(raw)
		if key == "CreationDate" || key == "ModDate" {
			v = pdfDate(v)
		}
		r.add(g, key, v)
	}

	saves := bytes.Count(b, []byte("%%EOF"))
	if bytes.Contains(head(b, 2048), []byte("/Linearized")) {
		saves-- // linearized files have an extra eof marker for free
	}
	if saves > 1 {
		r.add(g, "IncrementalUpdates", strconv.Itoa(saves-1))
	}

	scanXMP(r, b, pfx+"XMP")
}

// reads a pdf string starting at b[i] which is either ( or <
func pdfString(b []byte, i int) ([]byte, bool) {
	if i >= len(b) {
		return nil, false
	}
	if b[i] == '<' {
		if i+1 < len(b) && b[i+1] == '<' {
			return nil, false // that's a dict, not a hex string
		}
		end := bytes.IndexByte(b[i:], '>')
		if end < 0 {
			return nil, false
		}
		var out []byte
		hex := bytes.Map(func(c rune) rune {
			if strings.ContainsRune("0123456789abcdefABCDEF", c) {
				return c
			}
			return -1
		}, b[i+1:i+end])
		if len(hex)%2 == 1 {
			hex = append(hex, '0')
		}
		for j := 0; j+1 < len(hex); j += 2 {
			v, _ := strconv.ParseUint(string(hex[j:j+2]), 16, 8)
			out = append(out, byte(v))
		}
		return out, true
	}

	// literal string, parens can nest and backslash escapes things
	var out []byte
	depth := 0
	for j := i; j < len(b) && j-i < 64<<10; j++ {
		c := b[j]
		switch c {
		case '\\':
			j++
			if j >= len(b) {
				return out, true
			}
			switch e := b[j]; e {
			case 'n':
				out = append(out, '\n')
			case 'r':
				out = append(out, '\r')
			case 't':
				out = append(out, '\t')
			case 'b':
				out = append(out, '\b')
			case 'f':
				out = append(out, '\f')
			case '\r', '\n':
				// line continuation, eat it
				if e == '\r' && j+1 < len(b) && b[j+1] == '\n' {
					j++
				}
			default:
				if e >= '0' && e <= '7' {
					n := 0
					k := 0
					for ; k < 3 && j+k < len(b) && b[j+k] >= '0' && b[j+k] <= '7'; k++ {
						n = n*8 + int(b[j+k]-'0')
					}
					out = append(out, byte(n))
					j += k - 1
				} else {
					out = append(out, e)
				}
			}
		case '(':
			depth++
			if depth > 1 {
				out = append(out, c)
			}
		case ')':
			depth--
			if depth == 0 {
				return out, true
			}
			out = append(out, c)
		default:
			out = append(out, c)
		}
	}
	return nil, false
}

func decodePDFText(b []byte) string {
	switch {
	case bytes.HasPrefix(b, []byte{0xFE, 0xFF}):
		return utf16be(b[2:])
	case bytes.HasPrefix(b, []byte{0xEF, 0xBB, 0xBF}):
		return string(b[3:])
	}
	return latin1(b) // close enough to PDFDocEncoding for metadata
}

// D:20230105143000+01'00' -> 2023-01-05 14:30:00 +01:00
func pdfDate(s string) string {
	d := strings.TrimPrefix(strings.TrimSpace(s), "D:")
	if len(d) < 4 {
		return s
	}
	digits := func(from, to int) string {
		if len(d) >= to {
			return d[from:to]
		}
		return "00"
	}
	out := fmt.Sprintf("%s-%s-%s %s:%s:%s", d[:4], digits(4, 6), digits(6, 8), digits(8, 10), digits(10, 12), digits(12, 14))
	if len(d) > 14 {
		tz := strings.ReplaceAll(d[14:], "'", "")
		switch {
		case tz == "Z":
			out += " UTC"
		case len(tz) >= 5:
			out += " " + tz[:3] + ":" + tz[3:5]
		case len(tz) >= 3:
			out += " " + tz[:3] + ":00"
		}
	}
	return out
}

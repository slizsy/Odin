package meta

import (
	"bytes"
	"html"
	"regexp"
	"strconv"
	"strings"
)

var (
	xmpList = regexp.MustCompile(`(?s)<([\w-]+):([\w-]+)>\s*<rdf:(?:Seq|Bag|Alt)>(.*?)</rdf:(?:Seq|Bag|Alt)>`)
	xmpLi   = regexp.MustCompile(`(?s)<rdf:li[^>]*>([^<]*)</rdf:li>`)
	xmpElem = regexp.MustCompile(`<([\w-]+):([\w-]+)(?:\s[^>]*)?>([^<]+)</([\w-]+):([\w-]+)>`)
	xmpAttr = regexp.MustCompile(`([\w-]+):([\w-]+)="([^"]*)"`)
)

// prefixes that are just xml plumbing
var xmpSkipPrefix = map[string]bool{"rdf": true, "xmlns": true, "xml": true, "x": true}

// noisy per-save ids, the document level ids are kept
var xmpSkipKey = map[string]bool{"stEvt:instanceID": true, "stRef:instanceID": true, "xmpMM:InstanceID": true}

// finds every xmp packet in a blob, returns true if it found any
func scanXMP(r *Result, b []byte, group string) bool {
	found := false
	for n := 0; n < 32; n++ {
		s := bytes.Index(b, []byte("<x:xmpmeta"))
		if s < 0 {
			break
		}
		e := bytes.Index(b[s:], []byte("</x:xmpmeta>"))
		if e < 0 {
			break
		}
		parseXMP(r, b[s:s+e], group)
		found = true
		b = b[s+e:]
	}
	return found
}

func parseXMP(r *Result, b []byte, group string) {
	doc := string(b)
	vals := map[string]string{}
	put := func(prefix, name, v string) {
		key := prefix + ":" + name
		if xmpSkipPrefix[prefix] || xmpSkipKey[key] {
			return
		}
		v = strings.TrimSpace(html.UnescapeString(v))
		if v == "" || len(v) > 1000 { // big blobs are base64 thumbnails
			return
		}
		r.add(group, key, v)
		vals[key] = v
	}

	// lists first (dc:creator, dc:subject etc), then cut them out so the
	// element pass below doesn't see their rdf:li bits
	for _, m := range xmpList.FindAllStringSubmatch(doc, -1) {
		var items []string
		for _, li := range xmpLi.FindAllStringSubmatch(m[3], -1) {
			if s := strings.TrimSpace(li[1]); s != "" {
				items = append(items, s)
			}
		}
		put(m[1], m[2], strings.Join(items, ", "))
	}
	stripped := xmpList.ReplaceAllString(doc, "")
	for _, m := range xmpElem.FindAllStringSubmatch(stripped, -1) {
		if m[1] == m[4] && m[2] == m[5] {
			put(m[1], m[2], m[3])
		}
	}
	for _, m := range xmpAttr.FindAllStringSubmatch(doc, -1) {
		put(m[1], m[2], m[3])
	}

	lat, ok1 := xmpCoord(vals["exif:GPSLatitude"])
	lon, ok2 := xmpCoord(vals["exif:GPSLongitude"])
	if ok1 && ok2 {
		r.addGPS(lat, lon, group)
	}
}

// xmp writes coords like "40,26.7720N" or "40,26,46.3N"
func xmpCoord(s string) (float64, bool) {
	s = strings.TrimSpace(s)
	if len(s) < 2 {
		return 0, false
	}
	ref := s[len(s)-1]
	sign := 1.0
	switch ref {
	case 'S', 's', 'W', 'w':
		sign = -1
		s = s[:len(s)-1]
	case 'N', 'n', 'E', 'e':
		s = s[:len(s)-1]
	}
	parts := strings.Split(s, ",")
	div := 1.0
	total := 0.0
	for _, p := range parts {
		f, err := strconv.ParseFloat(strings.TrimSpace(p), 64)
		if err != nil {
			return 0, false
		}
		total += f / div
		div *= 60
	}
	return sign * total, true
}

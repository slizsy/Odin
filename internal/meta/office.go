package meta

import (
	"archive/zip"
	"bytes"
	"encoding/xml"
	"io"
	"path"
	"regexp"
	"strconv"
	"strings"
)

// app.xml has a ton of boring counters, these are the useful ones
var appKeys = map[string]bool{
	"Application": true, "AppVersion": true, "Company": true, "Manager": true,
	"TotalTime": true, "Template": true, "HyperlinkBase": true, "Pages": true, "Words": true, "Slides": true,
}

var (
	customProp  = regexp.MustCompile(`(?s)<property[^>]*\bname="([^"]+)"[^>]*>\s*<vt:\w+>([^<]*)</vt:\w+>`)
	odfUserProp = regexp.MustCompile(`<meta:user-defined[^>]*meta:name="([^"]+)"[^>]*>([^<]*)<`)
	absPath     = regexp.MustCompile(`absPath[^>]*\burl="([^"]+)"`)
	relTarget   = regexp.MustCompile(`<Relationship\b[^>]*>`)
	attrRe      = regexp.MustCompile(`(\w+)="([^"]*)"`)
)

func parseZip(r *Result, b []byte, pfx string, setType func(string)) {
	zr, err := zip.NewReader(bytes.NewReader(b), int64(len(b)))
	if err != nil {
		r.errf("zip: %v", err)
		return
	}
	files := map[string]*zip.File{}
	for _, f := range zr.File {
		files[f.Name] = f
	}
	read := func(name string) []byte {
		f := files[name]
		if f == nil {
			return nil
		}
		rc, err := f.Open()
		if err != nil {
			return nil
		}
		defer rc.Close()
		data, _ := io.ReadAll(io.LimitReader(rc, 32<<20))
		return data
	}

	switch {
	case files["[Content_Types].xml"] != nil:
		kind := "Office Open XML"
		for _, f := range zr.File {
			switch {
			case strings.HasPrefix(f.Name, "word/"):
				kind = "Word document (docx)"
			case strings.HasPrefix(f.Name, "xl/"):
				kind = "Excel workbook (xlsx)"
			case strings.HasPrefix(f.Name, "ppt/"):
				kind = "PowerPoint deck (pptx)"
			default:
				continue
			}
			break
		}
		setType(kind)
		g := pfx + "Office"
		for _, kv := range xmlLeaves(read("docProps/core.xml"), 2) {
			r.add(g, kv[0], kv[1])
		}
		for _, kv := range xmlLeaves(read("docProps/app.xml"), 2) {
			if appKeys[kv[0]] {
				r.add(g, kv[0], kv[1])
			}
		}
		for _, m := range customProp.FindAllSubmatch(read("docProps/custom.xml"), -1) {
			r.add(pfx+"Office/custom", xmlText(m[1]), xmlText(m[2]))
		}
		// excel loves saving the full path of where the file lived
		if m := absPath.FindSubmatch(read("xl/workbook.xml")); m != nil {
			r.add(g, "AbsPath", xmlText(m[1]))
		}
		for _, f := range zr.File {
			if strings.HasSuffix(f.Name, ".rels") {
				externalTargets(r, read(f.Name), g)
			}
		}

	case files["meta.xml"] != nil:
		setType("OpenDocument")
		g := pfx + "ODF"
		meta := read("meta.xml")
		for _, kv := range xmlLeaves(meta, 3) {
			r.add(g, kv[0], kv[1])
		}
		for _, m := range odfUserProp.FindAllSubmatch(meta, -1) {
			r.add(g+"/custom", xmlText(m[1]), xmlText(m[2]))
		}

	default:
		r.add(pfx+"ZIP", "Entries", strconv.Itoa(len(zr.File)))
		r.add(pfx+"ZIP", "Comment", zr.Comment)
	}

	// photos dropped into documents keep their exif, gps and all
	n := 0
	for _, f := range zr.File {
		if n >= 200 || !isImageName(f.Name) || f.UncompressedSize64 > 64<<20 {
			continue
		}
		if data := read(f.Name); len(data) > 0 {
			parseBytes(r, data, pfx+f.Name+" › ")
			n++
		}
	}
}

// links to local files / network shares leak paths and usernames
func externalTargets(r *Result, data []byte, group string) {
	for _, tag := range relTarget.FindAll(data, -1) {
		attrs := map[string]string{}
		for _, a := range attrRe.FindAllSubmatch(tag, -1) {
			attrs[string(a[1])] = xmlText(a[2])
		}
		if attrs["TargetMode"] != "External" {
			continue
		}
		t := attrs["Target"]
		low := strings.ToLower(t)
		if strings.HasPrefix(low, "file:") || strings.HasPrefix(t, `\\`) || (len(t) > 2 && t[1] == ':') {
			r.add(group, "ExternalLink", t)
		}
	}
}

func isImageName(name string) bool {
	switch strings.ToLower(path.Ext(name)) {
	case ".jpg", ".jpeg", ".png", ".tif", ".tiff", ".webp":
		return true
	}
	return false
}

// pulls out the text of leaf elements sitting at a given depth, keyed by local name
func xmlLeaves(data []byte, want int) [][2]string {
	if len(data) == 0 {
		return nil
	}
	var out [][2]string
	d := xml.NewDecoder(bytes.NewReader(data))
	d.Strict = false
	depth := 0
	var name string
	var text strings.Builder
	hasChild := false
	for {
		tok, err := d.Token()
		if err != nil {
			break
		}
		switch t := tok.(type) {
		case xml.StartElement:
			depth++
			if depth == want {
				name, hasChild = t.Name.Local, false
				text.Reset()
			} else if depth > want {
				hasChild = true
			}
		case xml.CharData:
			if depth == want {
				text.Write(t)
			}
		case xml.EndElement:
			if depth == want && !hasChild {
				out = append(out, [2]string{name, strings.TrimSpace(text.String())})
			}
			depth--
		}
	}
	return out
}

func xmlText(b []byte) string {
	var s string
	if err := xml.Unmarshal([]byte("<x>"+string(b)+"</x>"), &s); err == nil {
		return s
	}
	return string(b)
}

package meta

import (
	"archive/zip"
	"bytes"
	"encoding/binary"
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

type tent struct {
	tag, typ uint16
	count    uint32
	data     []byte
}

// lays out ifds back to back. exif/gps pointer entries hold the index of the ifd they point at
func buildTIFF(ifds [][]tent) []byte {
	le := binary.LittleEndian
	offs := make([]uint32, len(ifds))
	pos := uint32(8)
	for i, ifd := range ifds {
		offs[i] = pos
		pos += 2 + uint32(len(ifd))*12 + 4
		for _, e := range ifd {
			if len(e.data) > 4 {
				pos += uint32(len(e.data))
			}
		}
	}
	b := make([]byte, pos)
	copy(b, "II*\x00")
	le.PutUint32(b[4:], 8)
	for i, ifd := range ifds {
		p := offs[i]
		le.PutUint16(b[p:], uint16(len(ifd)))
		dataPos := p + 2 + uint32(len(ifd))*12 + 4
		for j, e := range ifd {
			ep := p + 2 + uint32(j)*12
			le.PutUint16(b[ep:], e.tag)
			le.PutUint16(b[ep+2:], e.typ)
			le.PutUint32(b[ep+4:], e.count)
			switch {
			case e.tag == tagExifIFD || e.tag == tagGPSIFD:
				le.PutUint32(b[ep+8:], offs[e.data[0]])
			case len(e.data) <= 4:
				copy(b[ep+8:], e.data)
			default:
				le.PutUint32(b[ep+8:], dataPos)
				copy(b[dataPos:], e.data)
				dataPos += uint32(len(e.data))
			}
		}
	}
	return b
}

func ascii(tag uint16, s string) tent { return tent{tag, 2, uint32(len(s) + 1), append([]byte(s), 0)} }

func rats(tag uint16, vals ...[2]uint32) tent {
	var b []byte
	for _, v := range vals {
		b = binary.LittleEndian.AppendUint32(b, v[0])
		b = binary.LittleEndian.AppendUint32(b, v[1])
	}
	return tent{tag, 5, uint32(len(vals)), b}
}

func sampleJPEG() []byte {
	t := buildTIFF([][]tent{
		{ascii(0x010F, "Canon"), ascii(0x013B, "John Doe"), {tagExifIFD, 4, 1, []byte{1}}, {tagGPSIFD, 4, 1, []byte{2}}},
		{ascii(0xA431, "SN-0042")},
		{ascii(1, "S"), rats(2, [2]uint32{33, 1}, [2]uint32{51, 1}, [2]uint32{3600, 100}),
			ascii(3, "E"), rats(4, [2]uint32{151, 1}, [2]uint32{12, 1}, [2]uint32{0, 1})},
	})
	seg := append([]byte("Exif\x00\x00"), t...)
	b := []byte{0xFF, 0xD8, 0xFF, 0xE1}
	b = binary.BigEndian.AppendUint16(b, uint16(len(seg)+2))
	b = append(b, seg...)
	return append(b, 0xFF, 0xD9)
}

func writeTemp(t *testing.T, name string, data []byte) string {
	t.Helper()
	p := filepath.Join(t.TempDir(), name)
	if err := os.WriteFile(p, data, 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}

func field(r *Result, key string) string {
	for _, f := range r.Fields {
		if f.Key == key {
			return f.Value
		}
	}
	return ""
}

func hasFinding(r *Result, kind, substr string) bool {
	for _, f := range r.Findings {
		if f.Kind == kind && strings.Contains(f.Detail, substr) {
			return true
		}
	}
	return false
}

func TestJPEGExifAndGPS(t *testing.T) {
	r := Extract(writeTemp(t, "a.jpg", sampleJPEG()), Options{})
	if r.Type != "JPEG image" {
		t.Fatalf("type = %q", r.Type)
	}
	if field(r, "Make") != "Canon" || field(r, "BodySerialNumber") != "SN-0042" {
		t.Fatalf("missing fields: %+v", r.Fields)
	}
	if len(r.GPS) != 1 {
		t.Fatalf("gps = %+v", r.GPS)
	}
	// 33°51'36"S 151°12'0"E, sydney
	if g := r.GPS[0]; math.Abs(g.Lat+33.86) > 1e-6 || math.Abs(g.Lon-151.2) > 1e-6 {
		t.Fatalf("gps = %+v", g)
	}
	if !hasFinding(r, "location", "-33.860000") || !hasFinding(r, "identity", "John Doe") || !hasFinding(r, "device id", "SN-0042") {
		t.Fatalf("findings = %+v", r.Findings)
	}
}

func TestPDFInfoAndHistory(t *testing.T) {
	pdf := "%PDF-1.4\n1 0 obj << /Author (Alice \\(draft\\)) /Producer (Word) /CreationDate (D:20230105143000+01'00') >> endobj\n%%EOF\n" +
		"2 0 obj << /Author <FEFF0042006F0062> /Producer (Word) >> endobj\n%%EOF\n"
	r := Extract(writeTemp(t, "a.pdf", []byte(pdf)), Options{})
	if field(r, "CreationDate") != "2023-01-05 14:30:00 +01:00" {
		t.Fatalf("date = %q", field(r, "CreationDate"))
	}
	if field(r, "IncrementalUpdates") != "1" {
		t.Fatalf("updates = %q", field(r, "IncrementalUpdates"))
	}
	if !hasFinding(r, "edit history", "Alice (draft) → Bob") {
		t.Fatalf("findings = %+v", r.Findings)
	}
}

func TestDocxLeaks(t *testing.T) {
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	files := map[string]string{
		"[Content_Types].xml": `<Types/>`,
		"docProps/core.xml": `<cp:coreProperties xmlns:cp="c" xmlns:dc="d"><dc:creator>Jane</dc:creator>` +
			`<cp:lastModifiedBy>bob@corp.example</cp:lastModifiedBy></cp:coreProperties>`,
		"word/document.xml": `<w:document/>`,
		"word/_rels/settings.xml.rels": `<Relationships><Relationship Id="r1" Target="file:///\\fileserv01\share\tpl.dotx" TargetMode="External"/>` +
			`<Relationship Id="r2" Target="https://example.com" TargetMode="External"/></Relationships>`,
		"word/media/image1.jpeg": string(sampleJPEG()),
	}
	for name, body := range files {
		w, _ := zw.Create(name)
		w.Write([]byte(body))
	}
	zw.Close()

	r := Extract(writeTemp(t, "a.docx", buf.Bytes()), Options{})
	if r.Type != "Word document (docx)" {
		t.Fatalf("type = %q", r.Type)
	}
	for _, c := range [][2]string{
		{"identity", "Jane"},
		{"email", "bob@corp.example"},
		{"network share", "fileserv01"},
		{"location", "151.2"},
	} {
		if !hasFinding(r, c[0], c[1]) {
			t.Errorf("no %s finding with %q: %+v", c[0], c[1], r.Findings)
		}
	}
	if strings.Contains(field(r, "ExternalLink"), "example.com") {
		t.Error("plain web links shouldn't count as external file links")
	}
}

func TestXMP(t *testing.T) {
	x := `<x:xmpmeta><rdf:RDF><rdf:Description xmp:CreatorTool="Lightroom" exif:GPSLatitude="40,26.7720N" exif:GPSLongitude="79,58.932W">` +
		`<dc:creator><rdf:Seq><rdf:li>Ann</rdf:li><rdf:li>Ben</rdf:li></rdf:Seq></dc:creator>` +
		`<photoshop:City>Pittsburgh</photoshop:City></rdf:Description></rdf:RDF></x:xmpmeta>`
	r := Extract(writeTemp(t, "a.bin", []byte("junk junk "+x)), Options{})
	if field(r, "dc:creator") != "Ann, Ben" || field(r, "xmp:CreatorTool") != "Lightroom" {
		t.Fatalf("fields = %+v", r.Fields)
	}
	if len(r.GPS) != 1 || math.Abs(r.GPS[0].Lat-40.4462) > 1e-4 || math.Abs(r.GPS[0].Lon+79.9822) > 1e-4 {
		t.Fatalf("gps = %+v", r.GPS)
	}
	if !hasFinding(r, "place", "Pittsburgh") || !hasFinding(r, "identity", "Ann, Ben") {
		t.Fatalf("findings = %+v", r.Findings)
	}
}

func TestGarbageDoesntPanic(t *testing.T) {
	// truncated / mangled versions of each format should just error, never crash
	inputs := [][]byte{
		sampleJPEG()[:30],
		[]byte("II*\x00\xff\xff\xff\xff"),
		append([]byte("\x89PNG\r\n\x1a\n"), 0, 0, 0, 99, 't', 'E', 'X', 't'),
		[]byte("%PDF-1.7 /Author (unterminated"),
		[]byte("PK\x03\x04 not really a zip"),
		[]byte("RIFF\x00\x00\x00\x00WEBPEXIF\xff\xff\xff\x7f"),
	}
	for i, in := range inputs {
		Extract(writeTemp(t, "g"+string(rune('a'+i)), in), Options{})
	}
}

// go test ./internal/meta -fuzz FuzzParse -fuzztime 30s
func FuzzParse(f *testing.F) {
	f.Add(sampleJPEG())
	f.Add([]byte("%PDF-1.4 /Author <FEFF0041> /Title (a\\(b\\)c) %%EOF"))
	f.Add(append([]byte("\x89PNG\r\n\x1a\n"), 0, 0, 0, 3, 't', 'E', 'X', 't', 'a', 0, 'b', 0, 0, 0, 0))
	f.Fuzz(func(t *testing.T, b []byte) {
		r := &Result{seen: map[string]bool{}}
		parseBytes(r, b, "")
		analyze(r)
	})
}

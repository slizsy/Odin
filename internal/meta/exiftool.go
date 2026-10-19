package meta

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os/exec"
	"strconv"
	"strings"
)

func HaveExifTool() bool {
	_, err := exec.LookPath("exiftool")
	return err == nil
}

// swaps our field dump for exiftool's full one. we keep our own gps hits
// and add whatever exiftool finds on top
func runExifTool(r *Result) error {
	if !HaveExifTool() {
		return fmt.Errorf("not installed (grab it from exiftool.org or your package manager)")
	}
	out, err := exec.Command("exiftool", "-j", "-G1", "-a", "-s", "-c", "%+.6f", "-api", "largefilesupport=1", "--", r.Path).Output()
	if err != nil && len(out) == 0 {
		return err
	}

	// stream the json so we keep exiftool's tag order, a map would scramble it
	dec := json.NewDecoder(bytes.NewReader(out))
	dec.UseNumber()
	if t, err := dec.Token(); err != nil || t != json.Delim('[') {
		return fmt.Errorf("weird output")
	}
	if t, err := dec.Token(); err != nil || t != json.Delim('{') {
		return fmt.Errorf("weird output")
	}

	r.Fields = nil
	r.seen = map[string]bool{}
	var lat, lon string
	for dec.More() {
		kt, err := dec.Token()
		if err != nil {
			return err
		}
		var v any
		if err := dec.Decode(&v); err != nil {
			return err
		}
		k, _ := kt.(string)
		group, tag, ok := strings.Cut(k, ":")
		if !ok || group == "ExifTool" || group == "System" {
			continue
		}
		val := fmt.Sprint(v)
		if arr, isArr := v.([]any); isArr {
			parts := make([]string, len(arr))
			for i, a := range arr {
				parts[i] = fmt.Sprint(a)
			}
			val = strings.Join(parts, ", ")
		}
		r.add(group, tag, val)
		if group == "Composite" && tag == "GPSLatitude" {
			lat = val
		}
		if group == "Composite" && tag == "GPSLongitude" {
			lon = val
		}
	}
	if la, ok := signedCoord(lat); ok {
		if lo, ok := signedCoord(lon); ok {
			r.addGPS(la, lo, "exiftool")
		}
	}
	return nil
}

// "+40.446195" or "40.446195 N"
func signedCoord(s string) (float64, bool) {
	s = strings.TrimSpace(s)
	if s == "" {
		return 0, false
	}
	sign := 1.0
	if strings.HasSuffix(s, "S") || strings.HasSuffix(s, "W") {
		sign = -1
	}
	s = strings.TrimRight(s, " NSEW")
	f, err := strconv.ParseFloat(s, 64)
	return sign * f, err == nil
}

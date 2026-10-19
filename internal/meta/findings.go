package meta

import (
	"fmt"
	"regexp"
	"sort"
	"strings"
)

// keys are lowercased + namespace stripped before lookup
var (
	identityKeys = set("artist", "xpauthor", "author", "cameraownername", "ownername", "lastmodifiedby",
		"company", "manager", "initial-creator", "copyright", "rights", "by-line", "writer-editor",
		"credit", "printed-by", "copyrightnotice", "captionwriter", "contact")
	placeKeys = set("city", "sub-location", "sublocation", "province-state", "state", "country",
		"country-primarylocationname", "location")
	deviceKeys   = set("make", "model", "lensmake", "lensmodel", "hostcomputer", "uniquecameramodel")
	softwareKeys = set("software", "creatortool", "producer", "application", "appversion", "generator",
		"softwareagent", "originatingprogram", "historysoftwareagent")
	editKeys = set("totaltime", "editing-duration", "editing-cycles", "revision", "revisionnumber",
		"template", "incrementalupdates")
)

var (
	winUser  = regexp.MustCompile(`(?i)\b[a-z]:[\\/](?:users|documents and settings)[\\/]([^\\/:*?"<>|\r\n]+)`)
	nixUser  = regexp.MustCompile(`(?:^|[\s"'(=]|file://)/(?:home|Users)/([^/\s"']+)`)
	winPath  = regexp.MustCompile(`(?i)\b[a-z]:\\[^\s"<>|]+`)
	uncPath  = regexp.MustCompile(`\\\\([a-zA-Z0-9][\w.-]*)\\[^\s"<>|]*`)
	emailRe  = regexp.MustCompile(`[\w.+-]+@[\w-]+(?:\.[\w-]+)+`)
	levelOrd = map[string]int{High: 0, Medium: 1, Info: 2}
)

func set(keys ...string) map[string]bool {
	m := map[string]bool{}
	for _, k := range keys {
		m[k] = true
	}
	return m
}

func analyze(r *Result) []Finding {
	var out []Finding
	seen := map[string]bool{}
	add := func(level, kind, detail, src string) {
		id := kind + "\x00" + detail
		if seen[id] {
			return
		}
		seen[id] = true
		out = append(out, Finding{level, kind, detail, src})
	}

	for _, g := range r.GPS {
		add(High, "location", fmt.Sprintf("%.6f, %.6f  %s", g.Lat, g.Lon, g.MapsURL()), g.Source)
	}

	// pdf keys that show up with more than one value = someone edited it
	history := map[string][]string{}

	for _, f := range r.Fields {
		local := strings.ToLower(f.Key)
		if i := strings.LastIndex(local, ":"); i >= 0 {
			local = local[i+1:]
		}
		// in pdf info "Creator" is the app, everywhere else it's a person
		isPDF := strings.HasSuffix(f.Group, "PDF")
		label := f.Key + ": " + f.Value

		switch {
		case strings.Contains(local, "serialnumber"):
			add(Medium, "device id", label, f.Group)
		case identityKeys[local] || (local == "creator" && !isPDF):
			add(Medium, "identity", label, f.Group)
		case placeKeys[local]:
			add(Medium, "place", label, f.Group)
		case deviceKeys[local]:
			add(Info, "device", label, f.Group)
		case softwareKeys[local] || local == "creator":
			add(Info, "software", label, f.Group)
		case editKeys[local] && f.Value != "0":
			add(Info, "edit trail", label, f.Group)
		}

		if isPDF {
			k := f.Group + "\x00" + f.Key
			history[k] = append(history[k], f.Value)
		}

		leaked := false
		for _, m := range winUser.FindAllStringSubmatch(f.Value, -1) {
			add(High, "path leak", fmt.Sprintf("username %q in %s", m[1], f.Key), f.Group)
			leaked = true
		}
		for _, m := range nixUser.FindAllStringSubmatch(f.Value, -1) {
			add(High, "path leak", fmt.Sprintf("username %q in %s", m[1], f.Key), f.Group)
			leaked = true
		}
		for _, m := range uncPath.FindAllStringSubmatch(f.Value, -1) {
			add(Medium, "network share", fmt.Sprintf("host %q in %s: %s", m[1], f.Key, m[0]), f.Group)
			leaked = true
		}
		if !leaked {
			for _, m := range winPath.FindAllString(f.Value, -1) {
				add(Medium, "local path", f.Key+": "+m, f.Group)
			}
		}
		for _, m := range emailRe.FindAllString(f.Value, -1) {
			add(Medium, "email", f.Key+": "+m, f.Group)
		}
	}

	keys := make([]string, 0, len(history))
	for k := range history {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		vals := history[k]
		if len(vals) < 2 || strings.HasSuffix(k, "IncrementalUpdates") {
			continue
		}
		group, key, _ := strings.Cut(k, "\x00")
		add(Medium, "edit history", fmt.Sprintf("%s changed across saves: %s", key, strings.Join(vals, " → ")), group)
	}

	sort.SliceStable(out, func(i, j int) bool { return levelOrd[out[i].Level] < levelOrd[out[j].Level] })
	return out
}

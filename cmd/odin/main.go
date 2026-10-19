package main

import (
	"encoding/json"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"

	"github.com/slizsy/Odin/internal/banner"
	"github.com/slizsy/Odin/internal/meta"
	"github.com/slizsy/Odin/internal/report"
	"github.com/slizsy/Odin/internal/ui"
)

const usage = `usage: odin <command> [flags] [args]

commands:
  meta <path...>       rip metadata out of files and folders
  help                 this screen
  version              print the version

meta flags:
  -r, --recursive      walk folders all the way down
  -f, --findings       only show findings, skip the raw field dump
  -j, --json           json output (pipe it into jq or whatever)
  -x, --exiftool       full dump through exiftool if it's installed
  -q, --quiet          skip the banner
      --no-color       plain text

reads: jpeg, png, webp, tiff + camera raws, pdf, docx/xlsx/pptx,
       odt/ods/odp, plus xmp packets hiding in pretty much anything

coming soon: user, email, domain, phone, ip

examples:
  odin meta photo.jpg
  odin meta -r ~/Downloads -f
  odin meta report.pdf --json | jq '.[0].findings'
`

func main() {
	args := os.Args[1:]
	if len(args) == 0 {
		banner.Print(os.Stdout)
		fmt.Print(usage)
		return
	}
	switch args[0] {
	case "meta", "m":
		os.Exit(runMeta(args[1:]))
	case "help", "-h", "--help":
		banner.Print(os.Stdout)
		fmt.Print(usage)
	case "version", "-v", "--version":
		fmt.Println("odin", banner.Version)
	case "user", "email", "domain", "phone", "ip":
		fmt.Fprintf(os.Stderr, "odin: the %s module isn't built yet, metadata comes first\n", args[0])
		os.Exit(1)
	default:
		fmt.Fprintf(os.Stderr, "odin: no idea what %q is\n\n%s", args[0], usage)
		os.Exit(2)
	}
}

type metaFlags struct {
	recursive, findings, json, exiftool, quiet bool
}

func runMeta(args []string) int {
	var fl metaFlags
	var paths []string
	for _, a := range args {
		if len(a) < 2 || a[0] != '-' {
			paths = append(paths, a)
			continue
		}
		// let people smash short flags together like -rf
		var names []string
		if strings.HasPrefix(a, "--") {
			names = []string{a}
		} else {
			for _, c := range a[1:] {
				names = append(names, "-"+string(c))
			}
		}
		for _, n := range names {
			switch n {
			case "-r", "--recursive":
				fl.recursive = true
			case "-f", "--findings":
				fl.findings = true
			case "-j", "--json":
				fl.json = true
			case "-x", "--exiftool":
				fl.exiftool = true
			case "-q", "--quiet":
				fl.quiet = true
			case "--no-color":
				ui.Enabled = false
			case "-h", "--help":
				fmt.Print(usage)
				return 0
			default:
				fmt.Fprintf(os.Stderr, "odin: unknown flag %s\n", n)
				return 2
			}
		}
	}
	if fl.json {
		ui.Enabled = false
	}
	if len(paths) == 0 {
		fmt.Fprintf(os.Stderr, "odin: give me at least one file or folder\n\n%s", usage)
		return 2
	}
	if fl.exiftool && !meta.HaveExifTool() {
		fmt.Fprintln(os.Stderr, "odin: -x needs exiftool on your PATH (exiftool.org), using the native parser instead")
		fl.exiftool = false
	}

	files, explicit, errs := collect(paths, fl.recursive)
	for _, e := range errs {
		fmt.Fprintln(os.Stderr, "odin:", e)
	}
	if len(files) == 0 {
		fmt.Fprintln(os.Stderr, "odin: no files to scan")
		return 1
	}

	if !fl.json && !fl.quiet {
		banner.Print(os.Stdout)
	}

	results := extractAll(files, meta.Options{ExifTool: fl.exiftool})

	if fl.json {
		enc := json.NewEncoder(os.Stdout)
		enc.SetIndent("", "  ")
		enc.SetEscapeHTML(false)
		if err := enc.Encode(results); err != nil {
			fmt.Fprintln(os.Stderr, "odin:", err)
			return 1
		}
		return 0
	}

	// in folder scans, files with nothing in them are just noise
	var shown []*meta.Result
	skipped := 0
	for _, r := range results {
		if !explicit[r.Path] && len(r.Fields) == 0 && len(r.GPS) == 0 && len(r.Errors) == 0 {
			skipped++
			continue
		}
		shown = append(shown, r)
		report.Print(os.Stdout, r, fl.findings)
	}
	if len(results) > 1 {
		report.Summary(os.Stdout, shown, skipped)
	}
	return 0
}

// expands folders into files. explicit = files the user named directly
func collect(paths []string, recursive bool) ([]string, map[string]bool, []error) {
	var files []string
	var errs []error
	explicit := map[string]bool{}
	seen := map[string]bool{}
	push := func(p string) {
		if !seen[p] {
			seen[p] = true
			files = append(files, p)
		}
	}
	for _, p := range paths {
		st, err := os.Stat(p)
		if err != nil {
			errs = append(errs, err)
			continue
		}
		if !st.IsDir() {
			explicit[p] = true
			push(p)
			continue
		}
		filepath.WalkDir(p, func(path string, d fs.DirEntry, err error) error {
			if err != nil {
				errs = append(errs, err)
				return nil
			}
			if d.IsDir() {
				if path == p {
					return nil
				}
				// skip .git and friends, and don't go deeper without -r
				if !recursive || strings.HasPrefix(d.Name(), ".") {
					return filepath.SkipDir
				}
				return nil
			}
			if d.Type().IsRegular() {
				push(path)
			}
			return nil
		})
	}
	return files, explicit, errs
}

// parse files in parallel but keep the output in the original order
func extractAll(files []string, opt meta.Options) []*meta.Result {
	results := make([]*meta.Result, len(files))
	jobs := make(chan int)
	var wg sync.WaitGroup
	workers := min(runtime.NumCPU(), 8, len(files))
	for range workers {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := range jobs {
				results[i] = meta.Extract(files[i], opt)
			}
		}()
	}
	for i := range files {
		jobs <- i
	}
	close(jobs)
	wg.Wait()
	return results
}

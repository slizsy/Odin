package report

import (
	"fmt"
	"io"
	"strings"
	"unicode/utf8"

	"github.com/slizsy/Odin/internal/meta"
	"github.com/slizsy/Odin/internal/ui"
)

var bar = ui.Gray("│")

func levelTag(l string) string {
	switch l {
	case meta.High:
		return ui.Red("HIGH")
	case meta.Medium:
		return ui.Yellow("MED ")
	}
	return ui.Cyan("INFO")
}

func Print(w io.Writer, r *meta.Result, findingsOnly bool) {
	fmt.Fprintf(w, "%s %s\n", ui.Gray("┌─"), ui.Bold(r.Path))

	info := []string{r.Type, meta.HumanSize(r.Size)}
	if !r.ModTime.IsZero() {
		info = append(info, "modified "+r.ModTime.Format("2006-01-02 15:04"))
	}
	if r.Engine == "exiftool" {
		info = append(info, "via exiftool")
	}
	fmt.Fprintf(w, "%s  %s\n", bar, ui.Gray(strings.Join(info, " · ")))
	if r.SHA256 != "" {
		fmt.Fprintf(w, "%s  %s\n", bar, ui.Gray("sha256 "+r.SHA256))
	}

	if len(r.Findings) > 0 {
		fmt.Fprintf(w, "%s\n%s  %s\n", bar, bar, ui.Bold("findings"))
		for _, f := range r.Findings {
			// say where it came from when it's buried in another file
			from := ""
			if g, _, ok := strings.Cut(f.Source, " › "); ok {
				from = ui.Gray("  ← " + g)
			}
			fmt.Fprintf(w, "%s   %s  %-13s %s%s\n", bar, levelTag(f.Level), f.Kind, f.Detail, from)
		}
	} else if len(r.Fields) > 0 {
		fmt.Fprintf(w, "%s\n%s  %s\n", bar, bar, ui.Gray("nothing juicy, just the basics"))
	}

	if !findingsOnly {
		printFields(w, r.Fields)
	}

	if len(r.Fields) == 0 && len(r.Errors) == 0 {
		fmt.Fprintf(w, "%s\n%s  %s\n", bar, bar, ui.Gray("no metadata found (clean file, or a format odin can't read yet, try -x)"))
	}
	for _, e := range r.Errors {
		fmt.Fprintf(w, "%s  %s %s\n", bar, ui.Yellow("!"), ui.Gray(e))
	}
	fmt.Fprintf(w, "%s\n\n", ui.Gray("└─"))
}

func printFields(w io.Writer, fields []meta.Field) {
	// keep groups in the order they first showed up
	var order []string
	byGroup := map[string][]meta.Field{}
	for _, f := range fields {
		if _, ok := byGroup[f.Group]; !ok {
			order = append(order, f.Group)
		}
		byGroup[f.Group] = append(byGroup[f.Group], f)
	}
	for _, g := range order {
		fs := byGroup[g]
		width := 0
		for _, f := range fs {
			width = max(width, utf8.RuneCountInString(f.Key))
		}
		width = min(width, 28)
		fmt.Fprintf(w, "%s\n%s  %s\n", bar, bar, ui.C(160, g))
		for _, f := range fs {
			pad := max(width-utf8.RuneCountInString(f.Key), 0)
			fmt.Fprintf(w, "%s   %s%s  %s\n", bar, ui.Gray(f.Key), strings.Repeat(" ", pad), f.Value)
		}
	}
}

func Summary(w io.Writer, rs []*meta.Result, skipped int) {
	var high, med, errs int
	for _, r := range rs {
		errs += len(r.Errors)
		for _, f := range r.Findings {
			switch f.Level {
			case meta.High:
				high++
			case meta.Medium:
				med++
			}
		}
	}
	parts := []string{fmt.Sprintf("%d files scanned", len(rs)+skipped)}
	if skipped > 0 {
		parts = append(parts, fmt.Sprintf("%d had nothing", skipped))
	}
	parts = append(parts, ui.Red(fmt.Sprintf("%d high", high)), ui.Yellow(fmt.Sprintf("%d medium", med)))
	if errs > 0 {
		parts = append(parts, fmt.Sprintf("%d errors", errs))
	}
	fmt.Fprintf(w, "%s %s\n", ui.Gray("──"), strings.Join(parts, ui.Gray(" · ")))
}

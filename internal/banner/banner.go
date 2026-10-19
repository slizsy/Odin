package banner

import (
	"fmt"
	"io"
	"regexp"
	"strings"
	"unicode/utf8"

	"github.com/slizsy/Odin/internal/ui"
)

const Version = "0.1.0"

var logo = []string{
	" ▒█████  ▓█████▄  ██▓ ███▄    █ ",
	"▒██▒  ██▒▒██▀ ██▌▓██▒ ██ ▀█   █ ",
	"▒██░  ██▒░██   █▌▒██▒▓██  ▀█ ██▒",
	"▒██   ██░░▓█▄   ▌░██░▓██▒  ▐▌██▒",
	"░ ████▓▒░░▒████▓ ░██░▒██░   ▓██░",
	"░ ▒░▒░▒░  ▒▒▓  ▒ ░▓  ░ ▒░   ▒ ▒ ",
	"  ░ ▒ ▒░  ░ ▒  ▒  ▒ ░░ ░░   ░ ▒░",
	"░ ░ ░ ▒   ░ ░  ░  ▒ ░   ░   ░ ░ ",
	"    ░ ░     ░     ░           ░ ",
	"          ░                     ",
}

// red fading down into dark blood red
var shades = []int{196, 196, 160, 160, 124, 124, 88, 88, 52, 52}

// odin's eye (the one he traded for wisdom) + his two ravens underneath
// {g}=frame {r}=iris {R}=pupil {y}=runes {d}=dim text
var eye = []string{
	"{y}     ᛟ  ᛞ  ᛁ  ᚾ",
	"{g}  ▄▄▀▀▀▀▀▀▀▀▀▀▀▀▀▄▄",
	"{g}▄▀    {r}▄▄█████▄▄{g}    ▀▄",
	"{g}█   {r}▄██▀▀   ▀▀██▄{g}   █",
	"{g}█  {r}██   {R}▄███▄{r}   ██{g}  █",
	"{g}█  {r}██   {R}▀███▀{r}   ██{g}  █",
	"{g}█   {r}▀██▄▄   ▄▄██▀{g}   █",
	"{g}▀▄    {r}▀▀█████▀▀{g}    ▄▀",
	"{g}  ▀▀▄▄▄▄▄▄▄▄▄▄▄▄▄▀▀",
	"{d}   huginn · muninn",
}

var tagRe = regexp.MustCompile(`\{[a-zA-Z]\}`)

var paint = map[string]func(string) string{
	"{g}": func(s string) string { return ui.C(250, s) },
	"{r}": func(s string) string { return ui.C(124, s) },
	"{R}": func(s string) string { return ui.C(196, s) },
	"{y}": func(s string) string { return ui.C(178, s) },
	"{d}": func(s string) string { return ui.C(242, s) },
}

// turns the {x} markup into actual colors
func render(line string) string {
	var b strings.Builder
	locs := tagRe.FindAllStringIndex(line, -1)
	for i, loc := range locs {
		end := len(line)
		if i+1 < len(locs) {
			end = locs[i+1][0]
		}
		tag := line[loc[0]:loc[1]]
		chunk := line[loc[1]:end]
		if fn, ok := paint[tag]; ok {
			b.WriteString(fn(chunk))
		} else {
			b.WriteString(chunk)
		}
	}
	return b.String()
}

func Print(w io.Writer) {
	fmt.Fprintln(w)
	for i, l := range logo {
		// pad by rune count so the eye lines up even with multibyte chars
		pad := 34 - utf8.RuneCountInString(l)
		fmt.Fprintf(w, " %s%s  %s\n", ui.C(shades[i], l), strings.Repeat(" ", pad), render(eye[i]))
	}
	fmt.Fprintln(w)
	fmt.Fprintf(w, "%19s%s %s %s\n", "", ui.Gray("──"), ui.Bold("made by slizsy"), ui.Gray("──"))
	fmt.Fprintf(w, "%11s%s\n\n", "", ui.Dim("the all-seeing osint toolkit · v"+Version))
}

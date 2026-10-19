package ui

import (
	"fmt"
	"os"
)

// flipped off automatically when we're not on a terminal or NO_COLOR is set
var Enabled = isTTY(os.Stdout) && os.Getenv("NO_COLOR") == ""

func isTTY(f *os.File) bool {
	st, err := f.Stat()
	return err == nil && st.Mode()&os.ModeCharDevice != 0
}

func wrap(code, s string) string {
	if !Enabled {
		return s
	}
	return "\x1b[" + code + "m" + s + "\x1b[0m"
}

// 256 color fg
func C(n int, s string) string { return wrap(fmt.Sprintf("38;5;%d", n), s) }

func Bold(s string) string   { return wrap("1", s) }
func Dim(s string) string    { return wrap("2", s) }
func Red(s string) string    { return wrap("1;38;5;196", s) }
func Yellow(s string) string { return wrap("38;5;214", s) }
func Cyan(s string) string   { return wrap("38;5;81", s) }
func Gray(s string) string   { return wrap("38;5;245", s) }

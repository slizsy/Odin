#include "ui/color.h"

#include <stdio.h>
#include <stdlib.h>

#include "core/platform.h"

bool color_on = false;

void color_init(void) { color_on = platform_is_tty(1) && platform_colors_ok() && !getenv("NO_COLOR"); }

#define RING 16
#define SLOT 2048

static const char *wrap(const char *code, const char *s) {
    if (!color_on) return s;
    static char ring[RING][SLOT];
    static int idx;
    char *out = ring[idx++ % RING];
    snprintf(out, SLOT, "\x1b[%sm%s\x1b[0m", code, s);
    return out;
}

const char *c256(int n, const char *s) {
    char code[16];
    snprintf(code, sizeof code, "38;5;%d", n);
    return wrap(code, s);
}

const char *c_bold(const char *s) { return wrap("1", s); }
const char *c_red(const char *s) { return wrap("1;38;5;196", s); }
const char *c_yellow(const char *s) { return wrap("38;5;214", s); }
const char *c_cyan(const char *s) { return wrap("38;5;81", s); }
const char *c_gray(const char *s) { return wrap("38;5;245", s); }

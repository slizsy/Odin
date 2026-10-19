#ifndef ODIN_COLOR_H
#define ODIN_COLOR_H

#include <stdbool.h>

extern bool color_on;
void color_init(void); // auto off when not a terminal or NO_COLOR is set

// these hand back a static buffer from a small ring, so a few can share one printf
const char *c256(int n, const char *s);
const char *c_bold(const char *s);
const char *c_red(const char *s);
const char *c_yellow(const char *s);
const char *c_cyan(const char *s);
const char *c_gray(const char *s);

#endif

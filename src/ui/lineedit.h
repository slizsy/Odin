#ifndef ODIN_LINEEDIT_H
#define ODIN_LINEEDIT_H

#include <stdbool.h>
#include <stddef.h>

// reads one line for the interactive shell. on a mac/linux terminal you get arrow keys,
// history and the usual ctrl shortcuts; windows' console does its own editing + history.
// prompt can have color codes, prompt_width is how many columns it takes on screen.
// returns a malloc'd line (caller frees), "" after ctrl+c, NULL at end of input (ctrl+d)
char *lineedit_read(const char *prompt, size_t prompt_width);

void lineedit_history_add(const char *line);

#endif

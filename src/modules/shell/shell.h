#ifndef ODIN_SHELL_H
#define ODIN_SHELL_H

#include <stddef.h>

// `odin` with no arguments: banner once, then an odin> prompt until exit
int run_shell(void);

// splits a line into words the way a terminal would (quotes, \ escapes, ~, *.jpg globs).
// returns the word count, -1 with err filled on a broken line. free with shell_free_words
int shell_split(const char *line, char ***words, char *err, size_t errlen);
void shell_free_words(char **words, int n);

#endif

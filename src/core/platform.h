#ifndef ODIN_PLATFORM_H
#define ODIN_PLATFORM_H

// every windows vs mac/linux difference lives here so the rest of the code doesn't care

#include <stdbool.h>
#include <sys/stat.h>

// utf-8 console + ansi colors on windows, nothing to do elsewhere
void platform_init(void);
bool platform_is_tty(int fd);
bool platform_colors_ok(void);

// if someone double-clicked odin.exe the window would vanish instantly, so wait for enter
void platform_pause_if_launched_from_explorer(void);

// stat that doesn't follow symlinks (windows: plain stat)
int stat_nofollow(const char *path, struct stat *st);
bool is_path_sep(char c);
const char *path_base(const char *path);
unsigned platform_pid(void);

#endif

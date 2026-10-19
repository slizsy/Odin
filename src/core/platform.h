#ifndef ODIN_PLATFORM_H
#define ODIN_PLATFORM_H

// every windows vs mac/linux difference lives here so the rest of the code doesn't care

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

// utf-8 console + ansi colors on windows, nothing to do elsewhere
void platform_init(void);
bool platform_is_tty(int fd);
bool platform_colors_ok(void);

// ctrl+c in the interactive shell: the first press sets this so the running command can
// wrap up and drop back to the prompt, a second press while it's still set quits for real
extern volatile int odin_interrupted;
void platform_catch_interrupts(void);

uint64_t platform_now_ms(void); // monotonic, for timing retries
const char *platform_home(void); // NULL if there isn't one

// stat that doesn't follow symlinks (windows: plain stat)
int stat_nofollow(const char *path, struct stat *st);
bool is_path_sep(char c);
const char *path_base(const char *path);
unsigned platform_pid(void);

// full path of the running odin binary. false if the os won't say
bool platform_self_path(char *buf, size_t n);
// swaps the running binary for new bytes. on failure err says why (eg needs sudo)
bool platform_replace_self(const char *path, const unsigned char *data, size_t n, char *err, size_t errlen);

#endif

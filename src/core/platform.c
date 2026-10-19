#include "core/platform.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <windows.h>

static bool vt_ok = true;

void platform_init(void) {
    // block chars + runes need utf-8, and the console needs telling
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    DWORD handles[] = {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    for (int i = 0; i < 2; i++) {
        HANDLE h = GetStdHandle(handles[i]);
        DWORD mode;
        if (h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode)) continue; // redirected, that's fine
        // old consoles can't do ansi colors, we just go plain there
        if (!SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) vt_ok = false;
    }
}

bool platform_is_tty(int fd) { return _isatty(fd) != 0; }
bool platform_colors_ok(void) { return vt_ok; }

void platform_pause_if_launched_from_explorer(void) {
    // when explorer starts us, we're the only process attached to the console
    DWORD procs[2];
    if (GetConsoleProcessList(procs, 2) == 1) {
        fputs("\npress enter to close (tip: run odin from a terminal so it sticks around)\n", stdout);
        fflush(stdout);
        getchar();
    }
}

int stat_nofollow(const char *path, struct stat *st) { return stat(path, st); }
bool is_path_sep(char c) { return c == '/' || c == '\\'; }
unsigned platform_pid(void) { return (unsigned)_getpid(); }

#else
#include <unistd.h>

void platform_init(void) {}
bool platform_is_tty(int fd) { return isatty(fd) != 0; }
bool platform_colors_ok(void) { return true; }
void platform_pause_if_launched_from_explorer(void) {}
int stat_nofollow(const char *path, struct stat *st) { return lstat(path, st); }
bool is_path_sep(char c) { return c == '/'; }
unsigned platform_pid(void) { return (unsigned)getpid(); }
#endif

const char *path_base(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (is_path_sep(*p)) base = p + 1;
    return base;
}

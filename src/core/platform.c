#include "core/platform.h"

volatile int odin_interrupted;

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <windows.h>

static bool vt_ok = true;

void platform_init(void) {
    // leftover from the last `odin upgrade`, it couldn't delete itself while running
    char self[MAX_PATH + 8];
    DWORD got = GetModuleFileNameA(NULL, self, MAX_PATH);
    if (got > 0 && got < MAX_PATH) {
        strcat(self, ".old");
        DeleteFileA(self);
    }

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

static BOOL WINAPI on_ctrl(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
    if (odin_interrupted) ExitProcess(130); // second press, they really mean it
    odin_interrupted = 1;
    return TRUE;
}

void platform_catch_interrupts(void) { SetConsoleCtrlHandler(on_ctrl, TRUE); }
uint64_t platform_now_ms(void) { return GetTickCount64(); }

const char *platform_home(void) {
    const char *h = getenv("USERPROFILE");
    return h && *h ? h : NULL;
}

int stat_nofollow(const char *path, struct stat *st) { return stat(path, st); }
bool is_path_sep(char c) { return c == '/' || c == '\\'; }
unsigned platform_pid(void) { return (unsigned)_getpid(); }

bool platform_self_path(char *buf, size_t n) {
    DWORD got = GetModuleFileNameA(NULL, buf, (DWORD)n);
    return got > 0 && got < n;
}

// a running exe can't be overwritten but it can be renamed, so: old -> .old, new -> old.
// the .old file gets swept up next launch (see platform_init)
bool platform_replace_self(const char *path, const unsigned char *data, size_t n, char *err, size_t errlen) {
    char tmp[MAX_PATH + 8], old[MAX_PATH + 8];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    snprintf(old, sizeof old, "%s.old", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        snprintf(err, errlen, "can't write next to %s (try running the terminal as administrator)", path);
        return false;
    }
    bool ok = fwrite(data, 1, n, f) == n;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        remove(tmp);
        snprintf(err, errlen, "couldn't save the new version (disk full?)");
        return false;
    }
    DeleteFileA(old);
    if (!MoveFileExA(path, old, MOVEFILE_REPLACE_EXISTING)) {
        remove(tmp);
        snprintf(err, errlen, "couldn't move the old odin.exe out of the way");
        return false;
    }
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExA(old, path, MOVEFILE_REPLACE_EXISTING); // put the old one back
        remove(tmp);
        snprintf(err, errlen, "couldn't put the new odin.exe in place, kept the old one");
        return false;
    }
    return true;
}

#else
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

void platform_init(void) {}
bool platform_is_tty(int fd) { return isatty(fd) != 0; }
bool platform_colors_ok(void) { return true; }
static void on_sigint(int sig) {
    (void)sig;
    if (odin_interrupted) _exit(130); // second press, they really mean it
    odin_interrupted = 1;
}

void platform_catch_interrupts(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
}

uint64_t platform_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

const char *platform_home(void) {
    const char *h = getenv("HOME");
    if (h && *h) return h;
    struct passwd *pw = getpwuid(getuid());
    return pw && pw->pw_dir ? pw->pw_dir : NULL;
}
int stat_nofollow(const char *path, struct stat *st) { return lstat(path, st); }
bool is_path_sep(char c) { return c == '/'; }
unsigned platform_pid(void) { return (unsigned)getpid(); }

bool platform_self_path(char *buf, size_t n) {
#ifdef __APPLE__
    char raw[4096];
    uint32_t rs = sizeof raw;
    if (_NSGetExecutablePath(raw, &rs) != 0) return false;
    char *real = realpath(raw, NULL); // resolves symlinks like homebrew's
#else
    char *real = realpath("/proc/self/exe", NULL);
#endif
    if (!real) return false;
    bool ok = strlen(real) < n;
    if (ok) strcpy(buf, real);
    free(real);
    return ok;
}

// write the new binary next to the old one, then rename over it. rename is atomic,
// so a crash halfway leaves either the old odin or the new one, never half of each
bool platform_replace_self(const char *path, const unsigned char *data, size_t n, char *err, size_t errlen) {
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) {
        if (errno == EACCES || errno == EPERM || errno == EROFS)
            snprintf(err, errlen, "no permission to write to %s, run: sudo odin upgrade", path);
        else
            snprintf(err, errlen, "can't write next to %s: %s", path, strerror(errno));
        return false;
    }
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(fd, data + done, n - done);
        if (w <= 0) break;
        done += (size_t)w;
    }
    bool ok = done == n && fchmod(fd, 0755) == 0;
    ok = close(fd) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        snprintf(err, errlen, "couldn't swap in the new version: %s", strerror(errno));
        return false;
    }
    return true;
}
#endif

const char *path_base(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (is_path_sep(*p)) base = p + 1;
    return base;
}

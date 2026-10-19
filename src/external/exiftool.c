#include "external/exiftool.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/util.h"

#ifdef _WIN32
#include <windows.h>

bool have_exiftool(void) {
    char buf[MAX_PATH];
    return SearchPathA(NULL, "exiftool.exe", NULL, sizeof buf, buf, NULL) > 0;
}

// windows command lines are one string, so the filename has to be quoted the way
// CommandLineToArgvW expects: backslashes only get doubled right before a quote
static void quote_arg(sbuf *o, const char *a) {
    sb_putc(o, '"');
    for (const char *p = a;; p++) {
        size_t slashes = 0;
        while (*p == '\\') slashes++, p++;
        if (!*p) {
            for (size_t i = 0; i < slashes * 2; i++) sb_putc(o, '\\');
            break;
        }
        if (*p == '"') {
            for (size_t i = 0; i < slashes * 2 + 1; i++) sb_putc(o, '\\');
        } else {
            for (size_t i = 0; i < slashes; i++) sb_putc(o, '\\');
        }
        sb_putc(o, *p);
    }
    sb_putc(o, '"');
}

static char *capture(const char *file, size_t *len) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return NULL;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0); // only the child gets the write end

    sbuf cmd;
    sb_init(&cmd);
    sb_puts(&cmd, "exiftool.exe -j -G1 -a -s -c \"%+.6f\" -api largefilesupport=1 -- ");
    quote_arg(&cmd, file);

    HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    STARTUPINFOA si = {0};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = nul; // exiftool's warnings are noise here
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessA(NULL, cmd.s, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    sb_free(&cmd);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        return NULL;
    }

    sbuf out;
    sb_init(&out);
    char buf[8192];
    DWORD n;
    while (ReadFile(rd, buf, sizeof buf, &n, NULL) && n > 0) sb_putn(&out, buf, n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess), CloseHandle(pi.hThread);
    *len = out.len;
    return sb_take(&out);
}

#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

bool have_exiftool(void) {
    const char *path = getenv("PATH");
    if (!path) return false;
    char *dirs = xstrdup(path), *save = NULL;
    bool found = false;
    for (char *d = strtok_r(dirs, ":", &save); d && !found; d = strtok_r(NULL, ":", &save)) {
        char *full = xprintf("%s/exiftool", d);
        found = access(full, X_OK) == 0;
        free(full);
    }
    free(dirs);
    return found;
}

// runs exiftool with no shell in between, so filenames can't inject anything
static char *capture(const char *file, size_t *len) {
    int fd[2];
    if (pipe(fd)) return NULL;
    pid_t pid = fork();
    if (pid < 0) return NULL;
    if (pid == 0) {
        dup2(fd[1], STDOUT_FILENO);
        close(fd[0]), close(fd[1]);
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) dup2(null, STDERR_FILENO); // exiftool's warnings are noise here
        execlp("exiftool", "exiftool", "-j", "-G1", "-a", "-s", "-c", "%+.6f", "-api", "largefilesupport=1", "--",
               file, (char *)NULL);
        _exit(127);
    }
    close(fd[1]);
    sbuf out;
    sb_init(&out);
    char buf[8192];
    ssize_t n;
    while ((n = read(fd[0], buf, sizeof buf)) > 0) sb_putn(&out, buf, (size_t)n);
    close(fd[0]);
    waitpid(pid, NULL, 0);
    *len = out.len;
    return sb_take(&out);
}
#endif

// tiny json reader, just enough for exiftool's flat [{"Group:Tag": value, ...}]
typedef struct {
    const char *p, *end;
} jp;

static void ws(jp *j) {
    while (j->p < j->end && isspace((unsigned char)*j->p)) j->p++;
}

static char *jstring(jp *j) {
    if (j->p >= j->end || *j->p != '"') return NULL;
    j->p++;
    sbuf o;
    sb_init(&o);
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c != '\\' || j->p >= j->end) {
            sb_putc(&o, c);
            continue;
        }
        char e = *j->p++;
        switch (e) {
        case 'n': sb_putc(&o, '\n'); break;
        case 't': sb_putc(&o, '\t'); break;
        case 'r': sb_putc(&o, '\r'); break;
        case 'b': sb_putc(&o, '\b'); break;
        case 'f': sb_putc(&o, '\f'); break;
        case 'u': {
            if (j->end - j->p < 4) break;
            char hex[5] = {j->p[0], j->p[1], j->p[2], j->p[3], 0};
            j->p += 4;
            uint32_t cp = (uint32_t)strtoul(hex, NULL, 16);
            if (cp >= 0xD800 && cp <= 0xDBFF && j->end - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {
                char lo[5] = {j->p[2], j->p[3], j->p[4], j->p[5], 0};
                uint32_t l = (uint32_t)strtoul(lo, NULL, 16);
                if (l >= 0xDC00 && l <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (l - 0xDC00), j->p += 6;
            }
            sb_rune(&o, cp);
            break;
        }
        default: sb_putc(&o, e);
        }
    }
    j->p++; // closing quote
    return sb_take(&o);
}

static char *jvalue(jp *j, int depth) {
    ws(j);
    if (j->p >= j->end || depth > 8) return NULL;
    if (*j->p == '"') return jstring(j);
    if (*j->p == '[' || *j->p == '{') {
        // arrays get joined with commas, nested objects get skipped
        bool arr = *j->p == '[';
        char close = arr ? ']' : '}';
        j->p++;
        sbuf o;
        sb_init(&o);
        for (;;) {
            ws(j);
            if (j->p >= j->end) break;
            if (*j->p == close) {
                j->p++;
                break;
            }
            if (*j->p == ',') {
                j->p++;
                continue;
            }
            if (!arr) {
                free(jstring(j));
                ws(j);
                if (j->p < j->end && *j->p == ':') j->p++;
            }
            char *v = jvalue(j, depth + 1);
            if (!v) break;
            if (arr) {
                if (o.len) sb_puts(&o, ", ");
                sb_puts(&o, v);
            }
            free(v);
        }
        return sb_take(&o);
    }
    // number / true / false / null, keep the raw text
    const char *s = j->p;
    while (j->p < j->end && !strchr(",]} \t\r\n", *j->p)) j->p++;
    return j->p > s ? xstrndup(s, (size_t)(j->p - s)) : NULL;
}

// "+40.446195" or "40.446195 N"
static bool signed_coord(const char *s, double *out) {
    if (!s || !*s) return false;
    char *e;
    double v = strtod(s, &e);
    if (e == s) return false;
    if (strchr(e, 'S') || strchr(e, 'W')) v = -v;
    *out = v;
    return true;
}

bool run_exiftool(result_t *r) {
    size_t len;
    char *out = capture(r->path, &len);
    if (!out || !len) {
        free(out);
        result_errf(r, "exiftool: no output");
        return false;
    }
    jp j = {out, out + len};
    ws(&j);
    if (j.p >= j.end || *j.p != '[') goto bad;
    j.p++;
    ws(&j);
    if (j.p >= j.end || *j.p != '{') goto bad;
    j.p++;

    // swap our fields for exiftool's, except the stuff exiftool doesn't dig up:
    // photos embedded inside docs, file links in .rels, excel's saved path, pdf save count
    size_t keep = 0;
    for (size_t i = 0; i < r->nfields; i++) {
        field_t *f = &r->fields[i];
        bool ours = strstr(f->group, " › ") || !strcmp(f->key, "ExternalLink") || !strcmp(f->key, "AbsPath") ||
                    !strcmp(f->key, "IncrementalUpdates");
        if (ours) r->fields[keep++] = *f;
        else free(f->group), free(f->key), free(f->value);
    }
    r->nfields = keep;
    size_t native = keep;

    char *lat = NULL, *lon = NULL;
    for (;;) {
        ws(&j);
        if (j.p >= j.end || *j.p == '}') break;
        if (*j.p == ',') {
            j.p++;
            continue;
        }
        char *k = jstring(&j);
        if (!k) break;
        ws(&j);
        if (j.p < j.end && *j.p == ':') j.p++;
        char *v = jvalue(&j, 0);
        char *colon = strchr(k, ':');
        if (v && colon) {
            *colon = 0;
            const char *group = k, *tag = colon + 1;
            if (strcmp(group, "ExifTool") && strcmp(group, "System")) result_add(r, group, tag, v);
            if (!strcmp(group, "Composite") && !strcmp(tag, "GPSLatitude")) free(lat), lat = xstrdup(v);
            if (!strcmp(group, "Composite") && !strcmp(tag, "GPSLongitude")) free(lon), lon = xstrdup(v);
        }
        free(k), free(v);
    }
    // rotate so exiftool's fields come first and our extras trail after
    if (native && native < r->nfields) {
        field_t *tmp = xmalloc(sizeof(field_t) * r->nfields);
        memcpy(tmp, r->fields + native, sizeof(field_t) * (r->nfields - native));
        memcpy(tmp + (r->nfields - native), r->fields, sizeof(field_t) * native);
        memcpy(r->fields, tmp, sizeof(field_t) * r->nfields);
        free(tmp);
    }
    double la, lo;
    if (signed_coord(lat, &la) && signed_coord(lon, &lo)) result_add_gps(r, la, lo, "exiftool");
    free(lat), free(lon), free(out);
    r->engine = "exiftool";
    return true;

bad:
    free(out);
    result_errf(r, "exiftool: weird output");
    return false;
}

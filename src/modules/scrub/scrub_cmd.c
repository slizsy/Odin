// odin scrub: makes metadata-free copies of photos (or cleans them in place with -i)
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "core/extract.h"
#include "core/platform.h"
#include "core/scan.h"
#include "core/util.h"
#include "modules/scrub/scrub_cmd.h"
#include "modules/usage.h"
#include "scrub/scrub.h"
#include "ui/banner.h"
#include "ui/color.h"

#define MAX_READ (512LL << 20)

typedef struct {
    bool recursive, in_place, quiet;
} scrub_flags;

static int set_flag(scrub_flags *fl, const char *f) {
    if (!strcmp(f, "-r") || !strcmp(f, "--recursive")) fl->recursive = true;
    else if (!strcmp(f, "-i") || !strcmp(f, "--in-place")) fl->in_place = true;
    else if (!strcmp(f, "-q") || !strcmp(f, "--quiet")) fl->quiet = true;
    else if (!strcmp(f, "--no-color")) color_on = false;
    else if (!strcmp(f, "-h") || !strcmp(f, "--help")) return 1;
    else {
        fprintf(stderr, "odin: unknown flag %s\n", f);
        return 2;
    }
    return 0;
}

// IMG_1930.HEIC -> IMG_1930.clean.HEIC (dot in a folder name doesn't count)
static char *clean_name(const char *path) {
    const char *base = path_base(path);
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base) return xprintf("%s.clean", path);
    return xprintf("%.*s.clean%s", (int)(dot - path), path, dot);
}

static bool is_clean_copy(const char *path) { return strstr(path_base(path), ".clean") != NULL; }

// write to a temp file next to it, then swap it in, so a crash never leaves half a photo
static bool write_atomic(const char *path, const unsigned char *b, size_t n, char *err, size_t el) {
    char *tmp = xprintf("%s.odin-tmp-%u", path, platform_pid());
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        snprintf(err, el, "%s", strerror(errno));
        free(tmp);
        return false;
    }
    bool ok = fwrite(b, 1, n, f) == n;
    ok = fclose(f) == 0 && ok;
#ifdef _WIN32
    if (ok) remove(path); // windows rename won't overwrite
#endif
    if (!ok || rename(tmp, path) != 0) {
        snprintf(err, el, "%s", strerror(errno));
        remove(tmp);
        free(tmp);
        return false;
    }
    free(tmp);
    return true;
}

// double check our own work with the normal metadata reader. returns how many fields
// survived (orientation doesn't count, we keep that on purpose)
static size_t leftovers(const unsigned char *b, size_t n, size_t *gps) {
    result_t *r = result_new("scrubbed");
    extract_bytes(r, b, n);
    size_t left = 0;
    for (size_t i = 0; i < r->nfields; i++)
        if (strcmp(r->fields[i].key, "Orientation")) left++;
    *gps = r->ngps;
    result_free(r);
    return left;
}

typedef enum { DONE, SKIPPED, FAILED } outcome;

static outcome scrub_one(const char *path, const scrub_flags *fl, bool explicit_file) {
    const char *name = path_base(path);
    struct stat st;
    if (stat(path, &st) != 0) {
        printf("%s %s  %s\n", c_red("✗"), name, strerror(errno));
        return FAILED;
    }
    if (st.st_size > MAX_READ) {
        printf("%s %s  %s\n", c_yellow("·"), name, c_gray("skipped, too big"));
        return SKIPPED;
    }
    size_t n;
    unsigned char *b = read_file(path, &n);
    if (!b) {
        printf("%s %s  %s\n", c_red("✗"), name, strerror(errno));
        return FAILED;
    }

    unsigned char *out;
    size_t out_len;
    scrub_info info;
    char err[256];
    scrub_rc rc = scrub_bytes(b, n, &out, &out_len, &info, err, sizeof err);
    free(b);
    if (rc == SCRUB_UNSUPPORTED) {
        if (!explicit_file) return SKIPPED; // folder full of random stuff, don't spam
        printf("%s %s  %s\n", c_yellow("·"), name, c_gray("skipped, only jpeg / png / webp / heic / avif for now"));
        return SKIPPED;
    }
    if (rc == SCRUB_BROKEN) {
        printf("%s %s  %s\n", c_red("✗"), name, err);
        return FAILED;
    }

    // paranoia: if the reader still finds gps or real fields, don't hand it over as clean
    size_t gps, left = leftovers(out, out_len, &gps);
    if (gps || left) {
        printf("%s %s  %s\n", c_red("✗"), name, "still had metadata after scrubbing, left it alone (please report this)");
        free(out);
        return FAILED;
    }

    char *dest = fl->in_place ? xstrdup(path) : clean_name(path);
    char werr[256];
    bool ok = write_atomic(dest, out, out_len, werr, sizeof werr);
    free(out);
    if (!ok) {
        printf("%s %s  couldn't write %s: %s\n", c_red("✗"), name, path_base(dest), werr);
        free(dest);
        return FAILED;
    }

    char what[220];
    if (info.removed) snprintf(what, sizeof what, "removed %s", info.what);
    else snprintf(what, sizeof what, "was already clean");
    if (info.orientation) strncat(what, ", kept rotation", sizeof what - strlen(what) - 1);
    if (fl->in_place) printf("%s %s  %s\n", c_cyan("✓"), name, c_gray(what));
    else printf("%s %s → %s  %s\n", c_cyan("✓"), name, path_base(dest), c_gray(what));
    free(dest);
    return DONE;
}

int run_scrub(int argc, char **argv) {
    scrub_flags fl = {0};
    char **paths = calloc((size_t)argc + 1, sizeof *paths);
    int npaths = 0;

    bool only_paths = false;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!only_paths && !strcmp(a, "--")) {
            only_paths = true;
            continue;
        }
        if (only_paths || a[0] != '-' || !a[1]) {
            paths[npaths++] = argv[i];
            continue;
        }
        int rc = 0;
        if (a[1] == '-') {
            rc = set_flag(&fl, a);
        } else {
            for (const char *c = a + 1; *c && !rc; c++) { // -ri works
                char one[3] = {'-', *c, 0};
                rc = set_flag(&fl, one);
            }
        }
        if (rc) {
            if (rc == 1) fputs(usage, stdout);
            free(paths);
            return rc == 1 ? 0 : 2;
        }
    }
    if (!npaths) {
        fprintf(stderr, "odin: give me at least one photo or folder\n\n%s", usage);
        free(paths);
        return 2;
    }

    scan_list files = {0};
    scan_paths(&files, paths, npaths, fl.recursive);
    free(paths);
    if (!files.count) {
        fputs("odin: no files to scrub\n", stderr);
        return 1;
    }
    if (!fl.quiet) banner_print();

    int done = 0, skipped = 0, failed = 0;
    for (size_t i = 0; i < files.count && !odin_interrupted; i++) {
        const scan_item *it = &files.items[i];
        // folder runs: don't scrub the copies we made last time, and stay quiet about
        // stuff that isn't a photo
        if (!it->explicit_file && !fl.in_place && is_clean_copy(it->path)) continue;
        outcome o = scrub_one(it->path, &fl, it->explicit_file);
        if (o == DONE) done++;
        else if (o == SKIPPED) skipped++;
        else failed++;
    }
    scan_free(&files);

    if (done + skipped + failed > 1) {
        printf("%s %d scrubbed", c_gray("──"), done);
        if (skipped) printf("%s%d skipped", c_gray(" · "), skipped);
        if (failed) {
            char f[32];
            snprintf(f, sizeof f, "%d failed", failed);
            printf("%s%s", c_gray(" · "), c_red(f));
        }
        putchar('\n');
    }
    if (done && !fl.in_place)
        printf("%s\n", c_gray("originals untouched. use -i to clean them in place instead"));
    return failed ? 1 : 0;
}

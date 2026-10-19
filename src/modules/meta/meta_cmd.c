#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/extract.h"
#include "core/scan.h"
#include "external/exiftool.h"
#include "output/output.h"
#include "modules/meta/meta_cmd.h"
#include "modules/usage.h"
#include "ui/banner.h"
#include "ui/color.h"

typedef struct {
    bool recursive, findings, json, exiftool, quiet;
} meta_flags;

static int set_flag(meta_flags *fl, const char *f) {
    if (!strcmp(f, "-r") || !strcmp(f, "--recursive")) fl->recursive = true;
    else if (!strcmp(f, "-f") || !strcmp(f, "--findings")) fl->findings = true;
    else if (!strcmp(f, "-j") || !strcmp(f, "--json")) fl->json = true;
    else if (!strcmp(f, "-x") || !strcmp(f, "--exiftool")) fl->exiftool = true;
    else if (!strcmp(f, "-q") || !strcmp(f, "--quiet")) fl->quiet = true;
    else if (!strcmp(f, "--no-color")) color_on = false;
    else if (!strcmp(f, "-h") || !strcmp(f, "--help")) return 1;
    else {
        fprintf(stderr, "odin: unknown flag %s\n", f);
        return 2;
    }
    return 0;
}

int run_meta(int argc, char **argv) {
    meta_flags fl = {0};
    char **paths = calloc((size_t)argc + 1, sizeof *paths);
    int npaths = 0;

    bool only_paths = false;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!only_paths && !strcmp(a, "--")) {
            only_paths = true; // everything after -- is a file, even if it starts with a dash
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
            // let people smash short flags together like -rf
            for (const char *c = a + 1; *c && !rc; c++) {
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
    if (fl.json) color_on = false;
    if (!npaths) {
        fprintf(stderr, "odin: give me at least one file or folder\n\n%s", usage);
        free(paths);
        return 2;
    }
    if (fl.exiftool && !have_exiftool()) {
        fputs("odin: -x needs exiftool on your PATH (exiftool.org), using the native parser instead\n", stderr);
        fl.exiftool = false;
    }

    scan_list files = {0};
    scan_paths(&files, paths, npaths, fl.recursive);
    free(paths);
    if (!files.count) {
        fputs("odin: no files to scan\n", stderr);
        return 1;
    }

    if (!fl.json && !fl.quiet) banner_print();
    if (fl.json) json_begin();

    int skipped = 0, high = 0, med = 0, errors = 0;
    extract_opts opt = {fl.exiftool};
    // one file at a time, printed as we go so huge folders don't eat ram
    for (size_t i = 0; i < files.count; i++) {
        result_t *r = extract_file(files.items[i].path, opt);
        if (fl.json) {
            json_result(r);
        } else if (!files.items[i].explicit_file && !r->nfields && !r->ngps && !r->nerrors) {
            skipped++; // folder scans skip files with nothing in them
        } else {
            print_pretty(r, fl.findings);
            for (size_t k = 0; k < r->nfindings; k++) {
                if (r->findings[k].level == LVL_HIGH) high++;
                if (r->findings[k].level == LVL_MED) med++;
            }
            errors += (int)r->nerrors;
        }
        result_free(r);
    }

    if (fl.json) json_end();
    else if (files.count > 1) print_summary((int)files.count, skipped, high, med, errors);
    scan_free(&files);
    return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/extract.h"
#include "core/scan.h"
#include "external/exiftool.h"
#include "output/output.h"
#include "ui/banner.h"
#include "ui/color.h"

static const char *usage =
    "usage: odin <command> [flags] [args]\n"
    "\n"
    "commands:\n"
    "  meta <path...>       rip metadata out of files and folders\n"
    "  help                 this screen\n"
    "  version              print the version\n"
    "\n"
    "meta flags:\n"
    "  -r, --recursive      walk folders all the way down\n"
    "  -f, --findings       only show findings, skip the raw field dump\n"
    "  -j, --json           json output (pipe it into jq or whatever)\n"
    "  -x, --exiftool       full dump through exiftool if it's installed\n"
    "  -q, --quiet          skip the banner\n"
    "      --no-color       plain text\n"
    "\n"
    "reads: jpeg, png, webp, tiff + camera raws, pdf, docx/xlsx/pptx,\n"
    "       odt/ods/odp, plus xmp packets hiding in pretty much anything\n"
    "\n"
    "coming soon: user, email, domain, phone, ip\n"
    "\n"
    "examples:\n"
    "  odin meta photo.jpg\n"
    "  odin meta -r ~/Downloads -f\n"
    "  odin meta report.pdf --json | jq '.[0].findings'\n";

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

static int run_meta(int argc, char **argv) {
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

int main(int argc, char **argv) {
    color_init();
    if (argc < 2 || !strcmp(argv[1], "help") || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        banner_print();
        fputs(usage, stdout);
        return 0;
    }
    const char *cmd = argv[1];
    if (!strcmp(cmd, "meta") || !strcmp(cmd, "m")) return run_meta(argc - 2, argv + 2);
    if (!strcmp(cmd, "version") || !strcmp(cmd, "-v") || !strcmp(cmd, "--version")) {
        puts("odin " ODIN_VERSION);
        return 0;
    }
    const char *soon[] = {"user", "email", "domain", "phone", "ip", NULL};
    for (int i = 0; soon[i]; i++) {
        if (!strcmp(cmd, soon[i])) {
            fprintf(stderr, "odin: the %s module isn't built yet, metadata comes first\n", cmd);
            return 1;
        }
    }
    fprintf(stderr, "odin: no idea what \"%s\" is\n\n%s", cmd, usage);
    return 2;
}

#include <stdio.h>
#include <string.h>

#include "core/util.h"
#include "output/output.h"
#include "ui/color.h"

#define BAR c_gray("│")

static const char *level_tag(level_t l) {
    switch (l) {
    case LVL_HIGH: return c_red("HIGH");
    case LVL_MED: return c_yellow("MED ");
    default: return c_cyan("INFO");
    }
}

static void print_fields(const result_t *r) {
    // groups print in the order they first showed up
    for (size_t i = 0; i < r->nfields; i++) {
        const char *g = r->fields[i].group;
        bool seen = false;
        for (size_t j = 0; j < i && !seen; j++) seen = !strcmp(r->fields[j].group, g);
        if (seen) continue;

        size_t width = 0;
        for (size_t j = i; j < r->nfields; j++) {
            size_t w = utf8_width(r->fields[j].key);
            if (!strcmp(r->fields[j].group, g) && w > width) width = w;
        }
        if (width > 28) width = 28;

        printf("%s\n%s  %s\n", BAR, BAR, c256(160, g));
        for (size_t j = i; j < r->nfields; j++) {
            const field_t *f = &r->fields[j];
            if (strcmp(f->group, g)) continue;
            size_t w = utf8_width(f->key);
            printf("%s   %s%*s  %s\n", BAR, c_gray(f->key), (int)(width > w ? width - w : 0), "", f->value);
        }
    }
}

void print_pretty(const result_t *r, bool findings_only) {
    printf("%s %s\n", c_gray("┌─"), c_bold(r->path));

    char size[32], when[32] = "", line[256];
    human_size(r->size, size, sizeof size);
    if (r->mtime) strftime(when, sizeof when, " · modified %Y-%m-%d %H:%M", localtime(&r->mtime));
    snprintf(line, sizeof line, "%s · %s%s%s", r->type, size, when, strcmp(r->engine, "native") ? " · via exiftool" : "");
    printf("%s  %s\n", BAR, c_gray(line));
    if (*r->sha256) {
        snprintf(line, sizeof line, "sha256 %s", r->sha256);
        printf("%s  %s\n", BAR, c_gray(line));
    }

    if (r->nfindings) {
        printf("%s\n%s  %s\n", BAR, BAR, c_bold("findings"));
        for (size_t i = 0; i < r->nfindings; i++) {
            const finding_t *f = &r->findings[i];
            // say where it came from when it's buried inside another file
            char from[600] = "";
            const char *arrow = strstr(f->source, " › ");
            if (arrow) {
                char tmp[600];
                snprintf(tmp, sizeof tmp, "  ← %.*s", (int)(arrow - f->source), f->source);
                snprintf(from, sizeof from, "%s", c_gray(tmp));
            }
            printf("%s   %s  %-13s %s%s\n", BAR, level_tag(f->level), f->kind, f->detail, from);
        }
    } else if (r->nfields) {
        printf("%s\n%s  %s\n", BAR, BAR, c_gray("nothing juicy, just the basics"));
    }

    if (!findings_only) print_fields(r);
    if (!r->nfields && !r->nerrors)
        printf("%s\n%s  %s\n", BAR, BAR, c_gray("no metadata found (clean file, or a format odin can't read yet, try -x)"));
    for (size_t i = 0; i < r->nerrors; i++) printf("%s  %s %s\n", BAR, c_yellow("!"), c_gray(r->errors[i]));
    printf("%s\n\n", c_gray("└─"));
}

void print_summary(int scanned, int skipped, int high, int med, int errors) {
    char a[64], b[64], c[64];
    snprintf(a, sizeof a, "%d high", high);
    snprintf(b, sizeof b, "%d medium", med);
    printf("%s %d files scanned", c_gray("──"), scanned);
    if (skipped) printf("%s%d had nothing", c_gray(" · "), skipped);
    printf("%s%s%s%s", c_gray(" · "), c_red(a), c_gray(" · "), c_yellow(b));
    if (errors) {
        snprintf(c, sizeof c, "%d errors", errors);
        printf("%s%s", c_gray(" · "), c);
    }
    putchar('\n');
}

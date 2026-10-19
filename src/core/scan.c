#include "core/scan.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "core/platform.h"
#include "core/util.h"

static void push(scan_list *l, const char *path, bool explicit_file) {
    for (size_t i = 0; i < l->count; i++)
        if (!strcmp(l->items[i].path, path)) return;
    VEC_PUSH(l->items, l->count, l->cap);
    l->items[l->count - 1] = (scan_item){xstrdup(path), explicit_file};
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void walk(scan_list *l, const char *dir, bool recursive) {
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "odin: %s: %s\n", dir, strerror(errno));
        return;
    }
    // readdir order is random, sort so output is the same every run
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue; // skips . .. and hidden stuff like .git
        VEC_PUSH(names, n, cap);
        names[n - 1] = xstrdup(e->d_name);
    }
    closedir(d);
    if (n) qsort(names, n, sizeof *names, by_name);

    for (size_t i = 0; i < n; i++) {
        size_t dl = strlen(dir);
        char *full = dl && is_path_sep(dir[dl - 1]) ? xprintf("%s%s", dir, names[i]) : xprintf("%s/%s", dir, names[i]);
        struct stat st;
        // don't follow symlinks so they can't send us in circles
        if (stat_nofollow(full, &st) == 0) {
            if (S_ISREG(st.st_mode)) push(l, full, false);
            else if (S_ISDIR(st.st_mode) && recursive) walk(l, full, true);
        }
        free(full), free(names[i]);
    }
    free(names);
}

void scan_paths(scan_list *out, char **paths, int npaths, bool recursive) {
    for (int i = 0; i < npaths; i++) {
        struct stat st;
        if (stat(paths[i], &st) != 0) {
            fprintf(stderr, "odin: %s: %s\n", paths[i], strerror(errno));
            continue;
        }
        if (S_ISDIR(st.st_mode)) walk(out, paths[i], recursive);
        else push(out, paths[i], true);
    }
}

void scan_free(scan_list *l) {
    for (size_t i = 0; i < l->count; i++) free(l->items[i].path);
    free(l->items);
    l->items = NULL, l->count = l->cap = 0;
}

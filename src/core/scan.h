#ifndef ODIN_SCAN_H
#define ODIN_SCAN_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *path;
    bool explicit_file; // the user named it directly (vs found it in a folder)
} scan_item;

typedef struct {
    scan_item *items;
    size_t count, cap;
} scan_list;

// expands folders into files. without recursive it only takes a folder's top level
void scan_paths(scan_list *out, char **paths, int npaths, bool recursive);
void scan_free(scan_list *l);

#endif

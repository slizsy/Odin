#ifndef ODIN_SITES_H
#define ODIN_SITES_H

#include <stdbool.h>
#include <stddef.h>

// how we tell "account exists" from "no such account"
typedef enum {
    CHECK_STATUS,  // 2xx = found, 404/410/400/204 = not found
    CHECK_ABSENT,  // 2xx and the page does NOT contain text = found
    CHECK_PRESENT, // 2xx and the page DOES contain text = found
} check_kind;

typedef struct {
    char *name;
    char *probe;   // url we actually hit, {} = username
    char *profile; // url we show people (api urls are ugly), falls back to probe
    char *text;    // for absent/present checks
    char *claimed; // a username known to exist there, used by --selftest
    check_kind kind;
} site_t;

typedef struct {
    site_t *items;
    size_t count, cap;
} site_list;

void sites_load_default(site_list *l);
// returns false and fills err if the file is broken
bool sites_load_file(site_list *l, const char *path, char *err, size_t errlen);
void sites_free(site_list *l);

// swaps {} for the username. NULL if the name can't go there (eg dots in a subdomain)
char *site_url(const char *tmpl, const char *user);

#endif

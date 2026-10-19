#ifndef ODIN_CHECK_H
#define ODIN_CHECK_H

#include <stddef.h>

#include "modules/user/sites.h"

typedef enum { HIT_FOUND, HIT_MISSING, HIT_UNKNOWN } hit_state;

typedef struct {
    const site_t *site;
    const char *username;
    hit_state state;
    char *url;    // profile link to show
    char *detail; // why it's unknown (timeout, blocked, ...)
    long http;
    double secs;
} hit_t;

typedef struct {
    hit_t *hits;
    size_t count;
} hit_list;

typedef void (*progress_fn)(size_t done, size_t total);

// checks every site for every username, all in parallel
void check_all(const site_list *sites, char **users, int nusers, long timeout, progress_fn cb, hit_list *out);
// one username per site, names[i] goes with sites->items[i] (selftest uses this)
void check_pairs(const site_list *sites, char **names, long timeout, progress_fn cb, hit_list *out);
void hits_free(hit_list *h);

const char *hit_state_name(hit_state s);

#endif

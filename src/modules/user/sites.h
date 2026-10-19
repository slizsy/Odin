#ifndef ODIN_SITES_H
#define ODIN_SITES_H

#include <stdbool.h>
#include <stddef.h>

// how we tell "account exists" from "no such account"
typedef enum {
    CHECK_STATUS,  // 2xx = found, 404/410/204/400 = not found
    CHECK_MESSAGE, // 2xx, then: any `absent` text = not found, `present` text (if listed) needed for found
    CHECK_STRICT,  // whatsmyname style: found only on found_code + present text, not found only on
                   // missing_code + absent text, anything else = couldn't check
} check_kind;

// what a site is known to throw at bots (from maigret's data), shown when it blocks us
enum {
    PROT_IP = 1,      // blocks cloud / vpn ips
    PROT_BROWSER = 2, // wants a real browser (tls fingerprint / js challenge)
};

typedef struct {
    char **v;
    size_t n;
} strlist;

typedef struct {
    char *name;
    char *probe;   // url we actually hit, {} = username
    char *profile; // url we show people (api urls are ugly), falls back to probe
    check_kind kind;
    strlist present, absent;
    int found_code, missing_code; // CHECK_STRICT, 0 = any
    strlist blocked;              // pairs: page text, reason ("Rate limited" etc)
    strlist headers;              // "Name: value"
    char *post;                   // request body, POST when set
    char *regex;                  // username has to match or the site gets skipped
    char *strip;                  // characters to drop from the username first
    char *claimed;                // a username that exists there (--selftest)
    char *unclaimed;              // one that doesn't
    unsigned protect;
} site_t;

typedef struct {
    site_t *items;
    size_t count, cap;
} site_list;

void sites_load_default(site_list *l);
// text (name | url | check | profile | claimed), odin json, or whatsmyname's wmn-data.json.
// returns false and fills err if the file is broken
bool sites_load_file(site_list *l, const char *path, char *err, size_t errlen);
void sites_free(site_list *l);

// swaps {} for the username. NULL if the name can't go there (eg dots in a subdomain)
char *site_url(const char *tmpl, const char *user);
// the username as this site wants it (strip chars), malloc'd
char *site_username(const site_t *s, const char *user);
// does the site's username rule allow this name? (rules we can't read always allow)
bool site_accepts(const site_t *s, const char *user);

extern const char *const sites_builtin_json[]; // generated pieces, NULL ended, see tools/gen_sites.py
extern const char *const sites_builtin_credit;

#endif

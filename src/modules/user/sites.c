#include "modules/user/sites.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/json.h"
#include "core/regex.h"
#include "core/util.h"

static void push(strlist *l, const char *s) {
    l->v = xrealloc(l->v, (l->n + 1) * sizeof *l->v);
    l->v[l->n++] = xstrdup(s);
}

static void free_list(strlist *l) {
    for (size_t i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
}

static void free_site(site_t *s) {
    free(s->name), free(s->probe), free(s->profile), free(s->post), free(s->regex), free(s->strip);
    free(s->claimed), free(s->unclaimed);
    free_list(&s->present), free_list(&s->absent), free_list(&s->blocked), free_list(&s->headers);
}

// every project spells the placeholder differently, odin uses {}
static char *placeholder(const char *s) {
    if (!s) return NULL;
    sbuf o;
    sb_init(&o);
    for (const char *p = s; *p;) {
        if (!strncmp(p, "{account}", 9)) sb_puts(&o, "{}"), p += 9;
        else if (!strncmp(p, "{username}", 10)) sb_puts(&o, "{}"), p += 10;
        else sb_putc(&o, *p++);
    }
    return sb_take(&o);
}

static unsigned protection_bits(const char *p) {
    if (!strcmp(p, "ip_reputation")) return PROT_IP;
    static const char *const browser[] = {"tls_fingerprint", "cf_js_challenge", "js_challenge", "aws_waf_js_challenge",
                                          "ddos_guard_challenge", "cloudflare", "ddos-guard", "anubis", "captcha",
                                          "custom_bot_protection", "cf_firewall", NULL};
    for (int i = 0; browser[i]; i++)
        if (!strcmp(p, browser[i])) return PROT_BROWSER;
    return 0;
}

// a site needs a url with {} somewhere (or a post body with it) and a name
static const char *validate(site_t *s) {
    if (!s->name || !*s->name) return "missing site name";
    if (!s->probe) return "missing url";
    if (strncmp(s->probe, "http://", 7) && strncmp(s->probe, "https://", 8)) return "url has to start with http(s)://";
    if (!strstr(s->probe, "{}") && !(s->post && strstr(s->post, "{}"))) return "url has no {} for the username";
    if (!s->profile) s->profile = xstrdup(s->probe);
    return NULL;
}

static void add_site(site_list *l, site_t *s) {
    VEC_PUSH(l->items, l->count, l->cap);
    l->items[l->count - 1] = *s;
}

// ---- the simple text format: name | url | check | profile | claimed ----

static const char *parse_line(site_list *l, const char *line) {
    char *parts[5] = {0};
    int np = 0;
    const char *p = line;
    while (np < 5) {
        const char *bar = strchr(p, '|');
        size_t n = bar ? (size_t)(bar - p) : strlen(p);
        parts[np++] = trim_dup(p, n);
        if (!bar) break;
        p = bar + 1;
    }
    const char *err = NULL;
    site_t s = {0};
    if (np < 3) err = "need at least: name | url | check";
    if (!err) {
        const char *c = parts[2];
        if (!strcmp(c, "status")) s.kind = CHECK_STATUS;
        else if (!strncmp(c, "absent:", 7) && c[7]) s.kind = CHECK_MESSAGE, push(&s.absent, c + 7);
        else if (!strncmp(c, "present:", 8) && c[8]) s.kind = CHECK_MESSAGE, push(&s.present, c + 8);
        else err = "check must be status, absent:<text> or present:<text>";
    }
    if (!err) {
        s.name = xstrdup(parts[0]);
        s.probe = placeholder(parts[1]);
        s.profile = np > 3 && *parts[3] ? placeholder(parts[3]) : NULL;
        s.claimed = np > 4 && *parts[4] ? xstrdup(parts[4]) : NULL;
        err = validate(&s);
    }
    if (err) free_site(&s);
    else add_site(l, &s);
    for (int i = 0; i < np; i++) free(parts[i]);
    return err;
}

static bool load_text(site_list *l, char *text, const char *path, char *err, size_t errlen) {
    int lineno = 0;
    for (char *line = text; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        lineno++;
        char *t = trim_dup(line, strlen(line));
        if (*t && *t != '#') { // blank lines and # comments are fine
            const char *e = parse_line(l, t);
            if (e) {
                snprintf(err, errlen, "%s line %d: %s", path, lineno, e);
                free(t);
                return false;
            }
        }
        free(t);
        line = nl ? nl + 1 : NULL;
    }
    return true;
}

// ---- json: odin's own format, or whatsmyname's wmn-data.json ----

static char *jdup(const json *o, const char *key) {
    const char *v = json_str(o, key);
    return v && *v ? xstrdup(v) : NULL;
}

static void jlist(const json *o, const char *key, strlist *out) {
    json *a = json_get(o, key);
    if (a && a->t == J_STR && *a->s) push(out, a->s);
    for (size_t i = 0; a && a->t == J_ARR && i < a->len; i++)
        if (a->items[i]->t == J_STR && *a->items[i]->s) push(out, a->items[i]->s);
}

static void jheaders(const json *o, const char *key, strlist *out) {
    json *h = json_get(o, key);
    for (size_t i = 0; h && h->t == J_OBJ && i < h->len; i++) {
        if (h->items[i]->t != J_STR) continue;
        char *line = xprintf("%s: %s", h->keys[i], h->items[i]->s);
        push(out, line);
        free(line);
    }
}

static int jint(const json *o, const char *key) {
    json *v = json_get(o, key);
    return v && v->t == J_NUM ? (int)v->n : 0;
}

static unsigned jprotect(const json *o) {
    json *p = json_get(o, "protection");
    unsigned bits = 0;
    for (size_t i = 0; p && p->t == J_ARR && i < p->len; i++)
        if (p->items[i]->t == J_STR) bits |= protection_bits(p->items[i]->s);
    return bits;
}

static const char *from_odin(const json *j, site_t *s) {
    s->name = jdup(j, "name");
    s->probe = placeholder(json_str(j, "probe") ? json_str(j, "probe") : json_str(j, "url"));
    s->profile = json_str(j, "probe") ? placeholder(json_str(j, "url")) : NULL;
    const char *c = json_str(j, "check");
    s->kind = c && !strcmp(c, "strict") ? CHECK_STRICT : c && !strcmp(c, "message") ? CHECK_MESSAGE : CHECK_STATUS;
    jlist(j, "present", &s->present);
    jlist(j, "absent", &s->absent);
    s->found_code = jint(j, "found_code");
    s->missing_code = jint(j, "missing_code");
    json *b = json_get(j, "blocked");
    for (size_t i = 0; b && b->t == J_OBJ && i < b->len; i++)
        if (b->items[i]->t == J_STR) push(&s->blocked, b->keys[i]), push(&s->blocked, b->items[i]->s);
    jheaders(j, "headers", &s->headers);
    s->post = placeholder(json_str(j, "post"));
    s->regex = jdup(j, "regex");
    s->strip = jdup(j, "strip");
    s->claimed = jdup(j, "claimed");
    s->unclaimed = jdup(j, "unclaimed");
    s->protect = jprotect(j);
    return validate(s);
}

// whatsmyname: found = e_code + e_string, missing = m_code + m_string
static const char *from_wmn(const json *j, site_t *s) {
    s->name = jdup(j, "name");
    s->probe = placeholder(json_str(j, "uri_check"));
    s->profile = placeholder(json_str(j, "uri_pretty"));
    s->kind = CHECK_STRICT;
    jlist(j, "e_string", &s->present);
    jlist(j, "m_string", &s->absent);
    s->found_code = jint(j, "e_code");
    s->missing_code = jint(j, "m_code");
    jheaders(j, "headers", &s->headers);
    s->post = placeholder(json_str(j, "post_body"));
    s->strip = jdup(j, "strip_bad_char");
    json *known = json_get(j, "known");
    if (known && known->t == J_ARR && known->len && known->items[0]->t == J_STR) s->claimed = xstrdup(known->items[0]->s);
    s->protect = jprotect(j);
    return validate(s);
}

static bool load_json(site_list *l, const char *text, size_t n, const char *path, char *err, size_t errlen) {
    json *root = json_parse(text, n);
    json *sites = root ? json_get(root, "sites") : NULL;
    if (!sites || sites->t != J_ARR) {
        snprintf(err, errlen, "%s isn't a site list (wanted a json object with a \"sites\" array)", path);
        json_free(root);
        return false;
    }
    for (size_t i = 0; i < sites->len; i++) {
        json *j = sites->items[i];
        if (j->t != J_OBJ) continue;
        json *valid = json_get(j, "valid"); // whatsmyname marks dead entries "valid": false
        if (valid && valid->t == J_BOOL && !valid->b) continue;
        site_t s = {0};
        const char *e = json_get(j, "uri_check") ? from_wmn(j, &s) : from_odin(j, &s);
        if (e) {
            const char *nm = json_str(j, "name");
            snprintf(err, errlen, "%s site %zu (%s): %s", path, i + 1, nm ? nm : "?", e);
            free_site(&s);
            json_free(root);
            return false;
        }
        add_site(l, &s);
    }
    json_free(root);
    return true;
}

void sites_load_default(site_list *l) {
    sbuf all;
    sb_init(&all);
    for (int i = 0; sites_builtin_json[i]; i++) sb_puts(&all, sites_builtin_json[i]);
    char err[256];
    if (!load_json(l, all.s, all.len, "built in sites", err, sizeof err)) fprintf(stderr, "odin: %s\n", err);
    sb_free(&all);
}

bool sites_load_file(site_list *l, const char *path, char *err, size_t errlen) {
    size_t n;
    unsigned char *b = read_file(path, &n);
    if (!b) {
        snprintf(err, errlen, "can't read %s", path);
        return false;
    }
    const char *p = (const char *)b;
    while (*p && isspace((unsigned char)*p)) p++;
    bool ok = *p == '{' ? load_json(l, (const char *)b, n, path, err, errlen)
                        : load_text(l, (char *)b, path, err, errlen);
    free(b);
    if (ok && !l->count) {
        snprintf(err, errlen, "%s has no sites in it", path);
        ok = false;
    }
    return ok;
}

void sites_free(site_list *l) {
    for (size_t i = 0; i < l->count; i++) free_site(&l->items[i]);
    free(l->items);
    l->items = NULL, l->count = l->cap = 0;
}

char *site_username(const site_t *s, const char *user) {
    if (!s->strip) return xstrdup(user);
    sbuf o;
    sb_init(&o);
    for (const char *p = user; *p; p++)
        if (!strchr(s->strip, *p)) sb_putc(&o, *p);
    return sb_take(&o);
}

bool site_accepts(const site_t *s, const char *user) {
    if (!s->regex) return true;
    bool unsupported;
    return rx_search(s->regex, user, &unsupported);
}

char *site_url(const char *tmpl, const char *user) {
    const char *ph = strstr(tmpl, "{}");
    if (!ph) return xstrdup(tmpl); // username goes in the post body instead
    // is the {} inside the hostname? then only letters, digits and dashes make sense
    const char *host = strstr(tmpl, "://");
    host = host ? host + 3 : tmpl;
    const char *host_end = strchr(host, '/');
    bool in_host = ph >= host && (!host_end || ph < host_end);

    sbuf o;
    sb_init(&o);
    for (const char *p = tmpl; *p;) {
        if (p[0] != '{' || p[1] != '}') {
            sb_putc(&o, *p++);
            continue;
        }
        for (const unsigned char *u = (const unsigned char *)user; *u; u++) {
            if (isalnum(*u) || *u == '-' || (!in_host && strchr("._~", *u))) {
                sb_putc(&o, (char)*u);
            } else if (in_host) {
                sb_free(&o);
                return NULL;
            } else {
                sb_printf(&o, "%%%02X", *u); // url-encode everything else
            }
        }
        p += 2;
    }
    return sb_take(&o);
}

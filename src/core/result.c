#include "core/result.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/util.h"

result_t *result_new(const char *path) {
    result_t *r = calloc(1, sizeof *r);
    if (!r) exit(1);
    r->path = xstrdup(path);
    const char *slash = strrchr(path, '/');
    r->name = xstrdup(slash ? slash + 1 : path);
    r->type = xstrdup("unknown");
    r->engine = "native";
    return r;
}

void result_free(result_t *r) {
    if (!r) return;
    for (size_t i = 0; i < r->nfields; i++) free(r->fields[i].group), free(r->fields[i].key), free(r->fields[i].value);
    for (size_t i = 0; i < r->ngps; i++) free(r->gps[i].source);
    for (size_t i = 0; i < r->nfindings; i++)
        free(r->findings[i].kind), free(r->findings[i].detail), free(r->findings[i].source);
    for (size_t i = 0; i < r->nerrors; i++) free(r->errors[i]);
    free(r->fields), free(r->gps), free(r->findings), free(r->errors);
    free(r->path), free(r->name), free(r->type);
    free(r);
}

// decodes one utf8 char, returns its length or 0 if it's junk
static int utf8_dec(const unsigned char *s, size_t n, uint32_t *cp) {
    unsigned char c = s[0];
    int len;
    uint32_t v;
    if (c < 0x80) return *cp = c, 1;
    if ((c & 0xE0) == 0xC0) len = 2, v = c & 0x1F;
    else if ((c & 0xF0) == 0xE0) len = 3, v = c & 0x0F;
    else if ((c & 0xF8) == 0xF0) len = 4, v = c & 0x07;
    else return 0;
    if ((size_t)len > n) return 0;
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        v = v << 6 | (s[i] & 0x3F);
    }
    if ((len == 2 && v < 0x80) || (len == 3 && v < 0x800) || (len == 4 && (v < 0x10000 || v > 0x10FFFF))) return 0;
    if (v >= 0xD800 && v <= 0xDFFF) return 0;
    return *cp = v, len;
}

// strip control chars, squash whitespace, fix broken utf8 (read as latin1), cap at 500 chars
static char *clean(const char *in, size_t n) {
    const unsigned char *s = (const unsigned char *)in;
    sbuf o;
    sb_init(&o);
    bool space = false;
    size_t chars = 0;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        int len = utf8_dec(s + i, n - i, &cp);
        if (!len) cp = s[i], len = 1;
        i += (size_t)len;
        if (cp == 0xFFFD || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) continue;
        if (cp < 0x20 || cp == ' ' || cp == 0xA0) {
            if (cp == '\t' || cp == '\n' || cp == '\r' || cp == ' ' || cp == 0xA0) space = true;
            continue;
        }
        if (space && o.len) sb_putc(&o, ' ');
        space = false;
        if (++chars > 500) {
            sb_puts(&o, "…");
            break;
        }
        sb_rune(&o, cp);
    }
    return sb_take(&o);
}

void result_addn(result_t *r, const char *group, const char *key, const char *value, size_t n) {
    if (!key || !*key || !value) return;
    char *v = clean(value, n);
    if (!*v) {
        free(v);
        return;
    }
    for (size_t i = 0; i < r->nfields; i++) {
        field_t *f = &r->fields[i];
        if (!strcmp(f->value, v) && !strcmp(f->key, key) && !strcmp(f->group, group)) {
            free(v);
            return;
        }
    }
    VEC_PUSH(r->fields, r->nfields, r->capfields);
    r->fields[r->nfields - 1] = (field_t){xstrdup(group), xstrdup(key), v};
}

void result_add(result_t *r, const char *group, const char *key, const char *value) {
    if (value) result_addn(r, group, key, value, strlen(value));
}

void result_add_gps(result_t *r, double lat, double lon, const char *source) {
    if (isnan(lat) || isnan(lon) || fabs(lat) > 90 || fabs(lon) > 180) return;
    // 0,0 is a camera writing junk, not a selfie off the coast of africa
    if (lat == 0 && lon == 0) return;
    for (size_t i = 0; i < r->ngps; i++)
        if (fabs(r->gps[i].lat - lat) < 1e-6 && fabs(r->gps[i].lon - lon) < 1e-6) return;
    VEC_PUSH(r->gps, r->ngps, r->capgps);
    r->gps[r->ngps - 1] = (geo_t){lat, lon, xstrdup(source)};
}

void result_add_finding(result_t *r, level_t lvl, const char *kind, const char *detail, const char *source) {
    for (size_t i = 0; i < r->nfindings; i++)
        if (!strcmp(r->findings[i].kind, kind) && !strcmp(r->findings[i].detail, detail)) return;
    VEC_PUSH(r->findings, r->nfindings, r->capfindings);
    r->findings[r->nfindings - 1] = (finding_t){lvl, xstrdup(kind), xstrdup(detail), xstrdup(source ? source : "")};
}

void result_errf(result_t *r, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    for (size_t i = 0; i < r->nerrors; i++)
        if (!strcmp(r->errors[i], buf)) return;
    VEC_PUSH(r->errors, r->nerrors, r->caperrors);
    r->errors[r->nerrors - 1] = xstrdup(buf);
}

// only the top level file gets to name the type, not stuff embedded in it
void result_set_type(result_t *r, const char *pfx, const char *type) {
    if (pfx && *pfx) return;
    free(r->type);
    r->type = xstrdup(type);
}

const char *level_name(level_t l) {
    switch (l) {
    case LVL_HIGH: return "high";
    case LVL_MED: return "medium";
    default: return "info";
    }
}

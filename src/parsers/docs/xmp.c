// xmp is xml (rdf) that adobe stuffs into jpegs, pdfs, psds, videos... pretty much everything.
// values show up three ways: as attributes, as simple elements, or as rdf:Seq/Bag/Alt lists
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/util.h"
#include "parsers/docs/docs.h"
#include "parsers/docs/xml.h"

#define MAX_LISTS 256

typedef struct {
    result_t *r;
    const char *group;
    char *lat, *lon;
} xmp_ctx;

static bool namech(char c) { return isalnum((unsigned char)c) || c == '_' || c == '-'; }

static void put(xmp_ctx *c, const char *pre, size_t pl, const char *nm, size_t nl, const char *v, size_t vl) {
    char key[256];
    if (pl + nl + 2 > sizeof key) return;
    snprintf(key, sizeof key, "%.*s:%.*s", (int)pl, pre, (int)nl, nm);

    // xml plumbing + per-save ids that are just noise
    static const char *skip_pre[] = {"rdf:", "xmlns:", "xml:", "x:", NULL};
    for (int i = 0; skip_pre[i]; i++)
        if (!strncmp(key, skip_pre[i], strlen(skip_pre[i]))) return;
    if (!strcmp(key, "stEvt:instanceID") || !strcmp(key, "stRef:instanceID") || !strcmp(key, "xmpMM:InstanceID"))
        return;

    char *raw = xml_unescape(v, vl);
    char *val = trim_dup(raw, strlen(raw));
    free(raw);
    if (*val && strlen(val) <= 1000) { // anything bigger is a base64 thumbnail
        result_add(c->r, c->group, key, val);
        if (!strcmp(key, "exif:GPSLatitude")) free(c->lat), c->lat = xstrdup(val);
        if (!strcmp(key, "exif:GPSLongitude")) free(c->lon), c->lon = xstrdup(val);
    }
    free(val);
}

// reads "pre:name" starting at p, returns pointer just past it or NULL
static const char *qname(const char *p, const char *end, const char **pre, size_t *pl, const char **nm, size_t *nl) {
    const char *q = p;
    while (q < end && namech(*q)) q++;
    if (q == p || q >= end || *q != ':') return NULL;
    *pre = p, *pl = (size_t)(q - p);
    p = ++q;
    while (q < end && namech(*q)) q++;
    if (q == p) return NULL;
    *nm = p, *nl = (size_t)(q - p);
    return q;
}

// "40,26.7720N" or "40,26,46.3N" -> signed decimal degrees
static bool xmp_coord(const char *s, double *out) {
    if (!s || !*s) return false;
    size_t n = strlen(s);
    char ref = s[n - 1];
    double sign = (ref == 'S' || ref == 's' || ref == 'W' || ref == 'w') ? -1 : 1;
    double total = 0, div = 1;
    const char *p = s;
    for (int part = 0; part < 3 && *p; part++) {
        char *e;
        double v = strtod(p, &e);
        if (e == p) return false;
        total += v / div;
        div *= 60;
        p = e;
        if (*p == ',') p++;
        else break;
    }
    *out = sign * total;
    return true;
}

void parse_xmp(result_t *r, const unsigned char *b, size_t n, const char *group) {
    const char *doc = (const char *)b, *end = doc + n;
    xmp_ctx c = {r, group, NULL, NULL};
    const char *pre, *nm;
    size_t pl, nl;
    const char *ranges[MAX_LISTS][2];
    int nranges = 0;

    // pass 1: lists like <dc:creator><rdf:Seq><rdf:li>Ann</rdf:li>...</rdf:Seq></dc:creator>
    for (const char *p = doc; p < end;) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        if (!lt) break;
        p = lt + 1;
        const char *q = qname(lt + 1, end, &pre, &pl, &nm, &nl);
        if (!q || q >= end || *q != '>') continue;
        q++;
        while (q < end && isspace((unsigned char)*q)) q++;
        if ((size_t)(end - q) < 9) continue;
        if (strncmp(q, "<rdf:Seq>", 9) && strncmp(q, "<rdf:Bag>", 9) && strncmp(q, "<rdf:Alt>", 9)) continue;
        char close[11];
        snprintf(close, sizeof close, "</rdf:%.3s>", q + 5);
        const char *ce = (const char *)memfind((const unsigned char *)q, (size_t)(end - q), close);
        if (!ce) continue;

        sbuf items;
        sb_init(&items);
        for (const char *li = q + 9; li < ce;) {
            const char *s = (const char *)memfind((const unsigned char *)li, (size_t)(ce - li), "<rdf:li");
            if (!s) break;
            const char *gt = xml_tag_end(s, ce);
            if (!gt) break;
            li = gt + 1;
            if (gt[-1] == '/') continue;
            const char *te = memchr(li, '<', (size_t)(ce - li));
            if (!te || (size_t)(ce - te) < 9 || strncmp(te, "</rdf:li>", 9)) continue;
            char *item = trim_dup(li, (size_t)(te - li));
            if (*item) {
                if (items.len) sb_puts(&items, ", ");
                sb_puts(&items, item);
            }
            free(item);
            li = te + 9;
        }
        if (items.len) put(&c, pre, pl, nm, nl, items.s, items.len);
        sb_free(&items);
        if (nranges < MAX_LISTS) ranges[nranges][0] = lt, ranges[nranges][1] = ce, nranges++;
        p = ce;
    }

    // pass 2: simple <pre:name>text</pre:name>, skipping the lists we already did
    for (const char *p = doc; p < end;) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        if (!lt) break;
        p = lt + 1;
        bool inside = false;
        for (int i = 0; i < nranges && !inside; i++) inside = lt >= ranges[i][0] && lt < ranges[i][1];
        if (inside) continue;
        const char *q = qname(lt + 1, end, &pre, &pl, &nm, &nl);
        if (!q || q >= end || !(isspace((unsigned char)*q) || *q == '>')) continue;
        const char *gt = xml_tag_end(lt, end);
        if (!gt) break;
        if (gt[-1] == '/') continue;
        const char *te = memchr(gt + 1, '<', (size_t)(end - gt - 1));
        if (!te) break;
        // closing tag has to match: "</" pre ":" name ">"
        size_t need = 2 + pl + 1 + nl + 1;
        if ((size_t)(end - te) < need || te[1] != '/' || strncmp(te + 2, pre, pl) || te[2 + pl] != ':' ||
            strncmp(te + 3 + pl, nm, nl) || te[3 + pl + nl] != '>')
            continue;
        put(&c, pre, pl, nm, nl, gt + 1, (size_t)(te - gt - 1));
    }

    // pass 3: attributes, pre:name="value"
    for (const char *p = doc; p < end;) {
        const char *eq = (const char *)memfind((const unsigned char *)p, (size_t)(end - p), "=\"");
        if (!eq) break;
        const char *vs = eq + 2;
        const char *ve = memchr(vs, '"', (size_t)(end - vs));
        if (!ve) break;
        p = ve + 1;
        const char *ns = eq;
        while (ns > doc && namech(ns[-1])) ns--;
        if (ns == eq || ns <= doc || ns[-1] != ':') continue;
        const char *pe = ns - 1, *ps = pe;
        while (ps > doc && namech(ps[-1])) ps--;
        if (ps == pe || ps <= doc || !isspace((unsigned char)ps[-1])) continue;
        put(&c, ps, (size_t)(pe - ps), ns, (size_t)(eq - ns), vs, (size_t)(ve - vs));
    }

    double la, lo;
    if (xmp_coord(c.lat, &la) && xmp_coord(c.lon, &lo)) result_add_gps(r, la, lo, group);
    free(c.lat), free(c.lon);
}

bool scan_xmp(result_t *r, const unsigned char *b, size_t n, const char *group) {
    bool found = false;
    for (int i = 0; i < 32; i++) {
        const unsigned char *s = memfind(b, n, "<x:xmpmeta");
        if (!s) break;
        size_t left = n - (size_t)(s - b);
        const unsigned char *e = memfind(s, left, "</x:xmpmeta>");
        if (!e) break;
        parse_xmp(r, s, (size_t)(e - s), group);
        found = true;
        n -= (size_t)(e - b);
        b = e;
    }
    return found;
}

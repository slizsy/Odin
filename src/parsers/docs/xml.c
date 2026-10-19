// not a real xml parser, just enough to walk office/xmp files without a library
#include "parsers/docs/xml.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "core/util.h"

const char *xml_tag_end(const char *s, const char *end) {
    char quote = 0;
    for (const char *p = s + 1; p < end; p++) {
        if (quote) {
            if (*p == quote) quote = 0;
        } else if (*p == '"' || *p == '\'') {
            quote = *p;
        } else if (*p == '>') {
            return p;
        }
    }
    return NULL;
}

static const char *find_str(const char *s, const char *end, const char *needle) {
    const unsigned char *p = memfind((const unsigned char *)s, (size_t)(end - s), needle);
    return (const char *)p;
}

void xml_leaves(const char *s, size_t n, int want, xml_leaf_fn fn, void *ctx) {
    const char *p = s, *end = s + n;
    int depth = 0;
    bool has_child = false;
    char name[128] = "";
    sbuf text;
    sb_init(&text);

    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        if (!lt) break;
        if (depth == want) sb_putn(&text, p, (size_t)(lt - p));
        p = lt;

        if ((size_t)(end - p) >= 2 && p[1] == '?') {
            const char *q = find_str(p, end, "?>");
            p = q ? q + 2 : end;
            continue;
        }
        if ((size_t)(end - p) >= 4 && !strncmp(p, "<!--", 4)) {
            const char *q = find_str(p, end, "-->");
            p = q ? q + 3 : end;
            continue;
        }
        if ((size_t)(end - p) >= 9 && !strncmp(p, "<![CDATA[", 9)) {
            const char *q = find_str(p, end, "]]>");
            if (!q) break;
            if (depth == want) sb_putn(&text, p + 9, (size_t)(q - p - 9));
            p = q + 3;
            continue;
        }
        const char *gt = xml_tag_end(p, end);
        if (!gt) break;

        if (p + 1 < end && p[1] == '/') {
            if (depth == want && !has_child && *name) {
                char *raw = xml_unescape(text.s ? text.s : "", text.len);
                char *t = trim_dup(raw, strlen(raw));
                fn(ctx, name, t);
                free(raw), free(t);
            }
            depth--;
        } else if (p + 1 < end && p[1] != '!') {
            bool self_close = gt[-1] == '/';
            depth++;
            if (depth == want) {
                // grab the local name, drop any "ns:" prefix
                const char *a = p + 1, *b = a;
                while (b < gt && !isspace((unsigned char)*b) && *b != '/' && *b != '>') b++;
                const char *colon = memchr(a, ':', (size_t)(b - a));
                if (colon) a = colon + 1;
                size_t len = (size_t)(b - a) < sizeof name - 1 ? (size_t)(b - a) : sizeof name - 1;
                memcpy(name, a, len);
                name[len] = 0;
                text.len = 0;
                if (text.s) text.s[0] = 0;
                has_child = false;
            } else if (depth > want) {
                has_child = true;
            }
            if (self_close) depth--;
        }
        p = gt + 1;
    }
    sb_free(&text);
}

char *xml_attr(const char *tag, size_t n, const char *name) {
    size_t nl = strlen(name);
    const char *end = tag + n;
    for (const char *p = tag; p + nl + 2 <= end; p++) {
        // attr name has to start after whitespace and be followed by ="
        if (p == tag || !isspace((unsigned char)p[-1])) continue;
        if (strncmp(p, name, nl) || p[nl] != '=' || (p[nl + 1] != '"' && p[nl + 1] != '\'')) continue;
        char q = p[nl + 1];
        const char *v = p + nl + 2;
        const char *ve = memchr(v, q, (size_t)(end - v));
        if (!ve) return NULL;
        return xml_unescape(v, (size_t)(ve - v));
    }
    return NULL;
}

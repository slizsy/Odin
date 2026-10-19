// png = signature + chunks (length, type, data, crc). text chunks are where the goods are
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/util.h"
#include "parsers/archive/zip.h"
#include "parsers/docs/docs.h"
#include "parsers/images/images.h"

static void png_text(result_t *r, const char *key, const char *val, size_t vlen, const char *pfx) {
    char g[600];
    if (!strcmp(key, "XML:com.adobe.xmp")) {
        snprintf(g, sizeof g, "%sXMP", pfx);
        parse_xmp(r, (const unsigned char *)val, vlen, g);
        return;
    }
    snprintf(g, sizeof g, "%sPNG", pfx);
    result_addn(r, g, key, val, vlen);
}

void parse_png(result_t *r, const unsigned char *b, size_t n, const char *pfx) {
    char g[600];
    size_t i = 8;
    while (i + 12 <= n) {
        size_t len = rd32be(b + i);
        char typ[5] = {0};
        memcpy(typ, b + i + 4, 4);
        if (len > n || i + 12 + len > n) {
            result_errf(r, "png: truncated %s chunk", typ);
            return;
        }
        const unsigned char *d = b + i + 8;
        const unsigned char *nul = memchr(d, 0, len);
        size_t klen = nul ? (size_t)(nul - d) : 0;
        char *key = nul ? xstrndup((const char *)d, klen) : NULL;
        const unsigned char *rest = nul ? nul + 1 : NULL;
        size_t rlen = nul ? len - klen - 1 : 0;

        if (!strcmp(typ, "tEXt") && key) {
            char *v = latin1_to_utf8(rest, rlen);
            png_text(r, key, v, strlen(v), pfx);
            free(v);
        } else if (!strcmp(typ, "zTXt") && key && rlen > 1) {
            size_t out_len;
            unsigned char *raw = inflate_buf(rest + 1, rlen - 1, &out_len, 16 << 20, false);
            if (raw) {
                char *v = latin1_to_utf8(raw, out_len);
                png_text(r, key, v, strlen(v), pfx);
                free(v), free(raw);
            }
        } else if (!strcmp(typ, "iTXt") && key && rlen >= 2) {
            int compressed = rest[0] == 1;
            const unsigned char *p = rest + 2, *end = rest + rlen;
            // skip language tag and translated keyword
            for (int skip = 0; skip < 2 && p < end; skip++) {
                const unsigned char *z = memchr(p, 0, (size_t)(end - p));
                p = z ? z + 1 : end;
            }
            if (compressed) {
                size_t out_len;
                unsigned char *raw = inflate_buf(p, (size_t)(end - p), &out_len, 16 << 20, false);
                if (raw) {
                    png_text(r, key, (const char *)raw, out_len, pfx);
                    free(raw);
                }
            } else {
                png_text(r, key, (const char *)p, (size_t)(end - p), pfx);
            }
        } else if (!strcmp(typ, "eXIf")) {
            snprintf(g, sizeof g, "%sEXIF", pfx);
            if (STARTS(d, len, "Exif\0\0")) parse_exif(r, d + 6, len - 6, g);
            else parse_exif(r, d, len, g);
        } else if (!strcmp(typ, "tIME") && len == 7) {
            char buf[64];
            snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d UTC", rd16be(d), d[2], d[3], d[4], d[5], d[6]);
            snprintf(g, sizeof g, "%sPNG", pfx);
            result_add(r, g, "ModifyDate", buf);
        } else if (!strcmp(typ, "IEND")) {
            free(key);
            return;
        }
        free(key);
        i += 12 + len;
    }
}

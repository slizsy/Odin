// webp is a RIFF container, exif and xmp get their own chunks
#include <stdio.h>
#include <string.h>

#include "core/util.h"
#include "parsers/docs/docs.h"
#include "parsers/images/images.h"

void parse_webp(result_t *r, const unsigned char *b, size_t n, const char *pfx) {
    char g[600];
    size_t i = 12;
    while (i + 8 <= n) {
        const unsigned char *id = b + i;
        size_t len = rd32le(b + i + 4);
        if (len > n || i + 8 + len > n) {
            result_errf(r, "webp: truncated %.4s chunk", (const char *)id);
            return;
        }
        const unsigned char *d = b + i + 8;
        if (!memcmp(id, "EXIF", 4)) {
            snprintf(g, sizeof g, "%sEXIF", pfx);
            if (STARTS(d, len, "Exif\0\0")) parse_exif(r, d + 6, len - 6, g);
            else parse_exif(r, d, len, g);
        } else if (!memcmp(id, "XMP ", 4)) {
            snprintf(g, sizeof g, "%sXMP", pfx);
            parse_xmp(r, d, len, g);
        }
        i += 8 + len + (len & 1); // chunks are padded to even
    }
}

#include "parsers/parsers.h"

#include <stdio.h>
#include <string.h>

#include "core/util.h"
#include "parsers/docs/docs.h"
#include "parsers/images/images.h"

// figure out what the file actually is from its first bytes, not its extension
void parse_bytes(result_t *r, const unsigned char *b, size_t n, const char *pfx, int depth) {
    char g[600];
    if (STARTS(b, n, "\xFF\xD8\xFF")) {
        result_set_type(r, pfx, "JPEG image");
        parse_jpeg(r, b, n, pfx);
    } else if (STARTS(b, n, "\x89PNG\r\n\x1a\n")) {
        result_set_type(r, pfx, "PNG image");
        parse_png(r, b, n, pfx);
    } else if (STARTS(b, n, "II*\0") || STARTS(b, n, "MM\0*")) {
        // plain tiff and most camera raws (cr2, nef, dng, arw) are tiff underneath
        result_set_type(r, pfx, "TIFF / camera raw");
        snprintf(g, sizeof g, "%sEXIF", pfx);
        parse_exif(r, b, n, g);
    } else if (n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WEBP", 4)) {
        result_set_type(r, pfx, "WebP image");
        parse_webp(r, b, n, pfx);
    } else if (memfind(b, n < 1024 ? n : 1024, "%PDF-")) {
        result_set_type(r, pfx, "PDF document");
        parse_pdf(r, b, n, pfx);
    } else if (STARTS(b, n, "PK\x03\x04")) {
        result_set_type(r, pfx, "ZIP archive");
        if (depth == 0) parse_office(r, b, n, pfx, depth);
    } else {
        if (n >= 12 && !memcmp(b + 4, "ftyp", 4)) {
            char t[64];
            snprintf(t, sizeof t, "ISO media (%.4s)", (const char *)b + 8);
            for (char *c = t; *c; c++)
                if (*c < 0x20 || *c > 0x7e) *c = '?';
            result_set_type(r, pfx, t);
        }
        // last resort, tons of formats (psd, ai, mp4, svg...) hide an xmp packet somewhere
        snprintf(g, sizeof g, "%sXMP", pfx);
        scan_xmp(r, b, n, g);
    }
}

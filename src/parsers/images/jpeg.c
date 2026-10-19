// jpeg = a list of marker segments. all the metadata is before the image data starts
#include <stdio.h>
#include <string.h>

#include "core/util.h"
#include "parsers/docs/docs.h"
#include "parsers/images/images.h"

#define XMP_NS "http://ns.adobe.com/xap/1.0/"

void parse_jpeg(result_t *r, const unsigned char *b, size_t n, const char *pfx) {
    char g[600];
    size_t i = 2;
    while (i + 4 <= n) {
        if (b[i] != 0xFF) {
            result_errf(r, "jpeg: lost sync at byte %zu", i);
            return;
        }
        unsigned char m = b[i + 1];
        if (m == 0xFF) { // fill byte
            i++;
            continue;
        }
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
            i += 2;
            continue;
        }
        if (m == 0xDA || m == 0xD9) return; // start of scan, we're done

        size_t len = rd16be(b + i + 2);
        if (len < 2 || i + 2 + len > n) {
            result_errf(r, "jpeg: truncated segment at byte %zu", i);
            return;
        }
        const unsigned char *seg = b + i + 4;
        size_t sn = len - 2;
        if (m == 0xE1 && STARTS(seg, sn, "Exif\0\0")) {
            snprintf(g, sizeof g, "%sEXIF", pfx);
            parse_exif(r, seg + 6, sn - 6, g);
        } else if (m == 0xE1 && STARTS(seg, sn, XMP_NS "\0")) {
            snprintf(g, sizeof g, "%sXMP", pfx);
            parse_xmp(r, seg + sizeof XMP_NS, sn - sizeof XMP_NS, g);
        } else if (m == 0xED && STARTS(seg, sn, "Photoshop 3.0\0")) {
            snprintf(g, sizeof g, "%sIPTC", pfx);
            parse_8bim(r, seg + 14, sn - 14, g);
        } else if (m == 0xFE) {
            snprintf(g, sizeof g, "%sJPEG", pfx);
            result_addn(r, g, "Comment", (const char *)seg, sn);
        }
        i += 2 + len;
    }
}

#ifndef ODIN_SCRUB_H
#define ODIN_SCRUB_H

#include <stdbool.h>
#include <stddef.h>

// strips metadata out of images without re-encoding them, so the pixels stay exactly the
// same. what goes: exif (gps, camera, serials, dates, thumbnails), xmp, iptc, comments,
// png text chunks, maker junk, anything tacked on after the image ends.
// what stays: the image itself, color profiles, and the rotation (so it doesn't flip sideways)

typedef enum {
    SCRUB_OK,
    SCRUB_UNSUPPORTED, // not a format we can scrub (yet)
    SCRUB_BROKEN,      // looks like the format but the file is mangled, err says how
} scrub_rc;

typedef struct {
    const char *format; // "JPEG", "PNG", "WebP", "HEIC"
    int removed;        // how many metadata blocks got dropped / blanked
    char what[160];     // what kinds, like "exif, xmp, comment"
    int orientation;    // rotation we carried over, 0 = none needed
} scrub_info;

// out is malloc'd, caller frees. on anything but SCRUB_OK out is NULL
scrub_rc scrub_bytes(const unsigned char *b, size_t n, unsigned char **out, size_t *out_len, scrub_info *info,
                     char *err, size_t errlen);

// orientation tag (1-8) from a raw tiff/exif block, 0 if there isn't one
int tiff_orientation(const unsigned char *t, size_t n);

#endif

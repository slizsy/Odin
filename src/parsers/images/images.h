#ifndef ODIN_IMAGES_H
#define ODIN_IMAGES_H

#include <stdbool.h>
#include <stddef.h>

#include "core/result.h"

void parse_exif(result_t *r, const unsigned char *b, size_t n, const char *group);
void parse_jpeg(result_t *r, const unsigned char *b, size_t n, const char *pfx);
void parse_png(result_t *r, const unsigned char *b, size_t n, const char *pfx);
void parse_webp(result_t *r, const unsigned char *b, size_t n, const char *pfx);
// heic / avif. returns false if there's no meta box to read
bool parse_heif(result_t *r, const unsigned char *b, size_t n, const char *pfx);
// blanks out every exif + xmp item in a heic / avif in place (same size, nothing moves, so
// the image data stays byte for byte identical). returns how many items it couldn't
// reach, or -1 if there's no meta box
int heif_wipe_meta(unsigned char *b, size_t n, int *nexif, int *nxmp);
void parse_8bim(result_t *r, const unsigned char *b, size_t n, const char *group);

#endif

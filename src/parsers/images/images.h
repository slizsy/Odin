#ifndef ODIN_IMAGES_H
#define ODIN_IMAGES_H

#include <stddef.h>

#include "core/result.h"

void parse_exif(result_t *r, const unsigned char *b, size_t n, const char *group);
void parse_jpeg(result_t *r, const unsigned char *b, size_t n, const char *pfx);
void parse_png(result_t *r, const unsigned char *b, size_t n, const char *pfx);
void parse_webp(result_t *r, const unsigned char *b, size_t n, const char *pfx);
void parse_8bim(result_t *r, const unsigned char *b, size_t n, const char *group);

#endif

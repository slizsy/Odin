#ifndef ODIN_DOCS_H
#define ODIN_DOCS_H

#include <stdbool.h>
#include <stddef.h>

#include "core/result.h"

void parse_pdf(result_t *r, const unsigned char *b, size_t n, const char *pfx);
void parse_office(result_t *r, const unsigned char *b, size_t n, const char *pfx, int depth);
void parse_xmp(result_t *r, const unsigned char *b, size_t n, const char *group);
// hunts for xmp packets anywhere in a blob, true if it found one
bool scan_xmp(result_t *r, const unsigned char *b, size_t n, const char *group);

#endif

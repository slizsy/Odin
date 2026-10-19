#ifndef ODIN_PARSERS_H
#define ODIN_PARSERS_H

#include <stddef.h>

#include "core/result.h"

// sniffs the format and hands off to the right parser.
// pfx is "" for the real file, or "word/media/image1.jpeg › " for stuff embedded inside it
void parse_bytes(result_t *r, const unsigned char *b, size_t n, const char *pfx, int depth);

#endif

#ifndef ODIN_EXTRACT_H
#define ODIN_EXTRACT_H

#include <stdbool.h>
#include <stddef.h>

#include "core/result.h"

typedef struct {
    bool exiftool;
} extract_opts;

// the whole pipeline for one file: read, hash, parse, (exiftool), analyze
result_t *extract_file(const char *path, extract_opts opt);
// same thing on bytes already in memory (the tests use this)
void extract_bytes(result_t *r, const unsigned char *b, size_t n);

#endif

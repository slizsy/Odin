#ifndef ODIN_ZIP_H
#define ODIN_ZIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *name;
    uint16_t method;
    uint32_t csize, usize, local_off;
} zip_entry;

typedef struct {
    const unsigned char *b;
    size_t n;
    zip_entry *entries;
    size_t count, cap;
    char *comment;
} zip_t;

bool zip_open(zip_t *z, const unsigned char *b, size_t n);
void zip_close(zip_t *z);
const zip_entry *zip_find(const zip_t *z, const char *name);
// decompresses one entry, caller frees. NULL if it's broken or over max
unsigned char *zip_read(const zip_t *z, const zip_entry *e, size_t *len, size_t max);

// zlib inflate. raw = no zlib header (what zip uses)
unsigned char *inflate_buf(const unsigned char *in, size_t n, size_t *out_len, size_t max, bool raw);

#endif

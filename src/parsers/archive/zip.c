// minimal zip reader: find the central directory at the end, list entries, inflate on demand
#include "parsers/archive/zip.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "core/util.h"

unsigned char *inflate_buf(const unsigned char *in, size_t n, size_t *out_len, size_t max, bool raw) {
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (inflateInit2(&zs, raw ? -15 : 15) != Z_OK) return NULL;
    size_t cap = n * 4 + 1024;
    if (cap > max) cap = max;
    unsigned char *out = xmalloc(cap + 1);
    size_t len = 0;
    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)(n > UINT32_MAX ? UINT32_MAX : n);
    int rc;
    for (;;) {
        if (len == cap) {
            if (cap >= max) break; // hit the limit, keep what we've got
            cap = cap * 2 > max ? max : cap * 2;
            out = xrealloc(out, cap + 1);
        }
        zs.next_out = out + len;
        zs.avail_out = (uInt)(cap - len);
        rc = inflate(&zs, Z_NO_FLUSH);
        len = cap - zs.avail_out;
        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK) {
            if (len == 0) {
                inflateEnd(&zs);
                free(out);
                return NULL;
            }
            break; // truncated stream, the partial output is still useful
        }
    }
    inflateEnd(&zs);
    out[len] = 0;
    *out_len = len;
    return out;
}

bool zip_open(zip_t *z, const unsigned char *b, size_t n) {
    memset(z, 0, sizeof *z);
    z->b = b, z->n = n;
    if (n < 22) return false;

    // end of central directory record lives in the last 64k
    size_t lo = n > 65557 ? n - 65557 : 0;
    size_t eocd = SIZE_MAX;
    for (size_t i = n - 22 + 1; i-- > lo;) {
        if (!memcmp(b + i, "PK\x05\x06", 4)) {
            eocd = i;
            break;
        }
    }
    if (eocd == SIZE_MAX) return false;

    size_t total = rd16le(b + eocd + 10);
    size_t cd_off = rd32le(b + eocd + 16);
    size_t clen = rd16le(b + eocd + 20);
    if (eocd + 22 + clen <= n) z->comment = xstrndup((const char *)b + eocd + 22, clen);

    size_t p = cd_off;
    for (size_t i = 0; i < total; i++) {
        if (p + 46 > n || memcmp(b + p, "PK\x01\x02", 4)) break;
        size_t nl = rd16le(b + p + 28), xl = rd16le(b + p + 30), cl = rd16le(b + p + 32);
        if (p + 46 + nl > n) break;
        VEC_PUSH(z->entries, z->count, z->cap);
        zip_entry *e = &z->entries[z->count - 1];
        e->method = rd16le(b + p + 10);
        e->csize = rd32le(b + p + 20);
        e->usize = rd32le(b + p + 24);
        e->local_off = rd32le(b + p + 42);
        e->name = xstrndup((const char *)b + p + 46, nl);
        p += 46 + nl + xl + cl;
    }
    return z->count > 0;
}

void zip_close(zip_t *z) {
    for (size_t i = 0; i < z->count; i++) free(z->entries[i].name);
    free(z->entries);
    free(z->comment);
    memset(z, 0, sizeof *z);
}

const zip_entry *zip_find(const zip_t *z, const char *name) {
    for (size_t i = 0; i < z->count; i++)
        if (!strcmp(z->entries[i].name, name)) return &z->entries[i];
    return NULL;
}

unsigned char *zip_read(const zip_t *z, const zip_entry *e, size_t *len, size_t max) {
    if (!e) return NULL;
    size_t p = e->local_off;
    if (p + 30 > z->n || memcmp(z->b + p, "PK\x03\x04", 4)) return NULL;
    size_t data = p + 30 + rd16le(z->b + p + 26) + rd16le(z->b + p + 28);
    if (data > z->n || e->csize > z->n - data) return NULL;
    const unsigned char *src = z->b + data;

    if (e->method == 0) {
        size_t n = e->csize < max ? e->csize : max;
        unsigned char *out = xmalloc(n + 1);
        memcpy(out, src, n);
        out[n] = 0;
        *len = n;
        return out;
    }
    if (e->method == 8) return inflate_buf(src, e->csize, len, max, true);
    return NULL; // bzip2/lzma etc, office never uses them
}

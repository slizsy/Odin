#include "core/extract.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "analysis/findings.h"
#include "core/sha256.h"
#include "core/util.h"
#include "external/exiftool.h"
#include "parsers/parsers.h"

// anything bigger we don't slurp into memory
#define MAX_READ (512LL << 20)

void extract_bytes(result_t *r, const unsigned char *b, size_t n) {
    sha256_hex(b, n, r->sha256);
    parse_bytes(r, b, n, "", 0);
}

result_t *extract_file(const char *path, extract_opts opt) {
    result_t *r = result_new(path);
    struct stat st;
    if (stat(path, &st) != 0) {
        result_errf(r, "%s", strerror(errno));
        return r;
    }
    r->size = st.st_size;
    r->mtime = st.st_mtime;

    if (st.st_size > MAX_READ) {
        char sz[32];
        human_size(st.st_size, sz, sizeof sz);
        result_errf(r, "file is %s, too big for the native parser", sz);
    } else {
        size_t n;
        unsigned char *b = read_file(path, &n);
        if (!b) {
            result_errf(r, "%s", strerror(errno));
            return r;
        }
        extract_bytes(r, b, n);
        free(b);
    }

    if (opt.exiftool) run_exiftool(r);
    analyze(r);
    return r;
}

// libfuzzer harness, run with `make fuzz`
#include <stdlib.h>
#include <string.h>

#include "analysis/findings.h"
#include "core/extract.h"
#include "scrub/scrub.h"

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t n) {
    result_t *r = result_new("fuzz");
    extract_bytes(r, data, n);
    analyze(r);
    result_free(r);

    // the scrubber walks the same formats, throw the junk at it too
    unsigned char *out;
    size_t out_len;
    scrub_info info;
    char err[256];
    if (scrub_bytes(data, n, &out, &out_len, &info, err, sizeof err) == SCRUB_OK) free(out);
    return 0;
}

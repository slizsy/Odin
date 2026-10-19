// libfuzzer harness, run with `make fuzz`
#include <stdlib.h>
#include <string.h>

#include "analysis/findings.h"
#include "core/extract.h"

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t n) {
    result_t *r = result_new("fuzz");
    extract_bytes(r, data, n);
    analyze(r);
    result_free(r);
    return 0;
}

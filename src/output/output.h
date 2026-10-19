#ifndef ODIN_OUTPUT_H
#define ODIN_OUTPUT_H

#include <stdbool.h>

#include "core/result.h"

void print_pretty(const result_t *r, bool findings_only);
void print_summary(int scanned, int skipped, int high, int med, int errors);

// json is streamed: open, one result at a time, close
void json_begin(void);
void json_result(const result_t *r);
void json_end(void);

#endif

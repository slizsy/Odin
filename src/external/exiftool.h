#ifndef ODIN_EXIFTOOL_H
#define ODIN_EXIFTOOL_H

#include <stdbool.h>

#include "core/result.h"

bool have_exiftool(void);
// swaps our field dump for exiftool's full one. returns false + sets an error on failure
bool run_exiftool(result_t *r);

#endif

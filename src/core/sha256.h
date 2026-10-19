#ifndef ODIN_SHA256_H
#define ODIN_SHA256_H

#include <stddef.h>

// writes 64 hex chars + nul into out
void sha256_hex(const unsigned char *data, size_t len, char out[65]);

#endif

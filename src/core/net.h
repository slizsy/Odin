#ifndef ODIN_NET_H
#define ODIN_NET_H

#include <curl/curl.h>
#include <stdbool.h>
#include <stddef.h>

#include "core/util.h"

// points curl at the right https certs for this os (see net.c)
void net_apply_tls(CURL *e);

// one blocking GET. headers is NULL-terminated (or NULL). body is capped at max bytes.
// false = couldn't connect / too big, with the reason in err. any http status counts as true
bool net_get(const char *url, const char *const *headers, size_t max, long *status, sbuf *body, char *err,
             size_t errlen);

#endif

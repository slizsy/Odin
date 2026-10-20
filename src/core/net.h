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

// GET + json parse in one go. returns NULL on a network error, a non-2xx status, or
// unparseable json, with the reason in err. *status gets the http code when there was one.
// caller frees the result with json_free
struct json *net_get_json(const char *url, const char *const *headers, size_t max, long *status, char *err,
                          size_t errlen);

#endif

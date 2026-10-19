#ifndef ODIN_RESULT_H
#define ODIN_RESULT_H

#include <stddef.h>
#include <time.h>

#include "core/util.h"

typedef struct {
    char *group, *key, *value;
} field_t;

typedef struct {
    double lat, lon;
    char *source;
} geo_t;

typedef enum { LVL_HIGH, LVL_MED, LVL_INFO } level_t;

typedef struct {
    level_t level;
    char *kind, *detail, *source;
} finding_t;

typedef struct {
    char *path, *name, *type;
    const char *engine;
    long long size;
    time_t mtime;
    char sha256[65];

    field_t *fields;
    size_t nfields, capfields;
    geo_t *gps;
    size_t ngps, capgps;
    finding_t *findings;
    size_t nfindings, capfindings;
    char **errors;
    size_t nerrors, caperrors;
} result_t;

result_t *result_new(const char *path);
void result_free(result_t *r);

// value gets cleaned up (control chars, whitespace, bad utf8) and deduped
void result_add(result_t *r, const char *group, const char *key, const char *value);
void result_addn(result_t *r, const char *group, const char *key, const char *value, size_t n);
void result_add_gps(result_t *r, double lat, double lon, const char *source);
void result_add_finding(result_t *r, level_t lvl, const char *kind, const char *detail, const char *source);
void result_errf(result_t *r, const char *fmt, ...) ODIN_PRINTF(2, 3);
void result_set_type(result_t *r, const char *pfx, const char *type);

const char *level_name(level_t l);

#endif

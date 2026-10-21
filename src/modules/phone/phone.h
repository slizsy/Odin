#ifndef ODIN_PHONE_H
#define ODIN_PHONE_H

#include <stdbool.h>
#include <stddef.h>

// how many number types we classify (fixed line, mobile, toll free, ...)
#define PHONE_NTYPES 10

// one calling-region pulled from libphonenumber's metadata. patterns are the python-subset
// regex our core/regex.c understands (no lookahead/backrefs in this data, so it all matches)
typedef struct {
    const char *region;  // iso code like "US", "GB"
    int cc;              // country calling code, eg 1, 44
    bool main;           // the main country for this calling code (eg US for +1)
    const char *leading; // leading-digit regex to tell shared-code regions apart, or NULL
    const char *general; // whole-number validity pattern
    unsigned lengths;    // bitmask: bit L set = a national number of length L is possible
    const char *types[PHONE_NTYPES]; // per-type pattern, same order as phone_type_names, NULLs ok
} phone_region;

extern const phone_region phone_regions[];
extern const size_t phone_regions_n;
extern const char *const phone_type_names[];
extern const size_t phone_types_n;

// what we worked out about a number. national is malloc'd, free it with phone_info_free
typedef struct {
    bool ok;                   // we found a country calling code
    int cc;                    // the calling code
    const phone_region *region; // the region it belongs to, or NULL
    char *national;            // digits after the calling code
    bool valid;                // length + pattern both check out
    const char *type;          // line type (static string) or NULL
} phone_info;

// parse a raw number. default_cc is a 2-letter code for numbers typed without a +, or NULL
phone_info phone_analyze(const char *raw, const char *default_cc);
void phone_info_free(phone_info *p);

int run_phone(int argc, char **argv);

#endif

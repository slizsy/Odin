#include "modules/phone/phone.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/iso3166.h"
#include "core/json.h"
#include "core/regex.h"
#include "core/util.h"
#include "modules/usage.h"
#include "ui/banner.h"
#include "ui/color.h"

#define BAR c_gray("│")

typedef struct {
    bool json, quiet;
    const char *country; // default region for numbers typed without a + (eg "US")
} phone_flags;

// keep only digits, remember if the user led with a +
static char *only_digits(const char *in, bool *had_plus) {
    sbuf b;
    sb_init(&b);
    *had_plus = false;
    for (const char *p = in; *p; p++) {
        if (*p == '+' && b.len == 0) *had_plus = true;
        else if (*p >= '0' && *p <= '9') sb_putc(&b, *p);
    }
    return sb_take(&b);
}

static bool matches(const char *pat, const char *num) {
    if (!pat) return false;
    bool uns = false;
    bool r = rx_full(pat, num, &uns);
    return r && !uns; // if we can't read the pattern, don't claim a match
}

// find the calling code at the front of an international number and hand back the national
// part. tries 3 then 2 then 1 digits, longest real code wins
static int split_cc(const char *digits, const char **national) {
    for (int len = 3; len >= 1; len--) {
        if ((int)strlen(digits) <= len) continue;
        int cc = 0;
        for (int i = 0; i < len; i++) cc = cc * 10 + (digits[i] - '0');
        for (size_t i = 0; i < phone_regions_n; i++)
            if (phone_regions[i].cc == cc) {
                *national = digits + len;
                return cc;
            }
    }
    return 0;
}

// of the regions sharing a calling code, pick the one whose leading-digit rule fits the
// national number (falling back to the code's main country)
static const phone_region *pick_region(int cc, const char *national) {
    const phone_region *first = NULL, *main = NULL, *lead = NULL;
    for (size_t i = 0; i < phone_regions_n; i++) {
        const phone_region *r = &phone_regions[i];
        if (r->cc != cc) continue;
        if (!first) first = r;
        if (r->main && !main) main = r;
        if (r->leading && !lead) {
            bool uns = false;
            // leadingDigits anchors at the start, so search from position 0 is enough
            char *anchored = xprintf("^(?:%s)", r->leading);
            if (rx_search(anchored, national, &uns) && !uns) lead = r;
            free(anchored);
        }
    }
    // a calling code with only one country has no "main" flag, so fall back to it
    return lead ? lead : main ? main : first;
}

static const char *classify(const phone_region *r, const char *national) {
    // fixed line and mobile first; if both fit (and share a pattern) it's either one
    bool fixed = matches(r->types[0], national);
    bool mobile = matches(r->types[1], national);
    if (fixed && mobile) return "fixed line or mobile";
    if (fixed) return "fixed line";
    if (mobile) return "mobile";
    for (size_t i = 2; i < phone_types_n; i++)
        if (matches(r->types[i], national)) return phone_type_names[i];
    return NULL;
}

phone_info phone_analyze(const char *raw, const char *default_cc) {
    phone_info out = {0};
    bool had_plus;
    char *digits = only_digits(raw, &had_plus);
    const char *national = NULL;
    int cc = 0;

    if (had_plus) {
        // an international number: pull the calling code off the front
        cc = split_cc(digits, &national);
    } else if (default_cc) {
        // no +, but we were told the country: treat the digits as a national number there,
        // dropping a single leading trunk 0 the way libphonenumber does
        for (size_t i = 0; i < phone_regions_n; i++)
            if (!strcmp(phone_regions[i].region, default_cc)) {
                cc = phone_regions[i].cc;
                break;
            }
        national = digits;
        if (national[0] == '0') national++;
    } else {
        // no + and no hint: best effort, assume the digits already carry a calling code
        cc = split_cc(digits, &national);
    }
    if (!cc) {
        free(digits);
        return out;
    }

    out.ok = true;
    out.cc = cc;
    const phone_region *r = pick_region(cc, national ? national : "");
    out.region = r;
    out.national = xstrdup(national ? national : "");
    free(digits);

    if (r) {
        size_t nlen = strlen(out.national);
        bool len_ok = nlen < 32 && (r->lengths & (1u << nlen));
        out.valid = len_ok && matches(r->general, out.national);
        if (out.valid) out.type = classify(r, out.national);
    }
    return out;
}

static void print_json(const char *raw, const phone_info *p) {
    printf("{\n  \"input\": ");
    json_quote(raw);
    if (!p->ok) {
        printf(",\n  \"valid\": false,\n  \"error\": \"no country calling code\"\n}\n");
        return;
    }
    const char *cn = p->region ? iso3166_name(p->region->region) : NULL;
    printf(",\n  \"country_code\": %d", p->cc);
    printf(",\n  \"region\": ");
    json_quote(p->region ? p->region->region : "");
    printf(",\n  \"country\": ");
    json_quote(cn ? cn : "");
    printf(",\n  \"national_number\": ");
    json_quote(p->national);
    printf(",\n  \"e164\": ");
    char e164[64];
    snprintf(e164, sizeof e164, "+%d%s", p->cc, p->national);
    json_quote(e164);
    printf(",\n  \"valid\": %s", p->valid ? "true" : "false");
    printf(",\n  \"type\": ");
    json_quote(p->type ? p->type : "");
    printf("\n}\n");
}

static void row(const char *k, const char *v) {
    printf("%s   %s%s\n", BAR, c_gray(k), v);
}

static void print_pretty(const char *raw, const phone_info *p) {
    printf("%s %s\n", c_gray("┌─"), c_bold(raw));
    if (!p->ok) {
        printf("%s   %s\n%s\n", BAR, c_yellow("couldn't find a country calling code (try +1..., +44...)"),
               c_gray("└─"));
        return;
    }
    char cc[16], e164[64];
    snprintf(cc, sizeof cc, "+%d", p->cc);
    snprintf(e164, sizeof e164, "+%d%s", p->cc, p->national);
    const char *cn = p->region ? iso3166_name(p->region->region) : NULL;
    char loc[128];
    snprintf(loc, sizeof loc, "%s%s%s", cn ? cn : "?", p->region ? " (" : "", p->region ? p->region->region : "");
    if (p->region) strncat(loc, ")", sizeof loc - strlen(loc) - 1);

    row("country      ", loc);
    row("calling code ", cc);
    row("national no. ", p->national);
    row("e.164        ", e164);
    row("line type    ", p->type ? p->type : (p->valid ? "unknown" : "-"));
    printf("%s   %s%s\n", BAR, c_gray("valid        "),
           p->valid ? c_bold("yes") : c_yellow("no (bad length or not an assigned number)"));
    printf("%s\n\n", c_gray("└─"));
}

void phone_info_free(phone_info *p) {
    free(p->national);
    p->national = NULL;
}

int run_phone(int argc, char **argv) {
    phone_flags fl = {0};
    const char *numbers[64];
    int nn = 0;

    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-j") || !strcmp(a, "--json")) fl.json = true;
        else if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) fl.quiet = true;
        else if (!strcmp(a, "--no-color")) color_on = false;
        else if (!strcmp(a, "-c") || !strcmp(a, "--country")) {
            if (i + 1 >= argc) { fputs("odin: --country needs a 2-letter code\n", stderr); return 2; }
            fl.country = argv[++i];
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            fputs(usage, stdout);
            return 0;
        } else if (a[0] == '-' && a[1] && strcmp(a, "-")) {
            fprintf(stderr, "odin: unknown flag %s\n", a);
            return 2;
        } else if (nn < 64) {
            numbers[nn++] = a;
        }
    }
    if (fl.json) color_on = false;
    if (!nn) {
        fprintf(stderr, "odin: give me a phone number, like +14155552671\n\n%s", usage);
        return 2;
    }
    if (!fl.json && !fl.quiet) banner_print();

    int bad = 0;
    if (fl.json && nn > 1) putchar('[');
    for (int i = 0; i < nn; i++) {
        phone_info p = phone_analyze(numbers[i], fl.country);
        if (!p.valid) bad++;
        if (fl.json) {
            if (nn > 1 && i) putchar(',');
            print_json(numbers[i], &p);
        } else {
            print_pretty(numbers[i], &p);
        }
        phone_info_free(&p);
    }
    if (fl.json && nn > 1) puts("]");
    return bad ? 1 : 0;
}

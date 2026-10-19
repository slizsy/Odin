#include "modules/user/user_cmd.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/platform.h"
#include "core/util.h"
#include "modules/usage.h"
#include "modules/user/check.h"
#include "modules/user/sites.h"
#include "ui/banner.h"
#include "ui/color.h"

#define BAR c_gray("│")

typedef struct {
    bool all, json, quiet, list, selftest;
    long timeout;
    const char *sites_file;
} user_flags;

static bool show_progress;

static void progress(size_t done, size_t total) {
    if (!show_progress) return;
    fprintf(stderr, "\r  checking %zu sites... %zu/%zu", total, done, total);
    if (done == total) fputs("\r\x1b[K", stderr); // wipe the line when we're done
}

static void jstr(const char *s) {
    putchar('"');
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

static void print_json(hit_list *h, char **users, int nusers, size_t nsites) {
    puts("[");
    for (int u = 0; u < nusers; u++) {
        printf("  {\n    \"username\": "), jstr(users[u]);
        printf(",\n    \"sites\": [");
        for (size_t i = 0; i < nsites; i++) {
            hit_t *x = &h->hits[(size_t)u * nsites + i];
            printf("%s\n      {\"site\": ", i ? "," : ""), jstr(x->site->name);
            printf(", \"status\": \"%s\", \"url\": ", hit_state_name(x->state)), jstr(x->url);
            printf(", \"http\": %ld, \"detail\": ", x->http), jstr(x->detail ? x->detail : "");
            putchar('}');
        }
        printf("\n    ]\n  }%s\n", u + 1 < nusers ? "," : "");
    }
    puts("]");
}

static int name_width(const site_list *s) {
    size_t w = 0;
    for (size_t i = 0; i < s->count; i++)
        if (utf8_width(s->items[i].name) > w) w = utf8_width(s->items[i].name);
    return (int)w;
}

// sites cut off by ctrl+c, they get counted instead of listed
static bool stopped(const hit_t *x) { return x->state == HIT_UNKNOWN && x->detail && !strcmp(x->detail, "stopped"); }

static void print_pretty(hit_list *h, const char *user, size_t off, const site_list *sites, bool all) {
    size_t n = sites->count, found = 0, missing = 0, unknown = 0, halted = 0;
    for (size_t i = 0; i < n; i++) {
        const hit_t *x = &h->hits[off + i];
        if (stopped(x)) halted++;
        else found += x->state == HIT_FOUND, missing += x->state == HIT_MISSING, unknown += x->state == HIT_UNKNOWN;
    }
    int w = name_width(sites);
    char head[300];
    snprintf(head, sizeof head, "username: %s", user);
    char count[32];
    snprintf(count, sizeof count, "(%zu sites)", n);
    printf("%s %s  %s\n", c_gray("┌─"), c_bold(head), c_gray(count));

    const struct {
        hit_state st;
        const char *label;
        size_t count;
    } sections[] = {{HIT_FOUND, "found", found}, {HIT_UNKNOWN, "couldn't check", unknown}, {HIT_MISSING, "not found", missing}};
    for (int s = 0; s < 3; s++) {
        if (!sections[s].count || (sections[s].st == HIT_MISSING && !all)) continue;
        char label[64];
        snprintf(label, sizeof label, "%s (%zu)", sections[s].label, sections[s].count);
        printf("%s\n%s  %s\n", BAR, BAR,
               sections[s].st == HIT_FOUND ? c_red(label) : sections[s].st == HIT_UNKNOWN ? c_yellow(label) : c_gray(label));
        for (size_t i = 0; i < n; i++) {
            hit_t *x = &h->hits[off + i];
            if (x->state != sections[s].st || stopped(x)) continue;
            int pad = w - (int)utf8_width(x->site->name);
            const char *extra = x->detail ? x->detail : x->url;
            printf("%s   %s%*s  %s\n", BAR, x->state == HIT_FOUND ? c_bold(x->site->name) : c_gray(x->site->name),
                   pad > 0 ? pad : 0, "", x->state == HIT_FOUND ? extra : c_gray(extra ? extra : ""));
        }
    }
    if (!found) printf("%s\n%s  %s\n", BAR, BAR, c_gray("no accounts found"));
    printf("%s\n", c_gray("└─"));

    char a[64], b[64], c[64];
    snprintf(a, sizeof a, "%zu found", found);
    snprintf(b, sizeof b, "%zu not found", missing);
    snprintf(c, sizeof c, "%zu couldn't check", unknown);
    printf("%s %s%s%s%s%s", c_gray("──"), c_red(a), c_gray(" · "), b, c_gray(" · "), c_yellow(c));
    if (halted) printf("%s%zu stopped", c_gray(" · "), halted);
    printf("\n\n");
}

// a name nobody will ever register
static void gibberish(char *out, size_t n) {
    static const char cs[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    srand((unsigned)time(NULL) ^ platform_pid());
    size_t k = (size_t)snprintf(out, n, "odinx");
    for (; k + 1 < n && k < 17; k++) out[k] = cs[rand() % (sizeof cs - 1)];
    out[k] = 0;
}

// checks each site with a name that exists and one that can't, so you know which rules went stale
static int selftest(const site_list *sites, long timeout, bool json) {
    site_list real = {0};
    char fake[24];
    gibberish(fake, sizeof fake);

    // run each site twice: once with its known-good name, once with gibberish
    size_t n = 0;
    for (size_t i = 0; i < sites->count; i++) n += sites->items[i].claimed != NULL;
    hit_list good = {0}, bad = {0};
    char **names = calloc(n ? n : 1, sizeof *names);
    for (size_t i = 0; i < sites->count; i++) {
        if (!sites->items[i].claimed) continue;
        VEC_PUSH(real.items, real.count, real.cap);
        real.items[real.count - 1] = sites->items[i];
        names[real.count - 1] = sites->items[i].claimed;
    }
    if (!real.count) {
        fputs("odin: none of these sites have a known username to test with\n", stderr);
        free(names), free(real.items);
        return 1;
    }

    // both runs go fully parallel: every site with a fake name, every site with its known real one
    char **fakes = calloc(real.count, sizeof *fakes);
    // the site lists name a username known not to exist there, else make one up
    for (size_t i = 0; i < real.count; i++) fakes[i] = real.items[i].unclaimed ? real.items[i].unclaimed : fake;
    check_pairs(&real, fakes, timeout, progress, &bad);
    check_pairs(&real, names, timeout, progress, &good);
    free(fakes);

    int broken = 0, unsure = 0;
    int w = name_width(&real);
    if (json) puts("[");
    for (size_t i = 0; i < real.count; i++) {
        hit_t *g = &good.hits[i], *b = &bad.hits[i];
        const char *verdict, *why = "";
        if (g->state == HIT_UNKNOWN || b->state == HIT_UNKNOWN) {
            verdict = "unsure", unsure++;
            why = g->state == HIT_UNKNOWN ? g->detail : b->detail;
        } else if (g->state == HIT_FOUND && b->state == HIT_MISSING) {
            verdict = "ok";
        } else {
            verdict = "broken", broken++;
            why = g->state != HIT_FOUND ? "real account shows as missing" : "fake name shows as found (false positive)";
        }
        if (json) {
            printf("  {\"site\": "), jstr(real.items[i].name);
            printf(", \"verdict\": \"%s\", \"detail\": ", verdict), jstr(why ? why : "");
            printf("}%s\n", i + 1 < real.count ? "," : "");
            continue;
        }
        const char *tag = !strcmp(verdict, "ok") ? c_cyan("  ok  ") : !strcmp(verdict, "broken") ? c_red("BROKEN") : c_yellow("unsure");
        int pad = w - (int)utf8_width(real.items[i].name);
        printf("  %s  %s%*s  %s\n", tag, real.items[i].name, pad > 0 ? pad : 0, "", c_gray(why ? why : ""));
    }
    if (json) puts("]");
    else {
        char b1[32], b2[32];
        snprintf(b1, sizeof b1, "%d broken", broken);
        snprintf(b2, sizeof b2, "%d unsure", unsure);
        printf("\n%s %zu sites tested · %s · %s\n", c_gray("──"), real.count, c_red(b1), c_yellow(b2));
    }

    hits_free(&good), hits_free(&bad);
    free(names), free(real.items);
    return broken ? 1 : 0;
}

static int flag_needs_value(const char *f, int i, int argc) {
    if (i + 1 >= argc) {
        fprintf(stderr, "odin: %s needs a value\n", f);
        return 1;
    }
    return 0;
}

int run_user(int argc, char **argv) {
    user_flags fl = {.timeout = 15};
    char **users = calloc((size_t)argc + 1, sizeof *users);
    int nusers = 0;

    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1]) {
            users[nusers++] = argv[i];
            continue;
        }
        if (!strcmp(a, "-a") || !strcmp(a, "--all")) fl.all = true;
        else if (!strcmp(a, "-j") || !strcmp(a, "--json")) fl.json = true;
        else if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) fl.quiet = true;
        else if (!strcmp(a, "--no-color")) color_on = false;
        else if (!strcmp(a, "--list")) fl.list = true;
        else if (!strcmp(a, "--selftest")) fl.selftest = true;
        else if (!strcmp(a, "-t") || !strcmp(a, "--timeout")) {
            if (flag_needs_value(a, i, argc)) goto fail;
            fl.timeout = strtol(argv[++i], NULL, 10);
            if (fl.timeout < 1 || fl.timeout > 300) {
                fputs("odin: timeout has to be 1-300 seconds\n", stderr);
                goto fail;
            }
        } else if (!strcmp(a, "--sites")) {
            if (flag_needs_value(a, i, argc)) goto fail;
            fl.sites_file = argv[++i];
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            fputs(usage, stdout);
            free(users);
            return 0;
        } else {
            fprintf(stderr, "odin: unknown flag %s\n", a);
            goto fail;
        }
    }
    if (fl.json) color_on = false;

    site_list sites = {0};
    if (fl.sites_file) {
        char err[512];
        if (!sites_load_file(&sites, fl.sites_file, err, sizeof err)) {
            fprintf(stderr, "odin: %s\n", err);
            sites_free(&sites);
            goto fail;
        }
    } else {
        sites_load_default(&sites);
    }

    if (fl.list) {
        int w = name_width(&sites);
        for (size_t i = 0; i < sites.count; i++)
            printf("%s%*s  %s\n", sites.items[i].name, w - (int)utf8_width(sites.items[i].name), "",
                   c_gray(sites.items[i].profile));
        printf("\n%zu sites\n", sites.count);
        if (!fl.sites_file) printf("%s\n", c_gray(sites_builtin_credit));
        sites_free(&sites);
        free(users);
        return 0;
    }
    if (!nusers && !fl.selftest) {
        fprintf(stderr, "odin: give me at least one username\n\n%s", usage);
        sites_free(&sites);
        goto fail;
    }
    for (int u = 0; u < nusers; u++) {
        if (strlen(users[u]) > 100) {
            fputs("odin: that username is way too long\n", stderr);
            sites_free(&sites);
            goto fail;
        }
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    show_progress = !fl.json && platform_is_tty(2);
    if (!fl.json && !fl.quiet) banner_print();

    int rc = 0;
    if (fl.selftest) {
        rc = selftest(&sites, fl.timeout, fl.json);
    } else {
        hit_list hits = {0};
        check_all(&sites, users, nusers, fl.timeout, progress, &hits);
        if (show_progress) fputs("\r\x1b[K", stderr); // clear the progress line even if ctrl+c cut it short
        if (fl.json) print_json(&hits, users, nusers, sites.count);
        else
            for (int u = 0; u < nusers; u++) print_pretty(&hits, users[u], (size_t)u * sites.count, &sites, fl.all);
        hits_free(&hits);
    }
    curl_global_cleanup();
    sites_free(&sites);
    free(users);
    return rc;

fail:
    free(users);
    return 2;
}

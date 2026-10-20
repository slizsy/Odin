#include "modules/domain/domain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/dns.h"
#include "core/json.h"
#include "core/net.h"
#include "core/util.h"
#include "modules/usage.h"
#include "ui/banner.h"
#include "ui/color.h"

#define BAR c_gray("│")
#define MAX_SUBS_SHOWN 40

typedef struct {
    bool json, quiet, subs;
    long timeout_unused;
} dom_flags;

// turn whatever the user pasted (a url, user@host, HTTPS://..., trailing dot) into a bare
// lowercased hostname
static void normalize(const char *in, char *out, size_t outlen) {
    const char *p = in;
    const char *scheme = strstr(p, "://");
    if (scheme) p = scheme + 3;
    const char *at = strchr(p, '@'); // e.g. an email: keep the domain part
    if (at) p = at + 1;
    if (!strncmp(p, "*.", 2)) p += 2; // wildcard cert name
    size_t i = 0;
    for (; p[i] && i + 1 < outlen; i++) {
        char c = p[i];
        if (c == '/' || c == ':' || c == '?' || c == '#') break; // path/port/query
        out[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    }
    while (i > 0 && out[i - 1] == '.') i--; // trailing dot
    out[i] = 0;
}

// ---- dns ----

static void dns_line(const char *label, int type, const char *domain, sbuf *jptr, const char *jkey) {
    dns_answer a = dns_query(domain, type);
    if (jptr) {
        sb_printf(jptr, "    \"%s\": [", jkey);
        for (size_t i = 0; i < a.n; i++) {
            if (i) sb_putc(jptr, ',');
            // crude json string escape for record data
            sb_putc(jptr, '"');
            for (const char *c = a.data[i]; *c; c++) {
                if (*c == '"' || *c == '\\') sb_putc(jptr, '\\');
                sb_putc(jptr, *c);
            }
            sb_putc(jptr, '"');
        }
        sb_puts(jptr, "],\n");
    } else {
        if (!a.ok) {
            printf("%s   %s%s\n", BAR, c_gray(label), c_yellow("lookup failed"));
        } else if (a.n == 0) {
            printf("%s   %s%s\n", BAR, c_gray(label), c_gray("-"));
        } else {
            for (size_t i = 0; i < a.n; i++)
                printf("%s   %s%s\n", BAR, c_gray(i == 0 ? label : "             "), a.data[i]);
        }
    }
    dns_answer_free(&a);
}

// pull one tag (like "p") out of a dmarc/spf-ish record
static void tag_value(const char *rec, const char *tag, char *out, size_t outlen) {
    out[0] = 0;
    size_t tl = strlen(tag);
    for (const char *p = rec; *p; p++) {
        if ((p == rec || p[-1] == ';' || p[-1] == ' ') && !strncmp(p, tag, tl) && p[tl] == '=') {
            p += tl + 1;
            size_t i = 0;
            while (p[i] && p[i] != ';' && p[i] != ' ' && i + 1 < outlen) out[i] = p[i], i++;
            out[i] = 0;
            return;
        }
    }
}

// ---- rdap (registration record) ----

typedef struct {
    bool ok;
    char *registrar, *created, *expires, *updated, *status, *dnssec;
    char **ns;
    size_t nns;
} rdap_info;

static const char *vcard_fn(json *entity) {
    json *va = json_get(entity, "vcardArray");
    if (!va || va->t != J_ARR || va->len < 2) return NULL;
    json *props = va->items[1];
    if (props->t != J_ARR) return NULL;
    for (size_t i = 0; i < props->len; i++) {
        json *p = props->items[i];
        if (p->t == J_ARR && p->len >= 4 && p->items[0]->t == J_STR && !strcmp(p->items[0]->s, "fn") &&
            p->items[3]->t == J_STR)
            return p->items[3]->s;
    }
    return NULL;
}

static const char *registrar_name(json *root) {
    json *ents = json_get(root, "entities");
    if (!ents || ents->t != J_ARR) return NULL;
    for (size_t i = 0; i < ents->len; i++) {
        json *e = ents->items[i];
        json *roles = json_get(e, "roles");
        if (!roles || roles->t != J_ARR) continue;
        for (size_t r = 0; r < roles->len; r++)
            if (roles->items[r]->t == J_STR && !strcmp(roles->items[r]->s, "registrar")) return vcard_fn(e);
    }
    return NULL;
}

static char *event_date(json *root, const char *action) {
    json *evs = json_get(root, "events");
    if (!evs || evs->t != J_ARR) return NULL;
    for (size_t i = 0; i < evs->len; i++) {
        const char *a = json_str(evs->items[i], "eventAction");
        const char *d = json_str(evs->items[i], "eventDate");
        if (a && d && !strcmp(a, action)) return xstrdup(d);
    }
    return NULL;
}

static rdap_info rdap_lookup(const char *domain) {
    rdap_info out = {0};
    const char *base = getenv("ODIN_RDAP_URL");
    if (!base) base = "https://rdap.org";
    char url[1024];
    snprintf(url, sizeof url, "%s/domain/%s", base, domain);
    const char *hdrs[] = {"Accept: application/rdap+json", NULL};
    long status = 0;
    char err[256];
    json *root = net_get_json(url, hdrs, 1 << 20, &status, err, sizeof err);
    if (!root) return out;
    out.ok = true;
    const char *reg = registrar_name(root);
    if (reg) out.registrar = xstrdup(reg);
    out.created = event_date(root, "registration");
    out.expires = event_date(root, "expiration");
    out.updated = event_date(root, "last changed");

    json *st = json_get(root, "status");
    if (st && st->t == J_ARR && st->len) {
        sbuf b;
        sb_init(&b);
        for (size_t i = 0; i < st->len; i++)
            if (st->items[i]->t == J_STR) sb_printf(&b, "%s%s", i ? ", " : "", st->items[i]->s);
        out.status = sb_take(&b);
    }
    json *sec = json_get(root, "secureDNS");
    if (sec) {
        json *ds = json_get(sec, "delegationSigned");
        if (ds && ds->t == J_BOOL) out.dnssec = xstrdup(ds->b ? "signed" : "unsigned");
    }
    json *ns = json_get(root, "nameservers");
    if (ns && ns->t == J_ARR) {
        size_t cap = 0;
        for (size_t i = 0; i < ns->len; i++) {
            const char *n = json_str(ns->items[i], "ldhName");
            if (!n) continue;
            VEC_PUSH(out.ns, out.nns, cap);
            out.ns[out.nns - 1] = xstrdup(n);
        }
    }
    json_free(root);
    return out;
}

static void rdap_free(rdap_info *r) {
    free(r->registrar), free(r->created), free(r->expires), free(r->updated), free(r->status), free(r->dnssec);
    for (size_t i = 0; i < r->nns; i++) free(r->ns[i]);
    free(r->ns);
}

// ---- crt.sh subdomain enumeration ----

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

// returns a sorted, de-duped list of hostnames seen in certificate transparency logs
static char **crtsh_subs(const char *domain, size_t *count, bool *ok) {
    *count = 0;
    *ok = false;
    const char *base = getenv("ODIN_CRTSH_URL");
    if (!base) base = "https://crt.sh";
    char url[1024];
    snprintf(url, sizeof url, "%s/?q=%%25.%s&output=json", base, domain);
    const char *hdrs[] = {"Accept: application/json", NULL};
    long status = 0;
    char err[256];
    json *root = net_get_json(url, hdrs, 24u << 20, &status, err, sizeof err); // CT dumps get big
    if (!root || root->t != J_ARR) {
        if (root) json_free(root);
        return NULL;
    }
    *ok = true;
    char **all = NULL;
    size_t n = 0, cap = 0;
    size_t dlen = strlen(domain);
    for (size_t i = 0; i < root->len; i++) {
        const char *nv = json_str(root->items[i], "name_value");
        if (!nv) continue;
        // name_value can hold several names split by newlines
        const char *s = nv;
        while (*s) {
            const char *nl = strchr(s, '\n');
            size_t len = nl ? (size_t)(nl - s) : strlen(s);
            char host[256];
            if (len && len < sizeof host) {
                memcpy(host, s, len);
                host[len] = 0;
                for (char *c = host; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
                char *h = host;
                if (!strncmp(h, "*.", 2)) h += 2; // collapse wildcards to the bare host
                size_t hl = strlen(h);
                // keep only names actually under the domain
                if (hl >= dlen && !strcmp(h + hl - dlen, domain) && (hl == dlen || h[hl - dlen - 1] == '.')) {
                    VEC_PUSH(all, n, cap);
                    all[n - 1] = xstrdup(h);
                }
            }
            if (!nl) break;
            s = nl + 1;
        }
    }
    json_free(root);
    if (!n) {
        free(all);
        return NULL;
    }
    qsort(all, n, sizeof *all, cmp_str);
    // squeeze out duplicates
    size_t w = 1;
    for (size_t i = 1; i < n; i++) {
        if (strcmp(all[i], all[w - 1]) != 0) all[w++] = all[i];
        else free(all[i]);
    }
    *count = w;
    return all;
}

// ---- the command ----

static int domain_report(const char *domain, dom_flags *fl) {
    // grab spf + dmarc up front, they feed both the output and the findings
    char spf[1024] = "", dmarc[1024] = "";
    dns_answer txt = dns_query(domain, DNS_TXT);
    for (size_t i = 0; i < txt.n; i++)
        if (!strncmp(txt.data[i], "v=spf1", 6)) snprintf(spf, sizeof spf, "%s", txt.data[i]);
    char dmarc_name[300];
    snprintf(dmarc_name, sizeof dmarc_name, "_dmarc.%s", domain);
    dns_answer dm = dns_query(dmarc_name, DNS_TXT);
    for (size_t i = 0; i < dm.n; i++)
        if (!strncmp(dm.data[i], "v=DMARC1", 8)) snprintf(dmarc, sizeof dmarc, "%s", dm.data[i]);
    dns_answer_free(&txt);
    dns_answer_free(&dm);

    rdap_info rd = rdap_lookup(domain);
    size_t nsubs = 0;
    bool crt_ok = false;
    char **subs = fl->subs ? crtsh_subs(domain, &nsubs, &crt_ok) : NULL;

    if (fl->json) {
        sbuf j;
        sb_init(&j);
        sb_printf(&j, "{\n  \"domain\": \"%s\",\n  \"dns\": {\n", domain);
        dns_line("", DNS_A, domain, &j, "a");
        dns_line("", DNS_AAAA, domain, &j, "aaaa");
        dns_line("", DNS_MX, domain, &j, "mx");
        dns_line("", DNS_NS, domain, &j, "ns");
        dns_line("", DNS_TXT, domain, &j, "txt");
        dns_line("", DNS_CAA, domain, &j, "caa");
        sb_printf(&j, "    \"spf\": \"%s\",\n", spf);
        // close dns object (strip nothing fancy, just add dmarc)
        sb_printf(&j, "    \"dmarc\": \"%s\"\n  },\n", dmarc);
        sb_printf(&j, "  \"registration\": {\"registrar\": ");
        // reuse json_quote by writing to stdout directly later would break ordering, so hand-quote
        fputs(j.s, stdout);
        sb_free(&j);
        json_quote(rd.registrar ? rd.registrar : "");
        printf(", \"created\": ");
        json_quote(rd.created ? rd.created : "");
        printf(", \"expires\": ");
        json_quote(rd.expires ? rd.expires : "");
        printf(", \"status\": ");
        json_quote(rd.status ? rd.status : "");
        printf(", \"dnssec\": ");
        json_quote(rd.dnssec ? rd.dnssec : "");
        printf("},\n  \"subdomains\": [");
        for (size_t i = 0; i < nsubs; i++) {
            if (i) putchar(',');
            json_quote(subs[i]);
        }
        printf("]\n}\n");
    } else {
        printf("%s %s\n", c_gray("┌─"), c_bold(domain));
        printf("%s %s\n", BAR, c_gray("dns"));
        dns_line("A            ", DNS_A, domain, NULL, NULL);
        dns_line("AAAA         ", DNS_AAAA, domain, NULL, NULL);
        dns_line("MX           ", DNS_MX, domain, NULL, NULL);
        dns_line("NS           ", DNS_NS, domain, NULL, NULL);
        dns_line("CAA          ", DNS_CAA, domain, NULL, NULL);
        printf("%s   %s%s\n", BAR, c_gray("SPF          "), spf[0] ? spf : c_yellow("none (easier to spoof)"));
        printf("%s   %s%s\n", BAR, c_gray("DMARC        "), dmarc[0] ? dmarc : c_yellow("none"));

        printf("%s\n%s %s\n", BAR, BAR, c_gray("registration"));
        if (!rd.ok) {
            printf("%s   %s\n", BAR, c_yellow("no rdap record (odd tld, or the registry was unreachable)"));
        } else {
            if (rd.registrar) printf("%s   %s%s\n", BAR, c_gray("registrar    "), rd.registrar);
            if (rd.created) printf("%s   %s%s\n", BAR, c_gray("created      "), rd.created);
            if (rd.expires) printf("%s   %s%s\n", BAR, c_gray("expires      "), rd.expires);
            if (rd.updated) printf("%s   %s%s\n", BAR, c_gray("updated      "), rd.updated);
            if (rd.dnssec) printf("%s   %s%s\n", BAR, c_gray("dnssec       "), rd.dnssec);
            if (rd.status) printf("%s   %s%s\n", BAR, c_gray("status       "), rd.status);
            for (size_t i = 0; i < rd.nns; i++)
                printf("%s   %s%s\n", BAR, c_gray(i == 0 ? "nameservers  " : "             "), rd.ns[i]);
        }

        if (fl->subs) {
            printf("%s\n%s %s\n", BAR, BAR, c_gray("subdomains (certificate transparency)"));
            if (!crt_ok) {
                printf("%s   %s\n", BAR, c_yellow("crt.sh lookup failed"));
            } else if (nsubs == 0) {
                printf("%s   %s\n", BAR, c_gray("none found"));
            } else {
                size_t show = nsubs < MAX_SUBS_SHOWN ? nsubs : MAX_SUBS_SHOWN;
                for (size_t i = 0; i < show; i++) printf("%s   %s\n", BAR, subs[i]);
                if (nsubs > show) {
                    char more[64];
                    snprintf(more, sizeof more, "... +%zu more", nsubs - show);
                    printf("%s   %s\n", BAR, c_gray(more));
                }
            }
        }
        printf("%s\n", c_gray("└─"));

        // findings
        printf("%s %s\n", c_gray("──"), c_bold("findings"));
        bool any = false;
        if (!spf[0]) printf("   %s mail-spoofing: no SPF record, anyone can forge mail from this domain\n", c_yellow("medium")), any = true;
        char pol[64] = "";
        if (dmarc[0]) tag_value(dmarc, "p", pol, sizeof pol);
        if (!dmarc[0]) printf("   %s mail-spoofing: no DMARC record\n", c_yellow("medium")), any = true;
        else if (!strcmp(pol, "none")) printf("   %s DMARC is p=none (monitoring only, not enforced)\n", c_yellow("medium")), any = true;
        if (rd.dnssec && !strcmp(rd.dnssec, "unsigned")) printf("   %s DNSSEC not enabled\n", c_gray("info")), any = true;
        if (nsubs > 0) {
            char s[96];
            snprintf(s, sizeof s, "%zu subdomain%s exposed in CT logs", nsubs, nsubs == 1 ? "" : "s");
            printf("   %s %s\n", nsubs >= 50 ? c_red("high") : c_gray("info"), s);
            any = true;
        }
        if (!any) printf("   %s\n", c_gray("nothing jumped out"));
        putchar('\n');
    }

    rdap_free(&rd);
    for (size_t i = 0; i < nsubs; i++) free(subs[i]);
    free(subs);
    return 0;
}

int run_domain(int argc, char **argv) {
    dom_flags fl = {.subs = true};
    const char *targets[64];
    int nt = 0;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-j") || !strcmp(a, "--json")) fl.json = true;
        else if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) fl.quiet = true;
        else if (!strcmp(a, "--no-color")) color_on = false;
        else if (!strcmp(a, "--no-subs")) fl.subs = false;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { fputs(usage, stdout); return 0; }
        else if (a[0] == '-' && a[1]) { fprintf(stderr, "odin: unknown flag %s\n", a); return 2; }
        else if (nt < 64) targets[nt++] = a;
    }
    if (fl.json) color_on = false;
    if (!nt) {
        fprintf(stderr, "odin: give me a domain, like example.com\n\n%s", usage);
        return 2;
    }
    if (!fl.json && !fl.quiet) banner_print();
    int rc = 0;
    for (int i = 0; i < nt; i++) {
        char dom[256];
        normalize(targets[i], dom, sizeof dom);
        if (!dom[0] || !strchr(dom, '.')) {
            fprintf(stderr, "odin: \"%s\" doesn't look like a domain\n", targets[i]);
            rc = 2;
            continue;
        }
        domain_report(dom, &fl);
    }
    return rc;
}

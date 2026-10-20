#include "modules/ip/ip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/dns.h"
#include "core/json.h"
#include "core/net.h"
#include "core/util.h"
#include "modules/usage.h"
#include "ui/banner.h"
#include "ui/color.h"

#define BAR c_gray("│")

typedef struct {
    bool json, quiet;
} ip_flags;

// pull a property value (fn, email, ...) out of an rdap vcard
static const char *vcard_get(json *entity, const char *prop) {
    json *va = json_get(entity, "vcardArray");
    if (!va || va->t != J_ARR || va->len < 2) return NULL;
    json *props = va->items[1];
    if (props->t != J_ARR) return NULL;
    for (size_t i = 0; i < props->len; i++) {
        json *p = props->items[i];
        if (p->t == J_ARR && p->len >= 4 && p->items[0]->t == J_STR && !strcmp(p->items[0]->s, prop) &&
            p->items[3]->t == J_STR)
            return p->items[3]->s;
    }
    return NULL;
}

static bool has_role(json *entity, const char *role) {
    json *roles = json_get(entity, "roles");
    if (!roles || roles->t != J_ARR) return false;
    for (size_t i = 0; i < roles->len; i++)
        if (roles->items[i]->t == J_STR && !strcmp(roles->items[i]->s, role)) return true;
    return false;
}

// walk the entity tree (entities nest) looking for one that plays a given role
static json *find_role(json *ents, const char *role) {
    if (!ents || ents->t != J_ARR) return NULL;
    for (size_t i = 0; i < ents->len; i++) {
        json *e = ents->items[i];
        if (has_role(e, role)) return e;
        json *deep = find_role(json_get(e, "entities"), role);
        if (deep) return deep;
    }
    return NULL;
}

typedef struct {
    bool rdap_ok, geo_ok;
    char *net_name, *net_range, *net_type, *rdap_country, *org, *abuse, *cidr;
    char *geo_country, *geo_region, *geo_city, *asn, *asn_org, *isp, *tz;
    char *ptr;
} ip_info;

static char *dup_or_null(const char *s) { return s ? xstrdup(s) : NULL; }

static void gather_rdap(const char *ip, ip_info *o) {
    const char *base = getenv("ODIN_RDAP_URL");
    if (!base) base = "https://rdap.org";
    char url[1024];
    snprintf(url, sizeof url, "%s/ip/%s", base, ip);
    const char *hdrs[] = {"Accept: application/rdap+json", NULL};
    long status = 0;
    char err[256];
    json *root = net_get_json(url, hdrs, 1 << 20, &status, err, sizeof err);
    if (!root) return;
    o->rdap_ok = true;
    o->net_name = dup_or_null(json_str(root, "name"));
    o->net_type = dup_or_null(json_str(root, "type"));
    o->rdap_country = dup_or_null(json_str(root, "country"));
    const char *start = json_str(root, "startAddress"), *end = json_str(root, "endAddress");
    if (start && end) o->net_range = xprintf("%s - %s", start, end);

    json *cidrs = json_get(root, "cidr0_cidrs");
    if (cidrs && cidrs->t == J_ARR && cidrs->len) {
        json *c = cidrs->items[0];
        const char *pfx = json_str(c, "v4prefix");
        if (!pfx) pfx = json_str(c, "v6prefix");
        double len = 0;
        if (pfx && json_num(c, "length", &len)) o->cidr = xprintf("%s/%d", pfx, (int)len);
    }
    json *ents = json_get(root, "entities");
    json *reg = find_role(ents, "registrant");
    if (!reg) reg = find_role(ents, "administrative");
    if (reg) {
        const char *fn = vcard_get(reg, "fn");
        o->org = dup_or_null(fn);
    }
    json *ab = find_role(ents, "abuse");
    if (ab) o->abuse = dup_or_null(vcard_get(ab, "email"));
    json_free(root);
}

static void gather_geo(const char *ip, ip_info *o) {
    const char *base = getenv("ODIN_IPINFO_URL");
    char url[1024];
    if (base) snprintf(url, sizeof url, "%s/%s", base, ip);
    else snprintf(url, sizeof url, "https://ipwho.is/%s", ip);
    long status = 0;
    char err[256];
    json *root = net_get_json(url, NULL, 1 << 20, &status, err, sizeof err);
    if (!root) return;
    json *ok = json_get(root, "success");
    if (ok && ok->t == J_BOOL && !ok->b) { json_free(root); return; }
    o->geo_ok = true;
    o->geo_country = dup_or_null(json_str(root, "country"));
    o->geo_region = dup_or_null(json_str(root, "region"));
    o->geo_city = dup_or_null(json_str(root, "city"));
    json *conn = json_get(root, "connection");
    if (conn) {
        double asn = 0;
        if (json_num(conn, "asn", &asn)) o->asn = xprintf("AS%d", (int)asn);
        o->asn_org = dup_or_null(json_str(conn, "org"));
        o->isp = dup_or_null(json_str(conn, "isp"));
    }
    json *tz = json_get(root, "timezone");
    if (tz) o->tz = dup_or_null(json_str(tz, "id"));
    json_free(root);
}

static void gather_ptr(const char *ip, ip_info *o) {
    char rev[128];
    if (!dns_reverse_name(ip, rev, sizeof rev)) return;
    dns_answer a = dns_query(rev, DNS_PTR);
    if (a.ok && a.n) o->ptr = xstrdup(a.data[0]);
    dns_answer_free(&a);
}

static void info_free(ip_info *o) {
    char **all[] = {&o->net_name, &o->net_range, &o->net_type, &o->rdap_country, &o->org, &o->abuse, &o->cidr,
                    &o->geo_country, &o->geo_region, &o->geo_city, &o->asn, &o->asn_org, &o->isp, &o->tz, &o->ptr};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) free(*all[i]);
}

// rfc1918 / loopback / link-local etc, where a public lookup won't tell you anything useful
static const char *private_range(const char *ip) {
    unsigned o0, o1, o2, o3;
    if (sscanf(ip, "%u.%u.%u.%u", &o0, &o1, &o2, &o3) == 4) {
        if (o0 == 10) return "private (10.0.0.0/8)";
        if (o0 == 172 && o1 >= 16 && o1 <= 31) return "private (172.16.0.0/12)";
        if (o0 == 192 && o1 == 168) return "private (192.168.0.0/16)";
        if (o0 == 127) return "loopback";
        if (o0 == 169 && o1 == 254) return "link-local";
        if (o0 == 100 && o1 >= 64 && o1 <= 127) return "carrier-grade NAT";
    }
    if (!strcmp(ip, "::1")) return "loopback";
    if (!strncasecmp(ip, "fe80:", 5)) return "link-local";
    if (!strncasecmp(ip, "fc", 2) || !strncasecmp(ip, "fd", 2)) return "unique local";
    return NULL;
}

static void row(const char *k, const char *v) {
    if (v) printf("%s   %s%s\n", BAR, c_gray(k), v);
}

static void print_json(const char *ip, const ip_info *o) {
    printf("{\n  \"ip\": ");
    json_quote(ip);
    printf(",\n  \"ptr\": ");
    json_quote(o->ptr ? o->ptr : "");
    printf(",\n  \"network\": {\"name\": ");
    json_quote(o->net_name ? o->net_name : "");
    printf(", \"cidr\": ");
    json_quote(o->cidr ? o->cidr : "");
    printf(", \"range\": ");
    json_quote(o->net_range ? o->net_range : "");
    printf(", \"org\": ");
    json_quote(o->org ? o->org : "");
    printf(", \"abuse\": ");
    json_quote(o->abuse ? o->abuse : "");
    printf("},\n  \"geo\": {\"country\": ");
    json_quote(o->geo_country ? o->geo_country : "");
    printf(", \"region\": ");
    json_quote(o->geo_region ? o->geo_region : "");
    printf(", \"city\": ");
    json_quote(o->geo_city ? o->geo_city : "");
    printf(", \"asn\": ");
    json_quote(o->asn ? o->asn : "");
    printf(", \"asn_org\": ");
    json_quote(o->asn_org ? o->asn_org : "");
    printf("}\n}\n");
}

int run_ip(int argc, char **argv) {
    ip_flags fl = {0};
    const char *targets[64];
    int nt = 0;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-j") || !strcmp(a, "--json")) fl.json = true;
        else if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) fl.quiet = true;
        else if (!strcmp(a, "--no-color")) color_on = false;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { fputs(usage, stdout); return 0; }
        else if (a[0] == '-' && a[1]) { fprintf(stderr, "odin: unknown flag %s\n", a); return 2; }
        else if (nt < 64) targets[nt++] = a;
    }
    if (fl.json) color_on = false;
    if (!nt) {
        fprintf(stderr, "odin: give me an ip address, like 8.8.8.8\n\n%s", usage);
        return 2;
    }
    if (!fl.json && !fl.quiet) banner_print();

    int rc = 0;
    for (int t = 0; t < nt; t++) {
        const char *ip = targets[t];
        unsigned char v6[16];
        if (!is_ipv4(ip) && !ipv6_bytes(ip, v6)) {
            fprintf(stderr, "odin: \"%s\" isn't a valid ip address\n", ip);
            rc = 2;
            continue;
        }
        ip_info o = {0};
        const char *priv = private_range(ip);
        if (!priv) {
            gather_rdap(ip, &o);
            gather_geo(ip, &o);
        }
        gather_ptr(ip, &o);

        if (fl.json) {
            print_json(ip, &o);
        } else {
            printf("%s %s\n", c_gray("┌─"), c_bold(ip));
            if (priv) {
                printf("%s   %s\n%s\n\n", BAR, c_yellow(priv), c_gray("└─"));
                info_free(&o);
                continue;
            }
            row("ptr          ", o.ptr);
            printf("%s %s\n", BAR, c_gray("network"));
            row("name         ", o.net_name);
            row("cidr         ", o.cidr);
            row("range        ", o.net_range);
            row("type         ", o.net_type);
            row("org          ", o.org);
            row("abuse        ", o.abuse);
            row("rir country  ", o.rdap_country);
            if (!o.rdap_ok) printf("%s   %s\n", BAR, c_yellow("no rdap record"));
            printf("%s %s\n", BAR, c_gray("geo / network path"));
            row("country      ", o.geo_country);
            row("region       ", o.geo_region);
            row("city         ", o.geo_city);
            row("asn          ", o.asn);
            row("asn org      ", o.asn_org);
            row("isp          ", o.isp);
            row("timezone     ", o.tz);
            if (!o.geo_ok) printf("%s   %s\n", BAR, c_yellow("geo lookup failed"));
            printf("%s\n\n", c_gray("└─"));
        }
        info_free(&o);
    }
    return rc;
}

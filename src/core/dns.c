#include "core/dns.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/json.h"
#include "core/net.h"
#include "core/util.h"

// where to send dns-over-https queries. ODIN_DOH_URL overrides both (used by the tests to
// point at a fake local resolver); otherwise cloudflare first, google if that falls over
static const char *doh_primary(void) {
    const char *o = getenv("ODIN_DOH_URL");
    return o ? o : "https://cloudflare-dns.com/dns-query";
}
static const char *doh_backup(void) {
    return getenv("ODIN_DOH_URL") ? NULL : "https://dns.google/resolve";
}

// percent-encode anything that isn't safe in a query value. dns names only ever have
// letters, digits, dot, hyphen and underscore, so this stays tiny
static char *url_escape(const char *s) {
    sbuf b;
    sb_init(&b);
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
            c == '_')
            sb_putc(&b, (char)c);
        else
            sb_printf(&b, "%%%02X", c);
    }
    return sb_take(&b);
}

// txt records arrive as one or more quoted chunks ("a" "b"); drop the quotes and glue them
static char *clean_txt(const char *s) {
    sbuf b;
    sb_init(&b);
    for (const char *p = s; *p; p++) {
        if (*p == '"') continue;
        if (*p == '\\' && p[1]) p++; // keep the escaped char, lose the backslash
        sb_putc(&b, *p);
    }
    return sb_take(&b);
}

static bool fetch(const char *base, const char *name_esc, int type, dns_answer *a) {
    char url[1024];
    const char *sep = strchr(base, '?') ? "&" : "?";
    snprintf(url, sizeof url, "%s%sname=%s&type=%d", base, sep, name_esc, type);
    const char *hdrs[] = {"Accept: application/dns-json", NULL};
    sbuf body;
    sb_init(&body);
    long status = 0;
    char err[256];
    if (!net_get(url, hdrs, 1 << 20, &status, &body, err, sizeof err)) {
        snprintf(a->err, sizeof a->err, "%s", err);
        sb_free(&body);
        return false;
    }
    if (status != 200) {
        snprintf(a->err, sizeof a->err, "resolver returned http %ld", status);
        sb_free(&body);
        return false;
    }
    json *root = json_parse(body.s, body.len);
    sb_free(&body);
    if (!root) {
        snprintf(a->err, sizeof a->err, "resolver sent back junk");
        return false;
    }
    double st = 0;
    json_num(root, "Status", &st);
    a->rcode = (int)st;
    a->ok = true;
    json *ans = json_get(root, "Answer");
    if (ans && ans->t == J_ARR) {
        size_t cap = 0;
        for (size_t i = 0; i < ans->len; i++) {
            json *rec = ans->items[i];
            double rt = 0;
            json_num(rec, "type", &rt);
            if ((int)rt != type) continue; // skip CNAMEs mixed into the answer
            const char *d = json_str(rec, "data");
            if (!d) continue;
            VEC_PUSH(a->data, a->n, cap);
            a->data[a->n - 1] = type == DNS_TXT ? clean_txt(d) : xstrdup(d);
        }
    }
    json_free(root);
    return true;
}

dns_answer dns_query(const char *name, int type) {
    dns_answer a = {0};
    char *esc = url_escape(name);
    if (!fetch(doh_primary(), esc, type, &a)) {
        const char *bk = doh_backup();
        if (bk) {
            dns_answer_free(&a);
            memset(&a, 0, sizeof a);
            fetch(bk, esc, type, &a);
        }
    }
    free(esc);
    return a;
}

void dns_answer_free(dns_answer *a) {
    for (size_t i = 0; i < a->n; i++) free(a->data[i]);
    free(a->data);
    a->data = NULL;
    a->n = 0;
}

bool dns_reverse_name(const char *ip, char *out, size_t outlen) {
    // ipv4: reverse the 4 octets + .in-addr.arpa
    unsigned o[4];
    if (sscanf(ip, "%u.%u.%u.%u", &o[0], &o[1], &o[2], &o[3]) == 4 && o[0] < 256 && o[1] < 256 && o[2] < 256 &&
        o[3] < 256) {
        snprintf(out, outlen, "%u.%u.%u.%u.in-addr.arpa", o[3], o[2], o[1], o[0]);
        return true;
    }
    // ipv6: expand to 32 nibbles, reverse them, dot-separated + .ip6.arpa
    if (strchr(ip, ':')) {
        unsigned char full[16];
        if (!ipv6_bytes(ip, full)) return false;
        sbuf b;
        sb_init(&b);
        for (int i = 15; i >= 0; i--) {
            sb_printf(&b, "%x.%x.", full[i] & 0xf, (full[i] >> 4) & 0xf);
        }
        sb_puts(&b, "ip6.arpa");
        char *s = sb_take(&b);
        snprintf(out, outlen, "%s", s);
        free(s);
        return true;
    }
    return false;
}

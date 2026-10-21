#ifndef ODIN_DNS_H
#define ODIN_DNS_H

#include <stdbool.h>
#include <stddef.h>

// dns record types we ask about (same numbers the wire format uses)
enum { DNS_A = 1, DNS_NS = 2, DNS_CNAME = 5, DNS_SOA = 6, DNS_PTR = 12, DNS_MX = 15, DNS_TXT = 16, DNS_AAAA = 28, DNS_CAA = 257 };

typedef struct {
    bool ok;      // the lookup itself worked (reached a resolver, got valid json)
    int rcode;    // dns response code: 0 = fine, 3 = no such name (NXDOMAIN)
    char **data;  // one string per answer, already cleaned up (txt quotes stripped etc)
    size_t n;
    char err[256]; // why ok is false
} dns_answer;

// resolve name/type over dns-over-https (cloudflare, google as backup). never blocks on a
// system resolver, so it behaves the same everywhere and works through the https proxy
dns_answer dns_query(const char *name, int type);
void dns_answer_free(dns_answer *a);

// build the reversed .in-addr.arpa / .ip6.arpa name for a PTR lookup. false if ip is junk.
bool dns_reverse_name(const char *ip, char *out, size_t outlen);

#endif

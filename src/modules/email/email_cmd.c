#include "modules/email/email.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/dns.h"
#include "core/json.h"
#include "core/net.h"
#include "core/sha256.h"
#include "core/util.h"
#include "modules/usage.h"
#include "ui/banner.h"
#include "ui/color.h"

#define BAR c_gray("│")

typedef struct {
    bool json, quiet;
} email_flags;

// mailboxes that belong to a function, not a person
static const char *ROLE[] = {"admin", "administrator", "info", "support", "sales", "contact", "hello", "noreply",
                             "no-reply", "donotreply", "postmaster", "webmaster", "hostmaster", "abuse", "security",
                             "billing", "help", "team", "office", "mail", "marketing", "hr", "jobs", "careers",
                             "root", "sysadmin", "privacy", "legal", "press", NULL};

static int cmp_key(const void *k, const void *e) { return strcmp((const char *)k, *(const char *const *)e); }

static bool is_disposable(const char *domain) {
    return bsearch(domain, disposable_domains, disposable_domains_n, sizeof disposable_domains[0], cmp_key) != NULL;
}

static bool is_role(const char *local) {
    for (int i = 0; ROLE[i]; i++)
        if (!strcasecmp(local, ROLE[i])) return true;
    return false;
}

// a pragmatic rfc5322-ish check: one @, a sane local part, a domain with a dot and no junk
static bool valid_syntax(const char *local, const char *domain) {
    size_t ll = strlen(local), dl = strlen(domain);
    if (ll == 0 || ll > 64 || dl == 0 || dl > 255) return false;
    if (local[0] == '.' || local[ll - 1] == '.' || strstr(local, "..")) return false;
    for (const char *p = local; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              strchr(".!#$%&'*+/=?^_`{|}~-", c)))
            return false;
    }
    if (!strchr(domain, '.') || domain[0] == '.' || domain[dl - 1] == '.' || strstr(domain, "..")) return false;
    for (const char *p = domain; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-'))
            return false;
    }
    return true;
}

// gravatar: hash the normalized email, fetch the public profile json if there is one.
// returns a malloc'd "display name" (or empty string) when a profile exists, else NULL
static char *gravatar(const char *email, char **profile_url) {
    char norm[320];
    snprintf(norm, sizeof norm, "%s", email);
    for (char *p = norm; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    char hex[65];
    sha256_hex((const unsigned char *)norm, strlen(norm), hex);

    const char *base = getenv("ODIN_GRAVATAR_URL");
    if (!base) base = "https://gravatar.com";
    char url[512];
    snprintf(url, sizeof url, "%s/%s.json", base, hex);
    const char *hdrs[] = {"Accept: application/json", NULL};
    long status = 0;
    char err[256];
    json *root = net_get_json(url, hdrs, 1 << 20, &status, err, sizeof err);
    if (!root) return NULL;
    char *name = NULL;
    json *entry = json_get(root, "entry");
    if (entry && entry->t == J_ARR && entry->len) {
        json *e = entry->items[0];
        const char *dn = json_str(e, "displayName");
        name = xstrdup(dn ? dn : "");
        if (profile_url) {
            const char *pu = json_str(e, "profileUrl");
            *profile_url = pu ? xstrdup(pu) : NULL;
        }
    }
    json_free(root);
    return name;
}

static int email_report(const char *raw, email_flags *fl) {
    // split on the last @
    char addr[320];
    snprintf(addr, sizeof addr, "%s", raw);
    char *at = strrchr(addr, '@');
    char local[320] = "", domain[320] = "";
    if (at) {
        *at = 0;
        snprintf(local, sizeof local, "%s", addr);
        snprintf(domain, sizeof domain, "%s", at + 1);
        for (char *p = domain; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    }

    bool syntax = at && valid_syntax(local, domain);
    bool disp = syntax && is_disposable(domain);
    bool role = syntax && is_role(local);

    // does the domain accept mail? MX first, then an A record as the fallback mail route
    int mx = 0;
    bool deliverable = false, dns_checked = false;
    if (syntax) {
        dns_answer m = dns_query(domain, DNS_MX);
        dns_checked = m.ok;
        mx = (int)m.n;
        if (m.ok && m.n) deliverable = true;
        dns_answer_free(&m);
        if (dns_checked && !deliverable) {
            dns_answer a = dns_query(domain, DNS_A);
            if (a.ok && a.n) deliverable = true;
            dns_answer_free(&a);
        }
    }

    char *prof_url = NULL;
    char *grav = syntax ? gravatar(raw, &prof_url) : NULL;

    if (fl->json) {
        printf("{\n  \"email\": ");
        json_quote(raw);
        printf(",\n  \"valid_syntax\": %s", syntax ? "true" : "false");
        printf(",\n  \"local\": ");
        json_quote(local);
        printf(",\n  \"domain\": ");
        json_quote(domain);
        printf(",\n  \"disposable\": %s", disp ? "true" : "false");
        printf(",\n  \"role_account\": %s", role ? "true" : "false");
        printf(",\n  \"mx_records\": %d", mx);
        printf(",\n  \"deliverable\": %s", deliverable ? "true" : "false");
        printf(",\n  \"gravatar\": %s", grav ? "true" : "false");
        printf(",\n  \"gravatar_name\": ");
        json_quote(grav ? grav : "");
        printf("\n}\n");
        free(grav), free(prof_url);
        return syntax ? 0 : 1;
    }

    printf("%s %s\n", c_gray("┌─"), c_bold(raw));
    if (!syntax) {
        printf("%s   %s\n%s\n\n", BAR, c_red("not a valid email address"), c_gray("└─"));
        free(grav), free(prof_url);
        return 1;
    }
    printf("%s   %s%s\n", BAR, c_gray("local part   "), local);
    printf("%s   %s%s\n", BAR, c_gray("domain       "), domain);
    printf("%s   %s%s\n", BAR, c_gray("deliverable  "),
           !dns_checked ? c_yellow("couldn't check (dns lookup failed)")
           : deliverable ? (mx ? "yes (has MX records)" : "maybe (no MX, but domain resolves)")
                         : c_yellow("no (domain can't receive mail)"));
    printf("%s   %s%s\n", BAR, c_gray("disposable   "), disp ? c_yellow("yes, throwaway provider") : "no");
    printf("%s   %s%s\n", BAR, c_gray("role account "), role ? "yes (not a person)" : "no");
    printf("%s   %s%s\n", BAR, c_gray("gravatar     "),
           grav ? (grav[0] ? grav : "yes (profile exists)") : c_gray("none"));
    if (grav && prof_url) printf("%s   %s%s\n", BAR, c_gray("profile      "), prof_url);
    printf("%s\n", c_gray("└─"));

    printf("%s %s\n", c_gray("──"), c_bold("findings"));
    bool any = false;
    if (disp) printf("   %s disposable address, likely a throwaway\n", c_yellow("medium")), any = true;
    if (dns_checked && !deliverable) printf("   %s domain can't receive mail, probably fake or a typo\n", c_yellow("medium")), any = true;
    if (role) printf("   %s role mailbox, belongs to a team not a person\n", c_gray("info")), any = true;
    if (grav) printf("   %s has a public Gravatar profile (often links other accounts)\n", c_gray("info")), any = true;
    if (!any) printf("   %s\n", c_gray("nothing jumped out"));
    putchar('\n');

    free(grav), free(prof_url);
    return 0;
}

int run_email(int argc, char **argv) {
    email_flags fl = {0};
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
        fprintf(stderr, "odin: give me an email address, like someone@example.com\n\n%s", usage);
        return 2;
    }
    if (!fl.json && !fl.quiet) banner_print();
    int rc = 0;
    for (int i = 0; i < nt; i++)
        if (email_report(targets[i], &fl) != 0) rc = 1;
    return rc;
}

#include "analysis/findings.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/util.h"

// keys get lowercased and the "ns:" prefix stripped before lookup
static const char *identity_keys[] = {"artist", "xpauthor", "author", "cameraownername", "ownername", "lastmodifiedby",
                                      "company", "manager", "initial-creator", "copyright", "rights", "by-line",
                                      "writer-editor", "credit", "printed-by", "copyrightnotice", "captionwriter",
                                      "contact", NULL};
static const char *place_keys[] = {"city", "sub-location", "sublocation", "province-state", "state", "country",
                                   "country-primarylocationname", "location", NULL};
static const char *device_keys[] = {"make", "model", "lensmake", "lensmodel", "hostcomputer", "uniquecameramodel", NULL};
static const char *software_keys[] = {"software", "creatortool", "producer", "application", "appversion", "generator",
                                      "softwareagent", "originatingprogram", "historysoftwareagent", NULL};
static const char *edit_keys[] = {"totaltime", "editing-duration", "editing-cycles", "revision", "revisionnumber",
                                  "template", "incrementalupdates", NULL};

static bool in(const char **list, const char *k) {
    for (; *list; list++)
        if (!strcmp(*list, k)) return true;
    return false;
}

static bool user_ch(char c) { return c && !strchr("\\/:*?\"<>|\r\n", c); }
static bool word_ch(char c) { return isalnum((unsigned char)c) || c == '_'; }
static bool host_ch(char c) { return word_ch(c) || c == '.' || c == '-'; }
static bool email_ch(char c) { return word_ch(c) || c == '.' || c == '+' || c == '-'; }

static void flag(result_t *r, level_t lvl, const char *kind, const char *src, const char *fmt, const char *a,
                 const char *b) {
    char *d = xprintf(fmt, a, b);
    result_add_finding(r, lvl, kind, d, src);
    free(d);
}

// hunts through one value for usernames in paths, network shares and emails
static void scan_leaks(result_t *r, const field_t *f) {
    const char *v = f->value;
    size_t n = strlen(v);
    bool leaked = false;
    char user[256];

    for (size_t i = 1; i + 1 < n; i++) {
        // C:\Users\bob  or  C:/Users/bob  (also "Documents and Settings" from the xp days)
        if (v[i] == ':' && isalpha((unsigned char)v[i - 1]) && (i < 2 || !word_ch(v[i - 2])) &&
            (v[i + 1] == '\\' || v[i + 1] == '/')) {
            const char *p = v + i + 2;
            size_t skip = 0;
            if (!strncasecmp(p, "users", 5)) skip = 5;
            else if (!strncasecmp(p, "documents and settings", 22)) skip = 22;
            if (skip && (p[skip] == '\\' || p[skip] == '/')) {
                p += skip + 1;
                size_t k = 0;
                while (user_ch(p[k]) && k < sizeof user - 1) user[k] = p[k], k++;
                user[k] = 0;
                if (k) {
                    flag(r, LVL_HIGH, "path leak", f->group, "username \"%s\" in %s", user, f->key);
                    leaked = true;
                }
            } else if (v[i + 1] == '\\') {
                size_t k = 0;
                const char *s = v + i - 1;
                while (s[k] && !isspace((unsigned char)s[k]) && !strchr("\"<>|", s[k]) && k < sizeof user - 1)
                    user[k] = s[k], k++;
                user[k] = 0;
                char *d = xprintf("%s: %s", f->key, user);
                if (!leaked) result_add_finding(r, LVL_MED, "local path", d, f->group);
                free(d);
            }
        }
        // /home/bob or /Users/bob, only at the start of a path (not inside a url)
        if (v[i] == '/' && (!strncmp(v + i, "/home/", 6) || !strncmp(v + i, "/Users/", 7))) {
            char prev = v[i - 1];
            bool ok = isspace((unsigned char)prev) || strchr("\"'(=", prev) ||
                      (i >= 7 && !strncmp(v + i - 7, "file://", 7));
            if (ok || i == 0) {
                const char *p = v + i + (v[i + 1] == 'h' ? 6 : 7);
                size_t k = 0;
                while (p[k] && p[k] != '/' && !isspace((unsigned char)p[k]) && p[k] != '"' && p[k] != '\'' &&
                       k < sizeof user - 1)
                    user[k] = p[k], k++;
                user[k] = 0;
                if (k) flag(r, LVL_HIGH, "path leak", f->group, "username \"%s\" in %s", user, f->key);
            }
        }
        // \\fileserver\share
        if (v[i - 1] == '\\' && v[i] == '\\' && isalnum((unsigned char)v[i + 1]) && (i < 2 || v[i - 2] != '\\')) {
            size_t k = 0;
            const char *p = v + i + 1;
            while (host_ch(p[k]) && k < sizeof user - 1) user[k] = p[k], k++;
            user[k] = 0;
            if (p[k] == '\\') {
                char *d = xprintf("host \"%s\" in %s", user, f->key);
                result_add_finding(r, LVL_MED, "network share", d, f->group);
                free(d);
            }
        }
    }
    // /home at the very start of the value
    if (!strncmp(v, "/home/", 6) || !strncmp(v, "/Users/", 7)) {
        const char *p = v + (v[1] == 'h' ? 6 : 7);
        size_t k = 0;
        while (p[k] && p[k] != '/' && !isspace((unsigned char)p[k]) && k < sizeof user - 1) user[k] = p[k], k++;
        user[k] = 0;
        if (k) flag(r, LVL_HIGH, "path leak", f->group, "username \"%s\" in %s", user, f->key);
    }

    // emails: walk out both ways from each @
    for (const char *at = strchr(v, '@'); at; at = strchr(at + 1, '@')) {
        const char *s = at, *e = at + 1;
        while (s > v && email_ch(s[-1])) s--;
        int dots = 0;
        while (word_ch(*e) || *e == '-' || (*e == '.' && (word_ch(e[1]) || e[1] == '-'))) dots += *e++ == '.';
        if (s == at || !dots) continue;
        char email[256];
        snprintf(email, sizeof email, "%.*s", (int)(e - s), s);
        flag(r, LVL_MED, "email", f->group, "%s: %s", f->key, email);
    }
}

void analyze(result_t *r) {
    char buf[700];
    for (size_t i = 0; i < r->ngps; i++) {
        snprintf(buf, sizeof buf, "%.6f, %.6f  https://www.google.com/maps?q=%.6f,%.6f", r->gps[i].lat, r->gps[i].lon,
                 r->gps[i].lat, r->gps[i].lon);
        result_add_finding(r, LVL_HIGH, "location", buf, r->gps[i].source);
    }

    for (size_t i = 0; i < r->nfields; i++) {
        const field_t *f = &r->fields[i];
        const char *k = strrchr(f->key, ':');
        k = k ? k + 1 : f->key;
        char local[128];
        size_t j = 0;
        for (; k[j] && j < sizeof local - 1; j++) local[j] = (char)tolower((unsigned char)k[j]);
        local[j] = 0;
        // in pdf info "Creator" is the app that made it, everywhere else it's a person
        bool pdf = ends_with(f->group, "PDF");

        if (strstr(local, "serialnumber")) flag(r, LVL_MED, "device id", f->group, "%s: %s", f->key, f->value);
        else if (in(identity_keys, local) || (!strcmp(local, "creator") && !pdf))
            flag(r, LVL_MED, "identity", f->group, "%s: %s", f->key, f->value);
        else if (in(place_keys, local)) flag(r, LVL_MED, "place", f->group, "%s: %s", f->key, f->value);
        else if (in(device_keys, local)) flag(r, LVL_INFO, "device", f->group, "%s: %s", f->key, f->value);
        else if (in(software_keys, local) || !strcmp(local, "creator"))
            flag(r, LVL_INFO, "software", f->group, "%s: %s", f->key, f->value);
        else if (in(edit_keys, local) && strcmp(f->value, "0"))
            flag(r, LVL_INFO, "edit trail", f->group, "%s: %s", f->key, f->value);

        scan_leaks(r, f);

        // same pdf key with different values across saves = someone edited it
        if (pdf && strcmp(f->key, "IncrementalUpdates")) {
            bool first = true;
            for (size_t p = 0; p < i && first; p++)
                first = !(!strcmp(r->fields[p].key, f->key) && !strcmp(r->fields[p].group, f->group));
            if (!first) continue;
            sbuf hist;
            sb_init(&hist);
            int count = 0;
            for (size_t p = i; p < r->nfields; p++) {
                if (strcmp(r->fields[p].key, f->key) || strcmp(r->fields[p].group, f->group)) continue;
                if (count++) sb_puts(&hist, " → ");
                sb_puts(&hist, r->fields[p].value);
            }
            if (count > 1) flag(r, LVL_MED, "edit history", f->group, "%s changed across saves: %s", f->key, hist.s);
            sb_free(&hist);
        }
    }

    // stable sort by level: high, then medium, then info
    finding_t *sorted = xmalloc(sizeof(finding_t) * (r->nfindings ? r->nfindings : 1));
    size_t k = 0;
    for (int lvl = LVL_HIGH; lvl <= LVL_INFO; lvl++)
        for (size_t i = 0; i < r->nfindings; i++)
            if ((int)r->findings[i].level == lvl) sorted[k++] = r->findings[i];
    if (k) memcpy(r->findings, sorted, sizeof(finding_t) * k);
    free(sorted);
}

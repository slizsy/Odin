#include "modules/user/check.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "core/util.h"

#define MAX_BODY (8u << 20)
#define PARALLEL 24

// looks like a normal browser, a lot of sites slam the door on obvious bots
#define UA "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0"

typedef struct {
    hit_t *hit;
    sbuf body;
    bool cut; // page was bigger than MAX_BODY, we only have the start of it
    char err[CURL_ERROR_SIZE];
} job_t;

static size_t on_body(char *data, size_t size, size_t n, void *ud) {
    job_t *j = ud;
    size_t len = size * n;
    if (j->body.len + len <= MAX_BODY) sb_putn(&j->body, data, len);
    else j->cut = true; // past the cap we stop keeping it
    return len;
}

const char *hit_state_name(hit_state s) {
    switch (s) {
    case HIT_FOUND: return "found";
    case HIT_MISSING: return "not_found";
    default: return "unknown";
    }
}

// bot walls (cloudflare, pypi's "client challenge" etc) answer 200 to everything.
// if we got one of those we learned nothing, so it can't count as found
static const char *bot_walls[] = {
    "<title>Client Challenge</title>",
    "<title>Just a moment...</title>",
    "<title>Attention Required! | Cloudflare</title>",
    "cf-browser-verification",
    "challenges.cloudflare.com",
    "<title>Access Denied</title>",
    "_Incapsula_Resource",
    "captcha-delivery.com",
    "px-captcha",
    NULL,
};

static bool is_bot_wall(const sbuf *body) {
    if (!body->s) return false;
    for (int i = 0; bot_walls[i]; i++)
        if (strstr(body->s, bot_walls[i])) return true;
    return false;
}

// the actual call: does this response mean the account exists?
static void judge(job_t *j, CURLcode rc) {
    hit_t *h = j->hit;
    const site_t *s = h->site;
    if (rc != CURLE_OK) {
        h->state = HIT_UNKNOWN;
        h->detail = xstrdup(rc == CURLE_OPERATION_TIMEDOUT ? "timed out" : (*j->err ? j->err : curl_easy_strerror(rc)));
        return;
    }
    long c = h->http;
    if (c == 404 || c == 410 || c == 204 || (c == 400 && s->kind == CHECK_STATUS)) {
        h->state = HIT_MISSING;
        return;
    }
    if (c < 200 || c >= 300) {
        // 403 / 429 / 5xx = the site said no to *us*, that tells us nothing about the account
        h->state = HIT_UNKNOWN;
        if (c == 429) h->detail = xstrdup("rate limited (429)");
        else if (c == 403 || c == 401) h->detail = xprintf("blocked (%ld)", c);
        else h->detail = xprintf("http %ld", c);
        return;
    }
    if (is_bot_wall(&j->body)) {
        h->state = HIT_UNKNOWN;
        h->detail = xstrdup("hit a bot check page");
        return;
    }
    if (s->kind == CHECK_STATUS) {
        h->state = HIT_FOUND;
        return;
    }
    bool has = j->body.s && strstr(j->body.s, s->text);
    // text wasn't in the part we kept, but it could be in the part we threw away. don't guess
    if (!has && j->cut) {
        h->state = HIT_UNKNOWN;
        h->detail = xstrdup("page too big to check");
        return;
    }
    if (s->kind == CHECK_ABSENT) h->state = has ? HIT_MISSING : HIT_FOUND;
    else h->state = has ? HIT_FOUND : HIT_MISSING;
}

// https certs. windows: use the system cert store. linux: a portable build might have been
// compiled on a distro that keeps its cert bundle somewhere else, so go find this one's
static void apply_tls(CURL *e) {
#ifdef _WIN32
    curl_easy_setopt(e, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#elif !defined(__APPLE__)
    static int probed;
    static const char *found;
    if (!probed) {
        probed = 1;
        const char *env = getenv("SSL_CERT_FILE");
        if (!env) env = getenv("CURL_CA_BUNDLE");
        curl_version_info_data *v = curl_version_info(CURLVERSION_NOW);
        struct stat st;
        if (env && stat(env, &st) == 0) {
            found = env;
        } else if (!(v->cainfo && stat(v->cainfo, &st) == 0) && !(v->capath && stat(v->capath, &st) == 0)) {
            static const char *spots[] = {
                "/etc/ssl/certs/ca-certificates.crt",                // debian, ubuntu, kali, arch, alpine
                "/etc/pki/tls/certs/ca-bundle.crt",                  // fedora, rhel
                "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", // newer rhel
                "/etc/ssl/ca-bundle.pem",                            // opensuse
                "/etc/ssl/cert.pem",                                 // void, older alpine
                NULL,
            };
            for (int i = 0; spots[i] && !found; i++)
                if (stat(spots[i], &st) == 0) found = spots[i];
        }
    }
    if (found) curl_easy_setopt(e, CURLOPT_CAINFO, found);
#else
    (void)e; // macos libcurl already knows where the system certs are
#endif
}

// runs every prepared hit (site + username already filled in) in parallel
static void run_hits(hit_t *hits, size_t total, long timeout, progress_fn cb) {
    job_t *jobs = calloc(total ? total : 1, sizeof(job_t));
    if (!jobs) exit(1);

    CURLM *m = curl_multi_init();
    // libcurl queues anything past this and starts it when a slot frees up
    curl_multi_setopt(m, CURLMOPT_MAX_TOTAL_CONNECTIONS, (long)PARALLEL);
    curl_multi_setopt(m, CURLMOPT_MAX_HOST_CONNECTIONS, 4L);
    struct curl_slist *hdrs = curl_slist_append(NULL, "Accept-Language: en-US,en;q=0.9");
    hdrs = curl_slist_append(hdrs, "Accept: text/html,application/json;q=0.9,*/*;q=0.8");

    size_t done = 0;
    for (size_t k = 0; k < total; k++) {
        hit_t *h = &hits[k];
        job_t *j = &jobs[k];
        h->state = HIT_UNKNOWN;
        j->hit = h;
        sb_init(&j->body);

        char *probe = site_url(h->site->probe, h->username);
        h->url = site_url(h->site->profile, h->username);
        if (!probe || !h->url) {
            h->detail = xstrdup("name can't exist here (bad characters)");
            h->state = HIT_MISSING;
            free(probe);
            done++;
            continue;
        }
        CURL *e = curl_easy_init();
        curl_easy_setopt(e, CURLOPT_URL, probe);
        curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, on_body);
        curl_easy_setopt(e, CURLOPT_WRITEDATA, j);
        curl_easy_setopt(e, CURLOPT_PRIVATE, j);
        curl_easy_setopt(e, CURLOPT_ERRORBUFFER, j->err);
        curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(e, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(e, CURLOPT_TIMEOUT, timeout);
        curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, timeout < 10 ? timeout : 10L);
        curl_easy_setopt(e, CURLOPT_USERAGENT, UA);
        curl_easy_setopt(e, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(e, CURLOPT_ACCEPT_ENCODING, ""); // gzip/br/whatever curl supports
        curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
        apply_tls(e);
        curl_multi_add_handle(m, e);
        free(probe);
    }
    if (cb) cb(done, total);

    int running = 1;
    while (running) {
        curl_multi_perform(m, &running);
        CURLMsg *msg;
        int left;
        while ((msg = curl_multi_info_read(m, &left))) {
            if (msg->msg != CURLMSG_DONE) continue;
            CURL *e = msg->easy_handle;
            job_t *j;
            curl_easy_getinfo(e, CURLINFO_PRIVATE, (char **)&j);
            curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &j->hit->http);
            curl_easy_getinfo(e, CURLINFO_TOTAL_TIME, &j->hit->secs);
            judge(j, msg->data.result);
            sb_free(&j->body);
            curl_multi_remove_handle(m, e);
            curl_easy_cleanup(e);
            if (cb) cb(++done, total);
        }
        if (running) curl_multi_poll(m, NULL, 0, 500, NULL);
    }

    for (size_t k = 0; k < total; k++) sb_free(&jobs[k].body);
    free(jobs);
    curl_slist_free_all(hdrs);
    curl_multi_cleanup(m);
}

void check_all(const site_list *sites, char **users, int nusers, long timeout, progress_fn cb, hit_list *out) {
    size_t total = sites->count * (size_t)nusers;
    out->hits = calloc(total ? total : 1, sizeof(hit_t));
    out->count = total;
    if (!out->hits) exit(1);
    for (int u = 0; u < nusers; u++) {
        for (size_t i = 0; i < sites->count; i++) {
            hit_t *h = &out->hits[(size_t)u * sites->count + i];
            h->site = &sites->items[i];
            h->username = users[u];
        }
    }
    run_hits(out->hits, total, timeout, cb);
}

void check_pairs(const site_list *sites, char **names, long timeout, progress_fn cb, hit_list *out) {
    out->hits = calloc(sites->count ? sites->count : 1, sizeof(hit_t));
    out->count = sites->count;
    if (!out->hits) exit(1);
    for (size_t i = 0; i < sites->count; i++) out->hits[i].site = &sites->items[i], out->hits[i].username = names[i];
    run_hits(out->hits, sites->count, timeout, cb);
}

void hits_free(hit_list *h) {
    for (size_t i = 0; i < h->count; i++) free(h->hits[i].url), free(h->hits[i].detail);
    free(h->hits);
    h->hits = NULL, h->count = 0;
}

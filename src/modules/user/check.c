#include "modules/user/check.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/net.h"
#include "core/platform.h"
#include "core/util.h"

#define MAX_BODY (8u << 20)
#define PARALLEL 24     // requests in flight at once
#define PER_HOST 2      // and at most this many to any one site, hammering is what gets you blocked
#define MAX_ATTEMPTS 2  // one retry for dropped connections, 429 / 5xx
#define MAX_RETRY_WAIT 20000

// real browser identities. each comes with the extra headers that browser actually sends,
// a "chrome" with no sec-ch-ua headers looks more like a bot than no claim at all
typedef struct {
    const char *ua;
    const char *ch_ua; // chrome-family client hints, NULL for firefox / safari
    const char *platform;
} browser_t;

static const browser_t browsers[] = {
    {"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/148.0.0.0 Safari/537.36",
     "\"Chromium\";v=\"148\", \"Google Chrome\";v=\"148\", \"Not.A/Brand\";v=\"99\"", "\"Windows\""},
    {"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/148.0.0.0 Safari/537.36",
     "\"Chromium\";v=\"148\", \"Google Chrome\";v=\"148\", \"Not.A/Brand\";v=\"99\"", "\"macOS\""},
    {"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/148.0.0.0 Safari/537.36 "
     "Edg/148.0.0.0",
     "\"Chromium\";v=\"148\", \"Microsoft Edge\";v=\"148\", \"Not.A/Brand\";v=\"99\"", "\"Windows\""},
    {"Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:152.0) Gecko/20100101 Firefox/152.0", NULL, NULL},
    {"Mozilla/5.0 (Macintosh; Intel Mac OS X 15.7; rv:152.0) Gecko/20100101 Firefox/152.0", NULL, NULL},
    {"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.4 "
     "Safari/605.1.15",
     NULL, NULL},
};
#define NBROWSERS (sizeof browsers / sizeof *browsers)

typedef enum { JOB_WAITING, JOB_RUNNING, JOB_DONE } job_phase;

typedef struct {
    hit_t *hit;
    job_phase phase;
    CURL *easy;
    char *probe, *post;
    char host[256];
    struct curl_slist *hdrs;
    sbuf body;
    bool cut;          // page was bigger than MAX_BODY, we only have the start of it
    int attempts;
    unsigned browser;  // which identity this attempt uses
    uint64_t not_before;
    long retry_after;  // seconds, from the Retry-After header
    char err[CURL_ERROR_SIZE];
} job_t;

static size_t on_body(char *data, size_t size, size_t n, void *ud) {
    job_t *j = ud;
    size_t len = size * n;
    size_t room = MAX_BODY - j->body.len;
    if (len <= room) {
        sb_putn(&j->body, data, len);
        return len;
    }
    // over the cap: keep exactly the first MAX_BODY bytes, then hang up. never keep
    // anything after a gap, or a later chunk could get glued onto the start of the page
    sb_putn(&j->body, data, room);
    j->cut = true;
    return 0; // tells curl to abort, judge() knows a cut page isn't an error
}

static size_t on_header(char *data, size_t size, size_t n, void *ud) {
    job_t *j = ud;
    size_t len = size * n;
    if (len > 12 && !strncasecmp(data, "retry-after:", 12)) j->retry_after = strtol(data + 12, NULL, 10);
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
    "Checking your browser before accessing",
    "<title>Security Measure</title>",
    "Our systems have detected unusual traffic",
    NULL,
};

static bool contains(const job_t *j, const char *text) { return j->body.s && strstr(j->body.s, text); }

static bool contains_any(const job_t *j, const strlist *l) {
    for (size_t i = 0; i < l->n; i++)
        if (contains(j, l->v[i])) return true;
    return false;
}

// xorshift, rand_r doesn't exist on windows
static unsigned next_rand(unsigned *x) {
    *x ^= *x << 13, *x ^= *x >> 17, *x ^= *x << 5;
    return *x;
}

static void unknown(hit_t *h, char *detail) {
    h->state = HIT_UNKNOWN;
    free(h->detail);
    h->detail = detail;
}

static void blocked_reason(hit_t *h, long c) {
    const site_t *s = h->site;
    const char *hint = s->protect & PROT_IP ? ", this site blocks vpn / cloud ips"
                       : s->protect & PROT_BROWSER ? ", this site wants a real browser" : "";
    if (c == 429) unknown(h, xprintf("rate limited (429)%s", hint));
    else if (c == 403 || c == 401) unknown(h, xprintf("blocked (%ld)%s", c, hint));
    else unknown(h, xprintf("http %ld", c));
}

// quick hiccups get one more go. timeouts don't: a site that took the whole timeout is slow
// or quietly blocking, and retrying would double how long the whole scan waits on it
static bool is_retryable(CURLcode rc, long c) {
    switch (rc) {
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_HTTP2:
    case CURLE_HTTP2_STREAM:
        return true;
    case CURLE_OK:
        return c == 429 || c == 502 || c == 503 || c == 504;
    default:
        return false;
    }
}

// the actual call: does this response mean the account exists?
static void judge(job_t *j, CURLcode rc) {
    hit_t *h = j->hit;
    const site_t *s = h->site;
    if (rc == CURLE_WRITE_ERROR && j->cut) rc = CURLE_OK; // we hung up on purpose
    if (rc != CURLE_OK) {
        unknown(h, xstrdup(rc == CURLE_OPERATION_TIMEDOUT ? "timed out" : (*j->err ? j->err : curl_easy_strerror(rc))));
        return;
    }
    long c = h->http;
    // the site's own "you're blocked / rate limited" pages, then the usual bot walls
    for (size_t i = 0; i + 1 < s->blocked.n; i += 2)
        if (contains(j, s->blocked.v[i])) {
            unknown(h, xprintf("%s", s->blocked.v[i + 1]));
            return;
        }
    for (int i = 0; bot_walls[i]; i++)
        if (contains(j, bot_walls[i])) {
            unknown(h, xstrdup("hit a bot check page"));
            return;
        }

    if (s->kind == CHECK_STRICT) {
        // whatsmyname style: both sides have to match exactly, anything else is a shrug
        bool found_code = s->found_code ? c == s->found_code : c >= 200 && c < 300;
        bool missing_code = s->missing_code ? c == s->missing_code : c == 404 || c == 410;
        if (found_code && (!s->present.n || contains_any(j, &s->present))) h->state = HIT_FOUND;
        else if (missing_code && (!s->absent.n || contains_any(j, &s->absent))) h->state = HIT_MISSING;
        else if (j->cut) unknown(h, xstrdup("page too big to check"));
        else if (c < 200 || c >= 400) blocked_reason(h, c);
        else unknown(h, xprintf("unexpected answer (http %ld)", c));
        return;
    }

    if (c == 404 || c == 410 || c == 204 || (c == 400 && s->kind == CHECK_STATUS)) {
        h->state = HIT_MISSING;
        return;
    }
    // 403 / 429 / 5xx = the site said no to *us*, that tells us nothing about the account
    if (c < 200 || c >= 300) {
            blocked_reason(h, c);
            return;
        }
    if (s->kind == CHECK_STATUS) {
        h->state = HIT_FOUND;
        return;
    }
    if (contains_any(j, &s->absent)) {
        h->state = HIT_MISSING;
        return;
    }
    if (s->present.n) {
        if (contains_any(j, &s->present)) h->state = HIT_FOUND;
        // text wasn't in the part we kept, but it could be in the part we threw away. don't guess
        else if (j->cut) unknown(h, xstrdup("page too big to check"));
        else h->state = HIT_MISSING;
        return;
    }
    if (j->cut && s->absent.n) unknown(h, xstrdup("page too big to check"));
    else h->state = HIT_FOUND;
}

// "https://bob.site.com:8080/x" -> "bob.site.com:8080", used to cap requests per site
static void host_of(const char *url, char *out, size_t n) {
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    size_t len = strcspn(h, "/?#");
    if (len >= n) len = n - 1;
    memcpy(out, h, len);
    out[len] = 0;
}

static int running_on(const job_t *jobs, size_t total, const char *host) {
    int c = 0;
    for (size_t k = 0; k < total; k++) c += jobs[k].phase == JOB_RUNNING && !strcmp(jobs[k].host, host);
    return c;
}

static bool site_sets(const site_t *s, const char *name) {
    size_t n = strlen(name);
    for (size_t i = 0; i < s->headers.n; i++)
        if (!strncasecmp(s->headers.v[i], name, n) && s->headers.v[i][n] == ':') return true;
    return false;
}

static struct curl_slist *add_default(struct curl_slist *l, const site_t *s, const char *name, const char *value) {
    if (!value || site_sets(s, name)) return l; // the site's own headers win
    char *line = xprintf("%s: %s", name, value);
    l = curl_slist_append(l, line);
    free(line);
    return l;
}

// headers a real browser would send for this kind of request, then the site's own on top
static struct curl_slist *build_headers(const job_t *j) {
    const site_t *s = j->hit->site;
    const browser_t *b = &browsers[j->browser % NBROWSERS];
    bool api = strstr(j->probe, "://api.") || strstr(j->probe, "/api/") || strstr(j->probe, ".json") || j->post;
    struct curl_slist *l = NULL;
    l = add_default(l, s, "User-Agent", b->ua);
    l = add_default(l, s, "Accept",
                    api ? "application/json, text/plain, */*"
                        : "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8");
    l = add_default(l, s, "Accept-Language", "en-US,en;q=0.9");
    if (b->ch_ua) {
        l = add_default(l, s, "sec-ch-ua", b->ch_ua);
        l = add_default(l, s, "sec-ch-ua-mobile", "?0");
        l = add_default(l, s, "sec-ch-ua-platform", b->platform);
    }
    l = add_default(l, s, "Sec-Fetch-Dest", api ? "empty" : "document");
    l = add_default(l, s, "Sec-Fetch-Mode", api ? "cors" : "navigate");
    l = add_default(l, s, "Sec-Fetch-Site", api ? "same-site" : "none");
    if (!api) {
        l = add_default(l, s, "Sec-Fetch-User", "?1");
        l = add_default(l, s, "Upgrade-Insecure-Requests", "1");
    }
    if (j->post) l = add_default(l, s, "Content-Type", "application/json");
    for (size_t i = 0; i < s->headers.n; i++) l = curl_slist_append(l, s->headers.v[i]);
    return l;
}

static void start_job(CURLM *m, job_t *j, long timeout) {
    sb_free(&j->body);
    j->cut = false, j->retry_after = 0, j->err[0] = 0;
    curl_slist_free_all(j->hdrs);
    j->hdrs = build_headers(j);

    CURL *e = curl_easy_init();
    curl_easy_setopt(e, CURLOPT_URL, j->probe);
    curl_easy_setopt(e, CURLOPT_HTTPHEADER, j->hdrs);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, j);
    curl_easy_setopt(e, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(e, CURLOPT_HEADERDATA, j);
    curl_easy_setopt(e, CURLOPT_PRIVATE, j);
    curl_easy_setopt(e, CURLOPT_ERRORBUFFER, j->err);
    curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(e, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(e, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, timeout < 10 ? timeout : 10L);
    curl_easy_setopt(e, CURLOPT_ACCEPT_ENCODING, ""); // gzip/br/whatever curl supports, like a browser
    curl_easy_setopt(e, CURLOPT_COOKIEFILE, "");       // keep cookies across redirects (consent walls etc)
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    if (j->post) {
        curl_easy_setopt(e, CURLOPT_POSTFIELDS, j->post);
        curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE, (long)strlen(j->post));
    }
    net_apply_tls(e);
    curl_multi_add_handle(m, e);
    j->easy = e;
    j->phase = JOB_RUNNING;
    j->attempts++;
}

// {} in a json post body, with the username json-escaped
static char *fill_body(const char *tmpl, const char *user) {
    sbuf o;
    sb_init(&o);
    for (const char *p = tmpl; *p;) {
        if (p[0] == '{' && p[1] == '}') {
            for (const char *u = user; *u; u++) {
                if (*u == '"' || *u == '\\') sb_putc(&o, '\\');
                sb_putc(&o, *u);
            }
            p += 2;
        } else {
            sb_putc(&o, *p++);
        }
    }
    return sb_take(&o);
}

// runs every prepared hit (site + username already filled in) in parallel.
// we do the queueing ourselves instead of letting curl do it: curl starts a request's
// timeout the moment it's queued, so anything stuck behind a slow site would "time out"
// before it was even sent. same loop handles retries after a short wait
static void run_hits(hit_t *hits, size_t total, long timeout, progress_fn cb) {
    job_t *jobs = calloc(total ? total : 1, sizeof(job_t));
    if (!jobs) exit(1);
    CURLM *m = curl_multi_init();
    unsigned seed = ((unsigned)platform_now_ms() ^ platform_pid()) | 1;

    size_t done = 0;
    for (size_t k = 0; k < total; k++) {
        hit_t *h = &hits[k];
        job_t *j = &jobs[k];
        const site_t *s = h->site;
        h->state = HIT_UNKNOWN;
        j->hit = h;
        j->browser = (unsigned)(k + seed);
        sb_init(&j->body);
        char *user = site_username(s, h->username);
        h->url = site_url(s->profile, user);
        if (site_accepts(s, user)) {
            j->probe = site_url(s->probe, user);
            if (s->post) j->post = fill_body(s->post, user);
        }
        if (!site_accepts(s, user) || !j->probe || !h->url) {
            // the site's username rules say this name can't exist there, no need to ask
            h->detail = xstrdup("name not allowed on this site");
            h->state = HIT_MISSING;
            j->phase = JOB_DONE;
            done++;
        } else {
            host_of(j->probe, j->host, sizeof j->host);
        }
        free(user);
    }
    if (cb) cb(done, total);

    size_t inflight = 0;
    for (;;) {
        if (odin_interrupted) break;
        uint64_t now = platform_now_ms(), next_wake = 0;
        // fill free slots, skipping sites that already have PER_HOST going or are cooling off
        for (size_t k = 0; k < total && inflight < PARALLEL; k++) {
            job_t *j = &jobs[k];
            if (j->phase != JOB_WAITING) continue;
            if (j->not_before > now) {
                if (!next_wake || j->not_before < next_wake) next_wake = j->not_before;
                continue;
            }
            if (running_on(jobs, total, j->host) >= PER_HOST) continue;
            start_job(m, j, timeout);
            inflight++;
        }
        if (!inflight && !next_wake) break; // everything's done

        int running;
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
            CURLcode rc = msg->data.result;
            curl_multi_remove_handle(m, e);
            curl_easy_cleanup(e);
            j->easy = NULL;
            inflight--;

            long wait_ms = j->retry_after > 0 ? j->retry_after * 1000 : 1500 + (long)(next_rand(&seed) % 2000);
            if (j->attempts < MAX_ATTEMPTS && is_retryable(rc == CURLE_WRITE_ERROR && j->cut ? CURLE_OK : rc, j->hit->http) &&
                wait_ms <= MAX_RETRY_WAIT && !odin_interrupted) {
                // give it a breather and come back as a different browser
                j->phase = JOB_WAITING;
                j->not_before = platform_now_ms() + (uint64_t)wait_ms;
                j->browser++;
                continue;
            }
            judge(j, rc);
            if (rc == CURLE_OK && j->hit->http == 429 && j->retry_after * 1000 > MAX_RETRY_WAIT)
                unknown(j->hit, xprintf("rate limited, site says wait %lds", j->retry_after));
            sb_free(&j->body);
            j->phase = JOB_DONE;
            if (cb) cb(++done, total);
        }
        if (inflight) {
            int wait = 500;
            now = platform_now_ms();
            if (next_wake && next_wake > now && next_wake - now < (uint64_t)wait) wait = (int)(next_wake - now);
            curl_multi_poll(m, NULL, 0, wait, NULL);
        } else if (next_wake) {
            // only retries left and they're cooling down. short naps so ctrl+c stays snappy
            now = platform_now_ms();
            int wait = next_wake > now ? (int)(next_wake - now) : 0;
            curl_multi_poll(m, NULL, 0, wait < 200 ? wait : 200, NULL);
        }
    }

    // ctrl+c: drop whatever's still going
    for (size_t k = 0; k < total; k++) {
        job_t *j = &jobs[k];
        if (j->easy) curl_multi_remove_handle(m, j->easy), curl_easy_cleanup(j->easy);
        if (j->phase != JOB_DONE) unknown(j->hit, xstrdup("stopped"));
        sb_free(&j->body);
        curl_slist_free_all(j->hdrs);
        free(j->probe), free(j->post);
    }
    free(jobs);
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

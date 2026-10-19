#include "core/net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "core/platform.h"
#include "ui/banner.h"

// https certs. windows: use the system cert store. linux: a portable build might have been
// compiled on a distro that keeps its cert bundle somewhere else, so go find this one's
void net_apply_tls(CURL *e) {
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

typedef struct {
    sbuf *body;
    size_t max;
    bool too_big;
} get_ctx;

static size_t on_data(char *data, size_t size, size_t n, void *ud) {
    get_ctx *c = ud;
    size_t len = size * n;
    if (c->body->len + len > c->max) {
        c->too_big = true;
        return 0; // abort
    }
    sb_putn(c->body, data, len);
    return len;
}

// ctrl+c in the shell: bail out of a download instead of finishing it
static int on_progress(void *ud, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)ud, (void)a, (void)b, (void)c, (void)d;
    return odin_interrupted ? 1 : 0;
}

bool net_get(const char *url, const char *const *headers, size_t max, long *status, sbuf *body, char *err,
             size_t errlen) {
    CURL *e = curl_easy_init();
    if (!e) {
        snprintf(err, errlen, "couldn't start curl");
        return false;
    }
    struct curl_slist *hdrs = NULL;
    for (int i = 0; headers && headers[i]; i++) hdrs = curl_slist_append(hdrs, headers[i]);
    char cerr[CURL_ERROR_SIZE] = "";
    get_ctx ctx = {body, max, false};
    curl_easy_setopt(e, CURLOPT_URL, url);
    curl_easy_setopt(e, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(e, CURLOPT_USERAGENT, "odin/" ODIN_VERSION);
    curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(e, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(e, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(e, CURLOPT_ERRORBUFFER, cerr);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(e, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(e, CURLOPT_NOPROGRESS, 0L);
    net_apply_tls(e);
    CURLcode rc = curl_easy_perform(e);
    curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, status);
    curl_easy_cleanup(e);
    curl_slist_free_all(hdrs);
    if (ctx.too_big) {
        snprintf(err, errlen, "download is bigger than expected (over %zu bytes)", max);
        return false;
    }
    if (rc != CURLE_OK) {
        snprintf(err, errlen, "%s", *cerr ? cerr : curl_easy_strerror(rc));
        return false;
    }
    return true;
}

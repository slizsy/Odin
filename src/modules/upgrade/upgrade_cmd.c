// odin upgrade: ask github for the latest release, grab this os's binary, check its
// sha256 against the release's SHA256SUMS, then swap it in for the running one
#include "modules/upgrade/upgrade_cmd.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/json.h"
#include "core/net.h"
#include "core/platform.h"
#include "core/sha256.h"
#include "core/util.h"
#include "ui/banner.h"
#include "ui/color.h"

#define RELEASES_API "https://api.github.com/repos/slizsy/Odin/releases/latest"
#define MAX_BINARY (64u << 20)

// which release file belongs to this build
#if defined(_WIN32)
#define ASSET "odin-windows-x86_64.exe"
#elif defined(__APPLE__)
#define ASSET "odin-macos-universal"
#elif defined(__aarch64__)
#define ASSET "odin-linux-arm64"
#else
#define ASSET "odin-linux-x86_64"
#endif

// "v0.1.2" or "0.1.2" -> {0,1,2}
static bool parse_version(const char *s, int v[3]) {
    if (!s) return false;
    if (*s == 'v' || *s == 'V') s++;
    return sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]) == 3;
}

static int cmp_version(const int a[3], const int b[3]) {
    for (int i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

// the api needs a token to see releases on a private repo
static const char *token(void) {
    const char *t = getenv("GITHUB_TOKEN");
    if (!t || !*t) t = getenv("GH_TOKEN");
    return t && *t ? t : NULL;
}

static bool fetch(const char *url, bool binary, size_t max, sbuf *out, long *status, char *err, size_t errlen) {
    char auth[512] = "";
    const char *hdrs[4];
    int k = 0;
    hdrs[k++] = binary ? "Accept: application/octet-stream" : "Accept: application/vnd.github+json";
    hdrs[k++] = "X-GitHub-Api-Version: 2022-11-28";
    if (token()) {
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", token());
        hdrs[k++] = auth;
    }
    hdrs[k] = NULL;
    sb_init(out);
    return net_get(url, hdrs, max, status, out, err, errlen);
}

// api url for a release file, by name
static const char *asset_url(const json *rel, const char *name) {
    json *assets = json_get(rel, "assets");
    if (!assets || assets->t != J_ARR) return NULL;
    for (size_t i = 0; i < assets->len; i++) {
        const char *n = json_str(assets->items[i], "name");
        if (n && !strcmp(n, name)) return json_str(assets->items[i], "url");
    }
    return NULL;
}

// does it at least look like a program for this os? stops us installing an html error page
static bool looks_like_binary(const unsigned char *b, size_t n) {
    if (n < 64 * 1024) return false;
#if defined(_WIN32)
    return b[0] == 'M' && b[1] == 'Z';
#elif defined(__APPLE__)
    uint32_t m = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
    return m == 0xCAFEBABE || m == 0xFEEDFACF || m == 0xCFFAEDFE; // universal or single arch mach-o
#else
    return !memcmp(b, "\x7f" "ELF", 4);
#endif
}

static int fail(const char *msg) {
    fprintf(stderr, "odin: %s\n", msg);
    return 1;
}

int run_upgrade(int argc, char **argv) {
    bool check_only = false, force = false;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--check")) check_only = true;
        else if (!strcmp(argv[i], "--force")) force = true;
        else {
            fprintf(stderr, "odin: unknown upgrade flag %s (try --check or --force)\n", argv[i]);
            return 2;
        }
    }

    // keep progress lines and errors in order even when piped
    setvbuf(stdout, NULL, _IOLBF, 0);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    int rc = 1;
    char err[512];
    long status = 0;
    sbuf body, bin, sums;
    sb_init(&bin), sb_init(&sums);
    json *rel = NULL;

    // ODIN_RELEASES_API lets the tests point this at a fake server
    const char *api = getenv("ODIN_RELEASES_API");
    if (!api || !*api) api = RELEASES_API;
    printf("checking for updates...\n");
    if (!fetch(api, false, 4u << 20, &body, &status, err, sizeof err)) {
        fprintf(stderr, "odin: couldn't reach github: %s\n", err);
        goto out;
    }
    if (status == 404) {
        fail(token() ? "no release found (or this token can't see the repo)"
                     : "no release found. if the repo is private, set GITHUB_TOKEN to a token that can read it");
        goto out;
    }
    if (status == 403 || status == 429) {
        fail("github is rate limiting us, try again in a bit (or set GITHUB_TOKEN)");
        goto out;
    }
    if (status != 200) {
        fprintf(stderr, "odin: github said http %ld\n", status);
        goto out;
    }

    rel = json_parse(body.s ? body.s : "", body.len);
    const char *tag = rel ? json_str(rel, "tag_name") : NULL;
    int latest[3], current[3];
    if (!parse_version(tag, latest) || !parse_version(ODIN_VERSION, current)) {
        fail("couldn't make sense of github's answer");
        goto out;
    }
    int cmp = cmp_version(current, latest);
    if (cmp >= 0 && !force) {
        printf("you're on the latest version (%s)\n", ODIN_VERSION);
        rc = 0;
        goto out;
    }
    if (check_only) {
        printf("update available: %s -> %s\nrun 'odin upgrade' to install it\n", ODIN_VERSION, tag);
        rc = 0;
        goto out;
    }

    char self[4096];
    if (!platform_self_path(self, sizeof self)) {
        fail("couldn't work out where odin is installed");
        goto out;
    }
    const char *bin_url = asset_url(rel, ASSET), *sums_url = asset_url(rel, "SHA256SUMS");
    if (!bin_url || !sums_url) {
        fprintf(stderr, "odin: release %s has no %s download\n", tag, bin_url ? "SHA256SUMS" : ASSET);
        goto out;
    }

    printf("downloading %s %s...\n", ASSET, tag);
    if (!fetch(bin_url, true, MAX_BINARY, &bin, &status, err, sizeof err) || status != 200) {
        fprintf(stderr, "odin: download failed: %s\n", status && status != 200 ? "bad http status" : err);
        goto out;
    }
    if (!fetch(sums_url, true, 64 * 1024, &sums, &status, err, sizeof err) || status != 200) {
        fail("couldn't download SHA256SUMS");
        goto out;
    }

    // the line for our file looks like "<64 hex>  odin-macos-universal"
    char got[65];
    sha256_hex((const unsigned char *)bin.s, bin.len, got);
    bool match = false;
    for (char *line = sums.s; line && *line && !match;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char hash[65], name[256];
        if (sscanf(line, "%64s %255s", hash, name) == 2 && !strcmp(name, ASSET) && !strcasecmp(hash, got)) match = true;
        line = nl ? nl + 1 : NULL;
    }
    if (!match) {
        fail("checksum didn't match SHA256SUMS, not installing it. try again later");
        goto out;
    }
    if (!looks_like_binary((const unsigned char *)bin.s, bin.len)) {
        fail("that download doesn't look like an odin program for this os, not installing it");
        goto out;
    }

    if (!platform_replace_self(self, (const unsigned char *)bin.s, bin.len, err, sizeof err)) {
        fprintf(stderr, "odin: %s\n", err);
        goto out;
    }
    printf("%s %s -> %s (%s)\n", c_bold("upgraded"), ODIN_VERSION, tag, self);
    rc = 0;

out:
    json_free(rel);
    sb_free(&body), sb_free(&bin), sb_free(&sums);
    curl_global_cleanup();
    return rc;
}

#include "modules/user/sites.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/util.h"

// the built in list. same format as a --sites file:
//   name | probe url | check | profile url | a username that exists there
// check is: status, absent:<text>, or present:<text>
// login-walled sites (instagram, x, tiktok, facebook) are left out on purpose,
// they answer the same way for every name so all you'd get is false positives
static const char *default_sites[] = {
    "GitHub          | https://github.com/{}                                   | status | | torvalds",
    "GitLab          | https://gitlab.com/api/v4/users?username={}            | present:\"username\" | https://gitlab.com/{} | dzaporozhets",
    "Codeberg        | https://codeberg.org/api/v1/users/{}                   | status | https://codeberg.org/{} | fnetx",
    "Reddit          | https://www.reddit.com/user/{}/about.json              | status | https://www.reddit.com/user/{} | spez",
    "Hacker News     | https://hacker-news.firebaseio.com/v0/user/{}.json     | present:\"created\" | https://news.ycombinator.com/user?id={} | pg",
    "Keybase         | https://keybase.io/_/api/1.0/user/lookup.json?usernames={} | present:\"basics\" | https://keybase.io/{} | chris",
    "DEV             | https://dev.to/api/users/by_username?url={}            | status | https://dev.to/{} | ben",
    "Docker Hub      | https://hub.docker.com/v2/users/{}/                    | status | https://hub.docker.com/u/{} | jessfraz",
    "PyPI            | https://pypi.org/user/{}/                              | status | | dstufft",
    "npm             | https://www.npmjs.com/~{}                              | status | | sindresorhus",
    "Hugging Face    | https://huggingface.co/{}                              | status | | julien-c",
    "Kaggle          | https://www.kaggle.com/{}                              | status | | jhoward",
    "Replit          | https://replit.com/@{}                                 | status | | amasad",
    "Codewars        | https://www.codewars.com/api/v1/users/{}               | status | https://www.codewars.com/users/{} | jhoffner",
    "TryHackMe       | https://tryhackme.com/api/user/exist/{}                | absent:\"success\":false | https://tryhackme.com/p/{} | ben",
    "SourceForge     | https://sourceforge.net/u/{}/profile                   | status | | ctsai",
    "Mastodon        | https://mastodon.social/api/v1/accounts/lookup?acct={} | status | https://mastodon.social/@{} | Gargron",
    "Bluesky         | https://public.api.bsky.app/xrpc/app.bsky.actor.getProfile?actor={}.bsky.social | status | https://bsky.app/profile/{}.bsky.social | jay",
    "Telegram        | https://t.me/{}                                        | present:tgme_page_title | | durov",
    "Tumblr          | https://{}.tumblr.com                                  | status | | staff",
    "Medium          | https://medium.com/@{}                                 | status | | ev",
    "YouTube         | https://www.youtube.com/@{}                            | status | | mkbhd",
    "Twitch          | https://passport.twitch.tv/usernames/{}                | status | https://www.twitch.tv/{} | twitch",
    "SoundCloud      | https://soundcloud.com/{}                              | status | | skrillex",
    "Vimeo           | https://vimeo.com/{}                                   | status | | staff",
    "Flickr          | https://www.flickr.com/people/{}                       | status | | flickr",
    "DeviantArt      | https://www.deviantart.com/{}                          | status | | spyed",
    "Dribbble        | https://dribbble.com/{}                                | status | | dribbble",
    "Behance         | https://www.behance.net/{}                             | status | | behance",
    "Patreon         | https://www.patreon.com/{}                             | status | | kurzgesagt",
    "Linktree        | https://linktr.ee/{}                                   | status | | linktree",
    "Gravatar        | https://en.gravatar.com/{}.json                        | status | https://gravatar.com/{} | beau",
    "Letterboxd      | https://letterboxd.com/{}/                             | status | | dave",
    "Wattpad         | https://www.wattpad.com/user/{}                        | status | | wattpad",
    "Disqus          | https://disqus.com/by/{}/                              | status | | disqus",
    "Trello          | https://trello.com/1/Members/{}                        | status | https://trello.com/u/{} | trello",
    "Wikipedia       | https://en.wikipedia.org/w/api.php?action=query&list=users&ususers={}&format=json | absent:\"missing\" | https://en.wikipedia.org/wiki/User:{} | Jimbo_Wales",
    "Duolingo        | https://www.duolingo.com/2017-06-30/users?username={}  | present:\"username\" | https://www.duolingo.com/profile/{} | luis",
    "Steam           | https://steamcommunity.com/id/{}                       | absent:The specified profile could not be found | | gabelogannewell",
    "Chess.com       | https://api.chess.com/pub/player/{}                    | status | https://www.chess.com/member/{} | hikaru",
    "Lichess         | https://lichess.org/api/user/{}                        | status | https://lichess.org/@/{} | thibault",
    "osu!            | https://osu.ppy.sh/users/{}                            | status | | peppy",
    "Speedrun.com    | https://www.speedrun.com/api/v1/users/{}               | status | https://www.speedrun.com/users/{} | pac",
    "Minecraft       | https://api.mojang.com/users/profiles/minecraft/{}     | status | https://namemc.com/profile/{} | Notch",
    "itch.io         | https://{}.itch.io                                     | status | | leafo",
    NULL,
};

static char *trim(const char *s, size_t n) { return trim_dup(s, n); }

// parses one "a | b | c | d | e" line. NULL err = fine
static const char *parse_line(site_list *l, const char *line) {
    char *parts[5] = {0};
    int np = 0;
    const char *p = line;
    while (np < 5) {
        const char *bar = strchr(p, '|');
        size_t n = bar ? (size_t)(bar - p) : strlen(p);
        parts[np++] = trim(p, n);
        if (!bar) break;
        p = bar + 1;
    }
    const char *err = NULL;
    if (np < 3) err = "need at least: name | url | check";
    else if (!*parts[0]) err = "missing site name";
    else if (!strstr(parts[1], "{}")) err = "url has no {} for the username";
    else if (strncmp(parts[1], "http://", 7) && strncmp(parts[1], "https://", 8)) err = "url has to start with http(s)://";

    site_t s = {0};
    if (!err) {
        const char *c = parts[2];
        if (!strcmp(c, "status")) s.kind = CHECK_STATUS;
        else if (!strncmp(c, "absent:", 7) && c[7]) s.kind = CHECK_ABSENT, s.text = xstrdup(c + 7);
        else if (!strncmp(c, "present:", 8) && c[8]) s.kind = CHECK_PRESENT, s.text = xstrdup(c + 8);
        else err = "check must be status, absent:<text> or present:<text>";
    }
    if (!err) {
        s.name = xstrdup(parts[0]);
        s.probe = xstrdup(parts[1]);
        s.profile = xstrdup(np > 3 && *parts[3] ? parts[3] : parts[1]);
        s.claimed = np > 4 && *parts[4] ? xstrdup(parts[4]) : NULL;
        VEC_PUSH(l->items, l->count, l->cap);
        l->items[l->count - 1] = s;
    }
    for (int i = 0; i < np; i++) free(parts[i]);
    return err;
}

void sites_load_default(site_list *l) {
    for (int i = 0; default_sites[i]; i++) {
        const char *err = parse_line(l, default_sites[i]);
        if (err) fprintf(stderr, "odin: built in site %d is broken: %s\n", i + 1, err);
    }
}

bool sites_load_file(site_list *l, const char *path, char *err, size_t errlen) {
    size_t n;
    unsigned char *b = read_file(path, &n);
    if (!b) {
        snprintf(err, errlen, "can't read %s", path);
        return false;
    }
    int lineno = 0;
    bool ok = true;
    for (char *line = (char *)b; line && *line && ok;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        lineno++;
        char *t = trim(line, strlen(line));
        if (*t && *t != '#') { // blank lines and # comments are fine
            const char *e = parse_line(l, t);
            if (e) {
                snprintf(err, errlen, "%s line %d: %s", path, lineno, e);
                ok = false;
            }
        }
        free(t);
        line = nl ? nl + 1 : NULL;
    }
    free(b);
    if (ok && !l->count) {
        snprintf(err, errlen, "%s has no sites in it", path);
        ok = false;
    }
    return ok;
}

void sites_free(site_list *l) {
    for (size_t i = 0; i < l->count; i++) {
        site_t *s = &l->items[i];
        free(s->name), free(s->probe), free(s->profile), free(s->text), free(s->claimed);
    }
    free(l->items);
    l->items = NULL, l->count = l->cap = 0;
}

char *site_url(const char *tmpl, const char *user) {
    const char *ph = strstr(tmpl, "{}");
    if (!ph) return NULL;
    // is the {} inside the hostname? then only letters, digits and dashes make sense
    const char *host = strstr(tmpl, "://");
    host = host ? host + 3 : tmpl;
    const char *host_end = strchr(host, '/');
    bool in_host = ph >= host && (!host_end || ph < host_end);

    sbuf o;
    sb_init(&o);
    for (const char *p = tmpl; *p;) {
        if (p[0] != '{' || p[1] != '}') {
            sb_putc(&o, *p++);
            continue;
        }
        for (const unsigned char *u = (const unsigned char *)user; *u; u++) {
            if (isalnum(*u) || *u == '-' || (!in_host && strchr("._~", *u))) {
                sb_putc(&o, (char)*u);
            } else if (in_host) {
                sb_free(&o);
                return NULL;
            } else {
                sb_printf(&o, "%%%02X", *u); // url-encode everything else
            }
        }
        p += 2;
    }
    return sb_take(&o);
}

#include "core/util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "odin: out of memory\n");
        exit(1);
    }
    return p;
}

void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) {
        fprintf(stderr, "odin: out of memory\n");
        exit(1);
    }
    return p;
}

char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

char *xstrndup(const char *s, size_t n) {
    char *d = xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

char *xprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *s = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return s;
}

void sb_init(sbuf *b) { b->s = NULL, b->len = b->cap = 0; }

static void sb_grow(sbuf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + extra + 1) cap *= 2;
    b->s = xrealloc(b->s, cap);
    b->cap = cap;
}

void sb_putn(sbuf *b, const char *s, size_t n) {
    sb_grow(b, n);
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

void sb_putc(sbuf *b, char c) { sb_putn(b, &c, 1); }
void sb_puts(sbuf *b, const char *s) { sb_putn(b, s, strlen(s)); }

void sb_printf(sbuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    sb_grow(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->s + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

void sb_rune(sbuf *b, uint32_t c) {
    char o[4];
    if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = 0xFFFD;
    if (c < 0x80) {
        sb_putc(b, (char)c);
    } else if (c < 0x800) {
        o[0] = (char)(0xC0 | c >> 6), o[1] = (char)(0x80 | (c & 0x3F));
        sb_putn(b, o, 2);
    } else if (c < 0x10000) {
        o[0] = (char)(0xE0 | c >> 12), o[1] = (char)(0x80 | ((c >> 6) & 0x3F)), o[2] = (char)(0x80 | (c & 0x3F));
        sb_putn(b, o, 3);
    } else {
        o[0] = (char)(0xF0 | c >> 18), o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        o[2] = (char)(0x80 | ((c >> 6) & 0x3F)), o[3] = (char)(0x80 | (c & 0x3F));
        sb_putn(b, o, 4);
    }
}

char *sb_take(sbuf *b) {
    char *s = b->s ? b->s : xstrdup("");
    sb_init(b);
    return s;
}

void sb_free(sbuf *b) {
    free(b->s);
    sb_init(b);
}

unsigned char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    struct stat st;
    if (fstat(fileno(f), &st) != 0) {
        fclose(f);
        return NULL;
    }
    size_t n = (size_t)st.st_size;
    unsigned char *b = xmalloc(n + 1);
    size_t got = fread(b, 1, n, f);
    fclose(f);
    b[got] = 0; // handy when we treat text files as strings
    *len = got;
    return b;
}

const unsigned char *memfind(const unsigned char *hay, size_t hlen, const char *needle) {
    size_t nlen = strlen(needle);
    if (!nlen || hlen < nlen) return NULL;
    const unsigned char *end = hay + hlen - nlen;
    for (const unsigned char *p = hay; p <= end; p++) {
        p = memchr(p, needle[0], (size_t)(end - p) + 1);
        if (!p) return NULL;
        if (memcmp(p, needle, nlen) == 0) return p;
    }
    return NULL;
}

uint16_t rd16be(const unsigned char *p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t rd32be(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint16_t rd16le(const unsigned char *p) { return (uint16_t)(p[1] << 8 | p[0]); }
uint32_t rd32le(const unsigned char *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }

char *latin1_to_utf8(const unsigned char *b, size_t n) {
    sbuf o;
    sb_init(&o);
    for (size_t i = 0; i < n; i++) sb_rune(&o, b[i]);
    return sb_take(&o);
}

char *utf16_to_utf8(const unsigned char *b, size_t n, bool be) {
    sbuf o;
    sb_init(&o);
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint32_t c = be ? rd16be(b + i) : rd16le(b + i);
        if (c == 0) break;
        // stitch surrogate pairs back together
        if (c >= 0xD800 && c <= 0xDBFF && i + 3 < n) {
            uint32_t lo = be ? rd16be(b + i + 2) : rd16le(b + i + 2);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        sb_rune(&o, c);
    }
    return sb_take(&o);
}

char *xml_unescape(const char *s, size_t n) {
    sbuf o;
    sb_init(&o);
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '&') {
            sb_putc(&o, s[i]);
            continue;
        }
        const char *semi = memchr(s + i, ';', n - i < 12 ? n - i : 12);
        if (!semi) {
            sb_putc(&o, '&');
            continue;
        }
        size_t len = (size_t)(semi - (s + i)) - 1;
        const char *ent = s + i + 1;
        if (len == 3 && !memcmp(ent, "amp", 3)) sb_putc(&o, '&');
        else if (len == 2 && !memcmp(ent, "lt", 2)) sb_putc(&o, '<');
        else if (len == 2 && !memcmp(ent, "gt", 2)) sb_putc(&o, '>');
        else if (len == 4 && !memcmp(ent, "quot", 4)) sb_putc(&o, '"');
        else if (len == 4 && !memcmp(ent, "apos", 4)) sb_putc(&o, '\'');
        else if (len > 1 && ent[0] == '#') {
            char tmp[16] = {0};
            memcpy(tmp, ent + 1, len - 1 < 15 ? len - 1 : 15);
            unsigned long cp = tmp[0] == 'x' || tmp[0] == 'X' ? strtoul(tmp + 1, NULL, 16) : strtoul(tmp, NULL, 10);
            sb_rune(&o, (uint32_t)cp);
        } else {
            sb_putc(&o, '&');
            continue;
        }
        i += len + 1;
    }
    return sb_take(&o);
}

char *trim_dup(const char *s, size_t n) {
    while (n && isspace((unsigned char)*s)) s++, n--;
    while (n && isspace((unsigned char)s[n - 1])) n--;
    return xstrndup(s, n);
}

// counts codepoints, good enough for lining up columns
size_t utf8_width(const char *s) {
    size_t w = 0;
    for (; *s; s++)
        if (((unsigned char)*s & 0xC0) != 0x80) w++;
    return w;
}

bool ends_with(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && memcmp(s + a - b, suffix, b) == 0;
}

void human_size(long long n, char *out, size_t outlen) {
    if (n < 1024) {
        snprintf(out, outlen, "%lld B", n);
        return;
    }
    const char *units = "KMGTPE";
    double v = (double)n / 1024;
    int i = 0;
    while (v >= 1024 && i < 5) v /= 1024, i++;
    snprintf(out, outlen, "%.1f %cB", v, units[i]);
}

bool is_ipv4(const char *s) {
    unsigned o[4];
    char extra;
    if (sscanf(s, "%u.%u.%u.%u%c", &o[0], &o[1], &o[2], &o[3], &extra) != 4) return false;
    for (int i = 0; i < 4; i++) if (o[i] > 255) return false;
    return true;
}

// parse textual ipv6 into 16 bytes. handles :: compression and a trailing embedded v4.
// not the tightest validator in the world but it rejects the obvious junk
bool ipv6_bytes(const char *s, unsigned char out[16]) {
    if (!strchr(s, ':')) return false;
    unsigned char buf[16] = {0};
    int filled = 0;   // bytes written before a ::
    int gap = -1;     // byte index where :: sat, -1 if none
    const char *p = s;
    if (p[0] == ':' && p[1] == ':') { gap = 0; p += 2; }
    else if (p[0] == ':') return false; // a single leading colon is bogus

    while (*p && filled < 16) {
        // an embedded ipv4 tail, like ::ffff:1.2.3.4 — only when the dotted quad is the
        // current group (a '.' before the next ':'), not just somewhere further along
        const char *dot = strchr(p, '.'), *colon = strchr(p, ':');
        if (dot && (!colon || dot < colon)) {
            unsigned o[4];
            char extra;
            if (sscanf(p, "%u.%u.%u.%u%c", &o[0], &o[1], &o[2], &o[3], &extra) != 4) return false;
            for (int i = 0; i < 4; i++) {
                if (o[i] > 255 || filled >= 16) return false;
                buf[filled++] = (unsigned char)o[i];
            }
            p += strlen(p);
            break;
        }
        char *end;
        long v = strtol(p, &end, 16);
        if (end == p || v < 0 || v > 0xffff) return false;
        if (filled + 2 > 16) return false;
        buf[filled++] = (unsigned char)(v >> 8);
        buf[filled++] = (unsigned char)(v & 0xff);
        p = end;
        if (*p == ':') {
            if (p[1] == ':') {
                if (gap >= 0) return false; // only one :: allowed
                gap = filled;
                p += 2;
            } else {
                p++;
            }
        } else if (*p) {
            return false; // some stray character
        }
    }
    if (*p) return false;

    if (gap >= 0) {
        // slide the post-:: bytes down to the end, zero-fill the hole
        int tail = filled - gap;
        for (int i = 0; i < tail; i++) out[16 - tail + i] = buf[gap + i];
        for (int i = 0; i < 16 - tail; i++) out[i] = (i < gap) ? buf[i] : 0;
    } else {
        if (filled != 16) return false; // no ::, so it has to be all 8 groups
        memcpy(out, buf, 16);
    }
    return true;
}

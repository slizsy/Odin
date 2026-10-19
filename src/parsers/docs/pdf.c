// pdf metadata lives in the /Info dictionary + an xmp packet.
// every incremental save appends a fresh /Info, so older values can still be sitting in the file
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/util.h"
#include "parsers/docs/docs.h"

static const char *info_keys[] = {"Title",   "Author",       "Subject", "Keywords", "Creator",        "Producer",
                                  "CreationDate", "ModDate", "Company",  "SourceModified", NULL};

static bool ishex(unsigned char c) { return isxdigit(c); }

// reads a pdf string starting at b[i] ('(' or '<'), raw bytes out
static unsigned char *pdf_string(const unsigned char *b, size_t n, size_t i, size_t *out_len) {
    sbuf o;
    sb_init(&o);
    if (b[i] == '<') {
        if (i + 1 < n && b[i + 1] == '<') return NULL; // that's a dict
        int nib = -1;
        for (size_t j = i + 1; j < n && b[j] != '>'; j++) {
            if (!ishex(b[j])) continue;
            int v = isdigit(b[j]) ? b[j] - '0' : (tolower(b[j]) - 'a' + 10);
            if (nib < 0) nib = v;
            else sb_putc(&o, (char)(nib << 4 | v)), nib = -1;
        }
        if (nib >= 0) sb_putc(&o, (char)(nib << 4));
        *out_len = o.len;
        return (unsigned char *)sb_take(&o);
    }

    // literal string: parens can nest, backslash escapes things
    int depth = 0;
    for (size_t j = i; j < n && j - i < 65536; j++) {
        unsigned char c = b[j];
        if (c == '\\') {
            if (++j >= n) break;
            unsigned char e = b[j];
            switch (e) {
            case 'n': sb_putc(&o, '\n'); break;
            case 'r': sb_putc(&o, '\r'); break;
            case 't': sb_putc(&o, '\t'); break;
            case 'b': sb_putc(&o, '\b'); break;
            case 'f': sb_putc(&o, '\f'); break;
            case '\r':
                if (j + 1 < n && b[j + 1] == '\n') j++;
                break; // line continuation
            case '\n': break;
            default:
                if (e >= '0' && e <= '7') {
                    int v = 0, k = 0;
                    for (; k < 3 && j + k < n && b[j + k] >= '0' && b[j + k] <= '7'; k++) v = v * 8 + (b[j + k] - '0');
                    sb_putc(&o, (char)v);
                    j += (size_t)k - 1;
                } else {
                    sb_putc(&o, (char)e);
                }
            }
        } else if (c == '(') {
            if (depth++ > 0) sb_putc(&o, '(');
        } else if (c == ')') {
            if (--depth == 0) {
                *out_len = o.len;
                return (unsigned char *)sb_take(&o);
            }
            sb_putc(&o, ')');
        } else {
            sb_putc(&o, (char)c);
        }
    }
    sb_free(&o);
    return NULL;
}

static char *decode_text(const unsigned char *b, size_t n) {
    if (n >= 2 && b[0] == 0xFE && b[1] == 0xFF) return utf16_to_utf8(b + 2, n - 2, true);
    if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) return xstrndup((const char *)b + 3, n - 3);
    return latin1_to_utf8(b, n); // close enough to PDFDocEncoding for metadata
}

// D:20230105143000+01'00' -> 2023-01-05 14:30:00 +01:00
static char *pdf_date(const char *s) {
    if (!strncmp(s, "D:", 2)) s += 2;
    size_t n = strlen(s);
    if (n < 4) return xstrdup(s);
    char d[15];
    memset(d, '0', 14);
    d[14] = 0;
    memcpy(d, s, n < 14 ? n : 14);
    sbuf o;
    sb_init(&o);
    sb_printf(&o, "%.4s-%.2s-%.2s %.2s:%.2s:%.2s", d, d + 4, d + 6, d + 8, d + 10, d + 12);
    if (n > 14) {
        char tz[8] = {0};
        size_t k = 0;
        for (const char *p = s + 14; *p && k < 7; p++)
            if (*p != '\'') tz[k++] = *p;
        if (!strcmp(tz, "Z")) sb_puts(&o, " UTC");
        else if (k >= 5) sb_printf(&o, " %.3s:%.2s", tz, tz + 3);
        else if (k >= 3) sb_printf(&o, " %.3s:00", tz);
    }
    return sb_take(&o);
}

void parse_pdf(result_t *r, const unsigned char *b, size_t n, const char *pfx) {
    char g[600];
    snprintf(g, sizeof g, "%sPDF", pfx);

    const unsigned char *ver = memfind(b, n < 1024 ? n : 1024, "%PDF-");
    if (ver && (size_t)(ver - b) + 8 <= n) {
        char v[4] = {(char)ver[5], (char)ver[6], (char)ver[7], 0};
        result_add(r, g, "PDFVersion", v);
    }
    if (memfind(b, n, "/Encrypt")) result_add(r, g, "Encrypted", "yes (info strings may come out scrambled)");

    for (const unsigned char *p = b; (p = memchr(p, '/', (size_t)(b + n - p))) != NULL; p++) {
        size_t left = (size_t)(b + n - p) - 1;
        for (int k = 0; info_keys[k]; k++) {
            size_t kl = strlen(info_keys[k]);
            if (left < kl + 1 || memcmp(p + 1, info_keys[k], kl)) continue;
            size_t i = (size_t)(p - b) + 1 + kl;
            while (i < n && isspace(b[i])) i++;
            if (i >= n || (b[i] != '(' && b[i] != '<')) continue;
            size_t raw_len;
            unsigned char *raw = pdf_string(b, n, i, &raw_len);
            if (!raw) continue;
            char *v = decode_text(raw, raw_len);
            if (!strcmp(info_keys[k], "CreationDate") || !strcmp(info_keys[k], "ModDate")) {
                char *d = pdf_date(v);
                free(v);
                v = d;
            }
            result_add(r, g, info_keys[k], v);
            free(v), free(raw);
            break;
        }
    }

    int saves = 0;
    for (const unsigned char *p = b; (p = memfind(p, (size_t)(b + n - p), "%%EOF")) != NULL; p += 5) saves++;
    if (memfind(b, n < 2048 ? n : 2048, "/Linearized")) saves--; // linearized files get a free extra eof
    if (saves > 1) {
        char buf[16];
        snprintf(buf, sizeof buf, "%d", saves - 1);
        result_add(r, g, "IncrementalUpdates", buf);
    }

    snprintf(g, sizeof g, "%sXMP", pfx);
    scan_xmp(r, b, n, g);
}

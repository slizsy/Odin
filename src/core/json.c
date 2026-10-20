#include "core/json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/util.h"

typedef struct {
    const char *p, *end;
    int depth;
} parser;

static void ws(parser *ps) {
    while (ps->p < ps->end && isspace((unsigned char)*ps->p)) ps->p++;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hex4(parser *ps, uint32_t *out) {
    if (ps->end - ps->p < 4) return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval(ps->p[i]);
        if (h < 0) return false;
        v = v << 4 | (uint32_t)h;
    }
    ps->p += 4;
    *out = v;
    return true;
}

static char *parse_string(parser *ps) {
    if (ps->p >= ps->end || *ps->p != '"') return NULL;
    ps->p++;
    sbuf o;
    sb_init(&o);
    while (ps->p < ps->end && *ps->p != '"') {
        char c = *ps->p++;
        if (c != '\\') {
            sb_putc(&o, c);
            continue;
        }
        if (ps->p >= ps->end) break;
        char e = *ps->p++;
        uint32_t cp;
        switch (e) {
        case 'n': sb_putc(&o, '\n'); break;
        case 't': sb_putc(&o, '\t'); break;
        case 'r': sb_putc(&o, '\r'); break;
        case 'b': sb_putc(&o, '\b'); break;
        case 'f': sb_putc(&o, '\f'); break;
        case 'u':
            if (!hex4(ps, &cp)) goto bad;
            // surrogate pair = one emoji etc
            if (cp >= 0xD800 && cp <= 0xDBFF && ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                parser save = *ps;
                ps->p += 2;
                uint32_t lo;
                if (hex4(ps, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                else *ps = save;
            }
            sb_rune(&o, cp);
            break;
        default: sb_putc(&o, e);
        }
    }
    if (ps->p >= ps->end) goto bad;
    ps->p++; // closing quote
    return sb_take(&o);
bad:
    sb_free(&o);
    return NULL;
}

static json *parse_value(parser *ps);

static json *node(jtype t) {
    json *j = calloc(1, sizeof *j);
    if (!j) exit(1);
    j->t = t;
    return j;
}

static json *parse_container(parser *ps, bool obj) {
    json *j = node(obj ? J_OBJ : J_ARR);
    size_t cap = 0;
    ps->p++; // [ or {
    ws(ps);
    if (ps->p < ps->end && *ps->p == (obj ? '}' : ']')) {
        ps->p++;
        return j;
    }
    for (;;) {
        char *key = NULL;
        ws(ps);
        if (obj) {
            if (!(key = parse_string(ps))) goto bad;
            ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') {
                free(key);
                goto bad;
            }
            ps->p++;
        }
        json *v = parse_value(ps);
        if (!v) {
            free(key);
            goto bad;
        }
        if (j->len == cap) {
            cap = cap ? cap * 2 : 8;
            j->items = xrealloc(j->items, cap * sizeof *j->items);
            if (obj) j->keys = xrealloc(j->keys, cap * sizeof *j->keys);
        }
        j->items[j->len] = v;
        if (obj) j->keys[j->len] = key;
        j->len++;
        ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == (obj ? '}' : ']')) {
            ps->p++;
            return j;
        }
        goto bad;
    }
bad:
    json_free(j);
    return NULL;
}

static json *parse_value(parser *ps) {
    ws(ps);
    if (ps->p >= ps->end || ++ps->depth > 64) return NULL; // nobody nests 64 deep on purpose
    json *j = NULL;
    char c = *ps->p;
    size_t left = (size_t)(ps->end - ps->p);
    if (c == '{' || c == '[') {
        j = parse_container(ps, c == '{');
    } else if (c == '"') {
        char *s = parse_string(ps);
        if (s) j = node(J_STR), j->s = s;
    } else if (left >= 4 && !strncmp(ps->p, "true", 4)) {
        j = node(J_BOOL), j->b = true, ps->p += 4;
    } else if (left >= 5 && !strncmp(ps->p, "false", 5)) {
        j = node(J_BOOL), ps->p += 5;
    } else if (left >= 4 && !strncmp(ps->p, "null", 4)) {
        j = node(J_NULL), ps->p += 4;
    } else if (c == '-' || isdigit((unsigned char)c)) {
        // copy out so strtod can't run past the end of the buffer
        char num[64];
        size_t k = 0;
        while (k < left && k < sizeof num - 1 && strchr("+-.eE0123456789", ps->p[k])) num[k] = ps->p[k], k++;
        num[k] = 0;
        char *e;
        double v = strtod(num, &e);
        if (e != num) j = node(J_NUM), j->n = v, ps->p += e - num;
    }
    ps->depth--;
    return j;
}

json *json_parse(const char *s, size_t n) {
    parser ps = {s, s + n, 0};
    json *j = parse_value(&ps);
    ws(&ps);
    if (j && ps.p != ps.end) { // junk after the value
        json_free(j);
        return NULL;
    }
    return j;
}

void json_free(json *j) {
    if (!j) return;
    for (size_t i = 0; i < j->len; i++) {
        json_free(j->items[i]);
        if (j->keys) free(j->keys[i]);
    }
    free(j->items), free(j->keys), free(j->s);
    free(j);
}

json *json_get(const json *obj, const char *key) {
    if (!obj || obj->t != J_OBJ) return NULL;
    for (size_t i = 0; i < obj->len; i++)
        if (!strcmp(obj->keys[i], key)) return obj->items[i];
    return NULL;
}

const char *json_str(const json *obj, const char *key) {
    json *v = json_get(obj, key);
    return v && v->t == J_STR ? v->s : NULL;
}

bool json_num(const json *obj, const char *key, double *out) {
    json *v = json_get(obj, key);
    if (!v || v->t != J_NUM) return false;
    *out = v->n;
    return true;
}

void json_quote(const char *s) {
    putchar('"');
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c == '\n') fputs("\\n", stdout);
        else if (c == '\t') fputs("\\t", stdout);
        else if (c == '\r') fputs("\\r", stdout);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

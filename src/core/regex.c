// backtracking matcher over unicode codepoints. usernames are short so this is plenty fast
#include "core/regex.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/util.h"

enum { N_CHAR, N_ANY, N_CLASS, N_GROUP, N_LOOK, N_BEHIND, N_BOL, N_EOL };

typedef struct node node;
typedef struct alt alt;

typedef struct {
    uint32_t lo, hi;
} range;

struct alt { // one alternative = a sequence of quantified nodes
    node **items;
    int n, cap;
};

struct node {
    int type;
    uint32_t cp;      // N_CHAR
    range *ranges;    // N_CLASS
    int nranges;
    bool negate;      // N_CLASS, N_LOOK, N_BEHIND
    alt **alts;       // N_GROUP, N_LOOK
    int nalts;
    int min, max;     // repeat counts, max -1 = no limit
};

typedef struct {
    const uint32_t *s;
    int i, n;
    bool bad;
} parser;

static node *mk(int type) {
    node *x = calloc(1, sizeof *x);
    if (!x) exit(1);
    x->type = type;
    x->min = x->max = 1;
    return x;
}

static void free_alts(alt **alts, int n);

static void free_node(node *x) {
    if (!x) return;
    free(x->ranges);
    free_alts(x->alts, x->nalts);
    free(x);
}

static void free_alts(alt **alts, int n) {
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < alts[i]->n; j++) free_node(alts[i]->items[j]);
        free(alts[i]->items);
        free(alts[i]);
    }
    free(alts);
}

static void add_range(node *x, uint32_t lo, uint32_t hi) {
    x->ranges = xrealloc(x->ranges, (size_t)(x->nranges + 1) * sizeof *x->ranges);
    x->ranges[x->nranges++] = (range){lo, hi};
}

// \d \w \s as ranges. like python 3, \w counts letters from any language (é, ж, 山, 김),
// approximated by the big letter blocks rather than the full unicode tables
static bool add_shorthand(node *x, uint32_t c) {
    switch (c) {
    case 'd': add_range(x, '0', '9'); return true;
    case 'w':
        add_range(x, 'a', 'z'), add_range(x, 'A', 'Z'), add_range(x, '0', '9'), add_range(x, '_', '_');
        add_range(x, 0xAA, 0xAA), add_range(x, 0xB5, 0xB5), add_range(x, 0xBA, 0xBA);
        add_range(x, 0xC0, 0xD6), add_range(x, 0xD8, 0xF6), add_range(x, 0xF8, 0x2FF); // latin accents
        add_range(x, 0x370, 0x1FFF);                                                    // greek, cyrillic, arabic, indic...
        add_range(x, 0x3040, 0x30FF), add_range(x, 0x3400, 0x4DBF), add_range(x, 0x4E00, 0x9FFF); // kana + cjk
        add_range(x, 0xAC00, 0xD7AF), add_range(x, 0xF900, 0xFAFF);                    // hangul, cjk compat
        add_range(x, 0xFF10, 0xFF19), add_range(x, 0xFF21, 0xFF3A), add_range(x, 0xFF41, 0xFF5A); // fullwidth
        return true;
    case 's': add_range(x, ' ', ' '), add_range(x, '\t', '\r'); return true;
    }
    return false;
}

static int hexv(uint32_t c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
    return -1;
}

// reads the char after a backslash: \uXXXX, \n, \t or just the char itself
static uint32_t escaped(parser *p) {
    uint32_t c = p->s[p->i++];
    if (c == 'u' && p->i + 4 <= p->n) {
        uint32_t v = 0;
        for (int k = 0; k < 4; k++) {
            int h = hexv(p->s[p->i + k]);
            if (h < 0) return 'u';
            v = v << 4 | (uint32_t)h;
        }
        p->i += 4;
        return v;
    }
    if (c == 'n') return '\n';
    if (c == 't') return '\t';
    return c;
}

static int parse_int(parser *p) {
    int v = -1;
    while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') {
        v = (v < 0 ? 0 : v * 10) + (int)(p->s[p->i++] - '0');
        if (v > 10000) v = 10000;
    }
    return v;
}

static alt **parse_alts(parser *p, int *nalts, int depth);

static node *parse_class(parser *p) {
    node *x = mk(N_CLASS);
    if (p->i < p->n && p->s[p->i] == '^') x->negate = true, p->i++;
    bool first = true;
    while (p->i < p->n && (p->s[p->i] != ']' || first)) {
        first = false;
        uint32_t c = p->s[p->i++];
        if (c == '\\' && p->i < p->n) {
            if (p->s[p->i] == 'D' || p->s[p->i] == 'W' || p->s[p->i] == 'S') p->bad = true;
            if (add_shorthand(x, p->s[p->i])) {
                p->i++;
                continue;
            }
            c = escaped(p);
        }
        // a-z range, unless the dash is the last thing in the class
        if (p->i + 1 < p->n && p->s[p->i] == '-' && p->s[p->i + 1] != ']') {
            p->i++;
            uint32_t hi = p->s[p->i++];
            if (hi == '\\' && p->i < p->n) hi = escaped(p);
            if (hi < c) p->bad = true;
            add_range(x, c, hi);
        } else {
            add_range(x, c, c);
        }
    }
    if (p->i >= p->n) p->bad = true; // never closed
    else p->i++;
    return x;
}

static node *parse_atom(parser *p, int depth) {
    uint32_t c = p->s[p->i++];
    node *x;
    switch (c) {
    case '.': return mk(N_ANY);
    case '^': return mk(N_BOL);
    case '$': return mk(N_EOL);
    case '[': return parse_class(p);
    case '(': {
        int type = N_GROUP;
        bool negate = false;
        if (p->i + 1 < p->n && p->s[p->i] == '?') {
            uint32_t k = p->s[p->i + 1];
            if (k == ':') p->i += 2;
            else if (k == '=' || k == '!') type = N_LOOK, negate = k == '!', p->i += 2;
            else if (k == '<' && p->i + 2 < p->n && (p->s[p->i + 2] == '=' || p->s[p->i + 2] == '!'))
                type = N_BEHIND, negate = p->s[p->i + 2] == '!', p->i += 3;
            else p->bad = true; // named groups, flags: not supported
        }
        x = mk(type);
        x->negate = negate;
        x->alts = parse_alts(p, &x->nalts, depth + 1);
        if (p->i >= p->n || p->s[p->i] != ')') p->bad = true;
        else p->i++;
        // lookbehind only works if every branch is a fixed number of plain characters
        for (int a = 0; type == N_BEHIND && a < x->nalts; a++)
            for (int j = 0; j < x->alts[a]->n; j++) {
                node *y = x->alts[a]->items[j];
                if (y->min != y->max || (y->type != N_CHAR && y->type != N_CLASS && y->type != N_ANY)) p->bad = true;
            }
        return x;
    }
    case '\\':
        if (p->i >= p->n) {
            p->bad = true;
            return mk(N_ANY);
        }
        x = mk(N_CLASS);
        if (add_shorthand(x, p->s[p->i])) {
            p->i++;
            return x;
        }
        // \D \W \S = anything except a digit / word char / space
        if (p->s[p->i] == 'D' || p->s[p->i] == 'W' || p->s[p->i] == 'S') {
            add_shorthand(x, p->s[p->i] + ('a' - 'A'));
            x->negate = true;
            p->i++;
            return x;
        }
        if (p->s[p->i] == 'b' || p->s[p->i] == 'B') {
            p->bad = true; // word boundaries, not needed for usernames
            p->i++;
            return x;
        }
        free_node(x);
        x = mk(N_CHAR);
        x->cp = escaped(p);
        return x;
    default:
        if (c == '*' || c == '+' || c == '?' || c == ')' || c == '|') p->bad = true;
        x = mk(N_CHAR);
        x->cp = c;
        return x;
    }
}

static alt **parse_alts(parser *p, int *nalts, int depth) {
    alt **alts = NULL;
    *nalts = 0;
    if (depth > 32) {
        p->bad = true;
        return NULL;
    }
    for (;;) {
        alt *a = calloc(1, sizeof *a);
        if (!a) exit(1);
        alts = xrealloc(alts, (size_t)(*nalts + 1) * sizeof *alts);
        alts[(*nalts)++] = a;
        while (p->i < p->n && p->s[p->i] != '|' && p->s[p->i] != ')' && !p->bad) {
            node *x = parse_atom(p, depth);
            // quantifier
            if (p->i < p->n) {
                uint32_t q = p->s[p->i];
                if (q == '*') x->min = 0, x->max = -1, p->i++;
                else if (q == '+') x->min = 1, x->max = -1, p->i++;
                else if (q == '?') x->min = 0, x->max = 1, p->i++;
                else if (q == '{') {
                    int save = p->i++;
                    int lo = parse_int(p), hi = lo;
                    if (lo < 0 && p->i < p->n && p->s[p->i] == ',') lo = 0; // {,5} means {0,5}
                    if (p->i < p->n && p->s[p->i] == ',') p->i++, hi = parse_int(p);
                    if (lo < 0 || p->i >= p->n || p->s[p->i] != '}') p->i = save; // literal {
                    else {
                        p->i++;
                        x->min = lo, x->max = hi;
                        if (hi >= 0 && hi < lo) p->bad = true;
                    }
                }
                if (p->i < p->n && p->s[p->i] == '?') p->i++; // lazy marker, we match the same either way for a yes/no answer
            }
            if (a->n == a->cap) a->cap = a->cap ? a->cap * 2 : 8, a->items = xrealloc(a->items, (size_t)a->cap * sizeof *a->items);
            a->items[a->n++] = x;
        }
        if (p->i < p->n && p->s[p->i] == '|') {
            p->i++;
            continue;
        }
        return alts;
    }
}

// ---- matching, continuation style so groups and repeats can backtrack into each other ----

typedef struct k_t k_t;
struct k_t {
    bool (*fn)(const k_t *k, int pos);
    const alt *seq;
    int idx, count;
    const k_t *next;
    const uint32_t *t;
    int n;
    int start; // where the current repeat iteration began (stops empty loops)
    int *budget;
};

static bool match_seq(const alt *a, int i, int pos, const k_t *next, const uint32_t *t, int n, int *budget);

static bool k_done(const k_t *k, int pos) {
    (void)k, (void)pos;
    return true;
}
static bool k_ends_at(const k_t *k, int pos) { return pos == k->start; }

// how many characters a (fixed length) lookbehind branch covers
static int fixed_len(const alt *a) {
    int len = 0;
    for (int j = 0; j < a->n; j++) len += a->items[j]->min;
    return len;
}

static bool class_has(const node *x, uint32_t c) {
    bool in = false;
    for (int r = 0; r < x->nranges && !in; r++) in = c >= x->ranges[r].lo && c <= x->ranges[r].hi;
    return in != x->negate;
}

static bool match_rep(const alt *a, int i, int count, int pos, int start, const k_t *next, const uint32_t *t, int n,
                      int *budget);

static bool k_rep(const k_t *k, int pos) {
    // a repeat that matched nothing would loop forever, only allow it once min is met
    const node *x = k->seq->items[k->idx];
    if (pos == k->start && k->count + 1 > x->min) return false;
    return match_rep(k->seq, k->idx, k->count + 1, pos, pos, k->next, k->t, k->n, k->budget);
}

// matches node x once at pos, then calls k
static bool match_one(const node *x, int pos, const k_t *k, const uint32_t *t, int n, int *budget) {
    if (--*budget < 0) return false; // pathological pattern, give up
    switch (x->type) {
    case N_CHAR: return pos < n && t[pos] == x->cp && k->fn(k, pos + 1);
    case N_ANY: return pos < n && t[pos] != '\n' && k->fn(k, pos + 1);
    case N_CLASS: return pos < n && class_has(x, t[pos]) && k->fn(k, pos + 1);
    case N_GROUP:
        for (int a = 0; a < x->nalts; a++)
            if (match_seq(x->alts[a], 0, pos, k, t, n, budget)) return true;
        return false;
    }
    return false;
}

static bool match_rep(const alt *a, int i, int count, int pos, int start, const k_t *next, const uint32_t *t, int n,
                      int *budget) {
    const node *x = a->items[i];
    // greedy: try one more first, then fall back to stopping here
    if (x->max < 0 || count < x->max) {
        k_t k = {k_rep, a, i, count, next, t, n, start, budget};
        if (match_one(x, pos, &k, t, n, budget)) return true;
    }
    if (count >= x->min) return match_seq(a, i + 1, pos, next, t, n, budget);
    return false;
}

static bool match_seq(const alt *a, int i, int pos, const k_t *next, const uint32_t *t, int n, int *budget) {
    if (i == a->n) return next->fn(next, pos);
    const node *x = a->items[i];
    switch (x->type) {
    case N_BOL: return pos == 0 && match_seq(a, i + 1, pos, next, t, n, budget);
    case N_EOL: return pos == n && match_seq(a, i + 1, pos, next, t, n, budget);
    case N_BEHIND: {
        // does some branch end exactly here?
        k_t here = {k_ends_at, NULL, 0, 0, NULL, t, n, pos, budget};
        bool hit = false;
        for (int g = 0; g < x->nalts && !hit; g++) {
            int len = fixed_len(x->alts[g]);
            hit = pos >= len && match_seq(x->alts[g], 0, pos - len, &here, t, n, budget);
        }
        return hit != x->negate && match_seq(a, i + 1, pos, next, t, n, budget);
    }
    case N_LOOK: {
        k_t done = {k_done, NULL, 0, 0, NULL, t, n, 0, budget};
        bool hit = false;
        for (int g = 0; g < x->nalts && !hit; g++) hit = match_seq(x->alts[g], 0, pos, &done, t, n, budget);
        return hit != x->negate && match_seq(a, i + 1, pos, next, t, n, budget);
    }
    }
    return match_rep(a, i, 0, pos, pos, next, t, n, budget);
}

static uint32_t *decode(const char *s, int *n) {
    size_t len = strlen(s);
    uint32_t *out = xmalloc((len + 1) * sizeof *out);
    int k = 0;
    const unsigned char *u = (const unsigned char *)s;
    for (size_t i = 0; i < len;) {
        uint32_t c = u[i];
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra) c &= extra == 3 ? 0x07 : extra == 2 ? 0x0F : 0x1F;
        i++;
        for (int e = 0; e < extra && i < len; e++, i++) c = c << 6 | (u[i] & 0x3F);
        out[k++] = c;
    }
    *n = k;
    return out;
}

bool rx_search(const char *pattern, const char *text, bool *unsupported) {
    *unsupported = false;
    parser p = {0};
    uint32_t *pat = decode(pattern, &p.n);
    p.s = pat;
    int nalts;
    alt **alts = parse_alts(&p, &nalts, 0);
    if (p.i < p.n) p.bad = true; // stray ) or similar
    bool result = true;
    if (p.bad) {
        *unsupported = true;
    } else {
        int n;
        uint32_t *t = decode(text, &n);
        int budget = 200000;
        k_t done = {k_done, NULL, 0, 0, NULL, t, n, 0, &budget};
        result = false;
        for (int start = 0; start <= n && !result; start++)
            for (int a = 0; a < nalts && !result; a++) result = match_seq(alts[a], 0, start, &done, t, n, &budget);
        if (budget < 0) *unsupported = true, result = true; // ran out of patience, don't block the name
        free(t);
    }
    free_alts(alts, nalts);
    free(pat);
    return result;
}

bool rx_full(const char *pattern, const char *text, bool *unsupported) {
    // wrap in ^(?:...)$ so the match has to cover the whole string
    char *full = xprintf("^(?:%s)$", pattern);
    bool r = rx_search(full, text, unsupported);
    free(full);
    return r;
}

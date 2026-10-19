#ifndef ODIN_UTIL_H
#define ODIN_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// windows' default printf is pre-c99, the makefile switches mingw to its c99 one (%zu etc)
// and this makes the format warnings check against that one too
#ifdef _WIN32
#define ODIN_PRINTF(fmt, args) __attribute__((format(gnu_printf, fmt, args)))
#else
#define ODIN_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))
#endif

// growable string, used everywhere we build text
typedef struct {
    char *s;
    size_t len, cap;
} sbuf;

void sb_init(sbuf *b);
void sb_putc(sbuf *b, char c);
void sb_putn(sbuf *b, const char *s, size_t n);
void sb_puts(sbuf *b, const char *s);
void sb_printf(sbuf *b, const char *fmt, ...) ODIN_PRINTF(2, 3);
void sb_rune(sbuf *b, uint32_t cp);
char *sb_take(sbuf *b); // hands over the string, caller frees
void sb_free(sbuf *b);

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xprintf(const char *fmt, ...) ODIN_PRINTF(1, 2);

// grow a dynamic array by one slot, dies on oom
#define VEC_PUSH(arr, len, cap)                                         \
    do {                                                                \
        if ((len) == (cap)) {                                           \
            (cap) = (cap) ? (cap) * 2 : 8;                              \
            (arr) = xrealloc((arr), (cap) * sizeof(*(arr)));            \
        }                                                               \
        (len)++;                                                        \
    } while (0)

unsigned char *read_file(const char *path, size_t *len);

// memmem but takes a c string needle
const unsigned char *memfind(const unsigned char *hay, size_t hlen, const char *needle);
#define STARTS(b, n, lit) ((n) >= sizeof(lit) - 1 && memcmp((b), (lit), sizeof(lit) - 1) == 0)

uint16_t rd16be(const unsigned char *p);
uint32_t rd32be(const unsigned char *p);
uint16_t rd16le(const unsigned char *p);
uint32_t rd32le(const unsigned char *p);

char *latin1_to_utf8(const unsigned char *b, size_t n);
char *utf16_to_utf8(const unsigned char *b, size_t n, bool big_endian);
char *xml_unescape(const char *s, size_t n);
char *trim_dup(const char *s, size_t n);

size_t utf8_width(const char *s);
bool ends_with(const char *s, const char *suffix);
void human_size(long long n, char *out, size_t outlen);

#endif

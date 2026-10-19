#include "ui/lineedit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/platform.h"
#include "core/util.h"

#define HISTORY_MAX 200

static char **history;
static size_t hist_len, hist_cap;

void lineedit_history_add(const char *line) {
    if (!*line) return;
    if (hist_len && !strcmp(history[hist_len - 1], line)) return; // no back to back dupes
    if (hist_len == HISTORY_MAX) {
        free(history[0]);
        memmove(history, history + 1, (hist_len - 1) * sizeof *history);
        hist_len--;
    }
    VEC_PUSH(history, hist_len, hist_cap);
    history[hist_len - 1] = xstrdup(line);
}

// plain line read for pipes, scripts and anything that isn't a terminal
static char *read_plain(const char *prompt, bool show_prompt) {
    if (show_prompt) {
        fputs(prompt, stdout);
        fflush(stdout);
    }
    sbuf b;
    sb_init(&b);
    int c;
    while ((c = getchar()) != EOF && c != '\n') sb_putc(&b, (char)c);
    if (c == EOF && !b.len) {
        sb_free(&b);
        return NULL;
    }
    if (b.len && b.s[b.len - 1] == '\r') b.s[--b.len] = 0;
    return sb_take(&b);
}

#ifdef _WIN32
#include <windows.h>

// the windows console already does line editing + up/down history in cooked mode, we just
// read it as utf-16 so non-english text survives, then convert to utf-8
char *lineedit_read(const char *prompt, size_t prompt_width) {
    (void)prompt_width;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (!GetConsoleMode(in, &mode)) return read_plain(prompt, false);
    fputs(prompt, stdout);
    fflush(stdout);

    sbuf out;
    sb_init(&out);
    wchar_t w[1024];
    for (;;) {
        DWORD n = 0;
        if (!ReadConsoleW(in, w, (DWORD)(sizeof w / sizeof *w), &n, NULL)) {
            sb_free(&out);
            return GetLastError() == ERROR_OPERATION_ABORTED ? xstrdup("") : NULL;
        }
        if (n == 0) { // ctrl+c lands here
            sb_free(&out);
            return xstrdup("");
        }
        char u[4096];
        int len = WideCharToMultiByte(CP_UTF8, 0, w, (int)n, u, sizeof u, NULL, NULL);
        if (len > 0) sb_putn(&out, u, (size_t)len);
        if (out.len && out.s[out.len - 1] == '\n') break;
    }
    while (out.len && (out.s[out.len - 1] == '\n' || out.s[out.len - 1] == '\r')) out.s[--out.len] = 0;
    if (out.len && out.s[0] == 0x1A) { // ctrl+z then enter = end of input on windows
        sb_free(&out);
        return NULL;
    }
    return sb_take(&out);
}

#else
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

typedef struct {
    char *buf;
    size_t len, cap, pos; // pos = byte offset of the cursor
    const char *prompt;
    size_t pw;            // prompt width in columns
    size_t old_cursor;    // cursor column (prompt included) at the last redraw
    size_t max_rows;      // most rows this line has taken up so far
    size_t hist_index;
    char *draft;          // what was typed before scrolling into history
} edit_t;

static struct termios saved;

static int term_cols(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    return 80;
}

static bool raw_on(void) {
    if (tcgetattr(STDIN_FILENO, &saved) == -1) return false;
    struct termios raw = saved;
    raw.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_cflag |= CS8;
    // no echo, no line buffering, and ctrl+c comes to us as a key instead of a signal
    raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    return tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0;
}

static void raw_off(void) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved); }

// columns taken by n bytes of utf-8 (one per codepoint, good enough for paths and names)
static size_t width(const char *s, size_t n) {
    size_t w = 0;
    for (size_t i = 0; i < n; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) w++;
    return w;
}

static void out(sbuf *b, const char *fmt, int n) { sb_printf(b, fmt, n); }

// redraws the whole line. handles lines that wrap onto several rows (long dragged-in paths),
// same approach as linenoise's multi-line mode
static void refresh(edit_t *e) {
    int cols = term_cols();
    size_t total = e->pw + width(e->buf, e->len);
    size_t cursor = e->pw + width(e->buf, e->pos);
    size_t rows = (total + (size_t)cols - 1) / (size_t)cols;
    if (!rows) rows = 1;
    size_t old_row = (e->old_cursor + (size_t)cols) / (size_t)cols; // 1 based
    size_t old_max = e->max_rows ? e->max_rows : 1;
    if (rows > e->max_rows) e->max_rows = rows;

    sbuf b;
    sb_init(&b);
    // go to the last row we used, then clear upwards
    if (old_max > old_row) out(&b, "\x1b[%dB", (int)(old_max - old_row));
    for (size_t i = 1; i < old_max; i++) sb_puts(&b, "\r\x1b[0K\x1b[1A");
    sb_puts(&b, "\r\x1b[0K");
    sb_puts(&b, e->prompt);
    sb_putn(&b, e->buf, e->len);
    // cursor sitting exactly at the right edge: terminals don't wrap until the next char, so force it
    if (e->pos && e->pos == e->len && total % (size_t)cols == 0) {
        sb_puts(&b, "\n\r");
        rows++;
        if (rows > e->max_rows) e->max_rows = rows;
    }
    size_t cur_row = (cursor + (size_t)cols) / (size_t)cols;
    if (rows > cur_row) out(&b, "\x1b[%dA", (int)(rows - cur_row));
    int col = (int)(cursor % (size_t)cols);
    if (col) out(&b, "\r\x1b[%dC", col);
    else sb_puts(&b, "\r");
    e->old_cursor = cursor;
    if (write(STDOUT_FILENO, b.s, b.len) < 0) {}
    sb_free(&b);
}

static void set_text(edit_t *e, const char *s) {
    size_t n = strlen(s);
    if (n + 1 > e->cap) e->cap = n + 64, e->buf = xrealloc(e->buf, e->cap);
    memcpy(e->buf, s, n + 1);
    e->len = e->pos = n;
}

static void insert(edit_t *e, const char *s, size_t n) {
    if (e->len + n + 1 > e->cap) e->cap = (e->len + n + 1) * 2, e->buf = xrealloc(e->buf, e->cap);
    memmove(e->buf + e->pos + n, e->buf + e->pos, e->len - e->pos + 1);
    memcpy(e->buf + e->pos, s, n);
    e->len += n, e->pos += n;
}

static size_t prev_char(const edit_t *e, size_t i) {
    if (!i) return 0;
    do i--;
    while (i && ((unsigned char)e->buf[i] & 0xC0) == 0x80);
    return i;
}

static size_t next_char(const edit_t *e, size_t i) {
    if (i >= e->len) return e->len;
    do i++;
    while (i < e->len && ((unsigned char)e->buf[i] & 0xC0) == 0x80);
    return i;
}

static void delete_range(edit_t *e, size_t from, size_t to) {
    memmove(e->buf + from, e->buf + to, e->len - to + 1);
    e->len -= to - from;
    if (e->pos > to) e->pos -= to - from;
    else if (e->pos > from) e->pos = from;
}

static void history_step(edit_t *e, int dir) {
    if (!hist_len) return;
    if (e->hist_index == hist_len) { // leaving the line being typed, keep it for later
        free(e->draft);
        e->draft = xstrdup(e->buf);
    }
    if (dir < 0 && e->hist_index > 0) e->hist_index--;
    else if (dir > 0 && e->hist_index < hist_len) e->hist_index++;
    else return;
    set_text(e, e->hist_index == hist_len ? (e->draft ? e->draft : "") : history[e->hist_index]);
}

static int read_byte(void) {
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    return n == 1 ? c : -1;
}

char *lineedit_read(const char *prompt, size_t prompt_width) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return read_plain(prompt, isatty(STDOUT_FILENO));
    const char *term = getenv("TERM");
    if (term && (!strcmp(term, "dumb") || !strcmp(term, "cons25"))) return read_plain(prompt, true);
    fflush(stdout);
    if (!raw_on()) return read_plain(prompt, true);

    edit_t e = {0};
    e.cap = 128;
    e.buf = xmalloc(e.cap);
    e.buf[0] = 0;
    e.prompt = prompt;
    e.pw = prompt_width;
    e.hist_index = hist_len;
    refresh(&e);

    char *result = NULL;
    for (;;) {
        int c = read_byte();
        if (c < 0) break; // input closed
        if (c == '\r' || c == '\n') {
            e.pos = e.len;
            refresh(&e);
            result = xstrdup(e.buf);
            break;
        }
        switch (c) {
        case 3: // ctrl+c: drop the line, fresh prompt
            if (write(STDOUT_FILENO, "^C", 2) < 0) {}
            result = xstrdup("");
            goto done;
        case 4: // ctrl+d: quit on an empty line, otherwise delete forward
            if (!e.len) goto done;
            delete_range(&e, e.pos, next_char(&e, e.pos));
            break;
        case 127:
        case 8: // backspace
            if (e.pos) delete_range(&e, prev_char(&e, e.pos), e.pos);
            break;
        case 1: e.pos = 0; break;                       // ctrl+a
        case 5: e.pos = e.len; break;                   // ctrl+e
        case 2: e.pos = prev_char(&e, e.pos); break;    // ctrl+b
        case 6: e.pos = next_char(&e, e.pos); break;    // ctrl+f
        case 11: delete_range(&e, e.pos, e.len); break; // ctrl+k
        case 21: delete_range(&e, 0, e.pos); break;     // ctrl+u
        case 23: {                                      // ctrl+w: delete the word before the cursor
            size_t i = e.pos;
            while (i && e.buf[i - 1] == ' ') i--;
            while (i && e.buf[i - 1] != ' ') i--;
            delete_range(&e, i, e.pos);
            break;
        }
        case 12: // ctrl+l
            if (write(STDOUT_FILENO, "\x1b[H\x1b[2J", 7) < 0) {}
            e.max_rows = 0, e.old_cursor = 0;
            break;
        case 16: history_step(&e, -1); break; // ctrl+p
        case 14: history_step(&e, 1); break;  // ctrl+n
        case 9: break;                        // tab, nothing to complete yet
        case 27: { // escape sequences: arrows, home/end, delete
            int a = read_byte(), b = read_byte();
            if (a == '[' && b >= '0' && b <= '9') {
                int t = read_byte();
                if (t == '~') {
                    if (b == '3') delete_range(&e, e.pos, next_char(&e, e.pos));
                    else if (b == '1' || b == '7') e.pos = 0;
                    else if (b == '4' || b == '8') e.pos = e.len;
                }
            } else if (a == '[' || a == 'O') {
                if (b == 'A') history_step(&e, -1);
                else if (b == 'B') history_step(&e, 1);
                else if (b == 'C') e.pos = next_char(&e, e.pos);
                else if (b == 'D') e.pos = prev_char(&e, e.pos);
                else if (b == 'H') e.pos = 0;
                else if (b == 'F') e.pos = e.len;
            }
            break;
        }
        default:
            if (c < 32) break; // other control keys do nothing
            char ch[4] = {(char)c};
            size_t n = 1, want = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            while (n < want) { // grab the rest of a multi-byte character in one go
                int more = read_byte();
                if (more < 0) break;
                ch[n++] = (char)more;
            }
            insert(&e, ch, n);
        }
        refresh(&e);
    }
done:
    raw_off();
    if (write(STDOUT_FILENO, "\n", 1) < 0) {}
    free(e.buf), free(e.draft);
    return result;
}
#endif

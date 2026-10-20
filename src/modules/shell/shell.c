#include "modules/shell/shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <glob.h>
#include <unistd.h>
#endif

#include "core/platform.h"
#include "core/util.h"
#include "modules/domain/domain.h"
#include "modules/email/email.h"
#include "modules/ip/ip.h"
#include "modules/meta/meta_cmd.h"
#include "modules/phone/phone.h"
#include "modules/upgrade/upgrade_cmd.h"
#include "modules/usage.h"
#include "modules/user/user_cmd.h"
#include "ui/banner.h"
#include "ui/color.h"
#include "ui/lineedit.h"

typedef struct {
    char **v;
    int n, cap;
} words_t;

static void push_word(words_t *w, char *s) {
    if (w->n == w->cap) {
        w->cap = w->cap ? w->cap * 2 : 8;
        w->v = xrealloc(w->v, (size_t)w->cap * sizeof *w->v);
    }
    w->v[w->n++] = s;
}

// a finished word: swap a leading ~ for the home folder and expand *.jpg style globs
static void finish_word(words_t *w, sbuf *cur, bool tilde, bool globby) {
    char *s = sb_take(cur);
    const char *home = platform_home();
    if (tilde && home && (s[0] == '~') && (s[1] == 0 || s[1] == '/' || s[1] == '\\')) {
        char *t = xprintf("%s%s", home, s + 1);
        free(s);
        s = t;
    }
#ifndef _WIN32
    if (globby) {
        glob_t g;
        if (glob(s, 0, NULL, &g) == 0 && g.gl_pathc) {
            for (size_t i = 0; i < g.gl_pathc; i++) push_word(w, xstrdup(g.gl_pathv[i]));
            globfree(&g);
            free(s);
            return;
        }
        globfree(&g); // no matches, keep it as typed
    }
#else
    (void)globby;
#endif
    push_word(w, s);
}

int shell_split(const char *line, char ***out, char *err, size_t errlen) {
    words_t w = {0};
    sbuf cur;
    sb_init(&cur);
    bool in_word = false, tilde = false, globby = false;
    char quote = 0;
    for (const char *p = line;; p++) {
        char c = *p;
        if (!c) {
            if (quote) {
                snprintf(err, errlen, "missing a closing %c quote", quote);
                sb_free(&cur);
                shell_free_words(w.v, w.n);
                return -1;
            }
            if (in_word) finish_word(&w, &cur, tilde, globby);
            break;
        }
        if (quote) {
            if (c == quote) quote = 0;
#ifndef _WIN32
            else if (quote == '"' && c == '\\' && p[1] && strchr("\"\\$`", p[1])) sb_putc(&cur, *++p);
#endif
            else sb_putc(&cur, c);
            continue;
        }
        if (c == ' ' || c == '\t') {
            if (in_word) finish_word(&w, &cur, tilde, globby);
            in_word = tilde = globby = false;
            continue;
        }
        if (!in_word) {
            in_word = true;
            tilde = c == '~';
        }
        if (c == '"' || c == '\'') {
            quote = c;
#ifndef _WIN32
        } else if (c == '\\' && p[1]) {
            // mac terminal turns a dragged-in "IMG 1.HEIC" into IMG\ 1.HEIC.
            // (not on windows, there \ is the folder separator)
            sb_putc(&cur, *++p);
#endif
        } else {
            if (c == '*' || c == '?' || c == '[') globby = true;
            sb_putc(&cur, c);
        }
    }
    *out = w.v;
    return w.n;
}

void shell_free_words(char **words, int n) {
    for (int i = 0; i < n; i++) free(words[i]);
    free(words);
}

static const char *shell_help =
    "\n"
    "you're inside odin, so just type the command:\n"
    "  meta <path...>       rip metadata out of files and folders\n"
    "  user <name...>       hunt for a username across a pile of sites\n"
    "  domain <domain...>   dns, registration, subdomains for a domain\n"
    "  ip <addr...>         who owns an ip, geo, reverse dns\n"
    "  email <addr...>      validate + profile an email address\n"
    "  phone <number...>    parse + validate a phone number\n"
    "  upgrade              update odin to the newest release\n"
    "  help                 this screen (help all = every flag)\n"
    "  clear                clean the screen\n"
    "  exit                 leave (ctrl+d works too)\n"
    "\n"
    "up/down arrows go back through what you typed. ctrl+c stops a running\n"
    "scan and brings you back here. drag a file into the window to paste its path.\n"
    "\n"
    "examples:\n"
    "  meta ~/Downloads/IMG_1930.HEIC\n"
    "  user johndoe -a\n"
    "  domain example.com\n"
    "  phone +14155552671\n\n";

static bool is_word(const char *s, const char *const *list) {
    for (; *list; list++)
        if (!strcmp(s, *list)) return true;
    return false;
}

// runs one line. returns false when it's time to leave
static bool run_line(char **argv, int argc) {
    if (argc && !strcmp(argv[0], "odin")) argv++, argc--; // "odin meta x" works too
    if (!argc) return true;

    static const char *const quit_words[] = {"exit", "quit", "q", ":q", "bye", NULL};
    const char *cmd = argv[0];
    if (is_word(cmd, quit_words)) return false;

    bool colors = color_on; // flags like --json switch colors off, put them back after
    odin_interrupted = 0;
    if (!strcmp(cmd, "help") || !strcmp(cmd, "?") || !strcmp(cmd, "-h") || !strcmp(cmd, "--help")) {
        fputs(argc > 1 && !strcmp(argv[1], "all") ? usage : shell_help, stdout);
    } else if (!strcmp(cmd, "clear") || !strcmp(cmd, "cls")) {
        if (color_on) fputs("\x1b[H\x1b[2J", stdout);
    } else if (!strcmp(cmd, "banner")) {
        banner_print_again();
    } else if (!strcmp(cmd, "version") || !strcmp(cmd, "-v") || !strcmp(cmd, "--version")) {
        puts("odin " ODIN_VERSION);
    } else if (!strcmp(cmd, "meta") || !strcmp(cmd, "m")) {
        run_meta(argc - 1, argv + 1);
    } else if (!strcmp(cmd, "user") || !strcmp(cmd, "u")) {
        run_user(argc - 1, argv + 1);
    } else if (!strcmp(cmd, "domain") || !strcmp(cmd, "d")) {
        run_domain(argc - 1, argv + 1);
    } else if (!strcmp(cmd, "ip")) {
        run_ip(argc - 1, argv + 1);
    } else if (!strcmp(cmd, "email") || !strcmp(cmd, "e")) {
        run_email(argc - 1, argv + 1);
    } else if (!strcmp(cmd, "phone") || !strcmp(cmd, "p")) {
        run_phone(argc - 1, argv + 1);
    } else if (!strcmp(cmd, "upgrade") || !strcmp(cmd, "update")) {
        if (run_upgrade(argc - 1, argv + 1) == 0 && !(argc > 1 && !strcmp(argv[1], "--check")))
            puts("(if it upgraded: exit and start odin again to use the new version)");
    } else {
        printf("no idea what \"%s\" is, type help\n", cmd);
    }
    if (odin_interrupted) printf("%s\n", c_yellow("stopped"));
    odin_interrupted = 0;
    color_on = colors;
    fflush(stdout);
    return true;
}

int run_shell(void) {
#ifdef _WIN32
    bool interactive = true;
#else
    bool interactive = isatty(STDIN_FILENO);
#endif
    if (interactive) {
        banner_print();
        printf("type %s for commands, %s to leave\n\n", c_bold("help"), c_bold("exit"));
    }
    platform_catch_interrupts();

    char prompt[64];
    snprintf(prompt, sizeof prompt, "%s ", c_red("odin>"));
    for (;;) {
        odin_interrupted = 0;
        char *line = lineedit_read(prompt, 6);
        if (!line) break; // ctrl+d / end of a piped script
        char *trimmed = trim_dup(line, strlen(line));
        free(line);
        if (*trimmed) lineedit_history_add(trimmed);

        char err[128], **words;
        int n = shell_split(trimmed, &words, err, sizeof err);
        free(trimmed);
        if (n < 0) {
            printf("%s\n", err);
            continue;
        }
        bool keep_going = run_line(words, n);
        shell_free_words(words, n);
        if (!keep_going) break;
    }
    if (interactive) puts("later");
    return 0;
}

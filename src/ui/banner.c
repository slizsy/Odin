#include "ui/banner.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "core/util.h"
#include "ui/color.h"

static const char *logo[] = {
    " ▒█████  ▓█████▄  ██▓ ███▄    █ ",
    "▒██▒  ██▒▒██▀ ██▌▓██▒ ██ ▀█   █ ",
    "▒██░  ██▒░██   █▌▒██▒▓██  ▀█ ██▒",
    "▒██   ██░░▓█▄   ▌░██░▓██▒  ▐▌██▒",
    "░ ████▓▒░░▒████▓ ░██░▒██░   ▓██░",
    "░ ▒░▒░▒░  ▒▒▓  ▒ ░▓  ░ ▒░   ▒ ▒ ",
    "  ░ ▒ ▒░  ░ ▒  ▒  ▒ ░░ ░░   ░ ▒░",
    "░ ░ ░ ▒   ░ ░  ░  ▒ ░   ░   ░ ░ ",
    "    ░ ░     ░     ░           ░ ",
    "          ░                     ",
};

// red fading down into dark blood red
static const int shades[] = {196, 196, 160, 160, 124, 124, 88, 88, 52, 52};

// odin's eye, sitting level with the top of the logo, runes underneath.
// each line is split into colored pieces: frame / iris / pupil
typedef struct {
    int color;
    const char *text;
} piece;

#define FRAME 250
#define IRIS 124
#define PUPIL 196
#define RUNE 178

static const piece eye[10][6] = {
    {{FRAME, "  ▄▄▀▀▀▀▀▀▀▀▀▀▀▀▀▄▄"}},
    {{FRAME, "▄▀    "}, {IRIS, "▄▄█████▄▄"}, {FRAME, "    ▀▄"}},
    {{FRAME, "█   "}, {IRIS, "▄██▀▀   ▀▀██▄"}, {FRAME, "   █"}},
    {{FRAME, "█  "}, {IRIS, "██   "}, {PUPIL, "▄███▄"}, {IRIS, "   ██"}, {FRAME, "  █"}},
    {{FRAME, "█  "}, {IRIS, "██   "}, {PUPIL, "▀███▀"}, {IRIS, "   ██"}, {FRAME, "  █"}},
    {{FRAME, "█   "}, {IRIS, "▀██▄▄   ▄▄██▀"}, {FRAME, "   █"}},
    {{FRAME, "▀▄    "}, {IRIS, "▀▀█████▀▀"}, {FRAME, "    ▄▀"}},
    {{FRAME, "  ▀▀▄▄▄▄▄▄▄▄▄▄▄▄▄▀▀"}},
    {{RUNE, "     ᛟ  ᛞ  ᛁ  ᚾ"}},
    {{0, NULL}},
};

static bool shown;

void banner_print(void) {
    if (shown) return;
    banner_print_again();
}

void banner_print_again(void) {
    shown = true;
    putchar('\n');
    for (int i = 0; i < 10; i++) {
        printf(" %s    ", c256(shades[i], logo[i]));
        for (int j = 0; j < 6 && eye[i][j].text; j++) fputs(c256(eye[i][j].color, eye[i][j].text), stdout);
        putchar('\n');
    }
    // centered under the whole thing (logo 32 + gap + eye 21)
    printf("\n%22s%s\n\n", "", c_bold("made by slizsy"));
}

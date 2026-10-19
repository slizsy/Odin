#include <stdio.h>
#include <time.h>

#include "output/output.h"

static int count;

static void jstr(const char *s) {
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"': fputs("\\\"", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '\n': fputs("\\n", stdout); break;
        case '\r': fputs("\\r", stdout); break;
        case '\t': fputs("\\t", stdout); break;
        default:
            if (c < 0x20) printf("\\u%04x", c);
            else putchar(c);
        }
    }
    putchar('"');
}

void json_begin(void) {
    count = 0;
    fputs("[", stdout);
}

void json_end(void) { fputs(count ? "\n]\n" : "]\n", stdout); }

void json_result(const result_t *r) {
    char when[32] = "";
    if (r->mtime) strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", gmtime(&r->mtime));

    fputs(count++ ? ",\n  {" : "\n  {", stdout);
    printf("\n    \"path\": "), jstr(r->path);
    printf(",\n    \"name\": "), jstr(r->name);
    printf(",\n    \"type\": "), jstr(r->type);
    printf(",\n    \"size\": %lld", r->size);
    printf(",\n    \"sha256\": "), jstr(r->sha256);
    printf(",\n    \"modified\": "), jstr(when);
    printf(",\n    \"engine\": "), jstr(r->engine);

    printf(",\n    \"gps\": [");
    for (size_t i = 0; i < r->ngps; i++) {
        printf("%s\n      {\"lat\": %.6f, \"lon\": %.6f, \"source\": ", i ? "," : "", r->gps[i].lat, r->gps[i].lon);
        jstr(r->gps[i].source);
        putchar('}');
    }
    printf(r->ngps ? "\n    ]" : "]");

    printf(",\n    \"findings\": [");
    for (size_t i = 0; i < r->nfindings; i++) {
        const finding_t *f = &r->findings[i];
        printf("%s\n      {\"level\": \"%s\", \"kind\": ", i ? "," : "", level_name(f->level));
        jstr(f->kind);
        printf(", \"detail\": "), jstr(f->detail);
        printf(", \"source\": "), jstr(f->source);
        putchar('}');
    }
    printf(r->nfindings ? "\n    ]" : "]");

    printf(",\n    \"fields\": [");
    for (size_t i = 0; i < r->nfields; i++) {
        const field_t *f = &r->fields[i];
        printf("%s\n      {\"group\": ", i ? "," : "");
        jstr(f->group);
        printf(", \"key\": "), jstr(f->key);
        printf(", \"value\": "), jstr(f->value);
        putchar('}');
    }
    printf(r->nfields ? "\n    ]" : "]");

    printf(",\n    \"errors\": [");
    for (size_t i = 0; i < r->nerrors; i++) {
        if (i) fputs(", ", stdout);
        jstr(r->errors[i]);
    }
    printf("]\n  }");
}

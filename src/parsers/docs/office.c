// docx/xlsx/pptx and odt/ods/odp are zip files full of xml.
// the metadata sits in a couple of known files, and every embedded photo keeps its own exif
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/util.h"
#include "parsers/archive/zip.h"
#include "parsers/docs/docs.h"
#include "parsers/docs/xml.h"
#include "parsers/parsers.h"

#define XML_MAX (32u << 20)
#define IMG_MAX (64u << 20)

typedef struct {
    result_t *r;
    const char *group;
    const char **only; // NULL = take everything
} leaf_ctx;

static void on_leaf(void *vctx, const char *name, const char *text) {
    leaf_ctx *c = vctx;
    if (c->only) {
        bool ok = false;
        for (int i = 0; c->only[i] && !ok; i++) ok = !strcmp(c->only[i], name);
        if (!ok) return;
    }
    result_add(c->r, c->group, name, text);
}

// app.xml has a ton of boring counters, these are the useful ones
static const char *app_keys[] = {"Application", "AppVersion", "Company", "Manager", "TotalTime", "Template",
                                 "HyperlinkBase", "Pages", "Words", "Slides", NULL};

static void leaves(result_t *r, const zip_t *z, const char *file, int depth, const char *group, const char **only) {
    size_t len;
    unsigned char *data = zip_read(z, zip_find(z, file), &len, XML_MAX);
    if (!data) return;
    leaf_ctx c = {r, group, only};
    xml_leaves((const char *)data, len, depth, on_leaf, &c);
    free(data);
}

// walks every <tagname ...> and calls fn with the tag text and what comes right after it
static void each_tag(const char *s, size_t n, const char *tagname,
                     void (*fn)(result_t *, const char *, const char *, size_t, const char *, const char *),
                     result_t *r, const char *group) {
    const char *end = s + n;
    for (const char *p = s; p < end;) {
        const char *t = (const char *)memfind((const unsigned char *)p, (size_t)(end - p), tagname);
        if (!t) break;
        const char *gt = xml_tag_end(t, end);
        if (!gt) break;
        fn(r, group, t, (size_t)(gt - t), gt + 1, end);
        p = gt + 1;
    }
}

// <property name="Client"><vt:lpwstr>ACME</vt:lpwstr></property>
static void custom_prop(result_t *r, const char *group, const char *tag, size_t tl, const char *after, const char *end) {
    char *name = xml_attr(tag, tl, "name");
    if (!name) return;
    while (after < end && isspace((unsigned char)*after)) after++;
    if ((size_t)(end - after) > 4 && !strncmp(after, "<vt:", 4)) {
        const char *gt = xml_tag_end(after, end);
        const char *lt = gt ? memchr(gt, '<', (size_t)(end - gt)) : NULL;
        if (lt) {
            char *v = xml_unescape(gt + 1, (size_t)(lt - gt - 1));
            result_add(r, group, name, v);
            free(v);
        }
    }
    free(name);
}

// <meta:user-defined meta:name="Client">ACME</meta:user-defined>
static void odf_prop(result_t *r, const char *group, const char *tag, size_t tl, const char *after, const char *end) {
    char *name = xml_attr(tag, tl, "meta:name");
    const char *lt = memchr(after, '<', (size_t)(end - after));
    if (name && lt) {
        char *v = xml_unescape(after, (size_t)(lt - after));
        result_add(r, group, name, v);
        free(v);
    }
    free(name);
}

// links to local files or network shares leak paths and usernames. plain web links don't count
static void rel_target(result_t *r, const char *group, const char *tag, size_t tl, const char *after, const char *end) {
    (void)after, (void)end;
    char *mode = xml_attr(tag, tl, "TargetMode");
    char *target = xml_attr(tag, tl, "Target");
    if (mode && target && !strcmp(mode, "External")) {
        if (!strncasecmp(target, "file:", 5) || !strncmp(target, "\\\\", 2) || (strlen(target) > 2 && target[1] == ':'))
            result_add(r, group, "ExternalLink", target);
    }
    free(mode), free(target);
}

static bool is_image(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    static const char *exts[] = {".jpg", ".jpeg", ".png", ".tif", ".tiff", ".webp", NULL};
    for (int i = 0; exts[i]; i++)
        if (!strcasecmp(dot, exts[i])) return true;
    return false;
}

void parse_office(result_t *r, const unsigned char *b, size_t n, const char *pfx, int depth) {
    zip_t z;
    if (!zip_open(&z, b, n)) {
        result_errf(r, "zip: couldn't read the central directory");
        zip_close(&z);
        return;
    }
    char g[600], g2[620];

    if (zip_find(&z, "[Content_Types].xml")) {
        const char *kind = "Office Open XML";
        for (size_t i = 0; i < z.count; i++) {
            const char *nm = z.entries[i].name;
            if (!strncmp(nm, "word/", 5)) { kind = "Word document (docx)"; break; }
            if (!strncmp(nm, "xl/", 3)) { kind = "Excel workbook (xlsx)"; break; }
            if (!strncmp(nm, "ppt/", 4)) { kind = "PowerPoint deck (pptx)"; break; }
        }
        result_set_type(r, pfx, kind);
        snprintf(g, sizeof g, "%sOffice", pfx);
        leaves(r, &z, "docProps/core.xml", 2, g, NULL);
        leaves(r, &z, "docProps/app.xml", 2, g, app_keys);

        size_t len;
        unsigned char *d = zip_read(&z, zip_find(&z, "docProps/custom.xml"), &len, XML_MAX);
        if (d) {
            snprintf(g2, sizeof g2, "%s/custom", g);
            each_tag((const char *)d, len, "<property", custom_prop, r, g2);
            free(d);
        }
        // excel loves saving the full path of wherever the file used to live
        if ((d = zip_read(&z, zip_find(&z, "xl/workbook.xml"), &len, XML_MAX))) {
            const char *ap = (const char *)memfind(d, len, "absPath");
            const char *gt = ap ? xml_tag_end(ap, (const char *)d + len) : NULL;
            if (gt) {
                char *url = xml_attr(ap, (size_t)(gt - ap), "url");
                result_add(r, g, "AbsPath", url);
                free(url);
            }
            free(d);
        }
        for (size_t i = 0; i < z.count; i++) {
            if (!ends_with(z.entries[i].name, ".rels")) continue;
            if ((d = zip_read(&z, &z.entries[i], &len, XML_MAX))) {
                each_tag((const char *)d, len, "<Relationship ", rel_target, r, g);
                free(d);
            }
        }
    } else if (zip_find(&z, "meta.xml")) {
        result_set_type(r, pfx, "OpenDocument");
        snprintf(g, sizeof g, "%sODF", pfx);
        leaves(r, &z, "meta.xml", 3, g, NULL);
        size_t len;
        unsigned char *d = zip_read(&z, zip_find(&z, "meta.xml"), &len, XML_MAX);
        if (d) {
            snprintf(g2, sizeof g2, "%s/custom", g);
            each_tag((const char *)d, len, "<meta:user-defined", odf_prop, r, g2);
            free(d);
        }
    } else {
        snprintf(g, sizeof g, "%sZIP", pfx);
        char cnt[32];
        snprintf(cnt, sizeof cnt, "%zu", z.count);
        result_add(r, g, "Entries", cnt);
        result_add(r, g, "Comment", z.comment);
    }

    // photos pasted into documents keep their exif, gps and all
    int scanned = 0;
    for (size_t i = 0; i < z.count && scanned < 200 && depth == 0; i++) {
        const zip_entry *e = &z.entries[i];
        if (!is_image(e->name) || e->usize > IMG_MAX) continue;
        size_t len;
        unsigned char *d = zip_read(&z, e, &len, IMG_MAX);
        if (!d) continue;
        char sub[600];
        snprintf(sub, sizeof sub, "%s%s › ", pfx, e->name);
        parse_bytes(r, d, len, sub, depth + 1);
        free(d);
        scanned++;
    }
    zip_close(&z);
}

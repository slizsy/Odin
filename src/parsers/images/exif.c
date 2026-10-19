// exif lives in a tiny tiff file: a chain of IFDs (tag tables) that point at each other
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/util.h"
#include "parsers/images/images.h"

typedef struct {
    uint16_t tag;
    const char *name;
} tagname;

// only the tags worth showing, -x gets you everything else through exiftool
static const tagname ifd0_tags[] = {
    {0x010E, "ImageDescription"}, {0x010F, "Make"},      {0x0110, "Model"},
    {0x0112, "Orientation"},      {0x0131, "Software"},  {0x0132, "ModifyDate"},
    {0x013B, "Artist"},           {0x013C, "HostComputer"}, {0x8298, "Copyright"},
    {0x9C9B, "XPTitle"},          {0x9C9C, "XPComment"}, {0x9C9D, "XPAuthor"},
    {0x9C9E, "XPKeywords"},       {0x9C9F, "XPSubject"}, {0, NULL},
};

static const tagname exif_tags[] = {
    {0x829A, "ExposureTime"},     {0x829D, "FNumber"},          {0x8827, "ISO"},
    {0x9003, "DateTimeOriginal"}, {0x9004, "CreateDate"},       {0x9010, "OffsetTime"},
    {0x9011, "OffsetTimeOriginal"}, {0x920A, "FocalLength"},    {0x9286, "UserComment"},
    {0xA002, "ImageWidth"},       {0xA003, "ImageHeight"},      {0xA420, "ImageUniqueID"},
    {0xA430, "CameraOwnerName"},  {0xA431, "BodySerialNumber"}, {0xA432, "LensSpecification"},
    {0xA433, "LensMake"},         {0xA434, "LensModel"},        {0xA435, "LensSerialNumber"},
    {0, NULL},
};

#define TAG_EXIF_IFD 0x8769
#define TAG_GPS_IFD 0x8825
#define MAX_ENTRIES 512

typedef struct {
    const unsigned char *b;
    size_t n;
    bool le;
} tiff_t;

typedef struct {
    uint16_t tag, typ;
    uint32_t count;
    const unsigned char *data;
    size_t len;
} entry_t;

static uint16_t t16(const tiff_t *t, const unsigned char *p) { return t->le ? rd16le(p) : rd16be(p); }
static uint32_t t32(const tiff_t *t, const unsigned char *p) { return t->le ? rd32le(p) : rd32be(p); }

// bytes per value for each tiff type, 0 = unknown
static int type_size(uint16_t typ) {
    static const int sz[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 4};
    return typ < sizeof sz / sizeof *sz ? sz[typ] : 0;
}

static const char *lookup(const tagname *tbl, uint16_t tag) {
    for (; tbl->name; tbl++)
        if (tbl->tag == tag) return tbl->name;
    return NULL;
}

// returns how many entries it read, or -1 if the offset is bogus
static int read_ifd(const tiff_t *t, uint32_t off, entry_t *out) {
    if (off < 8 || (size_t)off + 2 > t->n) return -1;
    int cnt = t16(t, t->b + off);
    size_t start = (size_t)off + 2;
    if (start + (size_t)cnt * 12 > t->n) return -1;
    int k = 0;
    for (int i = 0; i < cnt && k < MAX_ENTRIES; i++) {
        const unsigned char *e = t->b + start + (size_t)i * 12;
        entry_t en = {t16(t, e), t16(t, e + 2), t32(t, e + 4), NULL, 0};
        int sz = type_size(en.typ);
        if (!sz) continue;
        uint64_t total = (uint64_t)sz * en.count;
        if (total > t->n) continue; // garbage count
        if (total <= 4) {
            en.data = e + 8;
        } else {
            uint64_t o = t32(t, e + 8);
            if (o + total > t->n) continue;
            en.data = t->b + o;
        }
        en.len = (size_t)total;
        out[k++] = en;
    }
    return k;
}

static uint32_t entry_u32(const tiff_t *t, const entry_t *e) {
    if (e->len >= 4) return t32(t, e->data);
    if (e->len >= 2) return t16(t, e->data);
    return 0;
}

static double rat(const tiff_t *t, const entry_t *e, size_t i) {
    if ((i + 1) * 8 > e->len) return 0;
    uint32_t num = t32(t, e->data + i * 8), den = t32(t, e->data + i * 8 + 4);
    if (!den) return 0;
    if (e->typ == 10) return (double)(int32_t)num / (double)(int32_t)den;
    return (double)num / den;
}

static char *ascii(const entry_t *e) {
    size_t n = e->len;
    while (n && (e->data[n - 1] == 0 || e->data[n - 1] == ' ')) n--;
    return xstrndup((const char *)e->data, n);
}

static bool printable(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (s[i] < 0x20 && s[i] != 0 && s[i] != '\n' && s[i] != '\r' && s[i] != '\t') return false;
    return true;
}

// first 8 bytes of UserComment say what charset the rest is in
static char *user_comment(const tiff_t *t, const entry_t *e) {
    if (e->len < 8) return xstrdup("");
    if (!memcmp(e->data, "UNICODE", 7)) return utf16_to_utf8(e->data + 8, e->len - 8, !t->le);
    entry_t body = *e;
    body.data += 8, body.len -= 8;
    return ascii(&body);
}

static char *format(const tiff_t *t, const entry_t *e) {
    sbuf o;
    sb_init(&o);
    switch (e->typ) {
    case 2:
        return ascii(e);
    case 1:
    case 7:
        if (e->tag >= 0x9C9B && e->tag <= 0x9C9F) return utf16_to_utf8(e->data, e->len, false);
        if (e->tag == 0x9286) return user_comment(t, e);
        if (printable(e->data, e->len)) return ascii(e);
        for (size_t i = 0; i < e->len && i < 16; i++) sb_printf(&o, i ? " %02x" : "%02x", e->data[i]);
        break;
    case 3:
    case 8:
        for (size_t i = 0; i + 2 <= e->len; i += 2) {
            uint16_t v = t16(t, e->data + i);
            sb_printf(&o, "%s%d", i ? " " : "", e->typ == 8 ? (int)(int16_t)v : (int)v);
        }
        break;
    case 4:
    case 9:
    case 13:
        for (size_t i = 0; i + 4 <= e->len; i += 4) {
            uint32_t v = t32(t, e->data + i);
            if (e->typ == 9) sb_printf(&o, "%s%d", i ? " " : "", (int32_t)v);
            else sb_printf(&o, "%s%u", i ? " " : "", v);
        }
        break;
    case 5:
    case 10:
        for (size_t i = 0; i * 8 < e->len; i++) sb_printf(&o, "%s%g", i ? " " : "", rat(t, e, i));
        break;
    }
    return sb_take(&o);
}

static void parse_gps(result_t *r, const tiff_t *t, const entry_t *ents, int n, const char *group) {
    char lat_ref = 0, lon_ref = 0, date[32] = "";
    const entry_t *lat = NULL, *lon = NULL, *alt = NULL, *ts = NULL;
    bool below = false;
    for (int i = 0; i < n; i++) {
        const entry_t *e = &ents[i];
        switch (e->tag) {
        case 1: lat_ref = e->len ? (char)e->data[0] : 0; break;
        case 2: lat = e; break;
        case 3: lon_ref = e->len ? (char)e->data[0] : 0; break;
        case 4: lon = e; break;
        case 5: below = e->len && e->data[0] == 1; break;
        case 6: alt = e; break;
        case 7: ts = e; break;
        case 29: {
            char *d = ascii(e);
            snprintf(date, sizeof date, "%s", d);
            free(d);
            break;
        }
        }
    }
    if (lat && lon && lat->count == 3 && lon->count == 3) {
        double la = rat(t, lat, 0) + rat(t, lat, 1) / 60 + rat(t, lat, 2) / 3600;
        double lo = rat(t, lon, 0) + rat(t, lon, 1) / 60 + rat(t, lon, 2) / 3600;
        if (lat_ref == 'S' || lat_ref == 's') la = -la;
        if (lon_ref == 'W' || lon_ref == 'w') lo = -lo;
        char buf[64];
        snprintf(buf, sizeof buf, "%.6f", la);
        result_add(r, group, "GPSLatitude", buf);
        snprintf(buf, sizeof buf, "%.6f", lo);
        result_add(r, group, "GPSLongitude", buf);
        result_add_gps(r, la, lo, group);
    }
    if (alt && alt->count == 1) {
        char buf[64];
        snprintf(buf, sizeof buf, "%.1f m", below ? -rat(t, alt, 0) : rat(t, alt, 0));
        result_add(r, group, "GPSAltitude", buf);
    }
    if (*date) {
        char buf[96];
        if (ts && ts->count == 3)
            snprintf(buf, sizeof buf, "%s %02d:%02d:%02d UTC", date, (int)rat(t, ts, 0), (int)rat(t, ts, 1),
                     (int)rat(t, ts, 2));
        else
            snprintf(buf, sizeof buf, "%s UTC", date);
        result_add(r, group, "GPSDateTime", buf);
    }
}

static void add_known(result_t *r, const tiff_t *t, const entry_t *ents, int n, const tagname *tbl,
                      const char *group) {
    for (int i = 0; i < n; i++) {
        const char *name = lookup(tbl, ents[i].tag);
        if (!name) continue;
        char *v = format(t, &ents[i]);
        result_add(r, group, name, v);
        free(v);
    }
}

void parse_exif(result_t *r, const unsigned char *b, size_t n, const char *group) {
    if (n < 8) {
        result_errf(r, "%s: too short", group);
        return;
    }
    tiff_t t = {b, n, false};
    if (!memcmp(b, "II", 2)) t.le = true;
    else if (memcmp(b, "MM", 2)) {
        result_errf(r, "%s: bad tiff header", group);
        return;
    }
    if (t16(&t, b + 2) != 42) {
        result_errf(r, "%s: bad tiff magic", group);
        return;
    }

    entry_t *ents = xmalloc(sizeof(entry_t) * MAX_ENTRIES);
    int cnt = read_ifd(&t, t32(&t, b + 4), ents);
    if (cnt < 0) {
        result_errf(r, "%s: first ifd is out of range", group);
        free(ents);
        return;
    }
    uint32_t exif_off = 0, gps_off = 0;
    for (int i = 0; i < cnt; i++) {
        if (ents[i].tag == TAG_EXIF_IFD) exif_off = entry_u32(&t, &ents[i]);
        if (ents[i].tag == TAG_GPS_IFD) gps_off = entry_u32(&t, &ents[i]);
    }
    add_known(r, &t, ents, cnt, ifd0_tags, group);

    if (exif_off && (cnt = read_ifd(&t, exif_off, ents)) > 0) add_known(r, &t, ents, cnt, exif_tags, group);
    if (gps_off && (cnt = read_ifd(&t, gps_off, ents)) > 0) parse_gps(r, &t, ents, cnt, group);
    free(ents);
}

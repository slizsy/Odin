// iptc = old school press photo metadata (photographer, city, country...),
// stored inside photoshop's 8BIM resource blocks
#include <stdint.h>
#include <string.h>

#include "core/util.h"
#include "parsers/images/images.h"

static const char *iptc_name(unsigned char ds) {
    switch (ds) {
    case 5: return "ObjectName";
    case 25: return "Keywords";
    case 55: return "DateCreated";
    case 60: return "TimeCreated";
    case 65: return "OriginatingProgram";
    case 80: return "By-line";
    case 85: return "By-lineTitle";
    case 90: return "City";
    case 92: return "Sub-location";
    case 95: return "Province-State";
    case 101: return "Country";
    case 110: return "Credit";
    case 115: return "Source";
    case 116: return "CopyrightNotice";
    case 118: return "Contact";
    case 120: return "Caption-Abstract";
    case 122: return "Writer-Editor";
    }
    return NULL;
}

static void parse_iptc(result_t *r, const unsigned char *b, size_t n, const char *group) {
    size_t i = 0;
    while (i + 5 <= n && b[i] == 0x1C) {
        unsigned char rec = b[i + 1], ds = b[i + 2];
        size_t len = rd16be(b + i + 3);
        if (len & 0x8000 || i + 5 + len > n) return; // extended length, never used for text
        const char *name = rec == 2 ? iptc_name(ds) : NULL;
        if (name) result_addn(r, group, name, (const char *)b + i + 5, len);
        i += 5 + len;
    }
}

void parse_8bim(result_t *r, const unsigned char *b, size_t n, const char *group) {
    size_t i = 0;
    while (i + 12 <= n && !memcmp(b + i, "8BIM", 4)) {
        uint16_t id = rd16be(b + i + 4);
        size_t name_len = b[i + 6];
        size_t p = i + 7 + name_len;
        if ((name_len + 1) % 2) p++; // pascal string padded to even
        if (p + 4 > n) return;
        size_t size = rd32be(b + p);
        p += 4;
        if (size > n || p + size > n) return;
        if (id == 0x0404) parse_iptc(r, b + p, size, group);
        i = p + size + (size & 1);
    }
}

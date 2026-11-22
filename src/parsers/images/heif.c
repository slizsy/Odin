// heic (iphone photos) and avif are "ISO media" files: nested boxes, like mp4.
// the metadata lives in numbered items listed in the meta box:
//   iinf = what each item is (we want type 'Exif', and 'mime' items holding xmp)
//   iloc = where each item's bytes are (file offsets, or inside the idat box)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/util.h"
#include "parsers/docs/docs.h"
#include "parsers/images/images.h"

// big iphone shots are split into 48+ tiles, each its own item, so this has to be roomy
#define MAX_ITEMS 4096

typedef struct {
    const unsigned char *p; // box payload
    size_t len;
} box_t;

typedef struct {
    uint32_t id;
    char type[5];
    bool is_xmp;
} item_info;

// finds the first child box of `type` inside [b, b+n). skip = bytes before the children start
static bool find_box(const unsigned char *b, size_t n, const char *type, box_t *out) {
    size_t off = 0;
    while (off + 8 <= n) {
        uint64_t size = rd32be(b + off);
        size_t hdr = 8;
        if (size == 1) {
            if (off + 16 > n) return false;
            size = (uint64_t)rd32be(b + off + 8) << 32 | rd32be(b + off + 12);
            hdr = 16;
        } else if (size == 0) {
            size = n - off; // runs to the end
        }
        if (size < hdr || size > n - off) return false;
        if (!memcmp(b + off + 4, type, 4)) {
            out->p = b + off + hdr;
            out->len = (size_t)size - hdr;
            return true;
        }
        off += (size_t)size;
    }
    return false;
}

// reads an n-byte big endian number (n = 0, 4 or 8 in iloc), false if it runs off the end
static bool rd_sized(const unsigned char **p, const unsigned char *end, int n, uint64_t *out) {
    if (n == 0) return *out = 0, true;
    if (n != 4 && n != 8 && n != 2) return false;
    if (end - *p < n) return false;
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = v << 8 | (*p)[i];
    *p += n;
    *out = v;
    return true;
}

static int read_iinf(box_t iinf, item_info **out) {
    *out = NULL;
    if (iinf.len < 6) return 0;
    const unsigned char *p = iinf.p, *end = iinf.p + iinf.len;
    int version = p[0];
    p += 4;
    uint32_t count = version == 0 ? rd16be(p) : (end - p >= 4 ? rd32be(p) : 0);
    p += version == 0 ? 2 : 4;
    if (count > MAX_ITEMS) count = MAX_ITEMS;
    item_info *items = calloc(count ? count : 1, sizeof *items);
    if (!items) exit(1);
    *out = items;
    int k = 0;
    for (uint32_t i = 0; i < count && k < MAX_ITEMS; i++) {
        box_t infe;
        if (p >= end || !find_box(p, (size_t)(end - p), "infe", &infe)) break;
        size_t box_total = (size_t)(infe.p + infe.len - p);
        const unsigned char *q = infe.p, *qe = infe.p + infe.len;
        if (qe - q >= 4 && q[0] >= 2) { // only v2+ infe has an item type
            int v = q[0];
            q += 4;
            item_info it = {0};
            size_t idlen = v == 2 ? 2 : 4;
            if ((size_t)(qe - q) >= idlen + 2 + 4) {
                it.id = idlen == 2 ? rd16be(q) : rd32be(q);
                q += idlen + 2; // + protection index
                memcpy(it.type, q, 4);
                q += 4;
                if (!memcmp(it.type, "mime", 4)) {
                    // item_name\0 content_type\0, xmp is application/rdf+xml
                    const unsigned char *z = memchr(q, 0, (size_t)(qe - q));
                    if (z && memfind(z + 1, (size_t)(qe - z - 1), "rdf+xml")) it.is_xmp = true;
                }
                items[k++] = it;
            }
        }
        p += box_total;
    }
    return k;
}

typedef struct {
    size_t at, len; // absolute spot in the file buffer
} extent_t;

#define MAX_EXTENTS 64

// finds where an item's bytes live. fills ext with absolute file offsets (idat ones get
// translated too) and returns how many, or -1 if iloc is junk / the item isn't there
static int item_extents(const unsigned char *file, size_t n, box_t iloc, box_t idat, uint32_t want, extent_t *ext) {
    const unsigned char *p = iloc.p, *end = iloc.p + iloc.len;
    if (end - p < 8) return -1;
    int version = p[0];
    p += 4;
    int off_size = p[0] >> 4, len_size = p[0] & 15, base_size = p[1] >> 4;
    int idx_size = version >= 1 ? (p[1] & 15) : 0;
    p += 2;
    uint32_t count;
    if (version < 2) count = rd16be(p), p += 2;
    else if (end - p >= 4) count = rd32be(p), p += 4;
    else return -1;

    for (uint32_t i = 0; i < count; i++) {
        uint64_t id, method = 0, base, ext_count, tmp;
        if (!rd_sized(&p, end, version < 2 ? 2 : 4, &id)) return -1;
        if (version >= 1) {
            if (!rd_sized(&p, end, 2, &tmp)) return -1;
            method = tmp & 15;
        }
        if (!rd_sized(&p, end, 2, &tmp)) return -1; // data_reference_index
        if (!rd_sized(&p, end, base_size, &base) || !rd_sized(&p, end, 2, &ext_count)) return -1;

        int k = 0;
        uint64_t total = 0;
        for (uint64_t e = 0; e < ext_count; e++) {
            uint64_t eoff, elen;
            if (version >= 1 && idx_size && !rd_sized(&p, end, idx_size, &tmp)) return -1;
            if (!rd_sized(&p, end, off_size, &eoff) || !rd_sized(&p, end, len_size, &elen)) return -1;
            if (id != want) continue;
            // method 0 = offset in the file, 1 = offset inside idat
            size_t src_at = method == 1 && idat.p ? (size_t)(idat.p - file) : 0;
            size_t src_len = method == 1 ? idat.len : n;
            uint64_t at = base + eoff;
            if (method > 1 || (method == 1 && !idat.p) || at > src_len || k >= MAX_EXTENTS) return -1;
            if (elen == 0) elen = src_len - at; // 0 = rest of the data
            if (elen > src_len - at) return -1;
            total += elen;
            if (total > (64u << 20)) return -1;
            ext[k++] = (extent_t){src_at + (size_t)at, (size_t)elen};
        }
        if (id == want) return k;
    }
    return -1;
}

// glues an item's extents together. caller frees
static unsigned char *item_bytes(const unsigned char *file, size_t n, box_t iloc, box_t idat, uint32_t want,
                                 size_t *out_len) {
    extent_t ext[MAX_EXTENTS];
    int k = item_extents(file, n, iloc, idat, want, ext);
    if (k < 0) return NULL;
    sbuf data;
    sb_init(&data);
    for (int i = 0; i < k; i++) sb_putn(&data, (const char *)file + ext[i].at, ext[i].len);
    *out_len = data.len;
    return (unsigned char *)sb_take(&data);
}

// finds meta/iinf/iloc/idat. false if this isn't a heif with a meta box
static bool heif_boxes(const unsigned char *b, size_t n, box_t *iinf, box_t *iloc, box_t *idat) {
    box_t meta;
    idat->p = NULL, idat->len = 0;
    if (!find_box(b, n, "meta", &meta) || meta.len < 4) return false;
    // meta is a "full box", 4 bytes of version/flags before its children
    const unsigned char *mp = meta.p + 4;
    size_t ml = meta.len - 4;
    if (!find_box(mp, ml, "iinf", iinf) || !find_box(mp, ml, "iloc", iloc)) return false;
    find_box(mp, ml, "idat", idat);
    return true;
}

// spreads `rep` (exactly as long as the item) back over the item's extents
static void overwrite_item(unsigned char *b, const extent_t *ext, int k, const unsigned char *rep) {
    for (int i = 0; i < k; rep += ext[i].len, i++) memcpy(b + ext[i].at, rep, ext[i].len);
}

int heif_wipe_meta(unsigned char *b, size_t n, int *nexif, int *nxmp) {
    box_t iinf, iloc, idat;
    *nexif = *nxmp = 0;
    if (!heif_boxes(b, n, &iinf, &iloc, &idat)) return -1;

    // empty but still valid stand-ins, so picky readers don't choke on a zeroed item.
    // exif = 4 byte header offset (0) + big endian tiff header + an ifd with no entries.
    // orientation in heic lives in the irot/imir boxes, so nothing to keep from exif
    static const unsigned char empty_exif[] = {0, 0, 0, 0, 'M', 'M', 0, 42, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0};
    static const char empty_xmp[] = "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"/>";

    item_info *items;
    int k = read_iinf(iinf, &items);
    int bad = 0;
    for (int i = 0; i < k; i++) {
        bool exif = !memcmp(items[i].type, "Exif", 4);
        if (!exif && !items[i].is_xmp) continue;
        extent_t ext[MAX_EXTENTS];
        int ne = item_extents(b, n, iloc, idat, items[i].id, ext);
        if (ne < 0) {
            bad++;
            continue;
        }
        size_t total = 0;
        for (int e = 0; e < ne; e++) total += ext[e].len;
        // build the stand in at full size. too small for it? just zeros, nobody reads zeros
        unsigned char *rep = calloc(total ? total : 1, 1);
        if (!rep) exit(1);
        if (exif) {
            if (total >= sizeof empty_exif) memcpy(rep, empty_exif, sizeof empty_exif);
            (*nexif)++;
        } else {
            size_t xl = sizeof empty_xmp - 1;
            if (total >= xl) {
                memcpy(rep, empty_xmp, xl);
                memset(rep + xl, ' ', total - xl); // xmp pads with spaces, that's what the spec does
            }
            (*nxmp)++;
        }
        overwrite_item(b, ext, ne, rep);
        free(rep);
    }
    free(items);
    return bad;
}

bool parse_heif(result_t *r, const unsigned char *b, size_t n, const char *pfx) {
    box_t iinf, iloc, idat;
    if (!heif_boxes(b, n, &iinf, &iloc, &idat)) return false;

    item_info *items;
    int k = read_iinf(iinf, &items);
    bool found = false;
    char g[600];
    for (int i = 0; i < k; i++) {
        bool exif = !memcmp(items[i].type, "Exif", 4);
        if (!exif && !items[i].is_xmp) continue;
        size_t len;
        unsigned char *d = item_bytes(b, n, iloc, idat, items[i].id, &len);
        if (!d) {
            result_errf(r, "heic: couldn't read the %s item", exif ? "exif" : "xmp");
            continue;
        }
        if (exif) {
            // starts with a 4 byte offset to the tiff header (apple puts "Exif\0\0" in between)
            uint32_t skip = len >= 4 ? rd32be(d) : 0;
            snprintf(g, sizeof g, "%sEXIF", pfx);
            if (len >= 4 && skip <= len - 4) parse_exif(r, d + 4 + skip, len - 4 - skip, g);
            else result_errf(r, "heic: exif item has a bad header offset");
        } else {
            snprintf(g, sizeof g, "%sXMP", pfx);
            parse_xmp(r, d, len, g);
        }
        free(d);
        found = true;
    }
    free(items);
    return found;
}

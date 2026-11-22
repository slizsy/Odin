// rebuilds images with only the parts needed to show them. nothing gets re-encoded,
// we just copy the image data over and leave the metadata behind
#include "scrub/scrub.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "core/util.h"
#include "parsers/images/images.h"

static void note(scrub_info *in, const char *what) {
    in->removed++;
    if (strstr(in->what, what)) return; // already listed
    size_t l = strlen(in->what);
    snprintf(in->what + l, sizeof in->what - l, "%s%s", l ? ", " : "", what);
}

static scrub_rc broken(char *err, size_t errlen, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
    return SCRUB_BROKEN;
}

int tiff_orientation(const unsigned char *t, size_t n) {
    if (n < 8) return 0;
    bool le = t[0] == 'I';
    if (!(le && t[1] == 'I') && !(t[0] == 'M' && t[1] == 'M')) return 0;
    uint32_t off = le ? rd32le(t + 4) : rd32be(t + 4);
    if (off < 8 || (size_t)off + 2 > n) return 0;
    int cnt = le ? rd16le(t + off) : rd16be(t + off);
    for (int i = 0; i < cnt; i++) {
        size_t e = (size_t)off + 2 + (size_t)i * 12;
        if (e + 12 > n) return 0;
        uint16_t tag = le ? rd16le(t + e) : rd16be(t + e);
        if (tag != 0x0112) continue;
        int v = le ? rd16le(t + e + 8) : rd16be(t + e + 8); // SHORT, sits right in the entry
        return v >= 1 && v <= 8 ? v : 0;
    }
    return 0;
}

// smallest valid exif that only says which way is up. big endian tiff, one ifd, one tag
static void put_orientation_tiff(sbuf *o, int orient) {
    const char t[] = {'M', 'M', 0, 42, 0, 0, 0, 8,               // header, ifd0 at 8
                      0, 1,                                       // 1 entry
                      0x01, 0x12, 0, 3, 0, 0, 0, 1, 0, (char)orient, 0, 0, // orientation SHORT
                      0, 0, 0, 0};                                // no next ifd
    sb_putn(o, t, sizeof t);
}

// ---------- jpeg ----------

static int jpeg_orientation(const unsigned char *b, size_t n) {
    size_t i = 2;
    while (i + 4 <= n && b[i] == 0xFF) {
        unsigned char m = b[i + 1];
        if (m == 0xDA || m == 0xD9) break;
        if (m == 0xFF) {
            i++;
            continue;
        }
        size_t len = rd16be(b + i + 2);
        if (len < 2 || i + 2 + len > n) break;
        const unsigned char *seg = b + i + 4;
        if (m == 0xE1 && STARTS(seg, len - 2, "Exif\0\0")) return tiff_orientation(seg + 6, len - 8);
        i += 2 + len;
    }
    return 0;
}

// which segments to throw out. returns a label for the summary, NULL = keep it
static const char *jpeg_junk(unsigned char m, const unsigned char *seg, size_t sn) {
    if (m == 0xFE) return "comment";
    if (m < 0xE0 || m > 0xEF) return NULL; // tables, frame headers etc, the actual image
    if (m == 0xE0 && STARTS(seg, sn, "JFIF\0")) return NULL;  // basic header, harmless
    if (m == 0xE2 && STARTS(seg, sn, "ICC_PROFILE\0")) return NULL; // colors look wrong without it
    if (m == 0xEE && STARTS(seg, sn, "Adobe")) return NULL;   // needed to decode cmyk jpegs right
    if (m == 0xE1 && STARTS(seg, sn, "Exif\0\0")) return "exif";
    if (m == 0xE1 && STARTS(seg, sn, "http://ns.adobe.com/xap/1.0/")) return "xmp";
    if (m == 0xE1 && STARTS(seg, sn, "http://ns.adobe.com/xmp/extension/")) return "xmp";
    if (m == 0xED) return "iptc";
    if (m == 0xE2 && STARTS(seg, sn, "MPF\0")) return "extra images";
    if (m == 0xE0 && STARTS(seg, sn, "JFXX\0")) return "thumbnail";
    return "app data";
}

// writes a tiny exif segment with just the rotation, once. 1 = already upright, skip it
static void put_jpeg_orientation(sbuf *o, scrub_info *in, int *orient) {
    if (*orient <= 1) return;
    sbuf ex;
    sb_init(&ex);
    sb_putn(&ex, "Exif\0\0", 6);
    put_orientation_tiff(&ex, *orient);
    unsigned char hdr[4] = {0xFF, 0xE1, (unsigned char)((ex.len + 2) >> 8), (unsigned char)(ex.len + 2)};
    sb_putn(o, (const char *)hdr, 4);
    sb_putn(o, ex.s, ex.len);
    sb_free(&ex);
    in->orientation = *orient;
    *orient = 0;
}

static scrub_rc scrub_jpeg(const unsigned char *b, size_t n, sbuf *o, scrub_info *in, char *err, size_t el) {
    in->format = "JPEG";
    int orient = jpeg_orientation(b, n);
    sb_putn(o, "\xFF\xD8", 2);

    size_t i = 2;
    for (;;) {
        if (i + 2 > n) {
            // ran off the end without an EOI, close it off so viewers are happy
            put_jpeg_orientation(o, in, &orient);
            sb_putn(o, "\xFF\xD9", 2);
            break;
        }
        if (b[i] != 0xFF) return broken(err, el, "jpeg lost sync at byte %zu", i);
        unsigned char m = b[i + 1];
        if (m == 0xFF) { // fill byte
            i++;
            continue;
        }
        if (m == 0xD9) {
            put_jpeg_orientation(o, in, &orient);
            sb_putn(o, "\xFF\xD9", 2);
            if (i + 2 < n) note(in, "trailing data"); // iphone hdr gain maps, hidden zips, whatever
            break;
        }
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { // no length on these
            sb_putn(o, (const char *)b + i, 2);
            i += 2;
            continue;
        }
        if (i + 4 > n) return broken(err, el, "jpeg cut off at byte %zu", i);
        size_t len = rd16be(b + i + 2);
        if (len < 2 || i + 2 + len > n) return broken(err, el, "jpeg segment at byte %zu runs past the end", i);
        const unsigned char *seg = b + i + 4;
        size_t sn = len - 2;

        const char *junk = jpeg_junk(m, seg, sn);
        if (junk) {
            note(in, junk);
            i += 2 + len;
            continue;
        }
        // tuck the rotation in right after the jfif header (or right after SOI if there isn't one)
        if (m != 0xE0) put_jpeg_orientation(o, in, &orient);
        sb_putn(o, (const char *)b + i, 2 + len);
        i += 2 + len;

        if (m == 0xDA) {
            // start of scan: compressed data until the next real marker. FF00 is an escaped
            // FF and FFD0-FFD7 are restart markers, both are still part of the image data
            size_t j = i;
            while (j + 1 < n) {
                if (b[j] != 0xFF) {
                    j++;
                    continue;
                }
                unsigned char nb = b[j + 1];
                if (nb == 0x00 || (nb >= 0xD0 && nb <= 0xD7)) j += 2;
                else break;
            }
            if (j + 1 >= n) j = n; // no end marker at all
            sb_putn(o, (const char *)b + i, j - i);
            i = j;
        }
    }
    return SCRUB_OK;
}

// ---------- png ----------

// png chunks we keep: the image + anything that changes how it looks. everything else
// (tEXt, zTXt, iTXt, eXIf, tIME, random private chunks) goes
static bool png_keep(const char *t) {
    static const char *const keep[] = {"IHDR", "PLTE", "IDAT", "IEND", "tRNS", "gAMA", "cHRM", "sRGB",
                                       "iCCP", "sBIT", "bKGD", "pHYs", "cICP", "mDCV", "cLLI",
                                       "acTL", "fcTL", "fdAT", NULL}; // last 3 = animated png
    for (const char *const *k = keep; *k; k++)
        if (!memcmp(t, *k, 4)) return true;
    return false;
}

static void put_be32(sbuf *o, uint32_t v) {
    char c[4] = {(char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v};
    sb_putn(o, c, 4);
}

static int png_orientation(const unsigned char *b, size_t n) {
    for (size_t i = 8; i + 12 <= n;) {
        size_t len = rd32be(b + i);
        if (len > n || i + 12 + len > n) break;
        if (!memcmp(b + i + 4, "eXIf", 4)) {
            const unsigned char *d = b + i + 8;
            return STARTS(d, len, "Exif\0\0") ? tiff_orientation(d + 6, len - 6) : tiff_orientation(d, len);
        }
        i += 12 + len;
    }
    return 0;
}

static scrub_rc scrub_png(const unsigned char *b, size_t n, sbuf *o, scrub_info *in, char *err, size_t el) {
    in->format = "PNG";
    int orient = png_orientation(b, n);
    sb_putn(o, (const char *)b, 8);
    size_t i = 8;
    bool done = false;
    while (i + 12 <= n) {
        size_t len = rd32be(b + i);
        if (len > n || i + 12 + len > n) return broken(err, el, "png chunk at byte %zu runs past the end", i);
        const char *t = (const char *)b + i + 4;
        if (png_keep(t)) {
            sb_putn(o, (const char *)b + i, 12 + len);
            // browsers do rotate pngs by eXIf, so put back a tiny one right after the header
            if (!memcmp(t, "IHDR", 4) && orient > 1) {
                sbuf c;
                sb_init(&c);
                sb_putn(&c, "eXIf", 4);
                put_orientation_tiff(&c, orient);
                put_be32(o, (uint32_t)c.len - 4);
                sb_putn(o, c.s, c.len);
                put_be32(o, (uint32_t)crc32(0, (const Bytef *)c.s, (uInt)c.len));
                sb_free(&c);
                in->orientation = orient;
            }
        } else if (!memcmp(t, "eXIf", 4)) {
            note(in, "exif");
        } else if (!memcmp(t, "tIME", 4)) {
            note(in, "timestamp");
        } else if (!memcmp(t, "iTXt", 4) && STARTS(b + i + 8, len, "XML:com.adobe.xmp")) {
            note(in, "xmp");
        } else if (!memcmp(t, "tEXt", 4) || !memcmp(t, "zTXt", 4) || !memcmp(t, "iTXt", 4)) {
            note(in, "text");
        } else {
            note(in, "other chunks");
        }
        i += 12 + len;
        if (!memcmp(t, "IEND", 4)) {
            done = true;
            break;
        }
    }
    if (!done) return broken(err, el, "png has no end chunk");
    if (i < n) note(in, "trailing data");
    return SCRUB_OK;
}

// ---------- webp ----------

static scrub_rc scrub_webp(const unsigned char *b, size_t n, sbuf *o, scrub_info *in, char *err, size_t el) {
    in->format = "WebP";
    static const char *const keep[] = {"VP8 ", "VP8L", "VP8X", "ALPH", "ANIM", "ANMF", "ICCP", NULL};
    sb_putn(o, (const char *)b, 12); // RIFF, size (fixed below), WEBP
    size_t vp8x_at = 0;
    for (size_t i = 12; i + 8 <= n;) {
        size_t len = rd32le(b + i + 4);
        size_t total = 8 + len + (len & 1); // padded to even
        if (len > n || i + 8 + len > n) return broken(err, el, "webp chunk at byte %zu runs past the end", i);
        if (total > n - i) total = n - i; // missing pad byte on the last chunk, it happens
        const char *t = (const char *)b + i;
        bool kept = false;
        for (const char *const *k = keep; *k; k++)
            if (!memcmp(t, *k, 4)) kept = true;
        if (kept) {
            if (!memcmp(t, "VP8X", 4)) vp8x_at = o->len;
            sb_putn(o, t, total);
        } else if (!memcmp(t, "EXIF", 4)) {
            note(in, "exif");
        } else if (!memcmp(t, "XMP ", 4)) {
            note(in, "xmp");
        } else {
            note(in, "other chunks");
        }
        i += total;
    }
    unsigned char *p = (unsigned char *)o->s;
    // vp8x flags say which chunks exist, clear exif (0x08) + xmp (0x04) so nobody goes looking
    if (vp8x_at && o->len > vp8x_at + 8) p[vp8x_at + 8] &= (unsigned char)~0x0C;
    uint32_t riff = (uint32_t)(o->len - 8);
    p[4] = (unsigned char)riff, p[5] = (unsigned char)(riff >> 8), p[6] = (unsigned char)(riff >> 16),
    p[7] = (unsigned char)(riff >> 24);
    return SCRUB_OK;
}

// ---------- heic / avif ----------

static bool heif_brand(const unsigned char *b, size_t n) {
    static const char *const brands[] = {"heic", "heix", "hevc", "heim", "heis", "mif1", "msf1", "avif", "avis", NULL};
    if (n < 12 || memcmp(b + 4, "ftyp", 4)) return false;
    for (const char *const *br = brands; *br; br++)
        if (!memcmp(b + 8, *br, 4)) return true;
    return false;
}

static scrub_rc scrub_heif(const unsigned char *b, size_t n, sbuf *o, scrub_info *in, char *err, size_t el) {
    in->format = memcmp(b + 8, "avi", 3) ? "HEIC" : "AVIF";
    // heic keeps metadata as items that other boxes point at by file offset, so we can't
    // cut anything out without redoing every offset. blanking them in place is way safer:
    // same size, nothing moves, the image data doesn't change at all
    sb_putn(o, (const char *)b, n);
    int nexif, nxmp;
    int bad = heif_wipe_meta((unsigned char *)o->s, o->len, &nexif, &nxmp);
    if (bad < 0) return broken(err, el, "%s has no readable meta box", in->format);
    if (bad > 0) return broken(err, el, "couldn't reach %d metadata item%s, not trusting this one", bad, bad == 1 ? "" : "s");
    for (int k = 0; k < nexif; k++) note(in, "exif");
    for (int k = 0; k < nxmp; k++) note(in, "xmp");
    return SCRUB_OK;
}

scrub_rc scrub_bytes(const unsigned char *b, size_t n, unsigned char **out, size_t *out_len, scrub_info *info,
                     char *err, size_t errlen) {
    memset(info, 0, sizeof *info);
    *out = NULL;
    *out_len = 0;
    if (errlen) err[0] = 0;

    sbuf o;
    sb_init(&o);
    scrub_rc rc;
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) rc = scrub_jpeg(b, n, &o, info, err, errlen);
    else if (STARTS(b, n, "\x89PNG\r\n\x1a\n")) rc = scrub_png(b, n, &o, info, err, errlen);
    else if (n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WEBP", 4)) rc = scrub_webp(b, n, &o, info, err, errlen);
    else if (heif_brand(b, n)) rc = scrub_heif(b, n, &o, info, err, errlen);
    else rc = SCRUB_UNSUPPORTED;

    if (rc != SCRUB_OK) {
        sb_free(&o);
        return rc;
    }
    *out_len = o.len;
    *out = (unsigned char *)sb_take(&o);
    return SCRUB_OK;
}

// unit tests. run with `make test` (builds with address sanitizer)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "analysis/findings.h"
#include "core/extract.h"
#include "core/json.h"
#include "core/regex.h"
#include "modules/shell/shell.h"
#include "core/platform.h"
#include "core/sha256.h"
#include "core/util.h"
#include "modules/user/sites.h"

static int fails, checks;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        checks++;                                                     \
        if (!(cond)) {                                                \
            fails++;                                                  \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);             \
            printf(__VA_ARGS__);                                      \
            putchar('\n');                                            \
        }                                                             \
    } while (0)

static result_t *run(const unsigned char *b, size_t n) {
    result_t *r = result_new("test");
    extract_bytes(r, b, n);
    analyze(r);
    return r;
}

static const char *field(const result_t *r, const char *key) {
    for (size_t i = 0; i < r->nfields; i++)
        if (!strcmp(r->fields[i].key, key)) return r->fields[i].value;
    return "";
}

static bool finding(const result_t *r, const char *kind, const char *needle) {
    for (size_t i = 0; i < r->nfindings; i++)
        if (!strcmp(r->findings[i].kind, kind) && strstr(r->findings[i].detail, needle)) return true;
    return false;
}

static void dump_findings(const result_t *r) {
    for (size_t i = 0; i < r->nfindings; i++) printf("    %s | %s\n", r->findings[i].kind, r->findings[i].detail);
}

// --- tiny tiff builder so we can fake exif without fixture files ---

typedef struct {
    uint16_t tag, typ;
    uint32_t count;
    const unsigned char *data;
    size_t len;
} tent;

static void put16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}

// lays out ifds back to back. exif/gps pointer entries hold the index of the ifd they point at in data[0]
static unsigned char *build_tiff(tent **ifds, const int *counts, int nifd, size_t *out_len) {
    uint32_t offs[8], pos = 8;
    for (int i = 0; i < nifd; i++) {
        offs[i] = pos;
        pos += 2 + (uint32_t)counts[i] * 12 + 4;
        for (int j = 0; j < counts[i]; j++)
            if (ifds[i][j].len > 4) pos += (uint32_t)ifds[i][j].len;
    }
    unsigned char *b = calloc(pos, 1);
    memcpy(b, "II*\0", 4);
    put32(b + 4, 8);
    for (int i = 0; i < nifd; i++) {
        uint32_t p = offs[i], data = p + 2 + (uint32_t)counts[i] * 12 + 4;
        put16(b + p, (uint16_t)counts[i]);
        for (int j = 0; j < counts[i]; j++) {
            tent *e = &ifds[i][j];
            unsigned char *ep = b + p + 2 + j * 12;
            put16(ep, e->tag), put16(ep + 2, e->typ), put32(ep + 4, e->count);
            if (e->tag == 0x8769 || e->tag == 0x8825) put32(ep + 8, offs[e->data[0]]);
            else if (e->len <= 4) memcpy(ep + 8, e->data, e->len);
            else put32(ep + 8, data), memcpy(b + data, e->data, e->len), data += (uint32_t)e->len;
        }
    }
    *out_len = pos;
    return b;
}

#define ASCII(t, s) {t, 2, sizeof(s), (const unsigned char *)s, sizeof(s)}

// tiny exif: Canon, John Doe, serial SN-0042, gps 33°51'36"S 151°12'0"E (sydney)
static unsigned char *sample_tiff(size_t *len) {
    static const unsigned char idx1[] = {1}, idx2[] = {2};
    unsigned char lat[24], lon[24];
    uint32_t lv[6] = {33, 1, 51, 1, 3600, 100}, ov[6] = {151, 1, 12, 1, 0, 1};
    for (int i = 0; i < 6; i++) put32(lat + i * 4, lv[i]), put32(lon + i * 4, ov[i]);

    tent ifd0[] = {ASCII(0x010F, "Canon"), ASCII(0x013B, "John Doe"), {0x8769, 4, 1, idx1, 4}, {0x8825, 4, 1, idx2, 4}};
    tent exif[] = {ASCII(0xA431, "SN-0042")};
    tent gps[] = {ASCII(1, "S"), {2, 5, 3, lat, 24}, ASCII(3, "E"), {4, 5, 3, lon, 24}};
    tent *ifds[] = {ifd0, exif, gps};
    int counts[] = {4, 1, 4};
    return build_tiff(ifds, counts, 3, len);
}

static unsigned char *sample_jpeg(size_t *len) {
    size_t tl;
    unsigned char *t = sample_tiff(&tl);
    size_t n = 4 + 2 + 6 + tl + 2;
    unsigned char *b = malloc(n);
    memcpy(b, "\xFF\xD8\xFF\xE1", 4);
    b[4] = (unsigned char)((tl + 8) >> 8), b[5] = (unsigned char)(tl + 8);
    memcpy(b + 6, "Exif\0\0", 6);
    memcpy(b + 12, t, tl);
    memcpy(b + 12 + tl, "\xFF\xD9", 2);
    free(t);
    *len = n;
    return b;
}

static void test_jpeg(void) {
    puts("jpeg exif + gps");
    size_t n;
    unsigned char *b = sample_jpeg(&n);
    result_t *r = run(b, n);
    CHECK(!strcmp(r->type, "JPEG image"), "type = %s", r->type);
    CHECK(!strcmp(field(r, "Make"), "Canon"), "Make = %s", field(r, "Make"));
    CHECK(!strcmp(field(r, "BodySerialNumber"), "SN-0042"), "serial = %s", field(r, "BodySerialNumber"));
    CHECK(r->ngps == 1, "ngps = %zu", r->ngps);
    if (r->ngps)
        CHECK(fabs(r->gps[0].lat + 33.86) < 1e-6 && fabs(r->gps[0].lon - 151.2) < 1e-6, "gps = %f,%f", r->gps[0].lat,
              r->gps[0].lon);
    CHECK(finding(r, "location", "-33.860000"), "no location finding");
    CHECK(finding(r, "identity", "John Doe"), "no identity finding");
    CHECK(finding(r, "device id", "SN-0042"), "no serial finding");
    result_free(r), free(b);
}

static void test_pdf(void) {
    puts("pdf info + edit history");
    const char *pdf = "%PDF-1.4\n1 0 obj << /Author (Alice \\(draft\\)) /Producer (Word) "
                      "/CreationDate (D:20230105143000+01'00') >> endobj\n%%EOF\n"
                      "2 0 obj << /Author <FEFF0042006F0062> /Producer (Word) >> endobj\n%%EOF\n";
    result_t *r = run((const unsigned char *)pdf, strlen(pdf));
    CHECK(!strcmp(field(r, "CreationDate"), "2023-01-05 14:30:00 +01:00"), "date = %s", field(r, "CreationDate"));
    CHECK(!strcmp(field(r, "IncrementalUpdates"), "1"), "updates = %s", field(r, "IncrementalUpdates"));
    CHECK(finding(r, "edit history", "Alice (draft) → Bob"), "no edit history finding");
    if (fails) dump_findings(r);
    result_free(r);
}

// --- builds a minimal heic: ftyp, mdat, meta{hdlr, iinf, iloc, idat} ---

static void be16(sbuf *b, uint32_t v) {
    char c[2] = {(char)(v >> 8), (char)v};
    sb_putn(b, c, 2);
}
static void be32(sbuf *b, uint32_t v) {
    char c[4] = {(char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v};
    sb_putn(b, c, 4);
}
static size_t box_open(sbuf *b, const char *type) {
    size_t at = b->len;
    be32(b, 0);
    sb_putn(b, type, 4);
    return at;
}
static void box_close(sbuf *b, size_t at) {
    uint32_t n = (uint32_t)(b->len - at);
    unsigned char *p = (unsigned char *)b->s + at;
    p[0] = (unsigned char)(n >> 24), p[1] = (unsigned char)(n >> 16), p[2] = (unsigned char)(n >> 8), p[3] = (unsigned char)n;
}

// `tiles` dummy image items come before the exif item, like a tiled 48mp iphone shot.
// in_idat puts the exif inside the meta box (iloc construction method 1) instead of mdat
static unsigned char *sample_heic(int tiles, bool in_idat, size_t *len) {
    size_t tl;
    unsigned char *tiff = sample_tiff(&tl);
    sbuf exif;
    sb_init(&exif);
    be32(&exif, 6); // offset to the tiff header, apple style
    sb_putn(&exif, "Exif\0\0", 6);
    sb_putn(&exif, (char *)tiff, tl);
    free(tiff);

    sbuf b;
    sb_init(&b);
    size_t ftyp = box_open(&b, "ftyp");
    sb_putn(&b, "heic", 4), be32(&b, 0), sb_putn(&b, "mif1heic", 8);
    box_close(&b, ftyp);

    size_t mdat = box_open(&b, "mdat");
    uint32_t tile_at = (uint32_t)b.len;
    sb_putc(&b, 'T');
    uint32_t exif_at = (uint32_t)b.len;
    if (!in_idat) sb_putn(&b, exif.s, exif.len);
    box_close(&b, mdat);

    size_t meta = box_open(&b, "meta");
    be32(&b, 0);
    size_t hdlr = box_open(&b, "hdlr");
    be32(&b, 0), be32(&b, 0), sb_putn(&b, "pict", 4), be32(&b, 0), be32(&b, 0), be32(&b, 0), sb_putc(&b, 0);
    box_close(&b, hdlr);

    size_t iinf = box_open(&b, "iinf");
    be32(&b, 0), be16(&b, (uint32_t)tiles + 1);
    for (int i = 0; i <= tiles; i++) {
        size_t infe = box_open(&b, "infe");
        be32(&b, 2u << 24); // version 2
        be16(&b, (uint32_t)i + 1), be16(&b, 0);
        sb_putn(&b, i < tiles ? "hvc1" : "Exif", 4);
        sb_putc(&b, 0);
        box_close(&b, infe);
    }
    box_close(&b, iinf);

    size_t iloc = box_open(&b, "iloc");
    be32(&b, 1u << 24); // version 1
    sb_putc(&b, 0x44), sb_putc(&b, 0x00); // 4 byte offsets + lengths, no base offset
    be16(&b, (uint32_t)tiles + 1);
    for (int i = 0; i <= tiles; i++) {
        bool is_exif = i == tiles;
        be16(&b, (uint32_t)i + 1);
        be16(&b, is_exif && in_idat ? 1 : 0); // construction method
        be16(&b, 0), be16(&b, 1);              // data ref, 1 extent
        be32(&b, is_exif ? (in_idat ? 0 : exif_at) : tile_at);
        be32(&b, is_exif ? (uint32_t)exif.len : 1);
    }
    box_close(&b, iloc);
    if (in_idat) {
        size_t idat = box_open(&b, "idat");
        sb_putn(&b, exif.s, exif.len);
        box_close(&b, idat);
    }
    box_close(&b, meta);
    sb_free(&exif);
    *len = b.len;
    return (unsigned char *)sb_take(&b);
}

static void test_heic(void) {
    puts("heic (iphone photos)");
    for (int v = 0; v < 2; v++) {
        bool in_idat = v == 1;
        size_t n;
        unsigned char *b = sample_heic(100, in_idat, &n);
        result_t *r = run(b, n);
        CHECK(!strcmp(r->type, "HEIC image"), "type = %s", r->type);
        CHECK(!strcmp(field(r, "Make"), "Canon"), "%s: Make = '%s'", in_idat ? "idat" : "mdat", field(r, "Make"));
        CHECK(r->ngps == 1 && fabs(r->gps[0].lat + 33.86) < 1e-6, "%s: gps missing", in_idat ? "idat" : "mdat");
        CHECK(finding(r, "device id", "SN-0042"), "%s: serial missing", in_idat ? "idat" : "mdat");
        result_free(r);

        // every possible truncation, in an exact-size buffer so asan sees overreads
        for (size_t cut = 0; cut < n; cut++) {
            unsigned char *c = malloc(cut ? cut : 1);
            memcpy(c, b, cut);
            result_free(run(c, cut));
            free(c);
        }
        free(b);
    }
}

// --- builds a zip with stored (uncompressed) entries ---

typedef struct {
    const char *name;
    const unsigned char *data;
    size_t len;
} zfile;

static unsigned char *build_zip(const zfile *files, int n, size_t *out_len) {
    sbuf z, cd;
    sb_init(&z), sb_init(&cd);
    unsigned char h[46];
    for (int i = 0; i < n; i++) {
        uint32_t off = (uint32_t)z.len, crc = (uint32_t)crc32(0, files[i].data, (uInt)files[i].len);
        uint16_t nl = (uint16_t)strlen(files[i].name);
        memset(h, 0, sizeof h);
        memcpy(h, "PK\3\4", 4);
        put32(h + 14, crc), put32(h + 18, (uint32_t)files[i].len), put32(h + 22, (uint32_t)files[i].len);
        put16(h + 26, nl);
        sb_putn(&z, (char *)h, 30), sb_putn(&z, files[i].name, nl), sb_putn(&z, (const char *)files[i].data, files[i].len);

        memset(h, 0, sizeof h);
        memcpy(h, "PK\1\2", 4);
        put32(h + 16, crc), put32(h + 20, (uint32_t)files[i].len), put32(h + 24, (uint32_t)files[i].len);
        put16(h + 28, nl), put32(h + 42, off);
        sb_putn(&cd, (char *)h, 46), sb_putn(&cd, files[i].name, nl);
    }
    uint32_t cd_off = (uint32_t)z.len;
    sb_putn(&z, cd.s, cd.len);
    unsigned char e[22] = {0};
    memcpy(e, "PK\5\6", 4);
    put16(e + 8, (uint16_t)n), put16(e + 10, (uint16_t)n), put32(e + 12, (uint32_t)cd.len), put32(e + 16, cd_off);
    sb_putn(&z, (char *)e, 22);
    sb_free(&cd);
    *out_len = z.len;
    return (unsigned char *)sb_take(&z);
}

#define ZTEXT(name, s) {name, (const unsigned char *)s, sizeof(s) - 1}

static void test_docx(void) {
    puts("docx leaks + embedded photo");
    size_t jl;
    unsigned char *jpg = sample_jpeg(&jl);
    zfile files[] = {
        ZTEXT("[Content_Types].xml", "<Types/>"),
        ZTEXT("docProps/core.xml", "<?xml version=\"1.0\"?><cp:coreProperties xmlns:cp=\"c\" xmlns:dc=\"d\">"
                                   "<dc:creator>Jane &amp; Co</dc:creator><cp:lastModifiedBy>bob@corp.example"
                                   "</cp:lastModifiedBy></cp:coreProperties>"),
        ZTEXT("docProps/custom.xml", "<Properties><property fmtid=\"x\" pid=\"2\" name=\"Client\">"
                                     "<vt:lpwstr>ACME</vt:lpwstr></property></Properties>"),
        ZTEXT("word/document.xml", "<w:document/>"),
        ZTEXT("word/_rels/settings.xml.rels",
              "<Relationships><Relationship Id=\"r1\" Target=\"file:///\\\\fileserv01\\share\\tpl.dotx\" "
              "TargetMode=\"External\"/><Relationship Id=\"r2\" Target=\"https://example.com\" "
              "TargetMode=\"External\"/></Relationships>"),
        {"word/media/image1.jpeg", jpg, jl},
    };
    size_t zl;
    unsigned char *z = build_zip(files, 6, &zl);
    result_t *r = run(z, zl);
    CHECK(!strcmp(r->type, "Word document (docx)"), "type = %s", r->type);
    CHECK(finding(r, "identity", "Jane & Co"), "no creator finding");
    CHECK(finding(r, "email", "bob@corp.example"), "no email finding");
    CHECK(finding(r, "network share", "fileserv01"), "no network share finding");
    CHECK(finding(r, "location", "151.2"), "embedded photo gps missing");
    CHECK(!strcmp(field(r, "Client"), "ACME"), "custom prop = %s", field(r, "Client"));
    CHECK(!strstr(field(r, "ExternalLink"), "example.com"), "web links shouldn't count as file links");
    if (fails) dump_findings(r);
    result_free(r), free(z), free(jpg);
}

static void test_xmp(void) {
    puts("xmp in a random blob");
    const char *x = "junk junk <x:xmpmeta><rdf:RDF><rdf:Description xmp:CreatorTool=\"Lightroom\" "
                    "exif:GPSLatitude=\"40,26.7720N\" exif:GPSLongitude=\"79,58.932W\">"
                    "<dc:creator><rdf:Seq><rdf:li>Ann</rdf:li><rdf:li>Ben</rdf:li></rdf:Seq></dc:creator>"
                    "<photoshop:City>Pittsburgh</photoshop:City></rdf:Description></rdf:RDF></x:xmpmeta>";
    result_t *r = run((const unsigned char *)x, strlen(x));
    CHECK(!strcmp(field(r, "dc:creator"), "Ann, Ben"), "creator = %s", field(r, "dc:creator"));
    CHECK(!strcmp(field(r, "xmp:CreatorTool"), "Lightroom"), "tool = %s", field(r, "xmp:CreatorTool"));
    CHECK(r->ngps == 1 && fabs(r->gps[0].lat - 40.4462) < 1e-4 && fabs(r->gps[0].lon + 79.9822) < 1e-4, "bad gps");
    CHECK(finding(r, "place", "Pittsburgh"), "no place finding");
    CHECK(finding(r, "identity", "Ann, Ben"), "no identity finding");
    result_free(r);
}

static void test_leaks(void) {
    puts("leak detectors");
    result_t *r = result_new("x");
    result_add(r, "T", "A", "saved to /home/alice/secret.txt");
    result_add(r, "T", "B", "https://site.com/home/index.html");
    result_add(r, "T", "C", "D:\\Documents and Settings\\bob\\x.doc");
    result_add(r, "T", "D", "E:\\Projects\\thing.psd");
    analyze(r);
    CHECK(finding(r, "path leak", "\"alice\""), "missed /home/alice");
    CHECK(!finding(r, "path leak", "index"), "urls shouldn't count as home dirs");
    CHECK(finding(r, "path leak", "\"bob\""), "missed documents and settings");
    CHECK(finding(r, "local path", "E:\\Projects"), "missed plain windows path");
    if (fails) dump_findings(r);
    result_free(r);
}

static void test_sha256(void) {
    puts("sha256");
    char out[65];
    sha256_hex((const unsigned char *)"abc", 3, out);
    CHECK(!strcmp(out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "abc = %s", out);
    sha256_hex((const unsigned char *)"", 0, out);
    CHECK(!strcmp(out, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "empty = %s", out);
    // 56 bytes, the padding edge case
    const char *s = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256_hex((const unsigned char *)s, strlen(s), out);
    CHECK(!strcmp(out, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "56b = %s", out);
}

// string literal + its real length, no hand counting
#define LIT(s) {(const unsigned char *)s, sizeof(s) - 1}

static void test_garbage(void) {
    puts("garbage doesn't crash");
    size_t n;
    unsigned char *jpg = sample_jpeg(&n);
    struct {
        const unsigned char *b;
        size_t n;
    } in[] = {
        {jpg, 30},
        LIT("II*\0\xff\xff\xff\xff"),
        LIT("\x89PNG\r\n\x1a\n\0\0\0\x63tEXt"),
        LIT("%PDF-1.7 /Author (unterminated"),
        LIT("PK\x03\x04 not really a zip"),
        LIT("RIFF\0\0\0\0WEBPEXIF\xff\xff\xff\x7f"),
        LIT("<x:xmpmeta><a:b>"),
    };
    for (size_t i = 0; i < sizeof in / sizeof *in; i++) {
        // copy to an exact sized buffer so the sanitizer catches any overread
        unsigned char *c = malloc(in[i].n);
        memcpy(c, in[i].b, in[i].n);
        result_free(run(c, in[i].n));
        free(c);
    }
    free(jpg);
    checks++;
}

static void test_site_url(void) {
    puts("username urls");
    char *u = site_url("https://x.com/{}", "a b/../c?d=1&e");
    CHECK(u && !strcmp(u, "https://x.com/a%20b%2F..%2Fc%3Fd%3D1%26e"), "encoded = %s", u);
    free(u);
    u = site_url("https://{}.tumblr.com", "cool-name");
    CHECK(u && !strcmp(u, "https://cool-name.tumblr.com"), "subdomain = %s", u);
    free(u);
    // dots / underscores can't go in a hostname, so that site is a no
    CHECK(site_url("https://{}.tumblr.com", "a.b") == NULL, "dot in subdomain should be rejected");
    CHECK(site_url("https://{}.tumblr.com", "a_b") == NULL, "underscore in subdomain should be rejected");
    u = site_url("https://x.com/u/{}?q={}", "bob");
    CHECK(u && !strcmp(u, "https://x.com/u/bob?q=bob"), "two placeholders = %s", u);
    free(u);
}

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

static void test_sites_file(void) {
    puts("sites file parsing");
    // temp file in whatever temp dir this os uses
    const char *dir = getenv("TMPDIR");
    if (!dir) dir = getenv("TEMP");
    if (!dir) dir = ".";
    char *path = xprintf("%s/odin_sites_%u.txt", dir, platform_pid());
    const char *good = "# comment\n\nA | https://a.com/{} | status\n"
                       "B | https://b.com/api/{} | absent:not found | https://b.com/{} | bob\n";
    write_text(path, good);
    site_list l = {0};
    char err[256] = "";
    CHECK(sites_load_file(&l, path, err, sizeof err), "good file failed: %s", err);
    CHECK(l.count == 2, "count = %zu", l.count);
    if (l.count == 2) {
        CHECK(l.items[1].kind == CHECK_MESSAGE && l.items[1].absent.n == 1 && !strcmp(l.items[1].absent.v[0], "not found"),
              "absent rule");
        CHECK(!strcmp(l.items[1].profile, "https://b.com/{}") && !strcmp(l.items[1].claimed, "bob"), "profile/claimed");
        CHECK(!strcmp(l.items[0].profile, l.items[0].probe) && !l.items[0].claimed, "defaults");
    }
    sites_free(&l);

    const char *bad[] = {"A | https://a.com/nope | status\n", "A | ftp://a.com/{} | status\n",
                         "A | https://a.com/{} | sometimes\n", "A | https://a.com/{}\n", "# only comments\n"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        write_text(path, bad[i]);
        site_list b = {0};
        CHECK(!sites_load_file(&b, path, err, sizeof err), "bad file %zu was accepted", i);
        sites_free(&b);
    }
    remove(path);
    free(path);

    site_list d = {0};
    sites_load_default(&d);
    CHECK(d.count >= 200, "built in list only has %zu sites", d.count);
    // every built in site has to be usable as-is
    for (size_t i = 0; i < d.count; i++) {
        const site_t *x = &d.items[i];
        CHECK(strstr(x->probe, "{}") || (x->post && strstr(x->post, "{}")), "%s has nowhere to put the username", x->name);
        CHECK(x->claimed != NULL, "%s has no known account for --selftest", x->name);
        if (x->regex) {
            bool unsupported;
            rx_search(x->regex, "test", &unsupported);
            CHECK(!unsupported, "%s's username rule can't be read: %s", x->name, x->regex);
        }
    }
    sites_free(&d);
}

static void test_regex(void) {
    puts("username rules (regex)");
    struct {
        const char *pat, *text;
        bool want;
    } v[] = {
        {"^[a-zA-Z0-9](?:[a-zA-Z0-9]|-(?=[a-zA-Z0-9])){0,38}$", "good-name", true}, // github's rule
        {"^[a-zA-Z0-9](?:[a-zA-Z0-9]|-(?=[a-zA-Z0-9])){0,38}$", "bad--name", false},
        {"^[a-zA-Z0-9](?:[a-zA-Z0-9]|-(?=[a-zA-Z0-9])){0,38}$", "trailing-", false},
        {"^[^\\.]+$", "no.dots", false},
        {"^[\\w]{4,30}$", "j\xc3\xb6hn", true}, // \w matches letters from any language, like python
        {"^[^\\u3040-\\u30ff\\u4e00-\\u9fff]+$", "\xe5\xb1\xb1\xe7\x94\xb0", false},
        {"^[a-zA-Z0-9_\\.]{3,49}(?<!\\.com|\\.org)$", "shop.com", false},
        {"^[a-zA-Z0-9_\\.]{3,49}(?<!\\.com|\\.org)$", "shop.co", true},
        {"^(?!id\\d)[a-zA-Z0-9_]*$", "id123", false},
        {"^(?!id\\d)[a-zA-Z0-9_]*$", "idea", true},
        {"^[\\w-]{,5}$", "abcdef", false},
        {"[A-Za-z0-9][A-Za-z0-9_-]{0,24}", "--x", true}, // no anchors = search anywhere
    };
    for (size_t i = 0; i < sizeof v / sizeof *v; i++) {
        bool u, got = rx_search(v[i].pat, v[i].text, &u);
        CHECK(got == v[i].want && !u, "%s on '%s': got %d (unsupported=%d)", v[i].pat, v[i].text, got, u);
    }
    bool u;
    CHECK(rx_search("(?P<x>a)", "a", &u) && u, "unsupported syntax should allow the name and say so");
    // a regex bomb has to finish fast, and giving up means allowing the name, never blocking it
    bool got = rx_search("^(a+)+$", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa!", &u);
    CHECK(u ? got : !got, "regex bomb: got %d unsupported %d", got, u);
}

static void test_shell_split(void) {
    puts("shell word splitting");
    char err[128], **w;
    int n = shell_split("meta -rf \"my photos\" 'it''s' a\\ b", &w, err, sizeof err);
#ifdef _WIN32
    CHECK(n == 5, "words = %d", n); // no backslash escapes on windows
#else
    CHECK(n == 5 && !strcmp(w[2], "my photos") && !strcmp(w[3], "its") && !strcmp(w[4], "a b"), "split = %d", n);
#endif
    shell_free_words(w, n);
    n = shell_split("  ", &w, err, sizeof err);
    CHECK(n == 0, "blank line = %d words", n);
    shell_free_words(w, n);
    CHECK(shell_split("meta \"oops", &w, err, sizeof err) < 0 && strstr(err, "quote"), "unclosed quote: %s", err);
    const char *home = platform_home();
    if (home) {
        n = shell_split("~/x ~ a~b", &w, err, sizeof err);
        char *want = xprintf("%s/x", home);
        CHECK(n == 3 && !strcmp(w[0], want) && !strcmp(w[1], home) && !strcmp(w[2], "a~b"), "tilde");
        free(want);
        shell_free_words(w, n);
    }
}

static void test_json(void) {
    puts("json (for odin upgrade)");
    const char *rel = "{\"tag_name\": \"v1.2.3\", \"draft\": false, \"assets\": ["
                      "{\"name\": \"SHA256SUMS\", \"url\": \"https://api/1\", \"size\": 1e3},"
                      "{\"name\": \"caf\\u00e9 \\ud83d\\ude00\", \"url\": null}]}";
    json *j = json_parse(rel, strlen(rel));
    CHECK(j && !strcmp(json_str(j, "tag_name"), "v1.2.3"), "tag_name");
    json *a = json_get(j, "assets");
    CHECK(a && a->t == J_ARR && a->len == 2, "assets array");
    if (a && a->len == 2) {
        CHECK(!strcmp(json_str(a->items[1], "name"), "caf\xc3\xa9 \xf0\x9f\x98\x80"), "unicode escapes");
        CHECK(json_get(a->items[0], "size")->n == 1000, "number");
        CHECK(json_str(a->items[1], "url") == NULL, "null isn't a string");
    }
    json_free(j);

    const char *broken[] = {"", "{", "{\"a\":}", "[1,]", "{\"a\" 1}", "\"unterminated", "{} junk", "tru"};
    for (size_t i = 0; i < sizeof broken / sizeof *broken; i++) {
        json *b = json_parse(broken[i], strlen(broken[i]));
        CHECK(b == NULL, "accepted broken json: %s", broken[i]);
        json_free(b);
    }
    // way too deep should be refused, not blow the stack
    char deep[2001];
    memset(deep, '[', 2000), deep[2000] = 0;
    CHECK(json_parse(deep, 2000) == NULL, "2000-deep nesting accepted");
}

int main(void) {
    test_sha256();
    test_jpeg();
    test_pdf();
    test_docx();
    test_xmp();
    test_leaks();
    test_garbage();
    test_heic();
    test_json();
    test_site_url();
    test_sites_file();
    test_regex();
    test_shell_split();
    printf("\n%d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}

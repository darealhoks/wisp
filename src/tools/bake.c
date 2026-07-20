/* Build-time font baker. Three input paths, one output (`bake.h`):
 *
 *   TTF/OTF  — FreeType rasterizes a curated subset (ASCII + bar/HUD/menu
 *              icons) at one or more pixel sizes into alpha8 bitmaps.
 *   PSF1/PSF2, BDF — pixel-exact bitmap fonts, read directly (no FreeType,
 *              no anti-aliasing). A bitmap font has a single native size, so
 *              every requested size aliases to one `font_bm`.
 *   --ft-stub  — emits mutable per-size `Font` skeletons + the default and
 *              fallback font paths for the dlopen'd-freetype runtime backend
 *              (font_ft.c). No glyphs baked; rasterization happens at runtime.
 *
 * Run once at build time; freetype is never linked into the runtime daemon
 * (the freetype *backend* dlopen()s it instead).
 *
 * usage: bake <font.{ttf,otf,psf,psfu,bdf}> <out.h> <px-size>...
 *        bake --ft-stub <out.h> <default-font-path> <fallback-path|''> <px-size>...
 *
 * Each TTF size N is emitted as `static const Font font_N`. Sizes 14 and 22
 * always get `font_small` / `font_large` aliases for hand-written runtime
 * modules that pre-date the DSL; the DSL codegen refers to fonts by size. */

#define _GNU_SOURCE
#include <ft2build.h>
#include FT_FREETYPE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Codepoints. Plain ASCII 32..126 always baked at every size for status text
 * and menu items. Icon set follows. */
/* Codepoints transcribed directly from dwlb-status.c UTF-8 byte literals.
 * The 0xF000-range icons are 3-byte UTF-8; the 0xF0xxx-range are 4-byte —
 * an earlier pass conflated the two and baked unmapped private-use slots. */
static const uint32_t ICONS[] = {
    /* Bar status icons — codepoints match dwlb-status.c exactly */
    0xf02ca,  /* I_DISK    nf-md-harddisk    "\xf3\xb0\x8b\x8a" */
    0xf4bc,   /* I_CPU     nf-fa-microchip   "\xef\x92\xbc"     */
    0xf035b,  /* I_MEM     nf-md-memory      "\xf3\xb0\x8d\x9b" */
    0xf0238,  /* I_TEMP    nf-md-fire        "\xf3\xb0\x88\xb8" */
    0xf240,   /* I_BAT_FULL   "\xef\x89\x80" */
    0xf241,   /* I_BAT_75     "\xef\x89\x81" */
    0xf242,   /* I_BAT_50     "\xef\x89\x82" */
    0xf243,   /* I_BAT_25     "\xef\x89\x83" */
    0xf244,   /* I_BAT_EMPTY  "\xef\x89\x84" */
    0xf0084,  /* I_BAT_CHG    "\xf3\xb0\x82\x84" */
    0xf057e,  /* I_VOL_HI     "\xf3\xb0\x95\xbe" */
    0xf057f,  /* I_VOL_LO     "\xf3\xb0\x95\xbf" */
    0xf075f,  /* I_VOL_OFF    "\xf3\xb0\x9d\x9f" */
    0xf092b,  /* I_WIFI_OFF   "\xf3\xb0\xa4\xab" */
    0xf091f,  /* WIFI[0]      "\xf3\xb0\xa4\x9f" */
    0xf0922,  /* WIFI[1]      "\xf3\xb0\xa4\xa2" */
    0xf0925,  /* WIFI[2]      "\xf3\xb0\xa4\xa5" */
    0xf0928,  /* WIFI[3]      "\xf3\xb0\xa4\xa8" */
    /* VPN geometric shapes — base Unicode (≠ Nerd Font), so ^fg() colors apply */
    0x25cf,   /* ● VPN ON     */
    0x25b2,   /* ▲ VPN STALE  */
    0x25cb,   /* ○ VPN OFF    */
    /* Left logo */
    0xf32e,   /* nf-linux-void */
    /* HUD button glyphs (from WS_HUD_BUTTONS) */
    0xf186,   /* moon          */
    0xf1f6,   /* bell-slash    */
    0xf023,   /* lock          */
    0xf132,   /* shield        */
    0xf293,   /* bluetooth     */
    0xf1eb,   /* wifi (signal) */
    0xf028,   /* volume up     */
    /* OSD / notification slider glyphs (volume / mic / brightness) */
    0xf026,   /* volume off    */
    0xf130,   /* microphone    */
    0xf131,   /* microphone-slash */
    0xf185,   /* sun (brightness) */
    0xf0eb,   /* lightbulb (notification fallback icon) */
    /* Powermenu (built-in `wispctl powermenu`) */
    0xf011,   /* power-off */
    0xf021,   /* refresh / reboot */
    0xf28d,   /* pause / hibernate */
    0xf08b,   /* sign-out / logout */
};
#define N_ICONS (int)(sizeof ICONS / sizeof *ICONS)

/* Staging glyph (host-side). Field order matches the runtime Glyph emitted
 * into bake.h / declared in src/font.h. */
typedef struct {
    uint32_t cp;
    int      w, h;
    int      bx, by;   /* bearing: glyph top-left offset from pen+baseline */
    int      adv;      /* horizontal advance from pen */
    int      px_off;   /* offset into per-size pixel pool */
} Glyph;

typedef struct {
    int      px_size;
    int      line_h;       /* nominal line height */
    int      baseline;     /* baseline offset from line top */
    Glyph   *glyphs;
    int      n_glyphs;
    uint8_t *pixels;
    int      px_len;
    int      px_cap;
} Bake;

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) { fprintf(stderr, "oom\n"); exit(1); }
    return q;
}

static void push_pixels(Bake *b, const uint8_t *src, int n) {
    if (b->px_len + n > b->px_cap) {
        b->px_cap = (b->px_len + n) * 2 + 1024;
        b->pixels = xrealloc(b->pixels, b->px_cap);
    }
    memcpy(b->pixels + b->px_len, src, n);
    b->px_len += n;
}

/* Append one staged glyph (alpha8 rows already laid out tight, w bytes/row). */
static void add_glyph(Bake *b, uint32_t cp, int w, int h,
                      int bx, int by, int adv, const uint8_t *alpha) {
    Glyph g = { .cp = cp, .w = w, .h = h, .bx = bx, .by = by,
                .adv = adv, .px_off = b->px_len };
    if (w && h) push_pixels(b, alpha, w * h);
    b->glyphs = xrealloc(b->glyphs, sizeof(Glyph) * (b->n_glyphs + 1));
    b->glyphs[b->n_glyphs++] = g;
}

/* Codepoint-sorted comparator for runtime binary search. */
static int cmp_glyph(const void *a, const void *b) {
    return (int)((const Glyph *)a)->cp - (int)((const Glyph *)b)->cp;
}

static int is_wanted(uint32_t cp) {
    if (cp >= 32 && cp <= 126) return 1;
    for (int i = 0; i < N_ICONS; i++) if (ICONS[i] == cp) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* TTF / OTF via FreeType                                              */
/* ------------------------------------------------------------------ */

static int rasterize(FT_Face face, uint32_t cp, Bake *b) {
    FT_UInt gi = FT_Get_Char_Index(face, cp);
    if (!gi) return -1;
    if (FT_Load_Glyph(face, gi, FT_LOAD_DEFAULT)) return -1;
    if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) return -1;

    FT_GlyphSlot s = face->glyph;
    FT_Bitmap *bm = &s->bitmap;
    int w = bm->width, h = bm->rows;

    /* Repack to tight w-byte rows (drop FT pitch padding). */
    uint8_t *tmp = NULL;
    if (w && h) {
        tmp = xrealloc(NULL, (size_t)w * h);
        for (int y = 0; y < h; y++)
            memcpy(tmp + y * w, bm->buffer + y * bm->pitch, w);
    }
    add_glyph(b, cp, w, h, s->bitmap_left, s->bitmap_top, s->advance.x >> 6, tmp);
    free(tmp);
    return 0;
}

static void bake_ttf_size(FT_Library ft, const char *font_path, int px_size, Bake *b) {
    FT_Face face;
    if (FT_New_Face(ft, font_path, 0, &face)) {
        fprintf(stderr, "can't open %s\n", font_path); exit(1);
    }
    if (FT_Set_Pixel_Sizes(face, 0, px_size)) {
        fprintf(stderr, "set_pixel_sizes(%d) failed\n", px_size); exit(1);
    }
    b->px_size  = px_size;
    b->line_h   = face->size->metrics.height   >> 6;
    b->baseline = face->size->metrics.ascender >> 6;

    for (uint32_t c = 32; c <= 126; c++) rasterize(face, c, b);
    for (int i = 0; i < N_ICONS; i++)    rasterize(face, ICONS[i], b);

    qsort(b->glyphs, b->n_glyphs, sizeof(Glyph), cmp_glyph);
    FT_Done_Face(face);
}

/* ------------------------------------------------------------------ */
/* Raw file slurp + tiny UTF-8 decode (for the PSF2 unicode table)     */
/* ------------------------------------------------------------------ */

static uint8_t *read_file(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); exit(1); }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n < 0) { fprintf(stderr, "stat %s\n", path); exit(1); }
    uint8_t *buf = xrealloc(NULL, (size_t)n + 1);
    if (n && fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        fprintf(stderr, "short read %s\n", path); exit(1);
    }
    buf[n] = 0;
    fclose(fp);
    *out_len = (size_t)n;
    return buf;
}

static int utf8_next(const uint8_t *p, const uint8_t *end, uint32_t *cp) {
    if (p >= end) return 0;
    uint8_t c = p[0];
    if (c < 0x80)            { *cp = c; return 1; }
    if ((c & 0xe0) == 0xc0 && p + 1 < end) { *cp = ((c & 0x1f) << 6) | (p[1] & 0x3f); return 2; }
    if ((c & 0xf0) == 0xe0 && p + 2 < end) { *cp = ((c & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f); return 3; }
    if ((c & 0xf8) == 0xf0 && p + 3 < end) { *cp = ((c & 0x07) << 18) | ((p[1] & 0x3f) << 12) | ((p[2] & 0x3f) << 6) | (p[3] & 0x3f); return 4; }
    return 1; /* skip stray byte */
}

/* ------------------------------------------------------------------ */
/* PSF1 / PSF2 (Linux console bitmap fonts)                            */
/* ------------------------------------------------------------------ */

/* cp → glyph index table built from the optional unicode table. */
typedef struct { uint32_t cp; int idx; } CpMap;

static int cpmap_lookup(const CpMap *m, int n, uint32_t cp) {
    for (int i = 0; i < n; i++) if (m[i].cp == cp) return m[i].idx;
    return -1;
}

/* Expand a 1bpp glyph cell (MSB-first rows, rowbytes per row) to alpha8. */
static void expand_1bpp(const uint8_t *cell, int w, int h, int rowbytes, uint8_t *out) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int bit = cell[y * rowbytes + (x >> 3)] & (0x80 >> (x & 7));
            out[y * w + x] = bit ? 255 : 0;
        }
}

static void read_psf(const uint8_t *d, size_t len, Bake *b) {
    int w, h, charsize, length, hdr, has_uc;
    const uint8_t *glyphs;

    if (len >= 4 && d[0] == 0x72 && d[1] == 0xb5 && d[2] == 0x4a && d[3] == 0x86) {
        /* PSF2 */
        if (len < 32) { fprintf(stderr, "psf2: truncated\n"); exit(1); }
        uint32_t flags  = d[12] | d[13]<<8 | d[14]<<16 | (uint32_t)d[15]<<24;
        hdr      = d[8]  | d[9]<<8  | d[10]<<16 | (int)d[11]<<24;
        length   = d[16] | d[17]<<8 | d[18]<<16 | (int)d[19]<<24;
        charsize = d[20] | d[21]<<8 | d[22]<<16 | (int)d[23]<<24;
        h        = d[24] | d[25]<<8 | d[26]<<16 | (int)d[27]<<24;
        w        = d[28] | d[29]<<8 | d[30]<<16 | (int)d[31]<<24;
        has_uc   = flags & 0x01;
        glyphs   = d + hdr;
    } else if (len >= 4 && d[0] == 0x36 && d[1] == 0x04) {
        /* PSF1 */
        uint8_t mode = d[2];
        charsize = d[3];
        w = 8; h = charsize; hdr = 4;
        length = (mode & 0x01) ? 512 : 256;
        has_uc = (mode & 0x06) ? 1 : 0;
        glyphs = d + hdr;
    } else {
        fprintf(stderr, "psf: bad magic\n"); exit(1);
    }
    if (w <= 0 || h <= 0 || charsize <= 0) { fprintf(stderr, "psf: bad geometry\n"); exit(1); }

    int rowbytes = (w + 7) / 8;
    CpMap *map = NULL; int nmap = 0;

    if (has_uc) {
        const uint8_t *u = glyphs + (size_t)length * charsize;
        const uint8_t *end = d + len;
        if (d[0] == 0x36) {
            /* PSF1 unicode: uint16le codepoints, 0xFFFF terminates a glyph. */
            for (int gi = 0; gi < length && u + 1 < end; gi++) {
                while (u + 1 < end) {
                    uint16_t v = u[0] | u[1] << 8; u += 2;
                    if (v == 0xFFFF) break;
                    if (v == 0xFFFE) continue;   /* sequence sep — ignore */
                    map = xrealloc(map, sizeof(CpMap) * (nmap + 1));
                    map[nmap++] = (CpMap){ v, gi };
                }
            }
        } else {
            /* PSF2 unicode: UTF-8 codepoints, 0xFF terminates, 0xFE seps. */
            for (int gi = 0; gi < length && u < end; gi++) {
                while (u < end && *u != 0xFF) {
                    if (*u == 0xFE) { u++; continue; }
                    uint32_t cp = 0; int k = utf8_next(u, end, &cp);
                    if (!k) break;
                    u += k;
                    map = xrealloc(map, sizeof(CpMap) * (nmap + 1));
                    map[nmap++] = (CpMap){ cp, gi };
                }
                if (u < end) u++; /* skip 0xFF */
            }
        }
    }

    b->px_size = h; b->line_h = h; b->baseline = h;
    uint8_t *tmp = xrealloc(NULL, (size_t)w * h);

    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t lo = pass ? 0 : 32, hi = pass ? 0 : 126;
        /* pass 0: ASCII range; pass 1: icons */
        int count = pass ? N_ICONS : (int)(hi - lo + 1);
        for (int j = 0; j < count; j++) {
            uint32_t cp = pass ? ICONS[j] : lo + j;
            int idx = has_uc ? cpmap_lookup(map, nmap, cp)
                             : (cp < (uint32_t)length ? (int)cp : -1);
            if (idx < 0) continue;
            const uint8_t *cell = glyphs + (size_t)idx * charsize;
            if (cell + (size_t)rowbytes * h > d + len) continue;
            expand_1bpp(cell, w, h, rowbytes, tmp);
            /* full cell sits above the baseline (descent 0): by = h. */
            add_glyph(b, cp, w, h, 0, h, w, tmp);
        }
    }
    free(tmp);
    free(map);
    qsort(b->glyphs, b->n_glyphs, sizeof(Glyph), cmp_glyph);
}

/* ------------------------------------------------------------------ */
/* BDF (X11 bitmap font, text format)                                  */
/* ------------------------------------------------------------------ */

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

/* Copy the next line (sans terminator) into buf; advance cursor. 0 at EOF. */
static int next_line(const char **cur, const char *end, char *buf, int bufsz) {
    if (*cur >= end) return 0;
    const char *p = *cur;
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    int len  = nl ? (int)(nl - p) : (int)(end - p);
    int copy = len < bufsz - 1 ? len : bufsz - 1;
    memcpy(buf, p, (size_t)copy);
    if (copy > 0 && buf[copy - 1] == '\r') copy--;
    buf[copy] = 0;
    *cur = nl ? nl + 1 : end;
    return 1;
}

/* BDF metrics from FONT_ASCENT/DESCENT, falling back to FONTBOUNDINGBOX. */
static void read_bdf(uint8_t *d, size_t len, Bake *b) {
    const char *cur = (const char *)d, *end = (const char *)d + len;
    char buf[1024];
    int ascent = 0, descent = 0, bb_h = 0, bb_yoff = 0, have_fbb = 0, metrics = 0;
    uint32_t cp = 0; int gw = 0, gh = 0, gx = 0, gy = 0, dwx = 0, have_bbx = 0, have_enc = 0;
    uint8_t *tmp = NULL; size_t tmpcap = 0;

    while (next_line(&cur, end, buf, sizeof buf)) {
        if      (!strncmp(buf, "FONT_ASCENT", 11))   ascent  = atoi(buf + 11);
        else if (!strncmp(buf, "FONT_DESCENT", 12))  descent = atoi(buf + 12);
        else if (!strncmp(buf, "FONTBOUNDINGBOX", 15)) {
            int bw, bh, bx, by;
            if (sscanf(buf + 15, "%d %d %d %d", &bw, &bh, &bx, &by) == 4) {
                bb_h = bh; bb_yoff = by; have_fbb = 1;
            }
        }
        else if (!strncmp(buf, "STARTCHAR", 9)) { cp = 0; gw = gh = gx = gy = dwx = 0; have_bbx = have_enc = 0; }
        else if (!strncmp(buf, "ENCODING", 8))  { cp = (uint32_t)atoi(buf + 8); have_enc = 1; }
        else if (!strncmp(buf, "DWIDTH", 6))    { dwx = atoi(buf + 6); }
        else if (!strncmp(buf, "BBX", 3))       { sscanf(buf + 3, "%d %d %d %d", &gw, &gh, &gx, &gy); have_bbx = 1; }
        else if (!strncmp(buf, "BITMAP", 6)) {
            if (!metrics) {
                if (!ascent  && have_fbb) ascent  = bb_h + bb_yoff;
                if (!descent && have_fbb) descent = -bb_yoff;
                if (ascent + descent <= 0) { fprintf(stderr, "bdf: no usable metrics\n"); exit(1); }
                b->baseline = ascent; b->line_h = ascent + descent; b->px_size = ascent + descent;
                metrics = 1;
            }
            int wanted = have_enc && have_bbx && gw > 0 && gh > 0 && is_wanted(cp);
            if (wanted && (size_t)gw * gh > tmpcap) { tmpcap = (size_t)gw * gh; tmp = xrealloc(tmp, tmpcap); }
            for (int y = 0; y < gh; y++) {
                if (!next_line(&cur, end, buf, sizeof buf)) break;
                if (!wanted) continue;
                int ll = (int)strlen(buf);
                for (int x = 0; x < gw; x++) {
                    int nib = (x >> 3) * 2;
                    int hi = nib     < ll ? hexval((unsigned char)buf[nib])     : 0;
                    int lo = nib + 1 < ll ? hexval((unsigned char)buf[nib + 1]) : 0;
                    int bits = (hi << 4) | lo;
                    tmp[y * gw + x] = (bits & (0x80 >> (x & 7))) ? 255 : 0;
                }
            }
            if (wanted) add_glyph(b, cp, gw, gh, gx, gh + gy, dwx ? dwx : gw, tmp);
        }
    }
    if (!metrics) { fprintf(stderr, "bdf: no glyphs\n"); exit(1); }
    free(tmp);
    qsort(b->glyphs, b->n_glyphs, sizeof(Glyph), cmp_glyph);
}

/* ------------------------------------------------------------------ */
/* Emit                                                                */
/* ------------------------------------------------------------------ */

static void emit_preamble(FILE *f) {
    fprintf(f,
        "/* Auto-generated by tools/bake.c. Do not edit. */\n"
        "#ifndef WISP_BAKE_H\n"
        "#define WISP_BAKE_H\n"
        "#include <stdint.h>\n"
        "#include \"font.h\"\n\n");
}

static void emit_bake(FILE *f, const char *tag, const Bake *b) {
    fprintf(f,
        "static const uint8_t font_%s_px[%d] = {\n   ", tag, b->px_len ? b->px_len : 1);
    for (int i = 0; i < b->px_len; i++) {
        fprintf(f, " 0x%02x,", b->pixels[i]);
        if ((i & 15) == 15) fprintf(f, "\n   ");
    }
    fprintf(f, "\n};\n\n");

    fprintf(f, "static const Glyph font_%s_g[%d] = {\n", tag, b->n_glyphs ? b->n_glyphs : 1);
    for (int i = 0; i < b->n_glyphs; i++) {
        const Glyph *g = &b->glyphs[i];
        /* color column is always 0 here — baked/bitmap glyphs are alpha8. */
        fprintf(f, "    { 0x%05x, %d, %d, %d, %d, %d, 0, %d },\n",
                g->cp, g->w, g->h, g->bx, g->by, g->adv, g->px_off);
    }
    fprintf(f, "};\n\n");

    fprintf(f,
        "static const Font font_%s = {\n"
        "    .px_size  = %d,\n"
        "    .line_h   = %d,\n"
        "    .baseline = %d,\n"
        "    .n        = %d,\n"
        "    .g        = font_%s_g,\n"
        "    .px       = font_%s_px,\n"
        "};\n\n",
        tag, b->px_size, b->line_h, b->baseline, b->n_glyphs, tag, tag);
}

/* Aliases for hand-written runtime modules (osd.c, lock.c, bar.c, menu.c)
 * that still reference font_small / font_large by name. 14 and 22 are always
 * present so these resolve. */
#define ALIAS_SMALL 14
#define ALIAS_LARGE 22

static void emit_ft_stub(FILE *f, const char *default_path, const char *fallback_path,
                         const int *sizes, int nsizes) {
    emit_preamble(f);
    fprintf(f, "#define WISP_FONT_DEFAULT_PATH \"%s\"\n", default_path);
    /* Empty string → no fallback chain (font_ft.c skips it). */
    fprintf(f, "#define WISP_FONT_FALLBACK_PATH \"%s\"\n\n", fallback_path ? fallback_path : "");
    /* font_N are mutable globals defined exactly once (font_ft.c sets
     * WISP_FONT_DEFINE); every other TU sees the extern declarations. */
    fprintf(f, "#ifdef WISP_FONT_DEFINE\n");
    for (int i = 0; i < nsizes; i++)
        fprintf(f, "Font font_%d = { .px_size = %d };\n", sizes[i], sizes[i]);
    fprintf(f, "Font *const wisp_fonts[] = {");
    for (int i = 0; i < nsizes; i++) fprintf(f, " &font_%d,", sizes[i]);
    fprintf(f, " };\nconst int wisp_n_fonts = %d;\n", nsizes);
    fprintf(f, "#else\n");
    for (int i = 0; i < nsizes; i++)
        fprintf(f, "extern Font font_%d;\n", sizes[i]);
    fprintf(f, "extern Font *const wisp_fonts[];\nextern const int wisp_n_fonts;\n");
    fprintf(f, "#endif\n\n");
    fprintf(f, "#define font_small font_%d\n#define font_large font_%d\n",
            ALIAS_SMALL, ALIAS_LARGE);
    fprintf(f, "\n#endif\n");
}

static int cmp_int(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}

/* Collect, dedup, sort the requested sizes (always incl. 14 + 22). */
static int collect_sizes(int argc, char **argv, int first, int *sizes, int cap) {
    int n = 0;
    sizes[n++] = ALIAS_SMALL;
    sizes[n++] = ALIAS_LARGE;
    for (int i = first; i < argc && n < cap; i++) {
        int v = atoi(argv[i]);
        if (v < 4 || v > 256) {
            fprintf(stderr, "bake: ignoring out-of-range size %s\n", argv[i]);
            continue;
        }
        sizes[n++] = v;
    }
    qsort(sizes, n, sizeof(int), cmp_int);
    int u = 0;
    for (int i = 0; i < n; i++)
        if (i == 0 || sizes[i] != sizes[i - 1]) sizes[u++] = sizes[i];
    return u;
}

enum Fmt { FMT_TTF, FMT_PSF, FMT_BDF };

static enum Fmt detect_fmt(const uint8_t *d, size_t len) {
    if (len >= 4 && d[0] == 0x72 && d[1] == 0xb5 && d[2] == 0x4a && d[3] == 0x86) return FMT_PSF;
    if (len >= 4 && d[0] == 0x36 && d[1] == 0x04) return FMT_PSF;
    if (len >= 9 && !memcmp(d, "STARTFONT", 9)) return FMT_BDF;
    return FMT_TTF;
}

int main(int argc, char **argv) {
    int sizes[64];

    /* --ft-stub: emit runtime skeletons, no glyph data. */
    if (argc >= 3 && !strcmp(argv[1], "--ft-stub")) {
        const char *out_path = argv[2];
        const char *font_path = argc >= 4 ? argv[3] : "";
        const char *fb_path   = argc >= 5 ? argv[4] : "";
        int nsizes = collect_sizes(argc, argv, 5, sizes, 64);
        FILE *f = fopen(out_path, "w");
        if (!f) { perror(out_path); return 1; }
        emit_ft_stub(f, font_path, fb_path, sizes, nsizes);
        fclose(f);
        fprintf(stderr, "bake: %s freetype stub (%d sizes, default %s%s%s)\n",
                out_path, nsizes, font_path,
                fb_path && *fb_path ? ", fallback " : "", fb_path ? fb_path : "");
        return 0;
    }

    if (argc < 3) {
        fprintf(stderr,
            "usage: bake <font.{ttf,otf,psf,psfu,bdf}> <out.h> [<px-size>...]\n"
            "       bake --ft-stub <out.h> <default-font-path> [<fallback-path>] [<px-size>...]\n");
        return 2;
    }
    const char *font_path = argv[1];
    const char *out_path  = argv[2];
    int nsizes = collect_sizes(argc, argv, 3, sizes, 64);

    FILE *f = fopen(out_path, "w");
    if (!f) { perror(out_path); return 1; }
    emit_preamble(f);

    size_t flen = 0;
    uint8_t *fdata = read_file(font_path, &flen);
    enum Fmt fmt = detect_fmt(fdata, flen);

    if (fmt == FMT_TTF) {
        free(fdata);
        FT_Library ft;
        if (FT_Init_FreeType(&ft)) { fprintf(stderr, "ft init\n"); return 1; }
        for (int i = 0; i < nsizes; i++) {
            Bake b = {0};
            bake_ttf_size(ft, font_path, sizes[i], &b);
            char tag[16]; snprintf(tag, sizeof tag, "%d", sizes[i]);
            emit_bake(f, tag, &b);
            fprintf(stderr, "bake: %s font_%d=%d glyphs/%d B\n",
                    out_path, sizes[i], b.n_glyphs, b.px_len);
            free(b.glyphs); free(b.pixels);
        }
        FT_Done_FreeType(ft);
        fprintf(f, "\nstatic const Font *const wisp_fonts[] __attribute__((unused)) = {");
        for (int i = 0; i < nsizes; i++) fprintf(f, " &font_%d,", sizes[i]);
        fprintf(f, " };\nstatic const int wisp_n_fonts __attribute__((unused)) = %d;\n", nsizes);
        fprintf(f, "\n#define font_small font_%d\n#define font_large font_%d\n",
                ALIAS_SMALL, ALIAS_LARGE);
    } else {
        /* Bitmap font: one native size, every requested size aliases to it. */
        Bake b = {0};
        if (fmt == FMT_PSF) read_psf(fdata, flen, &b);
        else                read_bdf(fdata, flen, &b);
        free(fdata);
        emit_bake(f, "bm", &b);
        fprintf(stderr, "bake: %s font_bm=%d glyphs/%d B @ %dpx (bitmap)\n",
                out_path, b.n_glyphs, b.px_len, b.px_size);
        free(b.glyphs); free(b.pixels);
        fprintf(f, "\n");
        for (int i = 0; i < nsizes; i++)
            fprintf(f, "#define font_%d font_bm\n", sizes[i]);
        fprintf(f, "static const Font *const wisp_fonts[] __attribute__((unused)) = { &font_bm };\n");
        fprintf(f, "static const int wisp_n_fonts __attribute__((unused)) = 1;\n");
        fprintf(f, "#define font_small font_%d\n#define font_large font_%d\n",
                ALIAS_SMALL, ALIAS_LARGE);
    }

    fprintf(f, "\n#endif\n");
    fclose(f);
    return 0;
}

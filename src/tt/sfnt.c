/* sfnt container: mmap, table directory, head/maxp/hhea/OS_2/hmtx/loca/glyf,
 * and a flattened cmap index. Every read is bounds-checked against the mapping
 * — the file is untrusted input like any other wire format.
 *
 * The cmap is not consulted at lookup time: it is flattened once at open into
 * sorted (first,last,delta) ranges, so a steady-state session never faults a
 * font page for a character it has already drawn. */
#include "tt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t r32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
/* [off,off+len) inside the mapping, overflow-safe. */
static int fits(const TtFont *f, uint32_t off, uint32_t len) {
    return len <= f->size && off <= f->size - len;
}
static const uint8_t *at(const TtFont *f, uint32_t off, uint32_t len) {
    return fits(f, off, len) ? f->base + off : NULL;
}

/* --- cmap → range index ------------------------------------------------- */

static int push(TtFont *f, int *cap, uint32_t first, uint32_t last, int32_t delta) {
    if (f->n_map && f->map[f->n_map - 1].delta == delta
                 && f->map[f->n_map - 1].last + 1 == first) {
        f->map[f->n_map - 1].last = last;
        return 1;
    }
    if (f->n_map == *cap) {
        int nc = *cap ? *cap * 2 : 256;
        TtRange *n = realloc(f->map, (size_t)nc * sizeof *n);
        if (!n) return 0;
        f->map = n; *cap = nc;
    }
    f->map[f->n_map++] = (TtRange){ first, last, delta };
    return 1;
}

static int parse_cmap4(TtFont *f, uint32_t off, int *cap) {
    const uint8_t *t = at(f, off, 14);
    if (!t) return 0;
    uint32_t segs = r16(t + 6) / 2;
    if (!segs || !at(f, off + 16, segs * 8)) return 0;
    uint32_t o_end = off + 14, o_start = o_end + segs * 2 + 2;
    uint32_t o_delta = o_start + segs * 2, o_ro = o_delta + segs * 2;
    for (uint32_t i = 0; i < segs; i++) {
        uint32_t start = r16(f->base + o_start + i * 2), end = r16(f->base + o_end + i * 2);
        int32_t  delta = (int16_t)r16(f->base + o_delta + i * 2);
        uint32_t ro    = r16(f->base + o_ro + i * 2);
        for (uint32_t cp = start; cp <= end && cp <= 0xFFFF; cp++) {
            uint32_t gid;
            if (!ro) gid = (cp + (uint32_t)delta) & 0xFFFF;
            else {
                const uint8_t *g = at(f, o_ro + i * 2 + ro + (cp - start) * 2, 2);
                if (!g) continue;
                gid = r16(g);
                if (gid) gid = (gid + (uint32_t)delta) & 0xFFFF;
            }
            if (!gid || gid >= f->n_glyphs) continue;
            if (!push(f, cap, cp, cp, (int32_t)gid - (int32_t)cp)) return 0;
            if (cp == 0xFFFF) break;   /* cp++ would wrap */
        }
    }
    return 1;
}

static int parse_cmap12(TtFont *f, uint32_t off, int *cap) {
    const uint8_t *t = at(f, off, 16);
    if (!t) return 0;
    uint32_t n = r32(t + 12);
    if (n > (f->size - off) / 12) return 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *g = at(f, off + 16 + i * 12, 12);
        if (!g) return 0;
        uint32_t start = r32(g), end = r32(g + 4), gid = r32(g + 8);
        if (start > end || end > 0x10FFFF || !gid || gid >= f->n_glyphs) continue;
        if (gid + (end - start) >= f->n_glyphs) end = start + (f->n_glyphs - 1 - gid);
        if (!push(f, cap, start, end, (int32_t)gid - (int32_t)start)) return 0;
    }
    return 1;
}

/* Pick one subtable: format 12 (full Unicode) beats format 4 (BMP). */
static void parse_cmap(TtFont *f, uint32_t off, uint32_t len) {
    const uint8_t *t = at(f, off, 4);
    if (!t || len < 4) return;
    uint32_t n = r16(t + 2), best = 0, best_fmt = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = at(f, off + 4 + i * 8, 8);
        if (!e) break;
        uint32_t sub = off + r32(e + 4);
        const uint8_t *s = at(f, sub, 2);
        if (!s) continue;
        uint32_t fmt = r16(s);
        if ((fmt == 4 || fmt == 12) && fmt > best_fmt) { best_fmt = fmt; best = sub; }
    }
    int cap = 0;
    if (best_fmt == 12) parse_cmap12(f, best, &cap);
    else if (best_fmt == 4) parse_cmap4(f, best, &cap);
}

/* --- open --------------------------------------------------------------- */

static int find_table(const TtFont *f, uint32_t n_tab, const char *tag,
                      uint32_t *off, uint32_t *len) {
    for (uint32_t i = 0; i < n_tab; i++) {
        const uint8_t *e = at(f, 12 + i * 16, 16);
        if (!e) return 0;
        if (memcmp(e, tag, 4)) continue;
        uint32_t o = r32(e + 8), l = r32(e + 12);
        if (!fits(f, o, l)) return 0;
        *off = o; *len = l;
        return 1;
    }
    return 0;
}

int tt_open(TtFont *f, const char *path) {
    memset(f, 0, sizeof *f);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { fprintf(stderr, "wisp: tt: cannot open %s\n", path); return 0; }
    struct stat st;
    if (fstat(fd, &st) || st.st_size < 12 || (uint64_t)st.st_size > UINT32_MAX) {
        close(fd); fprintf(stderr, "wisp: tt: %s: not a font\n", path); return 0;
    }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { fprintf(stderr, "wisp: tt: mmap %s failed\n", path); return 0; }
    f->base = m; f->size = (size_t)st.st_size;

    uint32_t ver = r32(f->base);
    /* ponytail: no TrueType Collections (ttcf) — add a face index if a
     * .ttc ever shows up as the configured font. */
    if (ver != 0x00010000 && ver != 0x74727565 /*true*/ && ver != 0x4F54544F /*OTTO*/)
        goto bad;
    uint32_t n_tab = r16(f->base + 4), off, len;

    if (!find_table(f, n_tab, "head", &off, &len) || len < 54) goto bad;
    f->upem      = r16(f->base + off + 18);
    f->long_loca = (int16_t)r16(f->base + off + 50) != 0;
    if (!f->upem) goto bad;

    if (!find_table(f, n_tab, "maxp", &off, &len) || len < 6) goto bad;
    f->n_glyphs = r16(f->base + off + 4);
    if (!f->n_glyphs) goto bad;

    if (!find_table(f, n_tab, "hhea", &off, &len) || len < 36) goto bad;
    f->ascent     = (int16_t)r16(f->base + off + 4);
    f->descent    = (int16_t)r16(f->base + off + 6);
    f->line_gap   = (int16_t)r16(f->base + off + 8);
    f->n_hmetrics = r16(f->base + off + 34);

    /* fsSelection bit 7 (USE_TYPO_METRICS) means the typo values are the
     * authoritative line metrics; FreeType honours it, so we must too or the
     * two backends disagree on line height. */
    if (find_table(f, n_tab, "OS/2", &off, &len) && len >= 78
        && r16(f->base + off) >= 4 && (r16(f->base + off + 62) & 0x80)) {
        f->ascent   = (int16_t)r16(f->base + off + 68);
        f->descent  = (int16_t)r16(f->base + off + 70);
        f->line_gap = (int16_t)r16(f->base + off + 72);
    }

    if (!find_table(f, n_tab, "hmtx", &f->hmtx_off, &f->hmtx_len)) goto bad;
    if (find_table(f, n_tab, "loca", &f->loca_off, &f->loca_len))
        find_table(f, n_tab, "glyf", &f->glyf_off, &f->glyf_len);
    if (find_table(f, n_tab, "CFF ", &f->cff_off, &f->cff_len) && !f->glyf_len
        && !tt_cff_init(f))
        goto bad;

    if (find_table(f, n_tab, "CBLC", &f->cblc_off, &f->cblc_len))
        find_table(f, n_tab, "CBDT", &f->cbdt_off, &f->cbdt_len);
    if (find_table(f, n_tab, "COLR", &f->colr_off, &f->colr_len))
        find_table(f, n_tab, "CPAL", &f->cpal_off, &f->cpal_len);
    f->has_color = (f->cbdt_len && f->cblc_len) || (f->colr_len && f->cpal_len);

    /* A bitmap-only font (CBDT emoji, no glyf/CFF) is fine — every glyph comes
     * out of a strike. No outlines *and* no strikes is not a font we can draw. */
    if (!f->glyf_len && !f->cff_len && !(f->cbdt_len && f->cblc_len)) {
        fprintf(stderr, "wisp: tt: %s: no outlines or color strikes\n", path);
        tt_close(f);
        return 0;
    }

    if (find_table(f, n_tab, "cmap", &off, &len)) parse_cmap(f, off, len);
    if (!f->n_map) goto bad;
    return 1;
bad:
    fprintf(stderr, "wisp: tt: %s: unsupported or damaged font\n", path);
    tt_close(f);
    return 0;
}

void tt_close(TtFont *f) {
    if (f->base) munmap((void *)f->base, f->size);
    free(f->map);
    memset(f, 0, sizeof *f);
}

void tt_trim(const TtFont *f) {
    if (f->base) madvise((void *)f->base, f->size, MADV_DONTNEED);
}

/* --- lookups ------------------------------------------------------------ */

uint16_t tt_gid(const TtFont *f, uint32_t cp) {
    int lo = 0, hi = f->n_map - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (cp < f->map[m].first) hi = m - 1;
        else if (cp > f->map[m].last) lo = m + 1;
        else return (uint16_t)((int32_t)cp + f->map[m].delta);
    }
    return 0;
}

int tt_advance(const TtFont *f, uint16_t gid) {
    if (!f->n_hmetrics) return 0;
    uint32_t i = gid < f->n_hmetrics ? gid : (uint32_t)f->n_hmetrics - 1;
    const uint8_t *p = at(f, f->hmtx_off + i * 4, 2);
    return p ? r16(p) : 0;
}

const uint8_t *tt_glyf(const TtFont *f, uint16_t gid, uint32_t *len) {
    if (!f->glyf_len || gid >= f->n_glyphs) return NULL;
    uint32_t sz = f->long_loca ? 4 : 2, a, b;
    const uint8_t *l = at(f, f->loca_off + gid * sz, sz * 2);
    if (!l) return NULL;
    if (f->long_loca) { a = r32(l); b = r32(l + 4); }
    else              { a = r16(l) * 2u; b = r16(l + 2) * 2u; }
    if (b <= a || b > f->glyf_len) return NULL;   /* b == a: empty glyph (space) */
    *len = b - a;
    return at(f, f->glyf_off + a, b - a);
}

/* --- scaled metrics ----------------------------------------------------- */

/* FreeType's arithmetic, reproduced so the two backends round identically:
 * a 16.16 scale, FT_MulFix into 26.6, then FT_PIX_ROUND to whole pixels. */
static int32_t scale_of(const TtFont *f, int px) {
    return (int32_t)(((int64_t)px * 64 << 16) / f->upem);
}
static int64_t mulfix(int v, int32_t scale) {
    return ((int64_t)v * scale + 0x8000) >> 16;        /* 26.6 */
}
static int mulfix_round(int v, int32_t scale) { return (int)((mulfix(v, scale) + 32) >> 6); }

/* FT_Request_Metrics rounds these three differently — ascender up, descender
 * down, height to nearest — and the ascender is our baseline. */
void tt_vmetrics(const TtFont *f, int px, int *line_h, int *baseline) {
    int32_t s = scale_of(f, px);
    *baseline = (int)((mulfix(f->ascent, s) + 63) >> 6);
    *line_h   = mulfix_round(f->ascent - f->descent + f->line_gap, s);
}

int tt_advance_px(const TtFont *f, uint16_t gid, int px) {
    return mulfix_round(tt_advance(f, gid), scale_of(f, px));
}

/* Color glyphs: CBDT/CBLC bitmap strikes (PNG emoji) and COLRv0 layer stacks.
 * Output is always premultiplied BGRA — the format Glyph.color=1 means, drawn
 * as-is instead of tinted by the text colour.
 *
 * Both tables are read straight out of the mmap on every miss; like the rest of
 * src/tt nothing is cached here beyond one scratch canvas, so tt_trim() can
 * still drop the whole font. Untrusted input: every offset is bounds-checked
 * against the table it lives in before it is followed.
 *
 * Not linked into the standalone ttmetrics tool — this is the one src/tt file
 * that depends on the rest of wisp (image.c's PNG decoder).
 *
 * ponytail: COLRv1 (gradients, transforms) and sbix/SVG strikes are skipped.
 * COLRv1 needs a paint-graph interpreter and a gradient rasterizer; add it if a
 * font we actually ship uses it. */
#include "tt.h"
#include "../image.h"
#include <stdlib.h>
#include <string.h>

#define MAX_DIM 512

static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t r32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
/* [off,off+len) inside the mapping, overflow-safe. */
static const uint8_t *at(const TtFont *f, uint32_t off, uint32_t len) {
    return len <= f->size && off <= f->size - len ? f->base + off : NULL;
}

static uint8_t *canvas;
static int      cap_canvas;

static uint8_t *canvas_get(int bytes) {
    if (bytes > cap_canvas) {
        uint8_t *p = realloc(canvas, (size_t)bytes);
        if (!p) return NULL;
        canvas = p; cap_canvas = bytes;
    }
    return canvas;
}

void tt_color_release(void) { free(canvas); canvas = NULL; cap_canvas = 0; }

/* --- CBDT/CBLC ----------------------------------------------------------- */

/* Locate gid's image data within CBDT. Returns the record (metrics + PNG) and
 * sets *fmt to the CBDT image format; NULL if this strike has no such glyph. */
static const uint8_t *sbit_lookup(const TtFont *f, uint32_t arr_off, uint32_t n_sub,
                                  uint16_t gid, uint32_t *fmt, uint32_t *rec_len) {
    for (uint32_t i = 0; i < n_sub; i++) {
        const uint8_t *e = at(f, arr_off + i * 8, 8);
        if (!e) return NULL;
        uint16_t first = r16(e), last = r16(e + 2);
        if (gid < first || gid > last) continue;

        const uint8_t *h = at(f, arr_off + r32(e + 4), 8);
        if (!h) return NULL;
        uint32_t ifmt = r16(h), img_off = r32(h + 4);
        *fmt = r16(h + 2);
        uint32_t k = gid - first, a, b;
        if (ifmt == 1) {
            const uint8_t *o = at(f, arr_off + r32(e + 4) + 8 + k * 4, 8);
            if (!o) return NULL;
            a = r32(o); b = r32(o + 4);
        } else if (ifmt == 3) {
            const uint8_t *o = at(f, arr_off + r32(e + 4) + 8 + k * 2, 4);
            if (!o) return NULL;
            a = r16(o); b = r16(o + 2);
        } else {
            return NULL;   /* formats 2/4/5 are constant-size / sparse: unused by
                              PNG strikes, so not worth the code */
        }
        if (b <= a || b - a > f->cbdt_len) return NULL;   /* b == a: absent glyph */
        *rec_len = b - a;
        return at(f, f->cbdt_off + img_off + a, b - a);
    }
    return NULL;
}

/* Box-downscale a decoded RGBA strike bitmap to `dw`x`dh` premultiplied BGRA. */
static void scale_bgra(const uint8_t *src, int sw, int sh,
                       uint8_t *dst, int dw, int dh) {
    for (int dy = 0; dy < dh; dy++) {
        int sy0 = dy * sh / dh, sy1 = (dy + 1) * sh / dh;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int dx = 0; dx < dw; dx++) {
            int sx0 = dx * sw / dw, sx1 = (dx + 1) * sw / dw;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            unsigned r = 0, g = 0, b = 0, a = 0, cnt = 0;
            for (int y = sy0; y < sy1; y++)
                for (int x = sx0; x < sx1; x++) {
                    const uint8_t *p = src + ((size_t)y * sw + x) * 4;
                    /* Premultiply before averaging: averaging straight colours
                       bleeds the (undefined) RGB of fully transparent texels. */
                    a += p[3];
                    r += p[0] * p[3] / 255; g += p[1] * p[3] / 255; b += p[2] * p[3] / 255;
                    cnt++;
                }
            uint8_t *o = dst + ((size_t)dy * dw + dx) * 4;
            o[0] = (uint8_t)(b / cnt); o[1] = (uint8_t)(g / cnt);
            o[2] = (uint8_t)(r / cnt); o[3] = (uint8_t)(a / cnt);
        }
    }
}

static int cbdt_glyph(const TtFont *f, uint16_t gid, int px, TtBitmap *bm) {
    const uint8_t *hdr = at(f, f->cblc_off, 8);
    if (!hdr) return 0;
    uint32_t n_sizes = r32(hdr + 4);
    if (n_sizes > 64) return 0;

    /* Largest strike that has this glyph — we always downscale, never up. */
    const uint8_t *rec = NULL;
    uint32_t fmt = 0, rec_len = 0;
    int ppem = 0;
    for (uint32_t i = 0; i < n_sizes; i++) {
        const uint8_t *s = at(f, f->cblc_off + 8 + i * 48, 48);
        if (!s) break;
        int p = s[45];
        if (p <= ppem || gid < r16(s + 40) || gid > r16(s + 42)) continue;
        uint32_t fm = 0, rl = 0;
        const uint8_t *r = sbit_lookup(f, f->cblc_off + r32(s), r32(s + 8), gid, &fm, &rl);
        if (!r) continue;
        rec = r; fmt = fm; rec_len = rl; ppem = p;
    }
    if (!rec || ppem < 1) return 0;

    /* 17 = small metrics (5 B), 18 = big metrics (8 B); both are then a u32
     * length and a PNG. 19 keeps its metrics in the index subtable formats we
     * don't read, so it is skipped. */
    uint32_t mlen = fmt == 17 ? 5 : fmt == 18 ? 8 : 0;
    if (!mlen || rec_len < mlen + 4) return 0;
    int bear_x = (int8_t)rec[2], bear_y = (int8_t)rec[3];
    uint32_t dlen = r32(rec + mlen);
    if (dlen > rec_len - mlen - 4) return 0;

    /* Round-to-nearest that works for negative bearings too — (v + d/2)/d
     * truncates toward zero, which rounds the wrong way below zero. */
    #define RDIV(v, d) ((v) >= 0 ? ((v) + (d) / 2) / (d) : -((-(v) + (d) / 2) / (d)))
    int sw = 0, sh = 0;
    uint8_t *rgba = image_decode_png(rec + mlen + 4, (int)dlen, &sw, &sh);
    if (!rgba) return 0;
    int ok = 0;
    int w = (sw * px + ppem / 2) / ppem, h = (sh * px + ppem / 2) / ppem;
    if (sw > 0 && sh > 0 && w > 0 && h > 0 && w <= MAX_DIM && h <= MAX_DIM) {
        uint8_t *dst = canvas_get(w * h * 4);
        if (dst) {
            scale_bgra(rgba, sw, sh, dst, w, h);
            *bm = (TtBitmap){ .w = w, .h = h,
                              .bx = RDIV(bear_x * px, ppem),
                              .by = RDIV(bear_y * px, ppem),
                              .color = 1, .px = dst };
            ok = 1;
        }
    }
    image_free(rgba);
    return ok;
}

/* --- COLRv0 -------------------------------------------------------------- */

/* Base glyph records are sorted by gid. Returns 0 if gid isn't a colour base. */
static int colr_layers(const TtFont *f, uint16_t gid, uint32_t *first, uint32_t *n) {
    const uint8_t *h = at(f, f->colr_off, 14);
    if (!h || r16(h)) return 0;                 /* v0 only */
    uint32_t n_base = r16(h + 2), base = f->colr_off + r32(h + 4);
    int lo = 0, hi = (int)n_base - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        const uint8_t *r = at(f, base + (uint32_t)m * 6, 6);
        if (!r) return 0;
        uint16_t g = r16(r);
        if (gid < g) hi = m - 1;
        else if (gid > g) lo = m + 1;
        else { *first = r16(r + 2); *n = r16(r + 4); return *n != 0; }
    }
    return 0;
}

/* CPAL palette 0, straight (non-premultiplied) BGRA. */
static int cpal_color(const TtFont *f, uint16_t idx, uint8_t out[4]) {
    /* 0xFFFF means "the text foreground", which the cache has already lost by
     * the time it draws a colour glyph. ponytail: white — plain-white layers
     * are rare in practice, and the alternative is threading fg into the
     * cache key. */
    if (idx == 0xFFFF) { out[0] = out[1] = out[2] = out[3] = 255; return 1; }
    const uint8_t *h = at(f, f->cpal_off, 14);
    if (!h || !r16(h + 4)) return 0;
    if (idx >= r16(h + 2)) return 0;
    uint32_t recs = f->cpal_off + r32(h + 8) + (uint32_t)r16(h + 12) * 4;
    const uint8_t *c = at(f, recs + (uint32_t)idx * 4, 4);
    if (!c) return 0;
    memcpy(out, c, 4);
    return 1;
}

static int colr_glyph(const TtFont *f, uint16_t gid, int px, TtBitmap *bm) {
    uint32_t first = 0, n = 0;
    if (!colr_layers(f, gid, &first, &n)) return 0;
    const uint8_t *lh = at(f, f->colr_off, 14);
    uint32_t lbase = f->colr_off + r32(lh + 8), n_layers = r16(lh + 12);
    if (first + n > n_layers || n > 256) return 0;

    /* Two raster passes: the rasterizer owns a single scratch bitmap, so the
     * union box has to be known before any layer can be composited into the
     * canvas. Colour glyphs are cached after the first draw, so the second pass
     * costs nothing steady-state. */
    int x0 = 1 << 24, y0 = 1 << 24, x1 = -(1 << 24), y1 = -(1 << 24);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *l = at(f, lbase + (first + i) * 4, 4);
        TtBitmap lb;
        if (!l || !tt_render(f, r16(l), px, &lb) || !lb.w || !lb.h) continue;
        if (lb.bx < x0) x0 = lb.bx;
        if (lb.by > y1) y1 = lb.by;
        if (lb.bx + lb.w > x1) x1 = lb.bx + lb.w;
        if (lb.by - lb.h < y0) y0 = lb.by - lb.h;
    }
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0 || w > MAX_DIM || h > MAX_DIM) return 0;
    uint8_t *dst = canvas_get(w * h * 4);
    if (!dst) return 0;
    memset(dst, 0, (size_t)w * h * 4);

    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *l = at(f, lbase + (first + i) * 4, 4);
        uint8_t col[4];
        TtBitmap lb;
        if (!l || !cpal_color(f, r16(l + 2), col)) continue;
        if (!tt_render(f, r16(l), px, &lb) || !lb.w || !lb.h) continue;
        for (int y = 0; y < lb.h; y++) {
            int dy = (y1 - lb.by) + y;
            if (dy < 0 || dy >= h) continue;
            for (int x = 0; x < lb.w; x++) {
                int dx = (lb.bx - x0) + x;
                if (dx < 0 || dx >= w) continue;
                unsigned a = lb.px[(size_t)y * lb.w + x] * col[3] / 255;
                if (!a) continue;
                uint8_t *o = dst + ((size_t)dy * w + dx) * 4;
                for (int c = 0; c < 3; c++)
                    o[c] = (uint8_t)(col[c] * a / 255 + o[c] * (255 - a) / 255);
                o[3] = (uint8_t)(a + o[3] * (255 - a) / 255);
            }
        }
    }
    *bm = (TtBitmap){ .w = w, .h = h, .bx = x0, .by = y1, .color = 1, .px = dst };
    return 1;
}

int tt_color_glyph(const TtFont *f, uint16_t gid, int px, TtBitmap *bm) {
    if (!f->has_color || px < 1) return 0;
    if (f->cbdt_len && cbdt_glyph(f, gid, px, bm)) return 1;
    if (f->cpal_len && colr_glyph(f, gid, px, bm)) return 1;
    return 0;
}

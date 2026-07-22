/* glyf outlines → alpha8 coverage bitmaps. Signed-area scanline AA (the
 * stb_truetype / libschrift accumulate-then-prefix-sum trick): every edge
 * deposits an (area, cover) pair into the cells it crosses, and one linear
 * sweep per row turns those into coverage. No hinting, no dropout control —
 * we render unhinted like the freetype backend already did.
 *
 * All scratch is static and grows to the largest glyph ever seen: a bar
 * redraws the same few hundred glyphs forever, so it settles on frame one. */
#include "tt.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { float x, y; } Pt;
typedef struct { Pt a, b; } Seg;

#define MAX_DIM   1024   /* a glyph bigger than this is a malformed font */
#define MAX_DEPTH 5      /* composite nesting */

static Seg     *segs;  static int n_seg, cap_seg;
static Pt      *pts;   static uint8_t *ons; static float *xs; static int cap_pt;
static float   *cells; static int cap_cell;   /* {area, cover} per pixel */
static uint8_t *img;   static int cap_img;

static float cb_x0, cb_y0, cb_x1, cb_y1;   /* control box, pixels, y-up */
static float g_scale;
static int   g_bad;                        /* OOM or malformed outline */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* --- outline collection -------------------------------------------------- */

/* The control box includes off-curve points, matching FT_Outline_Get_CBox —
 * that (not the true bbox) is what FreeType sizes its bitmaps from, so bearings
 * and dimensions agree with the freetype backend. */
static Pt mkpt(float fx, float fy) {
    /* Quantized to 1/64 px because FreeType scales points into 26.6 *before*
     * taking the cbox — without this the box disagrees by a pixel whenever an
     * exact-integer edge lands on the wrong side of a float rounding error. */
    Pt p = { roundf(fx * g_scale * 64.0f) / 64.0f,
             roundf(fy * g_scale * 64.0f) / 64.0f };
    if (p.x < cb_x0) cb_x0 = p.x;
    if (p.x > cb_x1) cb_x1 = p.x;
    if (p.y < cb_y0) cb_y0 = p.y;
    if (p.y > cb_y1) cb_y1 = p.y;
    return p;
}

static void line(Pt a, Pt b) {
    if (a.y == b.y) return;               /* horizontal edges carry no coverage */
    if (n_seg == cap_seg) {
        int nc = cap_seg ? cap_seg * 2 : 256;
        Seg *n = realloc(segs, (size_t)nc * sizeof *n);
        if (!n) { g_bad = 1; return; }
        segs = n; cap_seg = nc;
    }
    segs[n_seg++] = (Seg){ a, b };
}

static void quad(Pt p0, Pt p1, Pt p2) {
    /* Chord error of a quadratic is |p0 - 2p1 + p2| / 4, and falls as 1/n²;
     * solve for ~1/10 px. */
    float dx = p0.x - 2 * p1.x + p2.x, dy = p0.y - 2 * p1.y + p2.y;
    int n = 1 + (int)sqrtf(sqrtf(dx * dx + dy * dy) * 2.5f);
    if (n > 32) n = 32;
    Pt prev = p0;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / (float)n, u = 1.0f - t;
        Pt q = { u * u * p0.x + 2 * u * t * p1.x + t * t * p2.x,
                 u * u * p0.y + 2 * u * t * p1.y + t * t * p2.y };
        line(prev, q);
        prev = q;
    }
}

static Pt mid(Pt a, Pt b) { return (Pt){ (a.x + b.x) / 2, (a.y + b.y) / 2 }; }

/* One TrueType contour: consecutive off-curve points imply an on-curve point
 * halfway between them. */
static void contour(int lo, int hi) {
    int n = hi - lo + 1;
    if (n < 2) return;
    const Pt *p = pts + lo;
    const uint8_t *on = ons + lo;
    int j;
    Pt start;
    if (on[0])        { start = p[0];              j = 1; }
    else if (on[n-1]) { start = p[n-1];            j = 0; }
    else              { start = mid(p[0], p[n-1]); j = 0; }

    Pt cur = start, c = {0, 0};
    int have_c = 0;
    for (int k = 0; k < n; k++) {
        int i = (j + k) % n;
        if (on[i]) {
            if (have_c) { quad(cur, c, p[i]); have_c = 0; } else line(cur, p[i]);
            cur = p[i];
        } else {
            if (have_c) { Pt m = mid(c, p[i]); quad(cur, c, m); cur = m; }
            c = p[i]; have_c = 1;
        }
    }
    if (have_c) quad(cur, c, start); else line(cur, start);
}

/* --- path sink (CFF charstrings) ----------------------------------------- */

static Pt   pth_cur, pth_start;
static int  pth_open;

void tt_path_close(void) {
    if (pth_open) line(pth_cur, pth_start);
    pth_open = 0;
}

void tt_path_move(float x, float y) {
    tt_path_close();
    pth_cur = pth_start = mkpt(x, y);
    pth_open = 1;
}

void tt_path_line(float x, float y) {
    Pt p = mkpt(x, y);
    line(pth_cur, p);
    pth_cur = p;
}

/* Same chord-error argument as quad(), with the cubic's |p0-2p1+p2| replaced by
 * the larger of its two second differences. */
void tt_path_cubic(float x1, float y1, float x2, float y2, float x3, float y3) {
    Pt p0 = pth_cur, p1 = mkpt(x1, y1), p2 = mkpt(x2, y2), p3 = mkpt(x3, y3);
    float ax = p0.x - 2 * p1.x + p2.x, ay = p0.y - 2 * p1.y + p2.y;
    float bx = p1.x - 2 * p2.x + p3.x, by = p1.y - 2 * p2.y + p3.y;
    float d0 = ax * ax + ay * ay, d1 = bx * bx + by * by;
    int n = 1 + (int)sqrtf(sqrtf((d0 > d1 ? d0 : d1)) * 3.0f);
    if (n > 32) n = 32;
    Pt prev = p0;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / (float)n, u = 1.0f - t;
        float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
        Pt q = { a * p0.x + b * p1.x + c * p2.x + d * p3.x,
                 a * p0.y + b * p1.y + c * p2.y + d * p3.y };
        line(prev, q);
        prev = q;
    }
    pth_cur = p3;
}

/* --- glyph decode -------------------------------------------------------- */

static void emit(const TtFont *f, uint16_t gid, float a, float d,
                 float dx, float dy, int depth);

/* Points are stored x-then-y as delta runs, each with its own flag bits. */
static void simple_glyph(const uint8_t *g, uint32_t len, int nc,
                         float a, float d, float dx, float dy) {
    uint32_t o = 10;
    if (len < o + (uint32_t)nc * 2 + 2) { g_bad = 1; return; }
    int np = rd16(g + o + (nc - 1) * 2) + 1;
    uint32_t ends = o;
    o += (uint32_t)nc * 2;
    o += 2u + rd16(g + o);                       /* skip hinting bytecode */
    if (o > len || np <= 0) { g_bad = 1; return; }

    if (np > cap_pt) {
        Pt *p = realloc(pts, (size_t)np * sizeof *p);
        uint8_t *fl = realloc(ons, (size_t)np);
        float *nx = realloc(xs, (size_t)np * sizeof *nx);
        if (p) pts = p;
        if (fl) ons = fl;
        if (nx) xs = nx;
        if (!p || !fl || !nx) { g_bad = 1; return; }
        cap_pt = np;
    }

    /* ons[] holds the whole flag byte until the coords are decoded — bits 1/4
     * (x) and 2/5 (y) drive the delta reads; it is masked to bit 0 at the end. */
    for (int i = 0; i < np; ) {
        if (o >= len) { g_bad = 1; return; }
        uint8_t fl = g[o++];
        ons[i++] = fl;
        if (fl & 8) {
            if (o >= len) { g_bad = 1; return; }
            int rep = g[o++];
            while (rep-- > 0 && i < np) ons[i++] = fl;
        }
    }

    /* x deltas run first for the whole glyph, then y */
    float x = 0, y = 0;
    for (int i = 0; i < np; i++) {
        uint8_t fl = ons[i];
        if (fl & 2) {
            if (o >= len) { g_bad = 1; return; }
            int v = g[o++];
            x += (fl & 16) ? v : -v;
        } else if (!(fl & 16)) {
            if (o + 2 > len) { g_bad = 1; return; }
            x += (int16_t)rd16(g + o); o += 2;
        }
        xs[i] = x;
    }
    for (int i = 0; i < np; i++) {
        uint8_t fl = ons[i];
        if (fl & 4) {
            if (o >= len) { g_bad = 1; return; }
            int v = g[o++];
            y += (fl & 32) ? v : -v;
        } else if (!(fl & 32)) {
            if (o + 2 > len) { g_bad = 1; return; }
            y += (int16_t)rd16(g + o); o += 2;
        }
        pts[i] = mkpt(a * xs[i] + dx, d * y + dy);
        ons[i] &= 1;
    }

    int lo = 0;
    for (int ci = 0; ci < nc; ci++) {
        int hi = rd16(g + ends + (uint32_t)ci * 2);
        if (hi >= np || hi < lo) { g_bad = 1; return; }
        contour(lo, hi);
        lo = hi + 1;
    }
}

/* ponytail: components honour offsets + a diagonal scale; a full 2x2 (rotated
 * or skewed components) is ignored — no shipped text font uses one. */
static void composite(const TtFont *f, const uint8_t *g, uint32_t len,
                      float a, float d, float dx, float dy, int depth) {
    uint32_t o = 10;
    for (;;) {
        if (o + 4 > len) { g_bad = 1; return; }
        uint16_t flags = rd16(g + o), gi = rd16(g + o + 2);
        o += 4;
        float ox, oy;
        if (flags & 1) {
            if (o + 4 > len) { g_bad = 1; return; }
            ox = (int16_t)rd16(g + o); oy = (int16_t)rd16(g + o + 2); o += 4;
        } else {
            if (o + 2 > len) { g_bad = 1; return; }
            ox = (int8_t)g[o]; oy = (int8_t)g[o + 1]; o += 2;
        }
        if (!(flags & 2)) ox = oy = 0;           /* point-matching: unsupported */
        float ca = 1, cd = 1;
        if (flags & 0x0008) {
            if (o + 2 > len) { g_bad = 1; return; }
            ca = cd = (int16_t)rd16(g + o) / 16384.0f; o += 2;
        } else if (flags & 0x0040) {
            if (o + 4 > len) { g_bad = 1; return; }
            ca = (int16_t)rd16(g + o) / 16384.0f;
            cd = (int16_t)rd16(g + o + 2) / 16384.0f; o += 4;
        } else if (flags & 0x0080) {
            if (o + 8 > len) { g_bad = 1; return; }
            ca = (int16_t)rd16(g + o) / 16384.0f;
            cd = (int16_t)rd16(g + o + 6) / 16384.0f; o += 8;
        }
        emit(f, gi, a * ca, d * cd, a * ox + dx, d * oy + dy, depth + 1);
        if (!(flags & 0x0020)) return;           /* MORE_COMPONENTS */
    }
}

static void emit(const TtFont *f, uint16_t gid, float a, float d,
                 float dx, float dy, int depth) {
    if (depth > MAX_DEPTH) { g_bad = 1; return; }
    uint32_t len = 0;
    const uint8_t *g = tt_glyf(f, gid, &len);
    if (!g || len < 10) return;                  /* empty glyph, e.g. space */
    int nc = (int16_t)rd16(g);
    if (nc >= 0) simple_glyph(g, len, nc, a, d, dx, dy);
    else         composite(f, g, len, a, d, dx, dy, depth);
}

/* --- scanline sweep ------------------------------------------------------ */

/* Walk the edge cell by cell, depositing the signed height it covers (cover)
 * and the area to the right of it within that cell (area). */
static void draw(int w, int h, Pt o, Pt gl) {
    float dx = gl.x - o.x, dy = gl.y - o.y;
    if (dy == 0) return;
    int sx = dx > 0 ? 1 : dx < 0 ? -1 : 0, sy = dy > 0 ? 1 : -1;
    float ix = sx ? fabsf(1.0f / dx) : 1.0f, iy = fabsf(1.0f / dy);
    int px, py, steps = 0;
    float nx, ny;

    if (!sx)          { px = (int)floorf(o.x); nx = 100.0f; }
    else if (sx > 0)  { px = (int)floorf(o.x); nx = ix - (o.x - px) * ix;
                        steps += (int)(ceilf(gl.x) - floorf(o.x)) - 1; }
    else              { px = (int)ceilf(o.x) - 1; nx = (o.x - px) * ix;
                        steps += (int)(ceilf(o.x) - floorf(gl.x)) - 1; }
    if (sy > 0)       { py = (int)floorf(o.y); ny = iy - (o.y - py) * iy;
                        steps += (int)(ceilf(gl.y) - floorf(o.y)) - 1; }
    else              { py = (int)ceilf(o.y) - 1; ny = (o.y - py) * iy;
                        steps += (int)(ceilf(o.y) - floorf(gl.y)) - 1; }

    float prev = 0, next = nx < ny ? nx : ny;
    float half = 0.5f * dx;
    if (steps < 0) steps = 0;
    for (int s = 0; s <= steps; s++) {
        float end = s == steps ? 1.0f : next;
        float xavg = o.x + (prev + end) * half;
        float ydif = (end - prev) * dy;
        /* Clamped, not trusted: the box comes from the same points, but a
         * malformed glyph must not write outside the cell buffer. */
        int cx = px < 0 ? 0 : px >= w ? w - 1 : px;
        int cy = py < 0 ? 0 : py >= h ? h - 1 : py;
        float *cell = cells + ((size_t)cy * w + cx) * 2;
        cell[1] += ydif;
        cell[0] += (1.0f - (xavg - (float)cx)) * ydif;
        if (s == steps) return;
        prev = next;
        if (nx < ny) { nx += ix; px += sx; } else { ny += iy; py += sy; }
        next = nx < ny ? nx : ny;
    }
}

/* --- entry point --------------------------------------------------------- */

int tt_render(const TtFont *f, uint16_t gid, int px, TtBitmap *bm) {
    memset(bm, 0, sizeof *bm);
    if (!f->upem || px < 1) return 0;
    n_seg = 0; g_bad = 0; pth_open = 0;
    g_scale = (float)px / (float)f->upem;
    cb_x0 = cb_y0 = 1e9f; cb_x1 = cb_y1 = -1e9f;
    if (f->glyf_len) emit(f, gid, 1, 1, 0, 0, 0);
    else if (!tt_cff_glyph(f, gid)) return 0;
    tt_path_close();
    if (g_bad) return 0;
    if (!n_seg) return 1;                        /* blank glyph: valid, no pixels */

    int bx = (int)floorf(cb_x0), by = (int)ceilf(cb_y1);
    int w = (int)ceilf(cb_x1) - bx, h = by - (int)floorf(cb_y0);
    if (w <= 0 || h <= 0) return 1;
    if (w > MAX_DIM || h > MAX_DIM) return 0;

    if (w * h * 2 > cap_cell) {
        float *c = realloc(cells, (size_t)w * h * 2 * sizeof *c);
        if (!c) return 0;
        cells = c; cap_cell = w * h * 2;
    }
    if (w * h > cap_img) {
        uint8_t *p = realloc(img, (size_t)w * h);
        if (!p) return 0;
        img = p; cap_img = w * h;
    }
    memset(cells, 0, (size_t)w * h * 2 * sizeof *cells);

    for (int i = 0; i < n_seg; i++) {
        Pt a = { segs[i].a.x - bx, by - segs[i].a.y };
        Pt b = { segs[i].b.x - bx, by - segs[i].b.y };
        draw(w, h, a, b);
    }

    /* Prefix-sum each row: cover accumulates the winding to the left, area is
     * the partial coverage of the cell the edge crosses. Per-row reset because
     * a closed contour's cover sums to zero within every row band. */
    for (int y = 0; y < h; y++) {
        float acc = 0;
        const float *c = cells + (size_t)y * w * 2;
        uint8_t *out = img + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            float v = fabsf(acc + c[x * 2]);
            if (v > 1.0f) v = 1.0f;
            out[x] = (uint8_t)(v * 255.0f + 0.5f);
            acc += c[x * 2 + 1];
        }
    }

    bm->w = w; bm->h = h; bm->bx = bx; bm->by = by; bm->px = img;
    return 1;
}

void tt_raster_release(void) {
    free(segs);  segs = NULL;  cap_seg = n_seg = 0;
    free(pts);   pts = NULL;
    free(ons);   ons = NULL;
    free(xs);    xs = NULL;    cap_pt = 0;
    free(cells); cells = NULL; cap_cell = 0;
    free(img);   img = NULL;   cap_img = 0;
}

/* Drawing primitives: alpha8 glyph blit, premultiplied rect fills, UTF-8.
 *
 * The buffer is premultiplied ARGB8888 (the WL_SHM wire format): every stored
 * pixel has rgb <= a. The blend formula (c*a + d*inv)/255 already premultiplies
 * the source by its coverage alpha on the fly, so it is correct as long as the
 * destination is premultiplied — which holds because every overwrite path below
 * runs its raw color through premul() first. */
#include "wisp.h"

#include <stdint.h>
#include <string.h>
#include <math.h>

/* Every primitive below takes LOGICAL geometry (including sw/sh — callers pass
 * w->w/w->h) and multiplies it by the output scale on entry; the buffer itself
 * is physical. Single-threaded, so one file-static is enough. At scale 1 the
 * multiplies fold away to the previous behaviour. */
static int cur_s120 = 120;

void render_set_scale(int s120) { cur_s120 = s120 < 120 ? 120 : s120; }

/* Logical -> physical. Rounded so a 1.5x edge lands on the same pixel the
 * neighbouring primitive computed for it. */
#define SC(v) (((v) * cur_s120 + 60) / 120)

/* floor(x/255) for x in [0, 0xffff]; exact, branchless. */
#define DIV255(x) (((x) + 1 + ((x) >> 8)) >> 8)

/* Premultiply a straight-alpha 0xAARRGGBB color. No-op for opaque colors. */
static inline uint32_t premul(uint32_t c) {
    uint32_t a = c >> 24;
    if (a == 0xff) return c;
    if (a == 0)    return 0;
    uint32_t r = DIV255(((c >> 16) & 0xff) * a);
    uint32_t g = DIV255(((c >> 8)  & 0xff) * a);
    uint32_t b = DIV255(( c        & 0xff) * a);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

void clear_buf(uint32_t *px, int w, int h, uint32_t c) {
    w = SC(w); h = SC(h);
    c = premul(c);
    int n = w * h, i = 0;
    /* 64-bit pair stores when aligned (shm pools are page-aligned, but a slot
     * with odd w*h can start 4-aligned) — ~2x over the scalar u32 loop at -Os. */
    if (((uintptr_t)px & 7) == 0) {
        uint64_t cc = ((uint64_t)c << 32) | c;
        for (; i + 2 <= n; i += 2) *(uint64_t *)(px + i) = cc;
    }
    for (; i < n; i++) px[i] = c;
}

/* src-over compositing of a straight (non-premultiplied) color modulated by
 * coverage alpha `a` onto one destination pixel. Matches the blend used by
 * fill_rect_rounded so all the AA primitives agree on edge color. */
static inline void blend_over(uint32_t *dst, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t a) {
    if (!a) return;
    if (a == 255) {
        *dst = 0xff000000u | ((uint32_t)cr << 16) | ((uint32_t)cg << 8) | cb;
        return;
    }
    uint32_t d = *dst;
    uint8_t dr = (d >> 16) & 0xff, dg = (d >> 8) & 0xff, db = d & 0xff;
    uint8_t da = (d >> 24) & 0xff;
    uint32_t inv = 255 - a;
    uint8_t or_ = DIV255(cr * a + dr * inv);
    uint8_t og  = DIV255(cg * a + dg * inv);
    uint8_t ob  = DIV255(cb * a + db * inv);
    uint8_t oa  = a + DIV255(da * inv);
    *dst = ((uint32_t)oa << 24) | ((uint32_t)or_ << 16) | ((uint32_t)og << 8) | ob;
}

/* Analytic 1px-wide anti-aliasing. `sd` is the signed distance (in pixels) from
 * the pixel CENTER to the filled region's boundary: negative inside, positive
 * outside. Coverage ramps linearly across the one-pixel band straddling the
 * edge — clamp(0.5 - sd). This resolves shallow-angle arc/edge transitions (the
 * tangent points where a corner arc runs nearly parallel to a straight edge)
 * that the old 2x2 supersampler quantised into chunky, non-monotone steps. One
 * sample per pixel, so it is also cheaper than the 4-sample grid it replaced. */
static inline double cov_from_sd(double sd) {
    double c = 0.5 - sd;
    return c < 0.0 ? 0.0 : (c > 1.0 ? 1.0 : c);
}

/* Signed distance from (px,py) to a rounded box centered at (cx,cy) with
 * half-extents (hx,hy) and per-corner radii. Each radius must already be
 * clamped to <= min(hx,hy). Negative inside the box, positive outside. */
static inline double sd_rbox(double px, double py, double cx, double cy,
                             double hx, double hy,
                             double r_tl, double r_tr, double r_br, double r_bl) {
    double qx = px - cx, qy = py - cy;
    double r = qx < 0.0 ? (qy < 0.0 ? r_tl : r_bl)
                        : (qy < 0.0 ? r_tr : r_br);
    double ax = fabs(qx) - (hx - r);
    double ay = fabs(qy) - (hy - r);
    double mx = ax > 0.0 ? ax : 0.0, my = ay > 0.0 ? ay : 0.0;
    double inner = ax > ay ? ax : ay; if (inner > 0.0) inner = 0.0;
    return sqrt(mx * mx + my * my) + inner - r;
}

/* Anti-aliased filled disc, analytic 1px AA, src-over composited.
 * Center may be fractional (knobs track sub-pixel positions). */
void fill_circle(uint32_t *px, int sw, int sh, double cx, double cy, double r, uint32_t c) {
    if (r <= 0) return;
    sw = SC(sw); sh = SC(sh);
    cx = SC(cx); cy = SC(cy); r = SC(r);
    uint8_t ca = (c >> 24) & 0xff;
    if (!ca) return;
    uint8_t cr = (c >> 16) & 0xff, cg = (c >> 8) & 0xff, cb = c & 0xff;
    int x0 = (int)(cx - r - 1); if (x0 < 0) x0 = 0;
    int y0 = (int)(cy - r - 1); if (y0 < 0) y0 = 0;
    int x1 = (int)(cx + r + 2); if (x1 > sw) x1 = sw;
    int y1 = (int)(cy + r + 2); if (y1 > sh) y1 = sh;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = px + j * sw;
        for (int i = x0; i < x1; i++) {
            double dx = (i + 0.5) - cx, dy = (j + 0.5) - cy;
            double cov = cov_from_sd(sqrt(dx * dx + dy * dy) - r);
            if (cov <= 0.0) continue;
            blend_over(&row[i], cr, cg, cb, (uint8_t)(ca * cov + 0.5));
        }
    }
}

/* Soft drop-shadow of a rounded rectangle, composed in one pass from the
 * signed distance to the (already offset+spread) rounded box. `blur` is the
 * softness band in px: alpha is full where d<=0 (inside the shape) and ramps
 * to 0 over `blur` px outside via a smoothstep. No temp buffer, no blur kernel
 * — cheap enough to run on every redraw while staying idle at 0 CPU. */
void fill_rounded_shadow(uint32_t *px, int sw, int sh,
                         int x, int y, int w, int h, int r, double blur, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    sw = SC(sw); sh = SC(sh);
    x = SC(x); y = SC(y); w = SC(w); h = SC(h);
    r = SC(r); blur = SC(blur);
    uint8_t ca = (c >> 24) & 0xff;
    if (!ca) return;
    uint8_t cr = (c >> 16) & 0xff, cg = (c >> 8) & 0xff, cb = c & 0xff;
    if (blur < 0.5) blur = 0.5;
    double bcx = x + w / 2.0, bcy = y + h / 2.0;
    double hx = w / 2.0, hy = h / 2.0;
    double rr = r; if (rr > hx) rr = hx; if (rr > hy) rr = hy; if (rr < 0) rr = 0;
    int x0 = (int)(x - blur - 1); if (x0 < 0) x0 = 0;
    int y0 = (int)(y - blur - 1); if (y0 < 0) y0 = 0;
    int x1 = (int)(x + w + blur + 2); if (x1 > sw) x1 = sw;
    int y1 = (int)(y + h + blur + 2); if (y1 > sh) y1 = sh;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = px + j * sw;
        double py = (j + 0.5) - bcy;
        double ay = fabs(py) - (hy - rr);
        for (int i = x0; i < x1; i++) {
            double pxx = (i + 0.5) - bcx;
            double ax = fabs(pxx) - (hx - rr);
            double mx = ax > 0 ? ax : 0, my = ay > 0 ? ay : 0;
            double inner = ax > ay ? ax : ay; if (inner > 0) inner = 0;
            double d = sqrt(mx * mx + my * my) + inner - rr;
            double t = 1.0 - d / blur;
            if (t <= 0) continue;
            if (t > 1) t = 1;
            t = t * t * (3.0 - 2.0 * t);     /* smoothstep */
            blend_over(&row[i], cr, cg, cb, (uint8_t)(ca * t + 0.5));
        }
    }
}

/* Anti-aliased track + filled portion + styled knob. value is 0..1; vertical
 * sliders fill bottom-up. Track ends and the filled portion round to
 * `st->track_radius` (negative → auto pill = half thickness). The knob is a
 * bar/pill/circle (or none), optionally bordered and shadowed. */
void draw_slider(uint32_t *px, int sw, int sh,
                 int x, int y, int w, int h,
                 int vertical, double value,
                 uint32_t track_bg, uint32_t track_fg,
                 const SliderStyle *st) {
    SliderStyle z = {0};
    if (!st) st = &z;
    if (value < 0) value = 0;
    if (value > 1) value = 1;
    if (w <= 0 || h <= 0) return;

    int thick = vertical ? w : h;
    int tr = st->track_radius;
    if (tr < 0) tr = thick / 2;          /* auto pill */
    if (tr > thick / 2) tr = thick / 2;
    if (tr < 0) tr = 0;

    int span = vertical ? h : w;
    int shape = st->thumb_shape;         /* 0 bar, 1 pill, 2 circle, 3 none */
    int ts = st->thumb_size;
    int has_knob = (ts > 0 && shape != 3);

    /* Inset the knob's travel by half its size so it stays fully inside the
     * track: at value 0/1 the knob's CENTER sits half-a-knob from the end, so
     * its leading EDGE is flush with the track end (the center can't reach the
     * end, which would push half the knob past it and look maxed-out early). */
    double half = has_knob ? ts / 2.0 : 0.0;
    double travel = (double)span - 2.0 * half;
    if (travel < 0) travel = 0;
    /* Main-axis position the value maps to. The knob's CENTER and the fill's
     * leading edge both sit here, so the opaque, ≥track-width knob always caps
     * the fill — the fill never pokes out past the narrowing part of a round
     * knob. With no knob the fill runs to the true value position. */
    double lead = vertical ? (y + half + (1.0 - value) * travel)
                           : (x + half + value * travel);
    double cx, cy;
    if (vertical) { cx = x + w / 2.0; cy = lead; }
    else          { cx = lead;        cy = y + h / 2.0; }

    if (track_bg & 0xff000000u)
        fill_rect_rounded(px, sw, sh, x, y, w, h, tr, tr, tr, tr, track_bg);

    if (track_fg & 0xff000000u) {
        int fx = x, fy = y, fw = w, fh = h;
        if (vertical) { int ly = (int)(lead + 0.5); fy = ly; fh = (y + h) - ly; }
        else          { fw = (int)(lead + 0.5) - x; }
        /* Flat leading edge, clipped to the rounded track: the capped end
         * matches the track corners and the leading edge rounds *progressively*
         * as it climbs into the cap, instead of snapping square→round only at
         * the final position. The knob (drawn next, on top) hides this edge. */
        if (fw > 0 && fh > 0)
            fill_rounded_clipped(px, sw, sh, fx, fy, fw, fh, 0, 0, 0, 0,
                                 x, y, w, h, tr, tr, tr, tr, track_fg);
    }

    if (!has_knob) return;

    uint32_t kc = st->thumb_color ? st->thumb_color : track_fg;
    uint32_t kb = st->thumb_border;
    int kbw = st->thumb_border_w > 0 ? st->thumb_border_w : 0;
    int shx = st->shadow_x, shy = st->shadow_y;
    double sblur = st->shadow_blur > 0 ? st->shadow_blur : 8.0;
    int shadow_on = (st->shadow_color & 0xff000000u) != 0;

    if (shape == 2) {                    /* circle knob — intentionally bulges
                                          * past the track, so drawn unclipped */
        double rad = ts / 2.0;
        if (shadow_on)
            fill_rounded_shadow(px, sw, sh, (int)(cx - rad) + shx, (int)(cy - rad) + shy,
                                ts, ts, (int)rad, sblur, st->shadow_color);
        if ((kb & 0xff000000u) && kbw > 0) {
            fill_circle(px, sw, sh, cx, cy, rad, kb);
            fill_circle(px, sw, sh, cx, cy, rad - kbw, kc);
        } else {
            fill_circle(px, sw, sh, cx, cy, rad, kc);
        }
        return;
    }

    /* bar (0) or pill (1): spans the full track thickness, so it is clipped to
     * the track's rounded rect — a flat-topped thumb reaching a rounded end has
     * its corners cut to follow the track outline instead of poking past it. */
    int tx, ty, tw2, th2;
    if (vertical) { tx = x; tw2 = w; ty = (int)(cy - ts / 2.0); th2 = ts; }
    else          { ty = y; th2 = h; tx = (int)(cx - ts / 2.0); tw2 = ts; }
    int rr = 0;
    if (shape == 1) {                    /* pill: round the short axis */
        rr = st->thumb_radius > 0 ? st->thumb_radius : (vertical ? th2 / 2 : tw2 / 2);
    }
    if (shadow_on)
        fill_rounded_shadow(px, sw, sh, tx + shx, ty + shy, tw2, th2, rr, sblur, st->shadow_color);
    if ((kb & 0xff000000u) && kbw > 0) {
        fill_rounded_clipped(px, sw, sh, tx, ty, tw2, th2, rr, rr, rr, rr,
                             x, y, w, h, tr, tr, tr, tr, kb);
        int irr = rr - kbw; if (irr < 0) irr = 0;
        fill_rounded_clipped(px, sw, sh, tx + kbw, ty + kbw, tw2 - 2 * kbw, th2 - 2 * kbw,
                             irr, irr, irr, irr, x, y, w, h, tr, tr, tr, tr, kc);
    } else {
        fill_rounded_clipped(px, sw, sh, tx, ty, tw2, th2, rr, rr, rr, rr,
                             x, y, w, h, tr, tr, tr, tr, kc);
    }
}

/* Physical-coordinate core; the public entry points scale into it. */
static void fill_rect_px(uint32_t *px, int sw, int sh, int x, int y, int w, int h, uint32_t c) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > sw ? sw : x + w;
    int y1 = y + h > sh ? sh : y + h;
    if (x0 >= x1 || y0 >= y1) return;
    c = premul(c);   /* overwrite must store a valid premultiplied pixel */
    for (int j = y0; j < y1; j++) {
        uint32_t *row = px + j * sw;
        for (int i = x0; i < x1; i++) row[i] = c;
    }
}

void fill_rect(uint32_t *px, int sw, int sh, int x, int y, int w, int h, uint32_t c) {
    fill_rect_px(px, SC(sw), SC(sh), SC(x), SC(y), SC(w), SC(h), c);
}

void fill_rect_rounded(uint32_t *px, int sw, int sh,
                       int x, int y, int w, int h,
                       int r_tl, int r_tr, int r_br, int r_bl,
                       uint32_t c) {
    if (w <= 0 || h <= 0) return;
    sw = SC(sw); sh = SC(sh);
    x = SC(x); y = SC(y); w = SC(w); h = SC(h);
    r_tl = SC(r_tl); r_tr = SC(r_tr); r_br = SC(r_br); r_bl = SC(r_bl);
    if (r_tl == 0 && r_tr == 0 && r_br == 0 && r_bl == 0) {
        fill_rect_px(px, sw, sh, x, y, w, h, c);
        return;
    }
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > sw ? sw : x + w;
    int y1 = y + h > sh ? sh : y + h;
    if (x0 >= x1 || y0 >= y1) return;

    /* Clamp radii to half-extent. */
    int rmax_w = w / 2, rmax_h = h / 2;
    int rm = rmax_w < rmax_h ? rmax_w : rmax_h;
    if (r_tl > rm) r_tl = rm;
    if (r_tr > rm) r_tr = rm;
    if (r_br > rm) r_br = rm;
    if (r_bl > rm) r_bl = rm;

    uint8_t cr = (c >> 16) & 0xff, cg = (c >> 8) & 0xff, cb = c & 0xff;
    uint8_t ca = (c >> 24) & 0xff;

    for (int j = y0; j < y1; j++) {
        int ly = j - y;          /* local row within rect */
        uint32_t *row = px + j * sw;
        for (int i = x0; i < x1; i++) {
            int lx = i - x;      /* local col within rect */
            /* Pick the corner this pixel belongs to (if any). */
            int r = 0; double ccx = 0, ccy = 0;
            if      (lx < r_tl && ly < r_tl)              { r = r_tl; ccx = x + r_tl;     ccy = y + r_tl; }
            else if (lx >= w - r_tr && ly < r_tr)         { r = r_tr; ccx = x + w - r_tr; ccy = y + r_tr; }
            else if (lx >= w - r_br && ly >= h - r_br)    { r = r_br; ccx = x + w - r_br; ccy = y + h - r_br; }
            else if (lx < r_bl && ly >= h - r_bl)         { r = r_bl; ccx = x + r_bl;     ccy = y + h - r_bl; }

            uint8_t a;
            if (r == 0) {
                a = ca;
            } else {
                /* Inside the corner square the rounded-rect boundary IS the
                 * corner disc; analytic distance to its center gives smooth AA
                 * right up to the tangent where the arc meets the straight edge
                 * (cov→1 there, matching the flat region just outside). */
                double dx = (i + 0.5) - ccx, dy = (j + 0.5) - ccy;
                double cov = cov_from_sd(sqrt(dx * dx + dy * dy) - r);
                if (cov <= 0.0) continue;
                a = (uint8_t)(ca * cov + 0.5);
            }
            if (!a) continue;
            /* Premultiplied src-over: out = src*a + dst*(1-a). (cr*a) premuls
             * the source on the fly; dst is already premultiplied. a==255 only
             * occurs when ca==255 (opaque), so the overwrite stores a valid
             * premultiplied pixel. */
            uint32_t d = row[i];
            uint8_t dr = (d >> 16) & 0xff, dg = (d >> 8) & 0xff, db = d & 0xff;
            uint8_t da = (d >> 24) & 0xff;
            if (a == 255) {
                row[i] = c;
            } else {
                uint32_t inv = 255 - a;
                uint8_t or_ = DIV255(cr * a + dr * inv);
                uint8_t og  = DIV255(cg * a + dg * inv);
                uint8_t ob  = DIV255(cb * a + db * inv);
                uint8_t oa  = a + DIV255(da * inv);
                row[i] = ((uint32_t)oa << 24) | ((uint32_t)or_ << 16)
                       | ((uint32_t)og << 8)  | ob;
            }
        }
    }
}

/* Signed distance from (px,py) to the rounded rect (x,y,w,h, per-corner radii),
 * clamping each radius to the half-extent. Negative inside, positive outside. */
static inline double sd_rrect(double px, double py, int x, int y, int w, int h,
                              int r_tl, int r_tr, int r_br, int r_bl) {
    double hx = w / 2.0, hy = h / 2.0;
    double rm = hx < hy ? hx : hy;
    double rtl = r_tl > rm ? rm : r_tl;
    double rtr = r_tr > rm ? rm : r_tr;
    double rbr = r_br > rm ? rm : r_br;
    double rbl = r_bl > rm ? rm : r_bl;
    return sd_rbox(px, py, x + hx, y + hy, hx, hy, rtl, rtr, rbr, rbl);
}

/* Conservative test: is the whole pixel box [i,i+1]×[j,j+1] strictly inside the
 * rounded rect and clear of every corner's r×r square? If so the pixel is fully
 * covered, so we can skip the per-pixel SDF/sqrt work. Returns 0 near
 * edges/corners (those still take the analytic path). */
static inline int box_inside_rounded(int i, int j, int x, int y, int w, int h,
                                     int r_tl, int r_tr, int r_br, int r_bl) {
    if (i < x || j < y || i + 1 > x + w || j + 1 > y + h) return 0;
    if (i <  x + r_tl       && j <  y + r_tl)       return 0;  /* TL square */
    if (i + 1 > x + w - r_tr && j <  y + r_tr)       return 0;  /* TR square */
    if (i + 1 > x + w - r_br && j + 1 > y + h - r_br) return 0;  /* BR square */
    if (i <  x + r_bl       && j + 1 > y + h - r_bl) return 0;  /* BL square */
    return 1;
}

void fill_rounded_clipped(uint32_t *px, int sw, int sh,
                          int x, int y, int w, int h,
                          int r_tl, int r_tr, int r_br, int r_bl,
                          int cx, int cy, int cw, int ch,
                          int cr_tl, int cr_tr, int cr_br, int cr_bl, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    sw = SC(sw); sh = SC(sh);
    x = SC(x); y = SC(y); w = SC(w); h = SC(h);
    r_tl = SC(r_tl); r_tr = SC(r_tr); r_br = SC(r_br); r_bl = SC(r_bl);
    cx = SC(cx); cy = SC(cy); cw = SC(cw); ch = SC(ch);
    cr_tl = SC(cr_tl); cr_tr = SC(cr_tr); cr_br = SC(cr_br); cr_bl = SC(cr_bl);
    uint8_t ca = (c >> 24) & 0xff;
    if (!ca) return;
    uint8_t cr = (c >> 16) & 0xff, cg = (c >> 8) & 0xff, cb = c & 0xff;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > sw ? sw : x + w;
    int y1 = y + h > sh ? sh : y + h;
    if (x0 >= x1 || y0 >= y1) return;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = px + j * sw;
        for (int i = x0; i < x1; i++) {
            /* Interior fast-path: clear of both rects' corners → full coverage,
             * no SDF/sqrt work. Covers the ~95% flat body. */
            if (box_inside_rounded(i, j, x, y, w, h, r_tl, r_tr, r_br, r_bl) &&
                box_inside_rounded(i, j, cx, cy, cw, ch, cr_tl, cr_tr, cr_br, cr_bl)) {
                blend_over(&row[i], cr, cg, cb, ca);
                continue;
            }
            /* Intersection of two rounded rects: signed distance is the max of
             * the two. Analytic 1px AA on the combined boundary. */
            double pxc = i + 0.5, pyc = j + 0.5;
            double sd = sd_rrect(pxc, pyc, x, y, w, h, r_tl, r_tr, r_br, r_bl);
            double sdc = sd_rrect(pxc, pyc, cx, cy, cw, ch, cr_tl, cr_tr, cr_br, cr_bl);
            if (sdc > sd) sd = sdc;
            double cov = cov_from_sd(sd);
            if (cov <= 0.0) continue;
            blend_over(&row[i], cr, cg, cb, (uint8_t)(ca * cov + 0.5));
        }
    }
}

void fill_rect_rounded_border(uint32_t *px, int sw, int sh,
                              int x, int y, int w, int h,
                              int r_tl, int r_tr, int r_br, int r_bl,
                              int bw, int side_t, int side_r, int side_b, int side_l,
                              int clip_top, uint32_t c) {
    if (bw <= 0 || w <= 0 || h <= 0) return;
    sw = SC(sw); sh = SC(sh);
    x = SC(x); y = SC(y); w = SC(w); h = SC(h);
    r_tl = SC(r_tl); r_tr = SC(r_tr); r_br = SC(r_br); r_bl = SC(r_bl);
    bw = SC(bw); clip_top = SC(clip_top);
    int rmax_w = w / 2, rmax_h = h / 2;
    int rm = rmax_w < rmax_h ? rmax_w : rmax_h;
    if (r_tl > rm) r_tl = rm;
    if (r_tr > rm) r_tr = rm;
    if (r_br > rm) r_br = rm;
    if (r_bl > rm) r_bl = rm;

    int ix = x + bw, iy = y + bw, iw = w - 2*bw, ih = h - 2*bw;
    int ir_tl = r_tl - bw > 0 ? r_tl - bw : 0;
    int ir_tr = r_tr - bw > 0 ? r_tr - bw : 0;
    int ir_br = r_br - bw > 0 ? r_br - bw : 0;
    int ir_bl = r_bl - bw > 0 ? r_bl - bw : 0;

    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > sw ? sw : x + w;
    int y1 = y + h > sh ? sh : y + h;
    if (y0 < clip_top) y0 = clip_top;

    uint8_t cr = (c >> 16) & 0xff, cg = (c >> 8) & 0xff, cb = c & 0xff;
    uint8_t ca = (c >> 24) & 0xff;
    if (!ca) return;

    for (int j = y0; j < y1; j++) {
        uint32_t *row = px + j * sw;
        for (int i = x0; i < x1; i++) {
            /* The border ring is (outer rrect) \ (inner rrect). Analytic AA on
             * both arcs: ring coverage = cov(outer) - cov(inner) (the inner and
             * outer 1px bands sit bw≥1 px apart, so they never overlap). */
            double sx = i + 0.5, sy = j + 0.5;
            double cov = cov_from_sd(sd_rrect(sx, sy, x, y, w, h,
                                              r_tl, r_tr, r_br, r_bl));
            if (cov <= 0.0) continue;
            if (iw > 0 && ih > 0)
                cov -= cov_from_sd(sd_rrect(sx, sy, ix, iy, iw, ih,
                                            ir_tl, ir_tr, ir_br, ir_bl));
            if (cov <= 0.0) continue;
            /* Per-side suppression: a pixel inside a rounded-corner wedge
             * belongs to that corner — enable if either adjacent side is on.
             * Pixels in straight edge strips enable only via their own side. */
            int in_tl = (r_tl > 0 && sx < x + r_tl && sy < y + r_tl);
            int in_tr = (r_tr > 0 && sx >= x + w - r_tr && sy < y + r_tr);
            int in_br = (r_br > 0 && sx >= x + w - r_br && sy >= y + h - r_br);
            int in_bl = (r_bl > 0 && sx < x + r_bl && sy >= y + h - r_bl);
            int ok;
            if      (in_tl) ok = side_t || side_l;
            else if (in_tr) ok = side_t || side_r;
            else if (in_br) ok = side_b || side_r;
            else if (in_bl) ok = side_b || side_l;
            else {
                int near_t = sy < y + bw;
                int near_b = sy >= y + h - bw;
                int near_l = sx < x + bw;
                int near_r = sx >= x + w - bw;
                ok = (near_t && side_t) || (near_b && side_b)
                   || (near_l && side_l) || (near_r && side_r);
            }
            if (!ok) continue;
            uint8_t a = (uint8_t)(ca * cov + 0.5);
            if (!a) continue;
            uint32_t d = row[i];
            uint8_t dr = (d >> 16) & 0xff, dg = (d >> 8) & 0xff, db = d & 0xff;
            uint8_t da = (d >> 24) & 0xff;
            if (a == 255) { row[i] = c; continue; }
            uint32_t inv = 255 - a;
            uint8_t or_ = DIV255(cr * a + dr * inv);
            uint8_t og  = DIV255(cg * a + dg * inv);
            uint8_t ob  = DIV255(cb * a + db * inv);
            uint8_t oa  = a + DIV255(da * inv);
            row[i] = ((uint32_t)oa << 24) | ((uint32_t)or_ << 16)
                   | ((uint32_t)og << 8)  | ob;
        }
    }
}

void fill_corner_fillet(uint32_t *px, int sw, int sh,
                        int x_corner, int y_corner, int r, int corner_id,
                        uint32_t bg) {
    if (r <= 0) return;
    sw = SC(sw); sh = SC(sh);
    x_corner = SC(x_corner); y_corner = SC(y_corner); r = SC(r);
    /* Armpit bound depends on which corner. Disc center sits at the outer
     * corner of the armpit (opposite the surface body). Pixels outside the
     * disc (d² ≥ r²) get bg → inner wedge filled, outer wedge transparent. */
    int xlo, xhi, ylo, yhi;
    double cx, cy;
    switch (corner_id) {
    case 0: /* tl */
        xlo = x_corner - r; xhi = x_corner;
        ylo = y_corner;     yhi = y_corner + r;
        cx = x_corner - r;  cy = y_corner + r;
        break;
    case 1: /* tr */
        xlo = x_corner;     xhi = x_corner + r;
        ylo = y_corner;     yhi = y_corner + r;
        cx = x_corner + r;  cy = y_corner + r;
        break;
    case 2: /* br */
        xlo = x_corner;     xhi = x_corner + r;
        ylo = y_corner - r; yhi = y_corner;
        cx = x_corner + r;  cy = y_corner - r;
        break;
    case 3: /* bl */
        xlo = x_corner - r; xhi = x_corner;
        ylo = y_corner - r; yhi = y_corner;
        cx = x_corner - r;  cy = y_corner - r;
        break;
    default: return;
    }
    if (xlo < 0) xlo = 0;
    if (ylo < 0) ylo = 0;
    if (xhi > sw) xhi = sw;
    if (yhi > sh) yhi = sh;
    uint8_t bgr = (bg >> 16) & 0xff, bgg = (bg >> 8) & 0xff, bgb = bg & 0xff;
    uint8_t bga = (bg >> 24) & 0xff;
    if (!bga) return;

    for (int j = ylo; j < yhi; j++) {
        uint32_t *row = px + j * sw;
        for (int i = xlo; i < xhi; i++) {
            /* Filled region is OUTSIDE the disc, so sd = r - dist (positive when
             * inside the disc). Analytic 1px AA → smooth coverage straight into
             * the tangent where the arc meets the surface body. */
            double dx = (i + 0.5) - cx, dy = (j + 0.5) - cy;
            double cov = cov_from_sd(r - sqrt(dx * dx + dy * dy));
            if (cov <= 0.0) continue;
            /* Overwrite rather than composite: the fillet's job is to extend
             * the surface body into the armpit; sub-pixel coverage modulates
             * alpha but the underlying buffer here is always cleared/transparent
             * so there's nothing to blend with. Using overwrite keeps the
             * color "still" (no double-darkening when it visually meets the
             * neighbouring surface). */
            uint8_t a = (uint8_t)(bga * cov + 0.5);
            if (a == 255) {
                row[i] = bg;   /* a==255 ⟹ bga==255 ⟹ bg opaque */
            } else {
                /* Premultiply the body color by the coverage alpha. */
                row[i] = ((uint32_t)a << 24) | (DIV255(bgr * a) << 16)
                       | (DIV255(bgg * a) << 8)  | DIV255(bgb * a);
            }
        }
    }
}

/* Border band hugging a corner fillet's arc. Same bbox/center geometry as
 * fill_corner_fillet, which fills dist >= r; this paints the annulus
 * r <= dist <= r+bw, i.e. the wedge's inner edge. That arc is the wedge's ONLY
 * exposed outline — its other two sides meet the surface body (tangentially,
 * so the band lines up with the body's straight side border) and the anchored
 * screen edge. Composites, unlike the fill: a translucent border must blend
 * over the fillet body already laid down underneath.
 *
 * The bbox is the fill's widened by bw on the body-facing side: the annulus is
 * OUTSIDE the r disc, so at the tangent it lies past x_corner, and the fill's
 * bbox would clip off precisely the stretch that has to meet the straight side
 * border. yhi stays at the tangent row — the band curves past it, but that is
 * where the straight border takes over, and double-painting a translucent
 * border there would darken the seam. */
void fill_corner_fillet_border(uint32_t *px, int sw, int sh,
                               int x_corner, int y_corner, int r, int corner_id,
                               int bw, uint32_t c) {
    if (r <= 0 || bw <= 0) return;
    sw = SC(sw); sh = SC(sh);
    x_corner = SC(x_corner); y_corner = SC(y_corner); r = SC(r); bw = SC(bw);
    int xlo, xhi, ylo, yhi;
    double cx, cy;
    switch (corner_id) {
    case 0: /* tl */
        xlo = x_corner - r; xhi = x_corner + bw;
        ylo = y_corner;     yhi = y_corner + r;
        cx = x_corner - r;  cy = y_corner + r;
        break;
    case 1: /* tr */
        xlo = x_corner - bw; xhi = x_corner + r;
        ylo = y_corner;      yhi = y_corner + r;
        cx = x_corner + r;   cy = y_corner + r;
        break;
    case 2: /* br */
        xlo = x_corner - bw; xhi = x_corner + r;
        ylo = y_corner - r;  yhi = y_corner;
        cx = x_corner + r;   cy = y_corner - r;
        break;
    case 3: /* bl */
        xlo = x_corner - r; xhi = x_corner + bw;
        ylo = y_corner - r; yhi = y_corner;
        cx = x_corner - r;  cy = y_corner - r;
        break;
    default: return;
    }
    if (xlo < 0) xlo = 0;
    if (ylo < 0) ylo = 0;
    if (xhi > sw) xhi = sw;
    if (yhi > sh) yhi = sh;
    uint8_t ca = (c >> 24) & 0xff;
    if (!ca) return;
    uint8_t cr = (c >> 16) & 0xff, cg = (c >> 8) & 0xff, cb = c & 0xff;
    for (int j = ylo; j < yhi; j++) {
        uint32_t *row = px + j * sw;
        for (int i = xlo; i < xhi; i++) {
            double dx = (i + 0.5) - cx, dy = (j + 0.5) - cy;
            double d = sqrt(dx * dx + dy * dy);
            /* Intersection of two half-spaces: outside the r disc AND inside
             * the r+bw one. Both use the negative-inside convention. */
            double cov = cov_from_sd(r - d);
            double cov_o = cov_from_sd(d - (r + bw));
            if (cov_o < cov) cov = cov_o;
            if (cov <= 0.0) continue;
            /* Taper out where the arc turns parallel to the anchored edge it
             * springs from (always horizontal: a fillet flares off the top or
             * bottom). A constant perpendicular width smears sideways there —
             * 2px at 15° covers ~7px of the row — so the tip row would show a
             * fat stub with bare bg either side, reading as a dash floating off
             * the corner rather than an outline running off the edge. The arc's
             * sine against that edge is |dx|/d; ramp to full by 45° so only the
             * last few rows fade and the body join stays at full strength. */
            double sn = (dx < 0 ? -dx : dx) / d;
            if (sn < 0.7) cov *= sn / 0.7;
            if (cov <= 0.0) continue;
            blend_over(&row[i], cr, cg, cb, (uint8_t)(ca * cov + 0.5));
        }
    }
}

/* Punch a quarter-disc shaped transparent hole. corner_id selects the
 * "inside" wedge: 0=tl (wedge extends down+right from cx,cy), 1=tr (down+left),
 * 2=br (up+left), 3=bl (up+right). Pixels inside the wedge bounding box but
 * outside the disc are kept; pixels inside the disc are erased (so the result
 * matches a concave fillet smoothing the inner corner). 2x supersampled at
 * the rim for AA. */
void punch_inner_corner(uint32_t *px, int sw, int sh,
                        int cx, int cy, int r, int corner_id) {
    if (r <= 0) return;
    sw = SC(sw); sh = SC(sh);
    cx = SC(cx); cy = SC(cy); r = SC(r);
    int bx0, by0, bx1, by1;
    switch (corner_id) {
    case 0: bx0 = cx;     by0 = cy;     bx1 = cx + r; by1 = cy + r; break;
    case 1: bx0 = cx - r; by0 = cy;     bx1 = cx;     by1 = cy + r; break;
    case 2: bx0 = cx - r; by0 = cy - r; bx1 = cx;     by1 = cy;     break;
    default:bx0 = cx;     by0 = cy - r; bx1 = cx + r; by1 = cy;     break;
    }
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > sw) bx1 = sw;
    if (by1 > sh) by1 = sh;
    for (int j = by0; j < by1; j++) {
        uint32_t *row = px + j * sw;
        for (int i = bx0; i < bx1; i++) {
            /* Erase INSIDE the disc: sd = dist - r, cov = fraction erased.
             * Analytic 1px AA on the rim. */
            double dx = (i + 0.5) - cx, dy = (j + 0.5) - cy;
            double cov = cov_from_sd(sqrt(dx * dx + dy * dy) - r);
            if (cov <= 0.0) continue;
            if (cov >= 1.0) { row[i] = 0; continue; }
            /* Partial coverage: blend the destination toward 0 (transparent). */
            uint32_t d = row[i];
            uint32_t inv = (uint32_t)((1.0 - cov) * 255.0 + 0.5);
            uint8_t dr = (d >> 16) & 0xff, dg = (d >> 8) & 0xff, db = d & 0xff;
            uint8_t da = (d >> 24) & 0xff;
            row[i] = (DIV255(da * inv) << 24)
                   | (DIV255(dr * inv) << 16)
                   | (DIV255(dg * inv) << 8)
                   |  DIV255(db * inv);
        }
    }
}

/* Fill a concave fillet at an L's inner corner. Same bbox semantics as
 * punch_inner_corner (cx,cy is the disc center; corner_id picks which quadrant
 * holds the bbox), but fills `color` into pixels OUTSIDE the disc — producing
 * a rounded bulge from the corner point at the bbox's near vertex into the
 * empty notch. 2x supersampled at the rim. Overwrites (no blending). */
void fill_inner_fillet(uint32_t *px, int sw, int sh,
                       int cx, int cy, int r, int corner_id, uint32_t color) {
    if (r <= 0) return;
    sw = SC(sw); sh = SC(sh);
    cx = SC(cx); cy = SC(cy); r = SC(r);
    int bx0, by0, bx1, by1;
    switch (corner_id) {
    case 0: bx0 = cx;     by0 = cy;     bx1 = cx + r; by1 = cy + r; break;
    case 1: bx0 = cx - r; by0 = cy;     bx1 = cx;     by1 = cy + r; break;
    case 2: bx0 = cx - r; by0 = cy - r; bx1 = cx;     by1 = cy;     break;
    default:bx0 = cx;     by0 = cy - r; bx1 = cx + r; by1 = cy;     break;
    }
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > sw) bx1 = sw;
    if (by1 > sh) by1 = sh;
    uint32_t pc = premul(color);   /* store premultiplied; scale by coverage */
    uint8_t ca = (pc >> 24) & 0xff;
    uint8_t cr = (pc >> 16) & 0xff;
    uint8_t cg = (pc >> 8) & 0xff;
    uint8_t cb =  pc        & 0xff;
    for (int j = by0; j < by1; j++) {
        uint32_t *row = px + j * sw;
        for (int i = bx0; i < bx1; i++) {
            /* Filled region is OUTSIDE the disc → sd = r - dist. */
            double dx = (i + 0.5) - cx, dy = (j + 0.5) - cy;
            double cov = cov_from_sd(r - sqrt(dx * dx + dy * dy));
            if (cov <= 0.0) continue;
            if (cov >= 1.0) { row[i] = pc; continue; }
            uint32_t cv = (uint32_t)(cov * 255.0 + 0.5);
            row[i] = (DIV255(ca * cv) << 24)
                   | (DIV255(cr * cv) << 16)
                   | (DIV255(cg * cv) << 8)
                   |  DIV255(cb * cv);
        }
    }
}

/* Decode one UTF-8 codepoint. Returns bytes consumed (0 if invalid). */
int utf8_decode(const char *s, uint32_t *cp) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return c ? 1 : 0; }
    if ((c & 0xe0) == 0xc0) {
        if ((s[1] & 0xc0) != 0x80) return 0;
        *cp = ((c & 0x1f) << 6) | (s[1] & 0x3f);
        return 2;
    }
    if ((c & 0xf0) == 0xe0) {
        if ((s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80) return 0;
        *cp = ((c & 0x0f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f);
        return 3;
    }
    if ((c & 0xf8) == 0xf0) {
        if ((s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80 || (s[3] & 0xc0) != 0x80)
            return 0;
        *cp = ((c & 0x07) << 18) | ((s[1] & 0x3f) << 12) |
              ((s[2] & 0x3f) << 6) | (s[3] & 0x3f);
        return 4;
    }
    return 0;
}

const Glyph *font_find(const Font *f, uint32_t cp) {
#ifdef WISP_FONT_FREETYPE
    /* freetype backend: dynamic, unsorted cache + rasterize-on-miss. */
    return font_ft_find(f, cp);
#else
    /* baked/bitmap backend: const, codepoint-sorted table → binary search. */
    int lo = 0, hi = f->n - 1;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        if (f->g[m].cp == cp) return &f->g[m];
        if (f->g[m].cp < cp) lo = m + 1; else hi = m - 1;
    }
    return NULL;
#endif
}

int text_width(const Font *f, const char *s) {
    int w = 0;
    while (*s) {
        uint32_t cp;
        int n = utf8_decode(s, &cp);
        if (!n) { s++; continue; }
        s += n;
        const Glyph *g = font_find(f, cp);
        if (g) w += g->adv;
        else   w += f->px_size / 2;
    }
    return w;
}

/* Alpha-blend a single glyph's alpha8 bitmap onto a premultiplied-ARGB target.
 * For simplicity we treat the destination as already premultiplied-or-opaque
 * background and blend FG color modulated by glyph alpha as src-over. */
/* `m` replicates each source pixel into an m x m block — the fallback when the
 * strike is native-size on a scaled output (baked/bitmap backends). The source
 * index is carried in a counter rather than an i/m divide: m == 1 is the whole
 * scale-1 world and must not grow an idiv in the per-pixel loop. */
static void draw_glyph_px(uint32_t *px, int sw, int sh, int x, int y,
                          const Font *f, const Glyph *g, uint32_t fg, int m) {
    if (!g || g->w <= 0 || g->h <= 0) return;
    int gx = x + g->bx * m;
    int gy = y - g->by * m;     /* y is baseline; bitmap top is baseline - by */
    const uint8_t *src = f->px + g->px_off;

    if (g->color) {
        /* Premultiplied BGRA (color-bitmap emoji); drawn as-is, fg ignored.
         * src-over onto the ARGB target: out = src + dst*(255-a)/255. */
        for (int j = 0; j < g->h; j++)
        for (int i = 0; i < g->w; i++) {
            const uint8_t *p = src + ((size_t)j * g->w + i) * 4;
            uint32_t a = p[3];
            if (!a) continue;
            uint32_t inv = 255 - a;
            for (int by = 0; by < m; by++) {
                int yy = gy + j * m + by;
                if (yy < 0 || yy >= sh) continue;
                for (int bx = 0; bx < m; bx++) {
                    int xx = gx + i * m + bx;
                    if (xx < 0 || xx >= sw) continue;
                    uint32_t d = px[yy * sw + xx];
                    uint32_t dr = (d >> 16) & 0xff, dg = (d >> 8) & 0xff, db = d & 0xff;
                    uint32_t da = (d >> 24) & 0xff;
                    /* Both src (premul BGRA emoji) and dst are premultiplied, so
                     * each channel stays <= 255 — no clamp needed. */
                    uint32_t or_ = p[2] + DIV255(dr * inv);
                    uint32_t og  = p[1] + DIV255(dg * inv);
                    uint32_t ob  = p[0] + DIV255(db * inv);
                    uint32_t oa  = a + DIV255(da * inv);
                    px[yy * sw + xx] = (oa << 24) | (or_ << 16) | (og << 8) | ob;
                }
            }
        }
        return;
    }

    uint8_t fr = (fg >> 16) & 0xff, fg_g = (fg >> 8) & 0xff, fb = fg & 0xff;
    uint8_t fa = (fg >> 24) & 0xff;
    for (int j = 0; j < g->h; j++)
    for (int i = 0; i < g->w; i++) {
        uint8_t a = src[j * g->w + i];
        if (!a) continue;
        uint32_t na = DIV255(a * fa);
        uint32_t inv = 255 - na;
        for (int by = 0; by < m; by++) {
            int yy = gy + j * m + by;
            if (yy < 0 || yy >= sh) continue;
            for (int bx = 0; bx < m; bx++) {
                int xx = gx + i * m + bx;
                if (xx < 0 || xx >= sw) continue;
                uint32_t d = px[yy * sw + xx];
                uint8_t dr = (d >> 16) & 0xff, dg = (d >> 8) & 0xff, db = d & 0xff;
                uint8_t da = (d >> 24) & 0xff;
                uint8_t or_ = DIV255(fr * na + dr * inv);
                uint8_t og  = DIV255(fg_g * na + dg * inv);
                uint8_t ob  = DIV255(fb * na + db * inv);
                uint8_t oa  = na + DIV255(da * inv);
                px[yy * sw + xx] = ((uint32_t)oa << 24) | ((uint32_t)or_ << 16)
                                 | ((uint32_t)og << 8)  | ob;
            }
        }
    }
}

/* Pick the strike to blit and how much to replicate it: the freetype backend
 * can rasterize a real scale*px_size twin, the const-table backends can only
 * pixel-double. Returns the font to read glyphs from; *m is the replication. */
/* Fallback replication for the const-table backends: nearest whole factor. */
#define REPL(s120) ((s120) < 180 ? 1 : ((s120) + 60) / 120)
static const Font *strike_for(const Font *f, int s120, int *m) {
#ifdef WISP_FONT_FREETYPE
    const Font *sf = font_ft_at_scale(f, s120);
    if (sf != f) { *m = 1; return sf; }
#endif
    *m = REPL(s120);
    return f;
}

void draw_glyph(uint32_t *px, int sw, int sh, int x, int y,
                const Font *f, const Glyph *g, uint32_t fg) {
    if (!g) return;
    int s = cur_s120, m;
    const Font *sf = strike_for(f, s, &m);
    if (sf != f) { const Glyph *sg = font_find(sf, g->cp); if (sg) { f = sf; g = sg; } else m = REPL(s); }
    draw_glyph_px(px, SC(sw), SC(sh), SC(x), SC(y), f, g, fg, m);
}

/* text_width() and the pen advances stay LOGICAL (advance * scale) so the
 * generated centering math and the measured width can't drift apart; only the
 * glyph bitmap comes from the physical strike. */
void draw_text(uint32_t *px, int sw, int sh, int x, int y,
               const Font *f, const char *s, uint32_t fg) {
    int sc = cur_s120, m;
    const Font *sf = strike_for(f, sc, &m);
    sw = SC(sw); sh = SC(sh);
    int pen_x = SC(x);
    int baseline = SC(y) + (sf == f ? SC(f->baseline) : sf->baseline);
    while (*s) {
        uint32_t cp;
        int n = utf8_decode(s, &cp);
        if (!n) { s++; continue; }
        s += n;
        const Glyph *g = font_find(f, cp);
        if (g) {
            /* Miss in the scaled strike (font lost a glyph the native one has)
             * falls back to pixel-doubling the native one. */
            const Glyph *sg = sf == f ? NULL : font_find(sf, cp);
            draw_glyph_px(px, sw, sh, pen_x, baseline, sg ? sf : f,
                          sg ? sg : g, fg, sg ? 1 : m);
            pen_x += SC(g->adv);
        } else {
            pen_x += SC(f->px_size / 2);
        }
    }
}


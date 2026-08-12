/* Per-pixel math shared by the two render TUs: coordinate scaling, alpha
 * premultiplication and the analytic-coverage blend every AA primitive must
 * agree on, or adjacent primitives disagree on their shared edge color. */
#ifndef WISP_RENDER_PX_H
#define WISP_RENDER_PX_H

#include <stdint.h>
#include <math.h>

/* Set by render_set_scale(); single-threaded, so one global is enough. */
extern int render_s120;

/* Logical -> physical. Rounded so a 1.5x edge lands on the same pixel the
 * neighbouring primitive computed for it. Must FLOOR, not truncate: C division
 * rounds toward zero, which shifts every negative coordinate a pixel down —
 * a surface sliding out past its buffer top (OSD retract) then paints one row
 * below the extent its caller computed, outside any dirty band, leaving a
 * one-pixel trail of its bottom border per frame. */
static inline int SC(int v) {
    int n = v * render_s120 + 60;
    return n >= 0 ? n / 120 : -((-n + 119) / 120);
}

/* Physical row band every content primitive is confined to, [y0, y1). Wide open
 * by default, narrowed by render_set_clip() around a scrolled container's draw
 * pass so rows that slid past the container's edge don't paint over the rest of
 * the surface. Only the content primitives honour it (rects, rounded rects,
 * borders, glyphs, argb blits) — surface chrome (shadows, fillets, arcs) is
 * drawn outside any clipped region, so it pays nothing for the feature. */
extern int render_clip_y0, render_clip_y1;
#define CLIP_Y0(v) ((v) > render_clip_y0 ? (v) : render_clip_y0)
#define CLIP_Y1(v) ((v) < render_clip_y1 ? (v) : render_clip_y1)

/* Physical column band, [x0, x1). Wide open by default, narrowed by
 * render_set_clip_x() around a bar's partial repaint so a frame that only
 * redraws a dirty cell writes nothing outside the damage it will attach. Unlike
 * the row band above this one DOES bind the drop shadows: a shadow spills past
 * the cell that owns it, and re-blending that tail over the copied-forward frame
 * would darken it a notch per tick. */
extern int render_clip_bx0, render_clip_bx1;
#define CLIP_X0(v) ((v) > render_clip_bx0 ? (v) : render_clip_bx0)
#define CLIP_X1(v) ((v) < render_clip_bx1 ? (v) : render_clip_bx1)

/* Optional rounded SHAPE the same content primitives are confined to, set by
 * render_set_clip_shape() (physical coords, independent of the y band above so a
 * damage band can narrow rows without moving the corner arcs). A scrolled
 * container clips to its own inner rounded rect: a card cut off at the bottom of
 * the panel then terminates along the panel's curve instead of squaring off over
 * the border. Cost is one predicted compare per row while unshaped, and two
 * sqrt per row only inside the r-tall corner bands. */
extern int render_clip_shaped;
extern int render_clip_x0, render_clip_x1, render_clip_sy0, render_clip_sy1;
extern int render_clip_rtl, render_clip_rtr, render_clip_rbr, render_clip_rbl;

/* Horizontal inset of a corner of radius r on the row `t` rows into its band.
 * Rounded up (and sampled at the row center) so the cut never pokes outside the
 * arc — a pixel too few reads as a clean edge, a pixel too many reads as a leak. */
static inline int rc_ins(int r, int t) {
    if (t >= r || r <= 0) return 0;
    double dy = r - t - 0.5;
    double s = (double)r * r - dy * dy;
    return s <= 0.0 ? r : (int)((double)r - sqrt(s) + 0.999);
}

/* Narrow the half-open column span [*lo,*hi) to the clip shape on physical row j. */
static inline void render_clip_row(int j, int *lo, int *hi) {
    if (render_clip_bx0 > *lo) *lo = render_clip_bx0;
    if (render_clip_bx1 < *hi) *hi = render_clip_bx1;
    if (!render_clip_shaped) return;
    int t = j - render_clip_sy0, b = render_clip_sy1 - 1 - j;
    int il = rc_ins(render_clip_rtl, t), i2 = rc_ins(render_clip_rbl, b);
    if (i2 > il) il = i2;
    int ir = rc_ins(render_clip_rtr, t); i2 = rc_ins(render_clip_rbr, b);
    if (i2 > ir) ir = i2;
    if (render_clip_x0 + il > *lo) *lo = render_clip_x0 + il;
    if (render_clip_x1 - ir < *hi) *hi = render_clip_x1 - ir;
}

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

#endif

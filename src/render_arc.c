/* Round AA primitives: the filled disc and the optionally segmented ring.
 * Split out of render.c to keep both under the file size cap; the two share
 * render_px.h so their edge coverage stays identical. */
#include "wisp.h"
#include "render_px.h"

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

/* The shared rasterizer for both: an annulus masked to the span
 * [offset, offset+len) degrees of every `period` degrees, 0 = 12 o'clock,
 * clockwise. Radial and angular edges are both signed distances in pixels and
 * the arc is their intersection, so one max() feeds the same cov_from_sd every
 * other primitive uses. The angular distance is scaled by the pixel's own
 * radius — that is what turns a constant degree span into a constant-width AA
 * band. */
#define DEG (3.14159265358979323846 / 180.0)
static void arc_span(uint32_t *px, int sw, int sh, double cx, double cy,
                     double r, double thickness, double period, double offset,
                     double len, uint32_t c) {
    if (r <= 0 || thickness <= 0 || len <= 0) return;
    sw = SC(sw); sh = SC(sh);
    cx = SC(cx); cy = SC(cy); r = SC(r); thickness = SC(thickness);
    uint8_t ca = (c >> 24) & 0xff;
    if (!ca) return;
    uint8_t cr = (c >> 16) & 0xff, cg = (c >> 8) & 0xff, cb = c & 0xff;
    int mask = len < period;
    double half = thickness / 2.0, outer = r + half;
    /* Wrap point: past here a pixel is closer to the NEXT span's start than to
     * this one's end, so fold it negative or the trailing edge loses its AA. */
    double fold = (period + len) / 2.0;
    int x0 = (int)(cx - outer - 1); if (x0 < 0) x0 = 0;
    int y0 = (int)(cy - outer - 1); if (y0 < 0) y0 = 0;
    int x1 = (int)(cx + outer + 2); if (x1 > sw) x1 = sw;
    int y1 = (int)(cy + outer + 2); if (y1 > sh) y1 = sh;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = px + j * sw;
        for (int i = x0; i < x1; i++) {
            double dx = (i + 0.5) - cx, dy = (j + 0.5) - cy;
            double d = sqrt(dx * dx + dy * dy);
            double sd = fabs(d - r) - half;
            if (mask && sd < 0.5) {
                double a = atan2(dx, -dy) / DEG - offset;
                a -= period * floor(a / period);
                if (a > fold) a -= period;
                double e = -a > a - len ? -a : a - len;
                e *= DEG * d;
                if (e > sd) sd = e;
            }
            double cov = cov_from_sd(sd);
            if (cov <= 0.0) continue;
            blend_over(&row[i], cr, cg, cb, (uint8_t)(ca * cov + 0.5));
        }
    }
}

void fill_ring(uint32_t *px, int sw, int sh, double cx, double cy,
               double r, double thickness, int segments, double gap, uint32_t c) {
    if (segments < 1) segments = 1;
    double step = 360.0 / segments;
    if (gap < 0) gap = 0;
    if (gap > step) return;              /* a gap at or past the sector = nothing */
    arc_span(px, sw, sh, cx, cy, r, thickness, step, gap / 2.0, step - gap, c);
}

void fill_arc(uint32_t *px, int sw, int sh, double cx, double cy,
              double r, double thickness, double start, double len, uint32_t c) {
    if (len > 360.0) len = 360.0;
    arc_span(px, sw, sh, cx, cy, r, thickness, 360.0, start, len, c);
}

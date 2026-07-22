/* truetype font backend (FONT_BACKEND=truetype) — our own reader + rasterizer
 * in src/tt/, no FreeType and no dlopen at all. Rasterizer only; the glyph
 * cache, trim scheduling and HiDPI twins live in font_cache.c, shared with the
 * freetype backend.
 *
 * One TtFont per font *file*, shared across every declared size: the scale is a
 * per-render argument, so N sizes cost one mmap. The optional fallback font
 * (WISP_FONT_FALLBACK_PATH, or $WISP_FONT_FALLBACK) opens lazily on the first
 * glyph the primary lacks. Color glyphs (CBDT strikes, COLRv0 layers) come out
 * of src/tt/color.c as premultiplied BGRA and are tagged Glyph.color=1. */
#include "wisp.h"

#ifdef WISP_FONT_TRUETYPE
#include <stdlib.h>
#include "tt/tt.h"

static TtFont primary, fallback;
static int    primary_ok, fb_tried, fb_ok;
static const char *fb_path;

void font_backend_init(void) {
    const char *path = getenv("WISP_FONT");
    if (!path || !*path) path = WISP_FONT_DEFAULT_PATH;
    fb_path = getenv("WISP_FONT_FALLBACK");
    if (!fb_path || !*fb_path) fb_path = WISP_FONT_FALLBACK_PATH;
    primary_ok = tt_open(&primary, path);
}

static TtFont *fb_get(void) {
    if (!fb_tried) {
        fb_tried = 1;
        fb_ok = fb_path && *fb_path && tt_open(&fallback, fb_path);
    }
    return fb_ok ? &fallback : NULL;
}

int font_backend_open(Font *f) {
    if (!primary_ok) return 0;
    tt_vmetrics(&primary, f->px_size, &f->line_h, &f->baseline);
    return 1;
}

void font_backend_trim(void) {
    if (primary_ok) tt_trim(&primary);
    if (fb_ok)      tt_trim(&fallback);
    /* The scratch is a few KB and regrows in one glyph; holding it across an
     * idle period is pure waste next to the RSS target. */
    tt_raster_release();
    tt_color_release();
}

int font_raster(Font *f, uint32_t cp, Glyph *g, const uint8_t **px) {
    if (!primary_ok) return 0;
    TtFont *src = &primary;
    uint16_t gid = tt_gid(src, cp);
    if (!gid) {
        TtFont *fb = fb_get();
        if (!fb) return 0;
        gid = tt_gid(fb, cp);
        if (!gid) return 0;
        src = fb;
    }
    TtBitmap bm;
    if (!tt_color_glyph(src, gid, f->px_size, &bm)
        && !tt_render(src, gid, f->px_size, &bm)) return 0;
    /* Advance still comes from hmtx, including for strikes: emoji fonts carry
     * real horizontal metrics, and using them keeps colour and outline glyphs
     * on the same rounding as every other advance in the line. */
    *g = (Glyph){ .w = (int16_t)bm.w, .h = (int16_t)bm.h,
                  .bx = (int16_t)bm.bx, .by = (int16_t)bm.by,
                  .color = (int16_t)bm.color,
                  .adv = (int16_t)tt_advance_px(src, gid, f->px_size) };
    *px = bm.px;
    return 1;
}

#endif /* WISP_FONT_TRUETYPE */

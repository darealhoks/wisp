/* Shared font runtime types. Stable, hand-written — the auto-generated
 * `bake.h` provides the *data* (or, for the truetype backend, mutable
 * skeletons), and `#include "font.h"` for these types.
 *
 * Two build-time backends, one runtime interface:
 *   truetype — `bake --stub` emits mutable per-size skeletons; font_tt.c
 *              (src/tt/) rasterizes TTF/OTF and fills the glyph cache on
 *              demand. Default; gated by WISP_FONT_RUNTIME.
 *   bitmap   — pixel-exact PSF/BDF read at build time into const tables. */
#ifndef WISP_FONT_H
#define WISP_FONT_H
#include <stdint.h>

typedef struct {
    uint32_t cp;
    int16_t  w, h;
    int16_t  bx, by;   /* bearing: glyph top-left offset from pen+baseline */
    int16_t  adv;      /* horizontal advance from pen */
    int16_t  color;    /* 0 = alpha8 (1 B/px); 1 = premultiplied BGRA (4 B/px) */
    int32_t  px_off;   /* byte offset into the per-size pixel pool */
} Glyph;

typedef struct {
    int            px_size;
    int            line_h;
    int            baseline;
    int            n;       /* number of glyphs in `g` (cached count for FT) */
    const Glyph   *g;
    const uint8_t *px;
#ifdef WISP_FONT_RUNTIME
    /* Runtime glyph cache (font_cache.c). `g`/`px` point into `gd`/`pxd` and
     * are kept current as the cache grows. font_N are mutable globals in these
     * backends (defined once in font_cache.c). */
    Glyph         *gd;      /* growable glyph cache backing `g`  */
    uint8_t       *pxd;     /* growable pixel pool backing `px` (alpha8 or BGRA) */
    int            cap;     /* glyph cache capacity   */
    int            px_len;  /* used bytes of pxd      */
    int            px_cap;  /* capacity of pxd        */
    int            tried;   /* face-open attempted    */
    int            ready;   /* face opened ok         */
#endif
} Font;

#ifdef WISP_FONT_RUNTIME
/* --- glyph cache (src/font_cache.c) --- */
/* Lookup-or-rasterize. render.c's font_find() delegates here. */
const Glyph *font_cache_find(const Font *f, uint32_t cp);
/* Release the font-file pages the rasterizer faulted: once a glyph is in the
 * cache, draw_glyph reads `px` and the backend is re-entered only on a miss, so
 * the mmap'd font bytes are dead weight in steady state. The main loop calls
 * this at its idle tail; it no-ops unless a new glyph was cached since the last
 * call. */
void font_trim(void);
/* HiDPI: the same face rasterized at round(px_size * s120/120), with its own
 * cache. Created on first use, so a scale-1 session never pays for it. Returns
 * `f` itself when the scaled size is no bigger. */
const Font *font_at_scale(const Font *f, int s120);

/* --- backend hooks (src/font_tt.c) --- */
/* One-time backend setup, called from the cache's constructor. */
void font_backend_init(void);
/* Fill f->line_h / f->baseline for f->px_size. 0 = font unusable. */
int  font_backend_open(Font *f);
/* Rasterize `cp` at f->px_size: fill *g (w/h/bx/by/adv/color — cp and px_off
 * are the cache's) and point *px at a tightly packed w*h*(color?4:1) buffer
 * owned by the backend, valid until the next call. 0 = no such glyph. */
int  font_raster(Font *f, uint32_t cp, Glyph *g, const uint8_t **px);
/* Drop the font mmap pages faulted since the last trim. */
void font_backend_trim(void);
#else
static inline void font_trim(void) {}
#endif

#endif

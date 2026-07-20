/* Shared font runtime types. Stable, hand-written — the auto-generated
 * `bake.h` provides the *data* (or, for the freetype backend, mutable
 * skeletons), and `#include "font.h"` for these types.
 *
 * Three build-time backends, one runtime interface:
 *   baked    — FreeType rasterizes a TTF at build time into const tables.
 *   bitmap   — pixel-exact PSF/BDF read at build time into the same tables.
 *   freetype — `bake --ft-stub` emits mutable per-size skeletons; font_ft.c
 *              dlopen()s libfreetype and fills the glyph cache on demand.
 * Only the freetype backend changes the runtime; it is gated by
 * WISP_FONT_FREETYPE (set by the Makefile when FONT_BACKEND=freetype). */
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
#ifdef WISP_FONT_FREETYPE
    /* Runtime glyph cache for the dlopen'd-freetype backend. `g`/`px` point
     * into `gd`/`pxd` and are kept current as the cache grows. font_N are
     * mutable globals in this backend (defined once in font_ft.c). */
    Glyph         *gd;      /* growable glyph cache backing `g`  */
    uint8_t       *pxd;     /* growable pixel pool backing `px` (alpha8 or BGRA) */
    int            cap;     /* glyph cache capacity   */
    int            px_len;  /* used bytes of pxd      */
    int            px_cap;  /* capacity of pxd        */
    void          *face;    /* FT_Face, lazily opened */
    int            tried;   /* face-open attempted    */
    int            ready;   /* face opened ok         */
#endif
} Font;

#ifdef WISP_FONT_FREETYPE
/* Lookup-or-rasterize for the freetype backend (src/font_ft.c). render.c's
 * font_find() delegates here under WISP_FONT_FREETYPE. */
const Glyph *font_ft_find(const Font *f, uint32_t cp);
/* Release the font-file pages FreeType faulted while rasterizing: once a glyph
 * is in the cache, draw_glyph reads `px` and FreeType is re-entered only on a
 * miss, so the mmap'd font bytes are dead weight in steady state. The main loop
 * calls this at its idle tail; it no-ops unless a new glyph was cached since the
 * last call, then madvise(DONTNEED)s the font mmaps (re-fault from page cache on
 * the next miss). */
void font_ft_trim(void);
/* HiDPI: the same face rasterized at px_size*scale, with its own cache.
 * Created on first use, so a scale-1 session never pays for it. Returns `f`
 * itself when scale <= 1 or freetype is unavailable. */
const Font *font_ft_at_scale(const Font *f, int scale);
#else
static inline void font_ft_trim(void) {}
#endif

#endif

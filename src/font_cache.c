/* Runtime glyph cache, shared by every rasterizing font backend
 * (WISP_FONT_RUNTIME; currently font_tt.c). Backend-agnostic: it owns the
 * growable per-Font glyph/pixel pools, the negative cache, the idle mmap trim
 * and the HiDPI twins, and asks the backend for pixels only on a miss through
 * the three hooks in font.h (font_backend_open / font_raster /
 * font_backend_trim).
 *
 * The per-size font_N globals are *declared* (extern) by the generated bake.h
 * stub everywhere, and *defined* exactly once — here, via WISP_FONT_DEFINE.
 * The stub also gives us wisp_fonts[] / wisp_n_fonts (the full set, opened at
 * startup).
 *
 * Two glyph formats coexist in one cache, tagged by Glyph.color: outline
 * glyphs are alpha8 (1 B/px, tinted by fg at blit time); color-bitmap glyphs
 * are premultiplied BGRA (4 B/px, drawn as-is). */
#define WISP_FONT_DEFINE
#include "wisp.h"

#ifdef WISP_FONT_RUNTIME
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

static int cache_dirty;   /* a new glyph was rasterized since the last trim */

/* Ensure room for one more glyph + `bytes` of pixels; return the pixel pool
 * byte-offset (= current px_len), or -1 on OOM. Does not advance px_len — the
 * caller writes the pixels then bumps it. */
static int reserve(Font *f, int bytes) {
    if (f->n >= f->cap) {
        int nc = f->cap ? f->cap * 2 : 128;
        Glyph *ng = realloc(f->gd, (size_t)nc * sizeof(Glyph));
        if (!ng) return -1;
        f->gd = ng; f->cap = nc; f->g = ng;
    }
    if (bytes > 0 && f->px_len + bytes > f->px_cap) {
        int nc = (f->px_len + bytes) * 2 + 1024;
        uint8_t *np = realloc(f->pxd, (size_t)nc);
        if (!np) return -1;
        f->pxd = np; f->px_cap = nc; f->px = np;
    }
    return f->px_len;
}

static const Glyph *commit(Font *f, Glyph g) {
    f->gd[f->n] = g;
    f->g = f->gd; f->px = f->pxd;
    cache_dirty = 1;   /* font mmap pages were just faulted; trim them when idle */
    return &f->gd[f->n++];
}

static void font_open(Font *f) {
    f->tried = 1;
    if (!font_backend_open(f)) return;
    f->ready = 1;
    /* Warm ASCII so metrics + common text are ready before the first frame. */
    for (uint32_t c = 32; c <= 126; c++) font_cache_find(f, c);
}

/* Lookup in the dynamic cache; on miss, rasterize at f->px_size and append.
 * font_N are mutable globals in these backends, so casting away const is
 * sound; the cache pointers (g/px) are kept current as the buffers realloc. */
const Glyph *font_cache_find(const Font *cf, uint32_t cp) {
    Font *f = (Font *)cf;
    for (int i = 0; i < f->n; i++)
        if (f->gd[i].cp == cp) return f->gd[i].w < 0 ? NULL : &f->gd[i];
    if (!f->tried) font_open(f);
    if (!f->ready) return NULL;

    Glyph g = {0};
    const uint8_t *src = NULL;
    if (!font_raster(f, cp, &g, &src)) {
        /* Negative-cache the miss: otherwise every redraw re-enters the
         * rasterizer for this codepoint, re-faulting cmap pages and keeping
         * cache_dirty=0 so the trim never runs. w<0 is the tombstone (skipped
         * in the loop above; callers already handle NULL). commit() also
         * re-arms the trim. */
        if (reserve(f, 0) < 0) return NULL;
        commit(f, (Glyph){ .cp = cp, .w = -1 });
        return NULL;
    }
    int bytes = g.w * g.h * (g.color ? 4 : 1);
    int off = reserve(f, bytes);
    if (off < 0) return NULL;
    if (bytes > 0) { memcpy(f->pxd + off, src, (size_t)bytes); f->px_len += bytes; }
    g.cp = cp; g.px_off = off;
    return commit(f, g);
}

void font_trim(void) {
    if (!cache_dirty) return;
    cache_dirty = 0;
    font_backend_trim();
    /* The rasterizer frees its scratch back to the arena but glibc keeps it;
     * hand the top of the heap back to the OS now that we're idle. */
    malloc_trim(0);
}

const Font *font_at_scale(const Font *f, int s120) {
    /* One twin per (font, px size). Two declared sizes x a handful of scales
     * fits easily; overflow falls back to the native strike (blocky, not
     * broken). Keyed on the rounded pixel size, so 1.25x and 1.3x that land on
     * the same ppem share one twin. */
    static struct { const Font *base; int px; Font *twin; } tw[8];
    static int n_tw;
    int px = (f->px_size * s120 + 60) / 120;
    if (px <= f->px_size) return f;
    for (int i = 0; i < n_tw; i++)
        if (tw[i].base == f && tw[i].px == px) return tw[i].twin;
    if (n_tw == (int)(sizeof tw / sizeof *tw)) return f;
    Font *t = calloc(1, sizeof *t);
    if (!t) return f;
    t->px_size = px;
    tw[n_tw].base = f; tw[n_tw].px = px; tw[n_tw].twin = t;
    n_tw++;
    return t;   /* font_open() runs on its first font_cache_find() */
}

__attribute__((constructor, used))
static void font_cache_init(void) {
    font_backend_init();
    for (int i = 0; i < wisp_n_fonts; i++) font_open(wisp_fonts[i]);
}

#endif /* WISP_FONT_RUNTIME */

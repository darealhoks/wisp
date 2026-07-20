/* freetype font backend (FONT_BACKEND=freetype).
 *
 * The daemon keeps NO link-time freetype dependency: this module dlopen()s
 * libfreetype.so.6 at startup and rasterizes glyphs on demand into each
 * Font's growable cache (filled by font_ft_find, which render.c's font_find
 * delegates to). Only compiled + linked when WISP_FONT_FREETYPE is set.
 *
 * One FT_Face is shared across every size (the rasterizer pins the pixel size
 * per glyph), so N declared sizes cost one face + one font-file mmap, not N.
 * An optional fallback font (WISP_FONT_FALLBACK_PATH, or $WISP_FONT_FALLBACK)
 * is opened lazily on the first glyph the primary lacks; its glyphs land in
 * the same per-Font cache. Two glyph formats coexist there, tagged by
 * Glyph.color: outline glyphs render to alpha8 (1 B/px, tinted by fg);
 * color-bitmap glyphs (CBDT/sbix emoji, FT_PIXEL_MODE_BGRA) are downscaled
 * from their fixed strike to the text size and stored premultiplied BGRA
 * (4 B/px, drawn as-is). SVG-only color fonts still won't render (FreeType
 * needs an external SVG renderer for those).
 *
 * The per-size font_N globals are *declared* (extern) by the generated
 * bake.h stub everywhere, and *defined* exactly once — here, via
 * WISP_FONT_DEFINE. The stub also gives us wisp_fonts[] / wisp_n_fonts (the
 * full set, opened at startup) and WISP_FONT_DEFAULT_PATH (the baked-in
 * fallback; $WISP_FONT overrides at runtime). */
#define WISP_FONT_DEFINE
#include "wisp.h"

#ifdef WISP_FONT_FREETYPE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <malloc.h>
#include <sys/mman.h>
#include <ft2build.h>
#include FT_FREETYPE_H

/* Resolved lazily via dlsym — never linked. */
static FT_Error (*p_Init)(FT_Library *);
static FT_Error (*p_NewFace)(FT_Library, const char *, FT_Long, FT_Face *);
static FT_Error (*p_SetPx)(FT_Face, FT_UInt, FT_UInt);
static FT_UInt  (*p_CharIndex)(FT_Face, FT_ULong);
static FT_Error (*p_LoadGlyph)(FT_Face, FT_UInt, FT_Int32);
static FT_Error (*p_RenderGlyph)(FT_GlyphSlot, FT_Render_Mode);
static FT_Error (*p_SelSize)(FT_Face, FT_Int);   /* optional: color-strike select */

static FT_Library ftlib;
static const char *ft_path;   /* primary font */
static const char *fb_path;   /* monochrome fallback (empty → no chain) */
static int ft_ok;   /* dlopen + init + all symbols resolved */

/* One FT_Face per font *file*, shared across all sizes: the rasterizer pins
 * the pixel size right before each load, so opening N sizes costs one face +
 * one font-file mmap instead of N. */
static FT_Face primary_face;
static int     primary_tried;
static FT_Face fb_face;        /* lazily opened on first primary miss */
static int     fb_tried;
static int     fb_is_strike;   /* fallback is a fixed-strike color-bitmap font */
static int     fb_strike_ppem; /* its strike y_ppem (BGRA bitmaps scaled from here) */
static int     ft_dirty;       /* a new glyph was rasterized since the last trim */

static void *sym(void *h, const char *n) {
    void *p = dlsym(h, n);
    if (!p) fprintf(stderr, "wisp: freetype: missing symbol %s\n", n);
    return p;
}

static void ft_load(void) {
    void *h = dlopen("libfreetype.so.6", RTLD_LAZY | RTLD_LOCAL);
    if (!h) h = dlopen("libfreetype.so", RTLD_LAZY | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "wisp: freetype backend unavailable: %s\n", dlerror());
        return;
    }
    p_Init        = (FT_Error (*)(FT_Library *))                                sym(h, "FT_Init_FreeType");
    p_NewFace     = (FT_Error (*)(FT_Library, const char *, FT_Long, FT_Face *))sym(h, "FT_New_Face");
    p_SetPx       = (FT_Error (*)(FT_Face, FT_UInt, FT_UInt))                   sym(h, "FT_Set_Pixel_Sizes");
    p_CharIndex   = (FT_UInt  (*)(FT_Face, FT_ULong))                           sym(h, "FT_Get_Char_Index");
    p_LoadGlyph   = (FT_Error (*)(FT_Face, FT_UInt, FT_Int32))                  sym(h, "FT_Load_Glyph");
    p_RenderGlyph = (FT_Error (*)(FT_GlyphSlot, FT_Render_Mode))                sym(h, "FT_Render_Glyph");
    /* Optional — only color-bitmap fonts need it; absence just disables those. */
    p_SelSize     = (FT_Error (*)(FT_Face, FT_Int))                            dlsym(h, "FT_Select_Size");
    if (!p_Init || !p_NewFace || !p_SetPx || !p_CharIndex || !p_LoadGlyph || !p_RenderGlyph)
        return;
    if (p_Init(&ftlib)) { fprintf(stderr, "wisp: FT_Init_FreeType failed\n"); return; }
    ft_path = getenv("WISP_FONT");
    if (!ft_path || !*ft_path) ft_path = WISP_FONT_DEFAULT_PATH;
    fb_path = getenv("WISP_FONT_FALLBACK");
    if (!fb_path || !*fb_path) fb_path = WISP_FONT_FALLBACK_PATH;
    ft_ok = 1;
}

static FT_Face open_face(const char *path) {
    if (!path || !*path) return NULL;
    FT_Face face;
    if (p_NewFace(ftlib, path, 0, &face)) {
        fprintf(stderr, "wisp: freetype: cannot open %s\n", path);
        return NULL;
    }
    return face;
}

/* Lazily open the fallback face on first need — costs nothing until a glyph
 * actually misses the primary font. A fixed-strike (color bitmap) font can't
 * be FT_Set_Pixel_Sizes'd, so select its largest strike once here; the
 * rasterizer downscales that strike's BGRA bitmaps to each text size. */
static FT_Face fb_get(void) {
    if (fb_tried) return fb_face;
    fb_tried = 1;
    if (!ft_ok) return NULL;
    fb_face = open_face(fb_path);
    if (fb_face && !(fb_face->face_flags & FT_FACE_FLAG_SCALABLE)
                && fb_face->num_fixed_sizes > 0 && p_SelSize) {
        int best = 0;
        for (int i = 1; i < fb_face->num_fixed_sizes; i++)
            if (fb_face->available_sizes[i].y_ppem > fb_face->available_sizes[best].y_ppem)
                best = i;
        if (!p_SelSize(fb_face, best)) {
            fb_is_strike = 1;
            fb_strike_ppem = (int)(fb_face->available_sizes[best].y_ppem >> 6);
            if (fb_strike_ppem < 1) fb_strike_ppem = 1;
        }
    }
    return fb_face;
}

static void font_open(Font *f) {
    f->tried = 1;
    if (!ft_ok) return;
    if (!primary_tried) { primary_tried = 1; primary_face = open_face(ft_path); }
    if (!primary_face) return;
    if (p_SetPx(primary_face, 0, (FT_UInt)f->px_size)) return;
    f->face     = primary_face;
    f->line_h   = (int)(primary_face->size->metrics.height   >> 6);
    f->baseline = (int)(primary_face->size->metrics.ascender >> 6);
    f->ready    = 1;
    /* Warm ASCII so metrics + common text are ready before the first frame. */
    for (uint32_t c = 32; c <= 126; c++) font_ft_find(f, c);
}

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
    ft_dirty = 1;   /* font mmap pages were just faulted; trim them when idle */
    return &f->gd[f->n++];
}

/* madvise(MADV_DONTNEED) every mapping of `path` in our address space. The
 * pages are read-only file-backed, so the kernel just drops them; FreeType
 * re-faults whatever it needs from the page cache on the next cache miss. */
static void drop_file_pages(const char *path) {
    if (!path || !*path) return;
    FILE *m = fopen("/proc/self/maps", "r");
    if (!m) return;
    char line[512];
    while (fgets(line, sizeof line, m)) {
        unsigned long a, b;
        int off = 0;
        if (sscanf(line, "%lx-%lx %*s %*s %*s %*s %n", &a, &b, &off) < 2 || !off)
            continue;
        char *p = line + off;            /* pathname field (may be empty) */
        char *nl = strchr(p, '\n'); if (nl) *nl = 0;
        if (*p && strcmp(p, path) == 0)
            madvise((void *)a, (size_t)(b - a), MADV_DONTNEED);
    }
    fclose(m);
}

void font_ft_trim(void) {
    if (!ft_dirty) return;
    ft_dirty = 0;
    drop_file_pages(ft_path);
    drop_file_pages(fb_path);
    /* FreeType frees its glyph-loading scratch back to the arena but glibc
     * keeps it; hand the top of the heap back to the OS now that we're idle. */
    malloc_trim(0);
}

/* Outline glyph → alpha8, 1 byte/pixel (tinted by fg at blit time). */
static const Glyph *cache_gray(Font *f, uint32_t cp, FT_GlyphSlot s, FT_Bitmap *bm) {
    int w = (int)bm->width, h = (int)bm->rows;
    int off = reserve(f, w * h);
    if (off < 0) return NULL;
    if (w > 0 && h > 0) {
        for (int y = 0; y < h; y++)
            memcpy(f->pxd + off + (size_t)y * w, bm->buffer + (size_t)y * bm->pitch, (size_t)w);
        f->px_len += w * h;
    }
    return commit(f, (Glyph){
        .cp = cp, .w = (int16_t)w, .h = (int16_t)h,
        .bx = (int16_t)s->bitmap_left, .by = (int16_t)s->bitmap_top,
        .adv = (int16_t)(s->advance.x >> 6), .px_off = off });
}

/* Color-bitmap glyph (premultiplied BGRA) → box-downscaled from its source
 * ppem to this Font's size, 4 bytes/pixel, drawn as-is. */
static const Glyph *cache_color(Font *f, uint32_t cp, FT_GlyphSlot s,
                                FT_Bitmap *bm, int src_ppem) {
    int sw = (int)bm->width, sh = (int)bm->rows;
    if (sw <= 0 || sh <= 0 || src_ppem <= 0) return NULL;
    int target = f->px_size;
    int ow = (sw * target + src_ppem / 2) / src_ppem;
    int oh = (sh * target + src_ppem / 2) / src_ppem;
    if (ow < 1) ow = 1;
    if (oh < 1) oh = 1;
    int need = ow * oh * 4;
    int off = reserve(f, need);
    if (off < 0) return NULL;
    uint8_t *dst = f->pxd + off;
    for (int dy = 0; dy < oh; dy++) {
        int sy0 = dy * sh / oh, sy1 = (dy + 1) * sh / oh;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int dx = 0; dx < ow; dx++) {
            int sx0 = dx * sw / ow, sx1 = (dx + 1) * sw / ow;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            unsigned b = 0, g = 0, r = 0, a = 0, cnt = 0;
            for (int yy = sy0; yy < sy1; yy++) {
                const uint8_t *row = bm->buffer + (size_t)yy * bm->pitch;
                for (int xx = sx0; xx < sx1; xx++) {
                    const uint8_t *p = row + (size_t)xx * 4;
                    b += p[0]; g += p[1]; r += p[2]; a += p[3]; cnt++;
                }
            }
            uint8_t *o = dst + ((size_t)dy * ow + dx) * 4;
            o[0] = (uint8_t)(b / cnt); o[1] = (uint8_t)(g / cnt);
            o[2] = (uint8_t)(r / cnt); o[3] = (uint8_t)(a / cnt);
        }
    }
    f->px_len += need; f->px = f->pxd;
    int adv = ((int)(s->advance.x >> 6) * target + src_ppem / 2) / src_ppem;
    if (adv < 1) adv = ow;
    return commit(f, (Glyph){
        .cp = cp, .w = (int16_t)ow, .h = (int16_t)oh,
        .bx = (int16_t)((s->bitmap_left * target) / src_ppem),
        .by = (int16_t)((s->bitmap_top  * target + src_ppem / 2) / src_ppem),
        .adv = (int16_t)adv, .color = 1, .px_off = off });
}

/* Lookup in the dynamic cache; on miss, rasterize at f->px_size and append.
 * font_N are mutable globals in this backend, so casting away const is sound;
 * the cache pointers (g/px) are kept current as the buffers realloc. */
const Glyph *font_ft_find(const Font *cf, uint32_t cp) {
    Font *f = (Font *)cf;
    for (int i = 0; i < f->n; i++)
        if (f->gd[i].cp == cp) return f->gd[i].w < 0 ? NULL : &f->gd[i];
    if (!f->tried) font_open(f);
    if (!f->ready) return NULL;

    /* Primary font first; on miss fall through to the fallback. Either face
     * rasterizes into *this* Font's cache (draw_glyph indexes f->px), so the
     * glyph's source face no longer matters once cached. */
    FT_Face face = primary_face;
    int strike = 0;
    FT_UInt gi = p_CharIndex(face, cp);
    if (!gi) {
        FT_Face fb = fb_get();
        if (fb) { FT_UInt g2 = p_CharIndex(fb, cp);
                  if (g2) { face = fb; gi = g2; strike = fb_is_strike; } }
    }
    if (!gi) {
        /* Negative-cache the miss: otherwise every redraw re-enters FreeType
         * for this codepoint, re-faulting cmap pages and keeping ft_dirty=0 so
         * the trim never runs. w<0 is the tombstone (skipped in the loop above;
         * callers already handle NULL). commit() also re-arms the trim. */
        if (reserve(f, 0) < 0) return NULL;
        commit(f, (Glyph){ .cp = cp, .w = -1 });
        return NULL;
    }

    /* Scalable faces: pin to this Font's size. A fixed-strike color face was
     * already FT_Select_Size'd in fb_get; its bitmap is scaled in cache_color. */
    if (!strike && p_SetPx(face, 0, (FT_UInt)f->px_size)) return NULL;
    if (p_LoadGlyph(face, gi, FT_LOAD_COLOR)) return NULL;
    if (p_RenderGlyph(face->glyph, FT_RENDER_MODE_NORMAL)) return NULL;

    FT_GlyphSlot s = face->glyph;
    FT_Bitmap *bm = &s->bitmap;
    if (bm->pixel_mode == FT_PIXEL_MODE_BGRA)
        return cache_color(f, cp, s, bm, strike ? fb_strike_ppem : f->px_size);
    return cache_gray(f, cp, s, bm);
}

const Font *font_ft_at_scale(const Font *f, int s120) {
    /* One twin per (font, px size). Two declared sizes x a handful of scales
     * fits easily; overflow falls back to the native strike (blocky, not
     * broken). Keyed on the rounded pixel size, so 1.25x and 1.3x that land on
     * the same ppem share one twin. */
    static struct { const Font *base; int px; Font *twin; } tw[8];
    static int n_tw;
    if (!ft_ok) return f;
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
    return t;   /* font_open() runs on its first font_ft_find() */
}

__attribute__((constructor, used))
static void font_ft_init(void) {
    ft_load();
    for (int i = 0; i < wisp_n_fonts; i++) font_open(wisp_fonts[i]);
}

#endif /* WISP_FONT_FREETYPE */

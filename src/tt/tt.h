/* From-scratch TrueType/OpenType reader — libc only, no FreeType.
 *
 * Deliberately free of wisp.h: sfnt.c is also linked into the standalone
 * `ttmetrics` debug tool, which diffs what we compute against libfreetype.
 *
 * Everything here reads out of one read-only mmap of the font file. The only
 * heap allocation is the cmap range index, which exists so that steady-state
 * lookups touch no font pages at all — that keeps the whole mapping evictable
 * by tt_trim(). */
#ifndef WISP_TT_H
#define WISP_TT_H
#include <stdint.h>
#include <stddef.h>

/* gid = cp + delta, for every cp in [first,last]. Segments the font's cmap
 * splits differently are coalesced here whenever the delta matches. */
typedef struct { uint32_t first, last; int32_t delta; } TtRange;

/* A CFF INDEX: `off` is the offset array, `data` the byte *before* the payload
 * (CFF offsets are 1-based from there), `dlen` the payload length. */
typedef struct {
    const uint8_t *off, *data;
    uint32_t       count, dlen;
    uint8_t        osz;
} TtIndex;

typedef struct {
    const uint8_t *base;
    size_t         size;
    uint16_t       upem, n_glyphs, n_hmetrics;
    int16_t        ascent, descent, line_gap;   /* hhea, font units */
    int            long_loca;                   /* head.indexToLocFormat */
    uint32_t       loca_off, loca_len;
    uint32_t       glyf_off, glyf_len;          /* 0 for CFF (OTTO) fonts */
    uint32_t       hmtx_off, hmtx_len;
    uint32_t       cff_off,  cff_len;
    TtIndex        cs, gsubrs, lsubrs;          /* CFF charstrings + subrs */
    int32_t        gbias, lbias;
    int            cff_ok;
    uint32_t       cblc_off, cblc_len, cbdt_off, cbdt_len;   /* CBDT emoji */
    uint32_t       colr_off, colr_len, cpal_off, cpal_len;   /* COLRv0 layers */
    int            has_color;
    TtRange       *map;
    int            n_map;
} TtFont;

/* 0 on any failure (unreadable, truncated, unsupported flavour); prints why. */
int  tt_open(TtFont *f, const char *path);
void tt_close(TtFont *f);
/* Drop the mapping's resident pages; the kernel re-faults on the next glyph. */
void tt_trim(const TtFont *f);

uint16_t tt_gid(const TtFont *f, uint32_t cp);        /* 0 = .notdef/missing */
int      tt_advance(const TtFont *f, uint16_t gid);   /* font units */
/* Outline bytes for `gid` from glyf; NULL when empty (space) or out of range. */
const uint8_t *tt_glyf(const TtFont *f, uint16_t gid, uint32_t *len);

/* --- rasterizer (raster.c) --- */
/* bx/by are the FreeType bearing convention: left edge and top edge relative to
 * the pen on the baseline. `px` is w*h packed alpha8, owned by the rasterizer
 * and valid until the next tt_render(). */
typedef struct {
    int w, h, bx, by;
    int color;              /* 0 = w*h alpha8; 1 = w*h premultiplied BGRA */
    const uint8_t *px;
} TtBitmap;
/* 1 on success, including blank glyphs (w = h = 0). 0 = malformed outline. */
int  tt_render(const TtFont *f, uint16_t gid, int px, TtBitmap *bm);
/* Free the rasterizer's scratch (grown to the largest glyph seen). */
void tt_raster_release(void);

/* --- CFF (cff.c) --- */
/* Locate charstrings + subrs; 0 (with a stderr line) if the CFF is one we
 * can't drive, in which case f->cff_ok stays 0 and glyf is the only source. */
int tt_cff_init(TtFont *f);
/* Run gid's Type 2 charstring into the path sink below. 1 on success. */
int tt_cff_glyph(const TtFont *f, uint16_t gid);

/* Path sink, in font units — raster.c owns the current point and flattening.
 * Only cff.c calls these; glyf outlines take the faster in-file path. */
void tt_path_move(float x, float y);
void tt_path_line(float x, float y);
void tt_path_cubic(float x1, float y1, float x2, float y2, float x3, float y3);
void tt_path_close(void);

/* --- color glyphs (color.c) --- */
/* CBDT strike (PNG) or COLRv0 layer stack for `gid`, composited to premultiplied
 * BGRA at `px`. 0 = this gid has no color form; the caller falls back to
 * tt_render(). Only linked into the daemon — needs image.c's PNG decoder. */
int  tt_color_glyph(const TtFont *f, uint16_t gid, int px, TtBitmap *bm);
void tt_color_release(void);

/* Scaled metrics, reproducing FreeType's rounding exactly (see sfnt.c). */
void tt_vmetrics(const TtFont *f, int px, int *line_h, int *baseline);
int  tt_advance_px(const TtFont *f, uint16_t gid, int px);

#endif

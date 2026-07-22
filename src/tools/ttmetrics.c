/* Debug tool: diff src/tt's metrics against libfreetype for the same font.
 *
 * Not built or shipped by default — `make ttmetrics && ./ttmetrics FONT [px…]`.
 * FreeType is dlopen()ed exactly like the freetype backend does, so this needs
 * no link dependency either. Exits non-zero on the first mismatching size. */
#include "../tt/tt.h"
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <ft2build.h>
#include FT_FREETYPE_H

static FT_Error (*p_Init)(FT_Library *);
static FT_Error (*p_NewFace)(FT_Library, const char *, FT_Long, FT_Face *);
static FT_Error (*p_SetPx)(FT_Face, FT_UInt, FT_UInt);
static FT_UInt  (*p_CharIndex)(FT_Face, FT_ULong);
static FT_Error (*p_LoadGlyph)(FT_Face, FT_UInt, FT_Int32);
static FT_Error (*p_RenderGlyph)(FT_GlyphSlot, FT_Render_Mode);

/* Bitmap diff: FreeType's rasterizer is a different algorithm, so per-pixel
 * equality is not the bar. Box and bearings must match exactly; coverage is
 * reported as a mean absolute difference (a few units of 255 is normal AA
 * disagreement, a wrong outline shows up as tens). */
static int diff_bitmap(TtFont *tt, FT_Face face, uint32_t gi, int px, uint32_t cp) {
    TtBitmap bm;
    if (!tt_render(tt, (uint16_t)gi, px, &bm)) {
        printf("    RENDER FAIL U+%04X px=%d\n", cp, px);
        return 1;
    }
    if (p_RenderGlyph(face->glyph, FT_RENDER_MODE_NORMAL)) return 0;
    FT_Bitmap *b = &face->glyph->bitmap;
    int fw = (int)b->width, fh = (int)b->rows;
    if (fw != bm.w || fh != bm.h
        || face->glyph->bitmap_left != bm.bx || face->glyph->bitmap_top != bm.by) {
        printf("    MISMATCH box U+%04X px=%d: ft %dx%d@%d,%d ours %dx%d@%d,%d\n",
               cp, px, fw, fh, (int)face->glyph->bitmap_left,
               (int)face->glyph->bitmap_top, bm.w, bm.h, bm.bx, bm.by);
        return 1;
    }
    long sum = 0;
    for (int y = 0; y < fh; y++)
        for (int x = 0; x < fw; x++) {
            int d = (int)b->buffer[y * b->pitch + x] - (int)bm.px[y * fw + x];
            sum += d < 0 ? -d : d;
        }
    int n = fw * fh;
    int mean = n ? (int)(sum / n) : 0;
    if (mean > 12) { printf("    COVERAGE U+%04X px=%d: mean |diff| %d\n", cp, px, mean); return 1; }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: ttmetrics FONT.ttf [px…]\n"); return 2; }
    TtFont tt;
    if (!tt_open(&tt, argv[1])) return 1;

    void *h = dlopen("libfreetype.so.6", RTLD_LAZY | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "no libfreetype: %s\n", dlerror()); return 1; }
    p_Init      = (FT_Error (*)(FT_Library *))                                 dlsym(h, "FT_Init_FreeType");
    p_NewFace   = (FT_Error (*)(FT_Library, const char *, FT_Long, FT_Face *)) dlsym(h, "FT_New_Face");
    p_SetPx     = (FT_Error (*)(FT_Face, FT_UInt, FT_UInt))                    dlsym(h, "FT_Set_Pixel_Sizes");
    p_CharIndex = (FT_UInt  (*)(FT_Face, FT_ULong))                            dlsym(h, "FT_Get_Char_Index");
    p_LoadGlyph = (FT_Error (*)(FT_Face, FT_UInt, FT_Int32))                   dlsym(h, "FT_Load_Glyph");
    p_RenderGlyph = (FT_Error (*)(FT_GlyphSlot, FT_Render_Mode))               dlsym(h, "FT_Render_Glyph");
    FT_Library lib; FT_Face face;
    if (!p_Init || !p_NewFace || !p_SetPx || !p_CharIndex || !p_LoadGlyph
        || p_Init(&lib) || p_NewFace(lib, argv[1], 0, &face)) {
        fprintf(stderr, "freetype setup failed\n"); return 1;
    }

    printf("%s: upem=%u glyphs=%u ranges=%d %s\n", argv[1], tt.upem, tt.n_glyphs,
           tt.n_map, tt.glyf_len ? "glyf" : "CFF");
    if (face->units_per_EM != tt.upem || face->num_glyphs != tt.n_glyphs)
        printf("  MISMATCH header: ft upem=%u glyphs=%ld\n",
               face->units_per_EM, (long)face->num_glyphs);

    /* Every cp the font claims to map, not just ASCII — a wrong cmap segment
     * usually only shows up outside Latin. */
    int bad = 0;
    for (int i = 0; i < tt.n_map; i++)
        for (uint32_t cp = tt.map[i].first; cp <= tt.map[i].last; cp++) {
            uint32_t ft = p_CharIndex(face, cp), our = tt_gid(&tt, cp);
            if (ft != our && bad++ < 20)
                printf("  MISMATCH gid U+%04X: ft=%u ours=%u\n", cp, ft, our);
        }

    static const char *def[] = { "", "", "14", "22" };
    if (argc < 3) { argv = (char **)(void *)def; argc = 4; }
    for (int a = 2; a < argc; a++) {
        int px = atoi(argv[a]);
        if (px < 1) continue;
        if (p_SetPx(face, 0, (FT_UInt)px)) { printf("  ft: no size %d\n", px); continue; }
        int lh, bl;
        tt_vmetrics(&tt, px, &lh, &bl);
        int ft_lh = (int)(face->size->metrics.height >> 6);
        int ft_bl = (int)(face->size->metrics.ascender >> 6);
        printf("  px=%2d line_h %d/%d %s  baseline %d/%d %s\n",
               px, lh, ft_lh, lh == ft_lh ? "ok" : "MISMATCH",
               bl, ft_bl, bl == ft_bl ? "ok" : "MISMATCH");
        if (lh != ft_lh || bl != ft_bl) bad++;
        for (uint32_t cp = 32; cp < 0x250; cp++) {
            uint32_t gi = p_CharIndex(face, cp);
            if (!gi || p_LoadGlyph(face, gi, FT_LOAD_NO_HINTING)) continue;
            int ours = tt_advance_px(&tt, (uint16_t)gi, px);
            /* Rounded, not floored: the TT driver grid-fits advances so the two
             * agree, but unhinted CFF advances stay fractional and wisp wants a
             * whole pixel — matching tt_advance_px's rounding. */
            int ft   = (int)((face->glyph->advance.x + 32) >> 6);
            if (ours != ft && bad++ < 40)
                printf("    MISMATCH adv U+%04X px=%d: ft=%d ours=%d\n", cp, px, ft, ours);
            if (p_RenderGlyph && diff_bitmap(&tt, face, gi, px, cp)) bad++;
        }
    }
    printf(bad ? "FAIL (%d mismatches)\n" : "PASS\n", bad);
    tt_close(&tt);
    return bad ? 1 : 0;
}

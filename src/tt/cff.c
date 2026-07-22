/* CFF (OTTO) outlines: INDEX/DICT container + a Type 2 charstring interpreter
 * feeding raster.c's path sink. Everything is read straight out of the mmap and
 * bounds-checked; nothing is copied or cached, so the mapping stays evictable
 * exactly like the glyf path.
 *
 * Only what shipping text fonts actually use: no CID/FDSelect, no CFF2, no
 * FontMatrix other than the implied 1/upem, and the arithmetic/storage
 * charstring operators are accepted-and-ignored rather than implemented. */
#include "tt.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define STACK 48   /* Type 2 spec limit */
#define DEPTH 10   /* subr nesting */

static uint32_t roff(const uint8_t *p, int sz) {
    uint32_t v = 0;
    for (int i = 0; i < sz; i++) v = v << 8 | p[i];
    return v;
}

/* --- INDEX --------------------------------------------------------------- */

/* Returns the first byte past the INDEX, or NULL if it doesn't fit. */
static const uint8_t *idx_read(const uint8_t *p, const uint8_t *end, TtIndex *ix) {
    memset(ix, 0, sizeof *ix);
    if (end - p < 2) return NULL;
    uint32_t count = (uint32_t)(p[0] << 8 | p[1]);
    p += 2;
    if (!count) return p;                       /* an empty INDEX is just a count */
    if (end - p < 1) return NULL;
    uint8_t osz = *p++;
    if (osz < 1 || osz > 4) return NULL;
    if ((uint64_t)(end - p) < (uint64_t)(count + 1) * osz) return NULL;
    ix->off = p; ix->osz = osz; ix->count = count;
    ix->data = p + (size_t)(count + 1) * osz - 1;
    uint32_t last = roff(ix->off + (size_t)count * osz, osz);
    if (last < 1 || (uint64_t)(end - ix->data) < last) return NULL;
    ix->dlen = last;
    return ix->data + last;
}

static const uint8_t *idx_get(const TtIndex *ix, uint32_t i, uint32_t *len) {
    if (i >= ix->count) return NULL;
    uint32_t a = roff(ix->off + (size_t)i * ix->osz, ix->osz);
    uint32_t b = roff(ix->off + (size_t)(i + 1) * ix->osz, ix->osz);
    if (a < 1 || b < a || b > ix->dlen) return NULL;
    *len = b - a;
    return ix->data + a;
}

static int32_t bias_of(uint32_t n) { return n < 1240 ? 107 : n < 33900 ? 1131 : 32768; }

/* --- DICT ---------------------------------------------------------------- */

/* Find `op` (12 xx encoded as 1200+xx) and copy its last `nv` operands.
 * Reals (op 30) are skipped, not decoded — no DICT value we need is one. */
static int dict_get(const uint8_t *p, uint32_t len, int op, int32_t *v, int nv) {
    int32_t st[STACK];
    int sp = 0;
    for (uint32_t i = 0; i < len; ) {
        uint8_t b = p[i];
        if (b >= 32 || b == 28 || b == 29 || b == 30) {
            int32_t val = 0;
            if (b == 30) {                       /* nibble-coded real: skip it */
                for (i++; i < len; i++)
                    if ((p[i] & 0xF) == 0xF || (p[i] >> 4) == 0xF) { i++; break; }
            } else if (b == 28)         { if (i + 2 >= len) return 0;
                                          val = (int16_t)(p[i+1] << 8 | p[i+2]); i += 3; }
            else if (b == 29)           { if (i + 4 >= len) return 0;
                                          val = (int32_t)roff(p + i + 1, 4); i += 5; }
            else if (b <= 246)          { val = (int32_t)b - 139; i++; }
            else if (b <= 250)          { if (i + 1 >= len) return 0;
                                          val = (b - 247) * 256 + p[i+1] + 108; i += 2; }
            else                        { if (i + 1 >= len) return 0;
                                          val = -((int32_t)(b - 251) * 256) - p[i+1] - 108; i += 2; }
            if (sp < STACK) st[sp++] = val;
            continue;
        }
        int cur = b;
        i++;
        if (b == 12) { if (i >= len) return 0; cur = 1200 + p[i++]; }
        if (cur == op) {
            if (sp < nv) return 0;
            for (int k = 0; k < nv; k++) v[k] = st[sp - nv + k];
            return 1;
        }
        sp = 0;
    }
    return 0;
}

int tt_cff_init(TtFont *f) {
    const uint8_t *cff = f->base + f->cff_off, *end = cff + f->cff_len;
    if (f->cff_len < 4) return 0;
    const uint8_t *p = cff + cff[2];             /* hdrSize */
    if (p < cff || p >= end) return 0;
    TtIndex names, tops, strs;
    if (!(p = idx_read(p, end, &names))) return 0;
    if (!(p = idx_read(p, end, &tops)))  return 0;
    if (!(p = idx_read(p, end, &strs)))  return 0;
    if (!idx_read(p, end, &f->gsubrs))   return 0;

    uint32_t tlen;
    const uint8_t *top = idx_get(&tops, 0, &tlen);
    if (!top) return 0;
    int32_t v[2];
    if (dict_get(top, tlen, 1230, v, 1)) {
        /* ponytail: CID-keyed CFF needs FDArray + FDSelect for per-glyph
         * private subrs; add it if a CJK OTF ever becomes the configured font. */
        fprintf(stderr, "wisp: tt: CID-keyed CFF not supported\n");
        return 0;
    }
    if (!dict_get(top, tlen, 17, v, 1) || v[0] < 0 || (uint32_t)v[0] >= f->cff_len)
        return 0;
    if (!idx_read(cff + v[0], end, &f->cs) || !f->cs.count) return 0;

    /* Private DICT is optional; without one there are simply no local subrs. */
    if (dict_get(top, tlen, 18, v, 2) && v[0] > 0 && v[1] > 0
        && (uint32_t)v[1] < f->cff_len && (uint32_t)v[0] <= f->cff_len - (uint32_t)v[1]) {
        const uint8_t *priv = cff + v[1];
        int32_t so;
        if (dict_get(priv, (uint32_t)v[0], 19, &so, 1) && so > 0
            && (uint32_t)so < f->cff_len - (uint32_t)v[1])
            idx_read(priv + so, end, &f->lsubrs);
    }
    f->gbias = bias_of(f->gsubrs.count);
    f->lbias = bias_of(f->lsubrs.count);
    f->cff_ok = 1;
    return 1;
}

/* --- Type 2 charstrings -------------------------------------------------- */

typedef struct {
    const TtFont *f;
    float x, y;
    float st[STACK];
    int   sp, nstem, got_width, bad;
} Ctx;

/* The first stack-clearing operator may carry an extra leading operand: the
 * glyph width. We take advances from hmtx, so it is only ever discarded — but
 * miscounting it shifts every following argument. `want` is the operator's own
 * arg count, or -1 for the even-count (stem) operators. */
static int wid(Ctx *c, int want) {
    if (c->got_width) return 0;
    c->got_width = 1;
    return want < 0 ? (c->sp & 1) : (c->sp > want);
}

static void moveto(Ctx *c, float dx, float dy) {
    c->x += dx; c->y += dy;
    tt_path_move(c->x, c->y);
}

static void lineto(Ctx *c, float dx, float dy) {
    c->x += dx; c->y += dy;
    tt_path_line(c->x, c->y);
}

static void curveto(Ctx *c, float dx1, float dy1, float dx2, float dy2,
                    float dx3, float dy3) {
    float x1 = c->x + dx1, y1 = c->y + dy1;
    float x2 = x1 + dx2,   y2 = y1 + dy2;
    c->x = x2 + dx3; c->y = y2 + dy3;
    tt_path_cubic(x1, y1, x2, y2, c->x, c->y);
}

static int run(Ctx *c, const uint8_t *p, uint32_t len, int depth);

static int call(Ctx *c, const TtIndex *ix, int32_t bias, int depth) {
    if (!c->sp) { c->bad = 1; return 0; }
    int32_t n = (int32_t)c->st[--c->sp] + bias;
    uint32_t sl;
    const uint8_t *s = n >= 0 ? idx_get(ix, (uint32_t)n, &sl) : NULL;
    if (!s) { c->bad = 1; return 0; }
    return run(c, s, sl, depth + 1);
}

/* StandardEncoding is ASCII over 32..126; seac's accents live above 192. Only
 * the accent block is tabulated — a seac base char is always a Latin letter. */
static uint32_t std_cp(int code) {
    static const uint16_t acc[] = {
        0x0060, 0x00B4, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x00A8,
        0,      0x02DA, 0x00B8, 0,      0x02DD, 0x02DB, 0x02C7,
    };
    if (code >= 32 && code <= 126) return (uint32_t)code;
    if (code >= 193 && code <= 207) return acc[code - 193];
    return 0;
}

/* `depth` continues the caller's subr depth: a seac must not reset it, or a
 * glyph whose seac resolves back to itself recurses until the stack dies. */
static int run_glyph(const TtFont *f, uint16_t gid, float x0, float y0, int depth);

/* Returns 0 only on a malformed charstring; 1 also covers endchar. */
static int run(Ctx *c, const uint8_t *p, uint32_t len, int depth) {
    if (depth > DEPTH) { c->bad = 1; return 0; }
    for (uint32_t i = 0; i < len; ) {
        uint8_t b = p[i];
        if (b >= 32 || b == 28) {                /* operand */
            float val;
            if (b == 28)      { if (i + 2 >= len) goto bad;
                                val = (int16_t)(p[i+1] << 8 | p[i+2]); i += 3; }
            else if (b <= 246){ val = (float)b - 139; i++; }
            else if (b <= 250){ if (i + 1 >= len) goto bad;
                                val = (float)((b - 247) * 256 + p[i+1] + 108); i += 2; }
            else if (b <= 254){ if (i + 1 >= len) goto bad;
                                val = -(float)((b - 251) * 256 + p[i+1] + 108); i += 2; }
            else              { if (i + 4 >= len) goto bad;
                                val = (float)(int32_t)roff(p + i + 1, 4) / 65536.0f; i += 5; }
            if (c->sp < STACK) c->st[c->sp++] = val;
            continue;
        }
        i++;
        int n, k;
        switch (b) {
        case 1: case 3: case 18: case 23:        /* h/v stem(hm) */
            c->nstem += (c->sp - wid(c, -1)) / 2;
            c->sp = 0;
            break;
        case 19: case 20:                        /* hintmask / cntrmask */
            c->nstem += (c->sp - wid(c, -1)) / 2;
            c->sp = 0;
            i += (uint32_t)(c->nstem + 7) / 8;
            if (i > len) goto bad;
            break;
        case 21: k = wid(c, 2);                  /* rmoveto */
            if (c->sp - k < 2) goto bad;
            moveto(c, c->st[k], c->st[k+1]); c->sp = 0;
            break;
        case 22: k = wid(c, 1);                  /* hmoveto */
            if (c->sp - k < 1) goto bad;
            moveto(c, c->st[k], 0); c->sp = 0;
            break;
        case 4: k = wid(c, 1);                   /* vmoveto */
            if (c->sp - k < 1) goto bad;
            moveto(c, 0, c->st[k]); c->sp = 0;
            break;
        case 5:                                  /* rlineto */
            for (k = 0; k + 1 < c->sp; k += 2) lineto(c, c->st[k], c->st[k+1]);
            c->sp = 0;
            break;
        case 6: case 7:                          /* hlineto / vlineto */
            for (k = 0, n = (b == 6); k < c->sp; k++, n = !n)
                lineto(c, n ? c->st[k] : 0, n ? 0 : c->st[k]);
            c->sp = 0;
            break;
        case 8:                                  /* rrcurveto */
            for (k = 0; k + 5 < c->sp; k += 6)
                curveto(c, c->st[k], c->st[k+1], c->st[k+2], c->st[k+3],
                           c->st[k+4], c->st[k+5]);
            c->sp = 0;
            break;
        case 24:                                 /* rcurveline */
            for (k = 0; c->sp - k >= 8; k += 6)
                curveto(c, c->st[k], c->st[k+1], c->st[k+2], c->st[k+3],
                           c->st[k+4], c->st[k+5]);
            if (c->sp - k >= 2) lineto(c, c->st[k], c->st[k+1]);
            c->sp = 0;
            break;
        case 25:                                 /* rlinecurve */
            for (k = 0; c->sp - k >= 8; k += 2) lineto(c, c->st[k], c->st[k+1]);
            if (c->sp - k >= 6)
                curveto(c, c->st[k], c->st[k+1], c->st[k+2], c->st[k+3],
                           c->st[k+4], c->st[k+5]);
            c->sp = 0;
            break;
        case 26: case 27: {                      /* vvcurveto / hhcurveto */
            k = (c->sp & 1) ? 1 : 0;
            float d1 = k ? c->st[0] : 0;         /* odd leading dx1 (or dy1) */
            for (; k + 3 < c->sp; k += 4) {
                if (b == 26) curveto(c, d1, c->st[k], c->st[k+1], c->st[k+2], 0, c->st[k+3]);
                else         curveto(c, c->st[k], d1, c->st[k+1], c->st[k+2], c->st[k+3], 0);
                d1 = 0;
            }
            c->sp = 0;
            break;
        }
        case 30: case 31: {                      /* vhcurveto / hvcurveto */
            int horiz = (b == 31);
            for (k = 0; k + 3 < c->sp; k += 4, horiz = !horiz) {
                /* The last curve may carry a 5th value: the free coordinate of
                 * its end point, which is otherwise pinned to the axis. */
                float last = (c->sp - k == 5) ? c->st[k+4] : 0;
                if (horiz) curveto(c, c->st[k], 0, c->st[k+1], c->st[k+2], last, c->st[k+3]);
                else       curveto(c, 0, c->st[k], c->st[k+1], c->st[k+2], c->st[k+3], last);
            }
            c->sp = 0;
            break;
        }
        case 10: if (!call(c, &c->f->lsubrs, c->f->lbias, depth)) return 0; break;
        case 29: if (!call(c, &c->f->gsubrs, c->f->gbias, depth)) return 0; break;
        case 11: return 1;                       /* return */
        case 14:                                 /* endchar */
            k = (c->sp == 1 || c->sp == 5);
            if (c->sp - k >= 4) {                /* seac: accent over base */
                uint16_t bg = tt_gid(c->f, std_cp((int)c->st[k+2]));
                uint16_t ag = tt_gid(c->f, std_cp((int)c->st[k+3]));
                tt_path_close();
                if (bg) run_glyph(c->f, bg, 0, 0, depth + 1);
                if (ag) run_glyph(c->f, ag, c->st[k], c->st[k+1], depth + 1);
            }
            tt_path_close();
            return 1;
        case 12: {
            if (i >= len) goto bad;
            uint8_t e = p[i++];
            /* Flex variants are four already-implemented curves with the
             * flex-depth argument dropped; the rest (arithmetic, storage,
             * random) never appear in shipped fonts — clear and move on. */
            const float *s = c->st;
            if (e == 35 && c->sp >= 13) {                        /* flex */
                curveto(c, s[0], s[1], s[2], s[3], s[4], s[5]);
                curveto(c, s[6], s[7], s[8], s[9], s[10], s[11]);
            } else if (e == 34 && c->sp >= 7) {                  /* hflex */
                curveto(c, s[0], 0, s[1], s[2], s[3], 0);
                curveto(c, s[4], 0, s[5], -s[2], s[6], 0);
            } else if (e == 36 && c->sp >= 9) {                  /* hflex1 */
                float sy = c->y;
                curveto(c, s[0], s[1], s[2], s[3], s[4], 0);
                curveto(c, s[5], 0, s[6], s[7], s[8], sy - (c->y + s[7]));
            } else if (e == 37 && c->sp >= 11) {                 /* flex1 */
                float sx = c->x, sy = c->y;
                float dx = s[0] + s[2] + s[4] + s[6] + s[8];
                float dy = s[1] + s[3] + s[5] + s[7] + s[9];
                curveto(c, s[0], s[1], s[2], s[3], s[4], s[5]);
                /* The last point returns to the start on the axis that moved
                 * least; d6 is the free coordinate on the other one. */
                if (fabsf(dx) > fabsf(dy))
                    curveto(c, s[6], s[7], s[8], s[9], s[10], sy - (c->y + s[7] + s[9]));
                else
                    curveto(c, s[6], s[7], s[8], s[9], sx - (c->x + s[6] + s[8]), s[10]);
            }
            c->sp = 0;
            break;
        }
        default:
            c->sp = 0;                           /* unknown operator: ignore */
            break;
        }
        if (c->bad) return 0;
    }
    return 1;
bad:
    c->bad = 1;
    return 0;
}

static int run_glyph(const TtFont *f, uint16_t gid, float x0, float y0, int depth) {
    uint32_t len;
    const uint8_t *cs = idx_get(&f->cs, gid, &len);
    if (!cs) return 0;
    Ctx c = { .f = f, .x = x0, .y = y0 };
    return run(&c, cs, len, depth) && !c.bad;
}

int tt_cff_glyph(const TtFont *f, uint16_t gid) {
    if (!f->cff_ok) return 0;
    return run_glyph(f, gid, 0, 0, 0);
}

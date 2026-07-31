/* gen_lock.h — the `lock { frame … text … }` body lowered to the LockEl table
 * src/lock.c walks. The lock has no flex layout and no sources: every element
 * is free-positioned and its text is a template over a closed token set, so
 * this is a table emitter, not a code emitter. */
#include "codegen_internal.h"

#include <string.h>

/* Runtime token bytes; must match the LT_* enum in wisp.h. */
#define LT_DOTS   1
#define LT_COUNT  2
#define LT_LAYOUT 3
#define LT_PROMPT 4
#define LT_TIME   5

/* show= conditions; must match the LSHOW_* enum in wisp.h. */
#define LSHOW_ALWAYS     0
#define LSHOW_TYPING     1
#define LSHOW_WRONG      2
#define LSHOW_CAPS       3
#define LSHOW_VERIFYING  4
#define LSHOW_LAYOUT_ALT 5
#define LSHOW_EMPTY      6
#define LSHOW_THROTTLED  7
#define LSHOW_LOCKED_OUT 8
#define LSHOW_NEG     0x80

static Expr *elem_prop(LockElem *e, const char *name) {
    size_t L = strlen(name);
    for (int i = 0; i < e->n; i++)
        if (e->props[i]->nlen == L && memcmp(e->props[i]->name, name, L) == 0)
            return e->props[i]->val;
    return NULL;
}

static int ident_is(Expr *e, const char *s) {
    return e && e->kind == EX_IDENT && e->ident.n == strlen(s) &&
           memcmp(e->ident.s, s, e->ident.n) == 0;
}

static int lock_token(Expr *e) {
    if (ident_is(e, "dots"))   return LT_DOTS;
    if (ident_is(e, "count"))  return LT_COUNT;
    if (ident_is(e, "layout")) return LT_LAYOUT;
    if (ident_is(e, "prompt")) return LT_PROMPT;
    if (ident_is(e, "time"))   return LT_TIME;
    return 0;
}

static int lock_show(Expr *e) {
    int neg = 0;
    if (e && e->kind == EX_UN && e->un.op == OP_NOT) { neg = LSHOW_NEG; e = e->un.e; }
    if (!e) return LSHOW_ALWAYS;
    if (ident_is(e, "always"))     return LSHOW_ALWAYS | neg;
    if (ident_is(e, "typing"))     return LSHOW_TYPING | neg;
    if (ident_is(e, "wrong"))      return LSHOW_WRONG | neg;
    if (ident_is(e, "caps"))       return LSHOW_CAPS | neg;
    if (ident_is(e, "verifying"))  return LSHOW_VERIFYING | neg;
    if (ident_is(e, "layout_alt")) return LSHOW_LAYOUT_ALT | neg;
    if (ident_is(e, "empty"))      return LSHOW_EMPTY | neg;
    if (ident_is(e, "throttled"))  return LSHOW_THROTTLED | neg;
    if (ident_is(e, "locked_out")) return LSHOW_LOCKED_OUT | neg;
    diag_error(e->loc, "lock show= must be always/typing/empty/wrong/caps/verifying/layout_alt/throttled/locked_out, optionally negated with '!'");
    return LSHOW_ALWAYS;
}

/* Emit `s` as the body of a C string literal, octal-escaping the token bytes
 * and anything else the compiler would misread. */
static void emit_cstr_body(FILE *o, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') fprintf(o, "\\%c", c);
        else if (c >= 0x20 && c < 0x7f) fputc(c, o);
        else fprintf(o, "\\%03o", c);
    }
}

/* A lock text template: literal bytes with an LT_* byte per {token}. */
static void emit_template(FILE *o, Expr *e) {
    fputc('"', o);
    if (!e) { /* nothing */ }
    else if (e->kind == EX_STRING) {
        emit_cstr_body(o, e->str.s, e->str.n);
    } else if (e->kind == EX_INTERP) {
        for (int i = 0; i < e->interp.nparts; i++) {
            InterpPart *p = &e->interp.parts[i];
            if (!p->is_expr) { emit_cstr_body(o, p->lit, p->llen); continue; }
            int t = lock_token(p->expr);
            if (!t) {
                diag_error(e->loc, "lock text may only interpolate {dots} {count} {layout} {prompt} {time}");
                continue;
            }
            fprintf(o, "\\%03o", t);
        }
    } else {
        diag_error(e->loc, "lock text= must be a string");
    }
    fputc('"', o);
}

static void emit_el(FILE *o, LockElem *e, CGCtx *ctx) {
    Expr *anc = elem_prop(e, "anchor");
    int anchor = anc ? eval_anchor(anc) : 0;
    static const char *kinds[] = { "LEL_FRAME", "LEL_TEXT", "LEL_RING" };
    int ring = e->kind == LK_RING;
    /* Ring geometry borrows the box slots: w = thickness, h = segments. */
    int w = ring ? eval_int(elem_prop(e, "thickness"), 8)
                 : eval_int(elem_prop(e, "width"), 0);
    int h = ring ? eval_int(elem_prop(e, "segments"), 1)
                 : eval_int(elem_prop(e, "height"), 0);
    if (ring && h < 1) h = 1;            /* sema already rejected literals */
    /* Rings default to the block's `ring` colour; text/frames have no such
     * fallback, so they keep the opaque-white default. */
    char fg[16];
    const char *fgs = "LOCK_RING";
    if (!ring || elem_prop(e, "fg")) {
        snprintf(fg, sizeof fg, "0x%08xu",
                 (unsigned)eval_color_ctx(ctx, elem_prop(e, "fg"), 0xffffffffu));
        fgs = fg;
    }
    unsigned hl = ring ? (unsigned)eval_color_ctx(ctx, elem_prop(e, "highlight"), 0) : 0;
    unsigned bs = ring ? (unsigned)eval_color_ctx(ctx, elem_prop(e, "highlight_bs"), hl) : 0;
    fprintf(o, "    { %s, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, 0x%08xu, %s, 0x%08xu, "
               "0x%08xu, 0x%08xu, 0x%08xu, ",
            kinds[e->kind],
            anchor, lock_show(elem_prop(e, "show")),
            eval_int(elem_prop(e, "x"), 0), eval_int(elem_prop(e, "y"), 0), w, h,
            eval_int(elem_prop(e, "radius"), ring ? 50 : 0),
            eval_int(elem_prop(e, "border_width"), elem_prop(e, "border") ? 1 : 0),
            eval_int(elem_prop(e, "font_size"), 0),
            ring ? eval_int(elem_prop(e, "gap"), 0) : 0,
            ring ? eval_int(elem_prop(e, "highlight_arc"), 60) : 0,
            (unsigned)eval_color_ctx(ctx, elem_prop(e, "bg"), 0),
            fgs,
            (unsigned)eval_color_ctx(ctx, elem_prop(e, "border"), 0),
            hl, bs,
            ring ? (unsigned)eval_color_ctx(ctx, elem_prop(e, "separator"), 0) : 0);
    emit_template(o, elem_prop(e, "text"));
    fputs(", ", o);
    Expr *fmt = elem_prop(e, "format");
    if (fmt && fmt->kind == EX_STRING) {
        fputc('"', o);
        emit_cstr_body(o, fmt->str.s, fmt->str.n);
        fputc('"', o);
    } else {
        fputs("\"%H:%M\"", o);
    }
    fputs(" },\n", o);
}

/* Does any element's template pull LT_TIME, and does its format want seconds?
 * The lock only repaints on input, so a clock has to arm a timer. */
static void scan_clock(LockElem **els, int n, int *has, int *secs) {
    *has = *secs = 0;
    for (int i = 0; i < n; i++) {
        Expr *t = elem_prop(els[i], "text");
        if (!t || t->kind != EX_INTERP) continue;
        for (int j = 0; j < t->interp.nparts; j++) {
            InterpPart *p = &t->interp.parts[j];
            if (!p->is_expr || lock_token(p->expr) != LT_TIME) continue;
            *has = 1;
            Expr *f = elem_prop(els[i], "format");
            if (!f || f->kind != EX_STRING) continue;
            for (size_t k = 0; k + 1 < f->str.n; k++)
                if (f->str.s[k] == '%' &&
                    (f->str.s[k+1] == 'S' || f->str.s[k+1] == 'T' || f->str.s[k+1] == 's'))
                    *secs = 1;
        }
    }
}

/* No declared elements → the layout every pre-frames config had: dots in the
 * middle, a wrong line and a CAPS line under it. Backward compatibility lives
 * in the compiler, so the runtime carries exactly one layout path. */
static void emit_default_els(FILE *o, int fs) {
    fprintf(o,
        "    { LEL_TEXT, 0, LSHOW_TYPING, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, LOCK_FG, 0, 0, 0, 0, \"\\001\", \"%%H:%%M\" },\n"
        "    { LEL_TEXT, 0, LSHOW_WRONG, 0, %d, 0, 0, 0, 0, 0, 0, 0, 0, LOCK_RING_WRONG, 0, 0, 0, 0, \"wrong password\", \"%%H:%%M\" },\n"
        "    { LEL_TEXT, 0, LSHOW_CAPS, 0, %d, 0, 0, 0, 0, 0, 0, 0, 0, LOCK_CAPS, 0, 0, 0, 0, \"CAPS\", \"%%H:%%M\" },\n",
        fs * 3 / 2, fs * 3);
}

void emit_lock(FILE *o, Unit *u, CGCtx *ctx) {
    Decl *lk = NULL;
    for (int i = 0; i < u->n; i++)
        if (u->decls[i]->kind == D_LOCK) { lk = u->decls[i]; break; }

    fputs("/* Generated by wispc. Do not edit. */\n", o);
    fputs("#ifndef WISP_GEN_LOCK_H\n#define WISP_GEN_LOCK_H\n\n", o);

    int nels = lk ? lk->block.nels : 0;
    fputs("static const LockEl lock_els[] = {\n", o);
    if (nels)
        for (int i = 0; i < nels; i++) emit_el(o, lk->block.els[i], ctx);
    else {
        int fs = 14;
        for (int i = 0; lk && i < lk->block.n; i++) {
            Prop *p = lk->block.props[i];
            if (p->nlen == 9 && memcmp(p->name, "font_size", 9) == 0)
                fs = eval_int(p->val, 14);
        }
        emit_default_els(o, fs);
        nels = 3;
    }
    fputs("};\n", o);
    fprintf(o, "#define LOCK_N_ELS %d\n", nels);
    fputs("\n#endif\n", o);
}

/* Repaint period a declared {time} needs, 0 when the lock has no clock. Lives
 * in gen_overrides.h (not the table header) so lock-main.c can arm the timer
 * without pulling in the element array. */
int lock_clock_ms(Unit *u) {
    Decl *lk = NULL;
    for (int i = 0; i < u->n; i++)
        if (u->decls[i]->kind == D_LOCK) { lk = u->decls[i]; break; }
    if (!lk || !lk->block.nels) return 0;
    int has = 0, secs = 0;
    scan_clock(lk->block.els, lk->block.nels, &has, &secs);
    return has ? (secs ? 1000 : 60000) : 0;
}

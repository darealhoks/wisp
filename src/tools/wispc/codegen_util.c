/* wispc codegen — common helpers (split from codegen.c). */
#define _GNU_SOURCE   /* fopencookie: track the generated file's own line count */
#include "codegen_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================ */
/* #line source mapping (Phase 4)                                */
/* ------------------------------------------------------------ */
/* So a residual gcc/-Werror error on generated C points at the  */
/* .wisp line, not gen_surfaces.c:NNN. cg_line() re-bases the     */
/* next emitted line onto the .wisp source; cg_line_reset() puts  */
/* it back onto the generated file so glue-code errors still name */
/* the generated file. The reset needs the generated file's own  */
/* current line number, which nothing else tracks — so gen files  */
/* are opened through cg_open(), a fopencookie stream that counts  */
/* newlines as they flush. Only one gen file is emitted at a time  */
/* (codegen.c open→emit→close, serially), so a single active-      */
/* stream pointer suffices.                                        */
/* ponytail: single active stream; if codegen ever emits two gen  */
/* files concurrently this needs a FILE*→state map instead.        */
int cg_line_map = 1;   /* --no-line-map clears it */

typedef struct { FILE *raw; FILE *wrap; long nl; char path[1024]; } CgStream;
static CgStream *cg_cur;

static ssize_t cg_cookie_write(void *c, const char *buf, size_t n) {
    CgStream *s = c;
    for (size_t i = 0; i < n; i++) if (buf[i] == '\n') s->nl++;
    return (ssize_t)fwrite(buf, 1, n, s->raw);
}
static int cg_cookie_close(void *c) {
    CgStream *s = c;
    int r = fclose(s->raw);
    if (cg_cur == s) cg_cur = NULL;
    free(s);
    return r;
}

FILE *cg_open(const char *path) {
    FILE *raw = fopen(path, "w");
    if (!raw) return NULL;
    if (!cg_line_map) return raw;   /* no tracking wanted → plain stream */
    CgStream *s = calloc(1, sizeof *s);
    if (!s) return raw;
    s->raw = raw;
    snprintf(s->path, sizeof s->path, "%s", path);
    cookie_io_functions_t io = { NULL, cg_cookie_write, NULL, cg_cookie_close };
    s->wrap = fopencookie(s, "w", io);
    if (!s->wrap) { free(s); return raw; }
    /* Unbuffered so cg_cur->nl is current whenever cg_line_reset() reads it,
     * without an fflush dance. Gen output is small; speed is a non-issue. */
    setvbuf(s->wrap, NULL, _IONBF, 0);
    cg_cur = s;
    return s->wrap;
}

void cg_line(FILE *o, Loc loc) {
    if (!cg_line_map || !cg_cur || cg_cur->wrap != o || !loc.file || loc.line <= 0)
        return;
    fprintf(o, "#line %d \"%s\"\n", loc.line, loc.file);
}
void cg_line_reset(FILE *o) {
    if (!cg_line_map || !cg_cur || cg_cur->wrap != o) return;
    /* This directive lands on physical line nl+1; the code after it on nl+2,
     * which is the number it must report. */
    fprintf(o, "#line %ld \"%s\"\n", cg_cur->nl + 2, cg_cur->path);
}

/* ============================================================ */
/* Common helpers                                                */
/* ============================================================ */

const char *sname(const char *s, size_t n) {
    static char buf[128];
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, s, n); buf[n] = 0;
    return buf;
}
char *strndup0(const char *s, size_t n) {
    char *r = malloc(n + 1); memcpy(r, s, n); r[n] = 0; return r;
}

/* Anchor / layer / align identifier eval (compile-time constants only). */
int eval_anchor(Expr *e) {
    if (!e) return -1;
    if (e->kind == EX_IDENT) {
        const char *s = e->ident.s; size_t n = e->ident.n;
        #define M(lit, v) if (n == sizeof(lit)-1 && memcmp(s, lit, n) == 0) return v
        M("top", 1); M("bottom", 2); M("left", 4); M("right", 8);
        #undef M
        return -1;
    }
    if (e->kind == EX_BIN && e->bin.op == OP_BITOR) {
        int a = eval_anchor(e->bin.l), b = eval_anchor(e->bin.r);
        if (a < 0 || b < 0) return -1;
        return a | b;
    }
    if (e->kind == EX_INT) return (int)e->i;
    return -1;
}
int eval_layer(Expr *e) {
    if (!e) return -1;
    if (e->kind == EX_IDENT) {
        const char *s = e->ident.s; size_t n = e->ident.n;
        #define M(lit, v) if (n == sizeof(lit)-1 && memcmp(s, lit, n) == 0) return v
        M("background", 0); M("bottom", 1); M("top", 2); M("overlay", 3);
        #undef M
    }
    if (e->kind == EX_INT) return (int)e->i;
    return -1;
}
int eval_int(Expr *e, int dflt) {
    if (!e) return dflt;
    if (e->kind == EX_INT) return (int)e->i;
    /* Negative literals parse as EX_UN(OP_NEG, EX_INT). */
    if (e->kind == EX_UN && e->un.op == OP_NEG && e->un.e && e->un.e->kind == EX_INT)
        return -(int)e->un.e->i;
    /* `lo..hi` evaluates to lo for the static path; callers that care about
     * the animated endpoint use eval_int_range(). */
    if (e->kind == EX_RANGE) return eval_int(e->range.lo, dflt);
    return dflt;
}

/* Decompose `lo..hi` into (lo,hi). Returns 1 if e is a range, 0 otherwise
 * (in which case *lo == *hi == eval_int(e, dflt)). Endpoints must be int
 * literals or negative-int literals (same shape eval_int accepts). */
int eval_int_range(Expr *e, int *lo, int *hi, int dflt) {
    if (e && e->kind == EX_RANGE) {
        *lo = eval_int(e->range.lo, dflt);
        *hi = eval_int(e->range.hi, dflt);
        return 1;
    }
    *lo = *hi = eval_int(e, dflt);
    return 0;
}
uint32_t eval_color(Expr *e, uint32_t dflt) {
    if (!e) return dflt;
    if (e->kind == EX_COLOR) return e->color;
    return dflt;
}
/* Like eval_color but resolves a `const NAME = #color` reference to its value,
 * so compile-time color props (shadows etc.) accept consts, not just literals.
 * Follows const→const chains; the hop cap breaks a cycle (`const A = B; B = A`).
 *
 * `dflt` is for an ABSENT prop only. A prop that IS given but does not fold is
 * a hard error, never a silent default: every caller gates its fill on
 * `color & 0xff000000u`, so a defaulted 0 means alpha 0 means the fill is
 * silently never emitted — a transparent widget with no diagnostic. */
uint32_t eval_color_ctx(CGCtx *ctx, Expr *e, uint32_t dflt) {
    if (!e) return dflt;
    Expr *r = e;
    for (int hop = 0; hop < 16; hop++) {
        if (r->kind == EX_COLOR) return r->color;
        if (r->kind != EX_IDENT || !ctx) break;
        Decl *k = find_konst(ctx, r->ident.s, r->ident.n);
        if (!k || !k->konst.val) break;
        r = k->konst.val;
    }
    if (e->kind == EX_IDENT)
        diag_error(e->loc, "`%s` does not resolve to a color literal here; this "
                           "prop is compile-time and needs a #aarrggbb or a const chain ending in one",
                   sname(e->ident.s, e->ident.n));
    else
        diag_error(e->loc, "compile-time color prop needs a #aarrggbb literal or a const naming one");
    return dflt;
}
/* Generalized to "start / end / center" so the same enum works for both
 * horizontal (left/right) and vertical (top/bottom) layouts. */
Align eval_align(Expr *e) {
    if (e && e->kind == EX_IDENT) {
        const char *s = e->ident.s; size_t n = e->ident.n;
        if (n == 5 && memcmp(s, "right",  5) == 0) return ALIGN_END;
        if (n == 6 && memcmp(s, "bottom", 6) == 0) return ALIGN_END;
        if (n == 6 && memcmp(s, "center", 6) == 0) return ALIGN_CENTER;
        /* left / top / start fall through to ALIGN_START */
    }
    return ALIGN_START;
}
int surface_is_vertical(Decl *sur) {
    Expr *p = NULL;
    for (int i = 0; i < sur->surface.n; i++) {
        SBody *b = &sur->surface.items[i];
        if (b->kind == SB_PROP && b->prop->nlen == 4 &&
            memcmp(b->prop->name, "axis", 4) == 0) { p = b->prop->val; break; }
    }
    if (p && p->kind == EX_IDENT && p->ident.n == 8 &&
        memcmp(p->ident.s, "vertical", 8) == 0) return 1;
    return 0;
}

Expr *surface_prop(Decl *sur, const char *name) {
    for (int i = 0; i < sur->surface.n; i++) {
        SBody *b = &sur->surface.items[i];
        if (b->kind == SB_PROP) {
            Prop *p = b->prop;
            if (strlen(name) == p->nlen && memcmp(p->name, name, p->nlen) == 0)
                return p->val;
        }
    }
    return NULL;
}
Expr *widget_prop(Widget *w, const char *name) {
    for (int i = 0; i < w->nitems; i++) {
        if (w->items[i].kind == WB_PROP) {
            Prop *p = w->items[i].prop;
            if (strlen(name) == p->nlen && memcmp(p->name, name, p->nlen) == 0)
                return p->val;
        }
    }
    return NULL;
}
WBody *widget_onclick(Widget *w) {
    for (int i = 0; i < w->nitems; i++)
        if (w->items[i].kind == WB_ONCLICK) return &w->items[i];
    return NULL;
}
/* Hit-testing gate: a widget needs a click rect if either button binds. */
int widget_clickable(Widget *w) {
    return widget_onclick(w) || widget_handler(w, WB_ONRCLICK)
        || widget_handler(w, WB_ONMCLICK);
}
WBody *widget_handler(Widget *w, WBKind k) {
    for (int i = 0; i < w->nitems; i++)
        if (w->items[i].kind == k) return &w->items[i];
    return NULL;
}
/* A widget is a slider when it has a `slider` marker prop (val == NULL or
 * EX_BOOL true). */
int widget_is_slider(Widget *w) {
    for (int i = 0; i < w->nitems; i++) {
        if (w->items[i].kind != WB_PROP) continue;
        Prop *p = w->items[i].prop;
        if (p->nlen == 6 && memcmp(p->name, "slider", 6) == 0) {
            if (!p->val) return 1;
            if (p->val->kind == EX_BOOL) return p->val->b ? 1 : 0;
            return 1;
        }
    }
    return 0;
}
/* Step 6.1: declarative transition props. A widget carrying `transition_bg = 200ms`
 * (or _fg / _border) makes that colour interpolate on change. Easing defaults to
 * EASE_OUT; override with `transition_easing = linear|ease_in|ease_out|ease_in_out`. The actual
 * tween storage + dispatch is emitted alongside render_<surf>. */
int transition_dur(Widget *wd, const char *which) {
    char buf[40]; snprintf(buf, sizeof buf, "transition_%s", which);
    return eval_int(widget_prop(wd, buf), 0);
}
const char *transition_easing_id(Widget *wd) {
    Expr *e = widget_prop(wd, "transition_easing");
    if (!e || e->kind != EX_IDENT) return "EASE_OUT";
    const char *s = e->ident.s; size_t n = e->ident.n;
    if (n == 6  && !memcmp(s, "linear",      6))  return "EASE_LINEAR";
    if (n == 7  && !memcmp(s, "ease_in",     7))  return "EASE_IN";
    if (n == 8  && !memcmp(s, "ease_out",    8))  return "EASE_OUT";
    if (n == 11 && !memcmp(s, "ease_in_out", 11)) return "EASE_IN_OUT";
    return "EASE_OUT";
}
/* Step 6.2: like transition_easing_id but reads surface-level `reveal_easing`. */
const char *surface_easing_id(Decl *sur, const char *prop_name) {
    Expr *e = NULL;
    for (int i = 0; i < sur->surface.n; i++) {
        SBody *b = &sur->surface.items[i];
        if (b->kind == SB_PROP && (size_t)b->prop->nlen == strlen(prop_name) &&
            memcmp(b->prop->name, prop_name, (size_t)b->prop->nlen) == 0) {
            e = b->prop->val; break;
        }
    }
    if (!e || e->kind != EX_IDENT) return "EASE_OUT";
    const char *s = e->ident.s; size_t n = e->ident.n;
    if (n == 6  && !memcmp(s, "linear",      6))  return "EASE_LINEAR";
    if (n == 7  && !memcmp(s, "ease_in",     7))  return "EASE_IN";
    if (n == 8  && !memcmp(s, "ease_out",    8))  return "EASE_OUT";
    if (n == 11 && !memcmp(s, "ease_in_out", 11)) return "EASE_IN_OUT";
    return "EASE_OUT";
}

int item_has_any_transition(Widget *wd) {
    return transition_dur(wd, "bg") > 0
        || transition_dur(wd, "fg") > 0
        || transition_dur(wd, "border") > 0
        || transition_dur(wd, "size") > 0;
}

/* Step 6.5: the tween slot pattern, emitted once per property instead of once
 * per property *kind*: seed on first render, retarget when the declared value
 * changes (anim_start_* restarts from the current value — the CSS semantic),
 * always read `.cur`. Without WISP_HAS_ANIM the property just snaps. */
void emit_color_slot(FILE *o, const char *ind, const char *var, const char *slot,
                     const char *tgt_expr, const SlotCtx *sc, int dur) {
    if (dur <= 0) { fprintf(o, "%suint32_t %s = (uint32_t)(%s);\n", ind, var, tgt_expr); return; }
    fprintf(o, "%suint32_t __%s_tgt = (uint32_t)(%s); uint32_t %s;\n", ind, var, tgt_expr, var);
    fprintf(o, "%s#ifdef WISP_HAS_ANIM\n", ind);
    fprintf(o, "%s{ TransSlot *__s = &%s_tr%d_%s[__wi][%s];\n", ind, sc->surf, sc->item, slot, sc->idx);
    fprintf(o, "%s  if (!__s->has) { __s->cur = __%s_tgt; __s->last = __%s_tgt; __s->has = 1; }\n", ind, var, var);
    fprintf(o, "%s  else if (__s->last != __%s_tgt) { anim_start_color(&__s->cur, __s->cur, __%s_tgt, %d, %s, NULL, w, NULL, NULL); __s->last = __%s_tgt; }\n",
            ind, var, var, dur, sc->ease, var);
    fprintf(o, "%s  %s = __s->cur; }\n", ind, var);
    fprintf(o, "%s#else\n%s  %s = __%s_tgt;\n%s#endif\n", ind, ind, var, var, ind);
}

/* Same, for an already-declared int size var tweened through a SizeSlot.
 * `even` quantises to even pixels — see the cross-axis note in codegen_items.c. */
void emit_size_slot(FILE *o, const char *ind, const char *var, const char *slot,
                    const SlotCtx *sc, int dur, int even) {
    if (dur <= 0) return;
    fprintf(o, "%s#ifdef WISP_HAS_ANIM\n", ind);
    fprintf(o, "%s{ SizeSlot *__s = &%s_tr%d_%s[__wi][%s];\n", ind, sc->surf, sc->item, slot, sc->idx);
    fprintf(o, "%s  if (!__s->has) { __s->cur = %s; __s->last = %s; __s->has = 1; }\n", ind, var, var);
    fprintf(o, "%s  else if (__s->last != %s) { anim_start_num(&__s->cur, ANIM_T_FLOAT, __s->cur, %s, %d, %s, NULL, w, NULL, NULL); __s->last = %s; }\n",
            ind, var, var, dur, sc->ease, var);
    fprintf(o, "%s  %s = anim_px(__s->cur)%s; }\n", ind, var, even ? " & ~1" : "");
    fprintf(o, "%s#endif\n", ind);
}

/* The storage behind the above — slot names are the contract between the two.
 * Outer dim = the surface's widget-registry cap: a surface instantiates once
 * per output, and sharing tween state across outputs makes two bars fight
 * over one animation (retarget ping-pong on every render). */
void emit_item_slot_decls(FILE *o, Widget *wd, const char *nm, int idx, int slots, int nwid) {
    if (transition_dur(wd, "bg")     > 0) fprintf(o, "static TransSlot %s_tr%d_bg[%d][%d];\n",     nm, idx, nwid, slots);
    if (transition_dur(wd, "fg")     > 0) fprintf(o, "static TransSlot %s_tr%d_fg[%d][%d];\n",     nm, idx, nwid, slots);
    if (transition_dur(wd, "border") > 0) fprintf(o, "static TransSlot %s_tr%d_border[%d][%d];\n", nm, idx, nwid, slots);
    if (transition_dur(wd, "size")   > 0) fprintf(o, "static SizeSlot %s_tr%d_tw[%d][%d], %s_tr%d_ch[%d][%d];\n", nm, idx, nwid, slots, nm, idx, nwid, slots);
}

/* Step 6.3: enter_anim / exit_anim on a widget's `visible` expression. */
int widget_enter_ms(Widget *wd) { return eval_int(widget_prop(wd, "enter_anim"), 0); }
int widget_exit_ms (Widget *wd) { return eval_int(widget_prop(wd, "exit_anim"),  0); }
int widget_has_vis_anim(Widget *wd) {
    return widget_prop(wd, "visible") &&
           (widget_enter_ms(wd) > 0 || widget_exit_ms(wd) > 0);
}
const char *widget_easing_id(Widget *wd, const char *prop_name) {
    Expr *e = widget_prop(wd, prop_name);
    if (!e || e->kind != EX_IDENT) return "EASE_OUT";
    const char *s = e->ident.s; size_t n = e->ident.n;
    if (n == 6  && !memcmp(s, "linear",      6))  return "EASE_LINEAR";
    if (n == 7  && !memcmp(s, "ease_in",     7))  return "EASE_IN";
    if (n == 8  && !memcmp(s, "ease_out",    8))  return "EASE_OUT";
    if (n == 11 && !memcmp(s, "ease_in_out", 11)) return "EASE_IN_OUT";
    return "EASE_OUT";
}

/* `orientation = vertical` (else horizontal). */
int widget_is_vertical(Widget *w) {
    Expr *e = widget_prop(w, "orientation");
    if (e && e->kind == EX_IDENT && e->ident.n == 8 &&
        memcmp(e->ident.s, "vertical", 8) == 0) return 1;
    return 0;
}

/* Compile-time double from a numeric literal (for value_scale etc.). */
double eval_double(Expr *e, double dflt) {
    if (!e) return dflt;
    if (e->kind == EX_FLOAT) return e->f;
    if (e->kind == EX_INT)   return (double)e->i;
    if (e->kind == EX_UN && e->un.op == OP_NEG && e->un.e) return -eval_double(e->un.e, 0);
    return dflt;
}

/* Presence / truthiness of a marker-style prop. Distinguishes a bare marker
 * (`show_value;`) and `= true` / `= 1` from absence, which plain widget_prop()
 * can't (it returns NULL for both an absent prop and a marker). */
int widget_flag(Widget *w, const char *name) {
    size_t L = strlen(name);
    for (int i = 0; i < w->nitems; i++) {
        if (w->items[i].kind != WB_PROP) continue;
        Prop *p = w->items[i].prop;
        if (p->nlen != L || memcmp(p->name, name, L) != 0) continue;
        if (!p->val) return 1;
        if (p->val->kind == EX_BOOL) return p->val->b ? 1 : 0;
        if (p->val->kind == EX_INT)  return p->val->i ? 1 : 0;
        return 1;
    }
    return 0;
}

/* `thumb_shape` enum → draw_slider shape id (0 bar, 1 pill, 2 circle, 3 none).
 * Default 0 (bar) preserves the legacy square thumb. */
int widget_thumb_shape(Widget *w) {
    Expr *e = widget_prop(w, "thumb_shape");
    if (!e || e->kind != EX_IDENT) return 0;
    const char *s = e->ident.s; size_t n = e->ident.n;
    if (n == 3 && !memcmp(s, "bar",    3)) return 0;
    if (n == 4 && !memcmp(s, "pill",   4)) return 1;
    if (n == 4 && !memcmp(s, "disc",   4)) return 2;
    if (n == 4 && !memcmp(s, "knob",   4)) return 2;
    if (n == 4 && !memcmp(s, "none",   4)) return 3;
    if (n == 6 && !memcmp(s, "circle", 6)) return 2;
    return 0;
}

/* value_align on a slider's value label: 0 start / 1 end / 2 center along the
 * slider's main axis. Default 1 (end — below a vertical track, right of a
 * horizontal one). */
int widget_value_align(Widget *w) {
    Expr *e = widget_prop(w, "value_align");
    if (!e || e->kind != EX_IDENT) return 1;
    const char *s = e->ident.s; size_t n = e->ident.n;
    if (n == 5 && !memcmp(s, "start",  5)) return 0;
    if (n == 6 && !memcmp(s, "center", 6)) return 2;
    if (n == 3 && !memcmp(s, "top",    3)) return 0;
    if (n == 4 && !memcmp(s, "left",   4)) return 0;
    return 1;
}

/* Every per-surface / per-region widget registry (the `__<name>_widgets[]` +
 * `__<name>_nw` pairs) gets its base name collected here as it is emitted, so
 * emit_surfaces can generate a single wispgen_widget_destroyed() that prunes a
 * destroyed widget out of every registry. Without this, widget_destroy (in the
 * hand-written widget.c) leaves a dangling pointer in these append-only arrays:
 * a later bar_redraw()/render_<surface>() then dereferences a freed widget and
 * commits to surface id 0 → wl protocol error → die(). This is the monitor-
 * disconnect crash. Reset at the top of emit_surfaces. */
#define REG_MAX 128
static char reg_names[REG_MAX][160];
static int  n_reg_names;
void reg_collect(const char *base) {
    for (int i = 0; i < n_reg_names; i++)
        if (strcmp(reg_names[i], base) == 0) return;
    if (n_reg_names < REG_MAX) snprintf(reg_names[n_reg_names++], 160, "%s", base);
}

/* Per-widget hit-table storage. A surface with one widget per output (bar,
 * hud, screen_corners, …) shares one render path across N widgets; click
 * dispatch must test the geometry of the *clicked* widget, not whichever
 * painted last. Emits a [maxw][64] snapshot indexed by the widget's slot in
 * __<nm>_widgets[], plus a slot lookup. Must be emitted AFTER the
 * __<nm>_widgets[]/__<nm>_nw declarations. */
void emit_hit_store(FILE *o, const char *nm, int maxw) {
    fprintf(o, "static %s_Hit __%s_hit[%d][64]; static int __%s_hit_n[%d];\n",
            nm, nm, maxw, nm, maxw);
    fprintf(o, "static int __%s_slot(Widget *w) {\n"
               "    for (int i = 0; i < __%s_nw; i++) if (__%s_widgets[i] == w) return i;\n"
               "    return -1;\n"
               "}\n", nm, nm, nm);
}

/* Snapshot the render working buffer (__<nm>_hits_buf / __<nm>_nhit) into the
 * clicked widget's per-slot row. Emitted at the end of render_<nm>. */
void emit_hit_snapshot(FILE *o, const char *nm) {
    fprintf(o, "    { int __wi = __%s_slot(w);\n"
               "      if (__wi >= 0) {\n"
               "          __%s_hit_n[__wi] = __%s_nhit;\n"
               "          for (int i = 0; i < __%s_nhit; i++) __%s_hit[__wi][i] = __%s_hits_buf[i];\n"
               "      } }\n",
            nm, nm, nm, nm, nm, nm);
}

void reg_reset(void) { n_reg_names = 0; }

/* Prune a destroyed widget out of every surface/region registry. Called by
 * widget.c:widget_destroy() (weak symbol). Swap-remove keeps each array
 * dense so the cap (8/32/4) is never hit by stale entries on monitor
 * hotplug, and so redraw/tag fanouts never touch a freed widget. */
void emit_reg_destroyed(FILE *o) {
    fputs("void wispgen_widget_destroyed(Widget *w) {\n", o);
    for (int i = 0; i < n_reg_names; i++)
        fprintf(o, "    for (int i = 0; i < __%s_nw; i++) if (__%s_widgets[i] == w) { __%s_widgets[i] = __%s_widgets[--__%s_nw]; break; }\n",
                reg_names[i], reg_names[i], reg_names[i], reg_names[i], reg_names[i]);
    fputs("}\n", o);
}

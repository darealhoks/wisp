/* wispc codegen — bar items: collection, measure, draw, dispatch (split from codegen.c). */
#include "codegen_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================ */
/* Surface emit — bar only                                       */
/* ============================================================ */

/* BarItem is declared in codegen_internal.h. */

/* Walk items in collection order; for each static clickable widget, assign a
 * fresh handler_idx matching the discriminator emitted by
 * emit_surface_click_dispatch (`arg == handler_idx`). For-cell items dispatch
 * by cell_idx in `arg`, not by handler_idx. */
void assign_handler_idx(BarItem *items, int nitems) {
    int hidx = 0;
    for (int i = 0; i < nitems; i++) {
        items[i].handler_idx = 0;
        if (!widget_onclick(items[i].w)) continue;
        if (items[i].is_for_cell || items[i].is_runtime_for_cell) continue;
        items[i].handler_idx = hidx++;
    }
}

/* Collect items: each plain widget = 1 item; each for-block = N items
 * (currently fixed to MAX_TAGS=9 for tags.list).
 *
 * Takes a slice of body items rather than a Decl so it can be reused for both
 * surface bodies (Decl->surface.items) and compound region bodies
 * (Region->items). SBody is the common element type. */
int collect_bar_items(SBody *body, int nbody, BarItem *out, int max,
                             CGCtx *ctx, int *err) {
    int n = 0;
    int gid_counter = 0;
    for (int i = 0; i < nbody; i++) {
        SBody *b = &body[i];
        if (b->kind == SB_WIDGET) {
            if (n >= max) { *err = 1; return n; }
            out[n] = (BarItem){0}; out[n].slider_idx = -1; out[n].group_id = -1;
            out[n].w = b->widget; n++;
        } else if (b->kind == SB_GROUP) {
            Group *g = b->group;
            if (g->nmembers == 0) continue;
            int gid = gid_counter++;
            for (int k = 0; k < g->nmembers; k++) {
                if (n >= max) { *err = 1; return n; }
                out[n] = (BarItem){0}; out[n].slider_idx = -1;
                out[n].w = g->members[k];
                out[n].group_id = gid;
                out[n].group_first = (k == 0);
                out[n].grp = (k == 0) ? g : NULL;
                n++;
            }
        } else if (b->kind == SB_FOR) {
            ForBlock *f = b->forb;
            /* iter must be IDENT.list (tags) or IDENT.history (dbus_signal). */
            const char *tags_src = NULL, *dbus_src = NULL;
            /* `for row in rows` — a menu's visible filtered rows. Not a
             * declared source: the rows live in the surface's own state. */
            int menu_rows = f->iter && f->iter->kind == EX_IDENT &&
                            f->iter->ident.n == 4 &&
                            memcmp(f->iter->ident.s, "rows", 4) == 0;
            if (f->iter && f->iter->kind == EX_MEMBER &&
                f->iter->member.base->kind == EX_IDENT) {
                SrcInst *si = find_inst(ctx->srcs, ctx->nsrc,
                                        f->iter->member.base->ident.s,
                                        f->iter->member.base->ident.n);
                if (si && f->iter->member.flen == 4 &&
                    memcmp(f->iter->member.field, "list", 4) == 0 &&
                    si->drv->drv == DRV_TAGS)
                    tags_src = sname(si->decl->name, si->decl->nlen);
                else if (si && f->iter->member.flen == 7 &&
                         memcmp(f->iter->member.field, "history", 7) == 0 &&
                         si->drv->drv == DRV_DBUS)
                    dbus_src = sname(si->decl->name, si->decl->nlen);
            }
            if (!tags_src && !dbus_src && !menu_rows) {
                diag_error(f->loc, "codegen: for-iter must be `rows`, <tags-src>.list or <dbus_signal-src>.history");
                *err = 1; return n;
            }
            if (f->ncells != 1) {
                diag_error(f->loc, "codegen: for-block must contain exactly one cell { … }");
                *err = 1; return n;
            }
            if (menu_rows) {
                if (n >= max) { *err = 1; return n; }
                out[n] = (BarItem){0}; out[n].slider_idx = -1; out[n].group_id = -1;
                out[n].w = f->cells[0];
                out[n].is_runtime_for_cell = true;
                out[n].runtime_for_count =
                    "((w->s.menu.n_filtered - w->s.menu.view_top) < MENU_ROWS_CAP"
                    " ? (w->s.menu.n_filtered - w->s.menu.view_top) : MENU_ROWS_CAP)";
                out[n].runtime_for_iter = "(w->s.menu.view_top + it)";
                out[n].runtime_for_kind = LB_MENU_ROW;
                out[n].runtime_for_cap = MENU_ROWS_CAP;
                out[n].for_var = f->var; out[n].for_var_n = f->vlen;
                n++;
            } else if (tags_src) {
                char *src_dup = strdup(tags_src);
                for (int k = 0; k < 9 /* MAX_TAGS */; k++) {
                    if (n >= max) { *err = 1; return n; }
                    out[n] = (BarItem){0}; out[n].slider_idx = -1; out[n].group_id = -1;
                    out[n].w = f->cells[0]; out[n].is_for_cell = true; out[n].cell_idx = k;
                    out[n].for_var = f->var; out[n].for_var_n = f->vlen;
                    out[n].for_src = src_dup;
                    n++;
                }
            } else {
                if (n >= max) { *err = 1; return n; }
                char *src_dup = strdup(dbus_src);
                out[n] = (BarItem){0}; out[n].slider_idx = -1; out[n].group_id = -1;
                out[n].w = f->cells[0];
                out[n].is_runtime_for_cell = true;
                out[n].runtime_for_src = src_dup;
                {
                    char *cnt = malloc(strlen(src_dup) + 24);
                    sprintf(cnt, "src_%s_hist_n", src_dup);
                    out[n].runtime_for_count = cnt;
                }
                out[n].runtime_for_iter = "it";
                out[n].runtime_for_kind = LB_DBUS_HIST_IT;
                out[n].runtime_for_cap = 8;  /* matches SRC_<NM>_HIST_CAP */
                out[n].for_var = f->var; out[n].for_var_n = f->vlen;
                n++;
            }
        } /* SB_PROP handled separately */
    }
    return n;
}

/* Two-phase emission: measure pass fills st[i] (computed text, icon, colors,
 * extent, alignment), draw pass uses st[i] to draw at the correct position
 * for left/right/center along the main axis (horizontal: width/x; vertical:
 * height/y).
 *
 * Center support: the measure pass sums "center_total" of all visible
 * center-aligned items. The draw pass picks center start = (end_extent -
 * center_total)/2 and advances. Start/end accumulators work as before.
 *
 * Vertical axis: in `axis=vertical` surfaces the main-axis extent is the
 * line height (or explicit `height`); items stack along Y instead of X. The
 * cross axis (X for vertical, Y for horizontal) is left at the start of the
 * surface plus widget pad. */
void emit_item_measure(FILE *o, BarItem *it, CGCtx *ctx, int vertical,
                              const char *surf_nm, int item_idx) {
    Widget *wd = it->w;
    /* Slider measure: a slider just claims an item slot with its main-axis
     * extent (from `width` / `height` prop, or surface-axis fallback). Cross
     * axis fills the surface. Pad/align reuse the bar-item flex pack. */
    if (it->slider_idx >= 0) {
        Expr *widthe = widget_prop(wd, vertical ? "height" : "width");
        Align al = eval_align(widget_prop(wd, "align"));
        int pad = eval_int(widget_prop(wd, "pad"), 0);
        const char *indent = "        ";
        fputs("    {\n", o);
        if (widthe) {
            CE wd_ce = lower(ctx, widthe); wd_ce = coerce_to_int(ctx, wd_ce);
            cgctx_flush_prelude(ctx, o, indent);
            fprintf(o, "%sint tw = %s;\n", indent, wd_ce.text);
        } else {
            fprintf(o, "%sint tw = %s;\n", indent, vertical ? "w->h" : "w->w");
        }
        /* `visible` gates a slider the same way it gates a text item — an OSD
         * slab without progress must not leave an empty track behind. */
        Expr *vise = widget_prop(wd, "visible");
        if (vise) {
            CE v = lower(ctx, vise);
            cgctx_flush_prelude(ctx, o, indent);
            fprintf(o, "%sint vis = !!(%s);\n", indent, v.text);
        } else {
            fprintf(o, "%sint vis = 1;\n", indent);
        }
        fprintf(o, "%sif (!vis) tw = 0;\n", indent);
        fprintf(o, "%sst[%d].vis = vis;\n", indent, it->st_base);
        fprintf(o, "%sst[%d].tw  = tw;\n", indent, it->st_base);
        fprintf(o, "%sst[%d].h   = 0;\n", indent, it->st_base);
        fprintf(o, "%sst[%d].pad = %d;\n", indent, it->st_base, pad);
        fprintf(o, "%sst[%d].align = %d;\n", indent, it->st_base, (int)al);
        fprintf(o, "%sst[%d].body_lines = 1;\n", indent, it->st_base);
        if (al == ALIGN_CENTER)
            fprintf(o, "%scenter_total += tw + %d; __center_trail_pad = %d;\n", indent, pad, pad);
        fputs("    }\n", o);
        return;
    }
    Expr *text  = widget_prop(wd, "text");
    Expr *icon  = widget_prop(wd, "icon");
    Expr *fge   = widget_prop(wd, "fg");
    Expr *bge   = widget_prop(wd, "bg");
    Expr *bord  = widget_prop(wd, "border");
    Expr *vise  = widget_prop(wd, "visible");
    Expr *widthe= widget_prop(wd, vertical ? "height" : "width");
    int   padE  = eval_int(widget_prop(wd, "pad"), 0);
    Expr *bline = widget_prop(wd, "body_lines");
    int   pad_xm= eval_int(widget_prop(wd, "pad_x"), 0);
    Align al    = eval_align(widget_prop(wd, "align"));

    char idx_buf[16];
    /* idx_expr is the C expression used to index st[]. Static items use a
     * literal int; runtime-for items use (st_base + it). */
    char idx_expr[32];
    const char *indent = "        ";
    if (it->is_runtime_for_cell) {
        snprintf(idx_expr, sizeof idx_expr, "(%d + it)", it->st_base);
        push_local(ctx, it->for_var, it->for_var_n, it->runtime_for_kind,
                   it->runtime_for_iter, it->runtime_for_src);
    } else {
        snprintf(idx_expr, sizeof idx_expr, "%d", it->st_base);
        if (it->is_for_cell) {
            snprintf(idx_buf, sizeof idx_buf, "%d", it->cell_idx);
            push_local(ctx, it->for_var, it->for_var_n, LB_TAG_IDX, idx_buf, it->for_src);
        }
    }

    if (it->is_runtime_for_cell) {
        fprintf(o, "    for (int it = 0; it < %s; it++) {\n",
                it->runtime_for_count);
    } else {
        fputs("    {\n", o);
    }

    CE vis = { .text = "1", .type = T_INT };
    if (vise) { vis = lower(ctx, vise); vis = coerce_to_int(ctx, vis); }
    cgctx_flush_prelude(ctx, o, indent);
    fprintf(o, "%sint vis = %s;\n", indent, vis.text);

    /* Step 6.3: enter/exit fade on `visible` edges. While `rev > 0`, the item
     * keeps rendering (and contributes layout) so the exit fade can play out
     * after the underlying expression flipped to false. Alpha-scales bg/fg/border. */
    int ve_in  = widget_enter_ms(wd);
    int ve_out = widget_exit_ms(wd);
    int has_ve = vise && (ve_in > 0 || ve_out > 0);
    const char *ve_idx = it->is_runtime_for_cell ? "it" : "0";
    if (has_ve) {
        fprintf(o, "%sdouble rev = 1.0;\n", indent);
        fprintf(o, "%s#ifdef WISP_HAS_ANIM\n", indent);
        fprintf(o, "%s{ VisSlot *__s = &%s_vis%d[__wi][%s];\n", indent, surf_nm, item_idx, ve_idx);
        fprintf(o, "%s  if (!__s->has) { __s->prev = vis; __s->rev = vis ? 1.0 : 0.0; __s->has = 1; }\n", indent);
        fprintf(o, "%s  else if (!__s->prev && vis) { anim_start_num(&__s->rev, ANIM_T_FLOAT, __s->rev, 1.0, %d, %s, NULL, w, NULL, NULL); __s->prev = 1; }\n",
                indent, ve_in > 0 ? ve_in : 1, widget_easing_id(wd, "enter_easing"));
        fprintf(o, "%s  else if (__s->prev && !vis) { anim_start_num(&__s->rev, ANIM_T_FLOAT, __s->rev, 0.0, %d, %s, NULL, w, NULL, NULL); __s->prev = 0; }\n",
                indent, ve_out > 0 ? ve_out : 1, widget_easing_id(wd, "exit_easing"));
        fprintf(o, "%s  rev = __s->rev; if (rev > 0.004) vis = 1; }\n", indent);
        fprintf(o, "%s#endif\n", indent);
    }

    CE ctxt = { .text = "((const char*)0)", .type = T_STR };
    if (text) { ctxt = lower(ctx, text); if (ctxt.type != T_STR) ctxt = coerce_to_str(ctx, ctxt, text->loc); }
    cgctx_flush_prelude(ctx, o, indent);
    fprintf(o, "%sconst char *txt = %s;\n", indent, ctxt.text);

    CE cicon = { .text = "0", .type = T_INT };
    if (icon) cicon = lower(ctx, icon);
    if (cicon.type == T_PIXMAP) {
        cgctx_flush_prelude(ctx, o, indent);
        fprintf(o, "%suint32_t cp = 0;\n", indent);
        fprintf(o, "%sconst uint32_t *pm = %s; int pms = %s;\n",
                indent, cicon.text, cicon.pm_size);
    } else {
        if (icon) cicon = coerce_to_int(ctx, cicon);
        cgctx_flush_prelude(ctx, o, indent);
        fprintf(o, "%suint32_t cp = (uint32_t)(%s);\n", indent, cicon.text);
        fprintf(o, "%sconst uint32_t *pm = 0; int pms = 0;\n", indent);
    }

    /* Step 6.1: each colour prop optionally interpolates via a TransSlot. The
     * lowered expression yields a target; on change we kick anim_start_color
     * and read .cur. The slot[] is sized 8 for runtime-for cells, 1 otherwise. */
    int tr_bg  = transition_dur(wd, "bg");
    int tr_fg  = transition_dur(wd, "fg");
    int tr_bdr = transition_dur(wd, "border");
    const char *tr_ease = transition_easing_id(wd);
    const char *tr_idx = it->is_runtime_for_cell ? "it" : "0";
    SlotCtx sc = { surf_nm, item_idx, tr_idx, tr_ease };

    CE fg = { .text = "0xffffffffu", .type = T_COLOR };
    if (fge) fg = lower(ctx, fge);
    cgctx_flush_prelude(ctx, o, indent);
    emit_color_slot(o, indent, "fg", "fg", fg.text, &sc, tr_fg);

    CE bg = { .text = "0u", .type = T_COLOR };
    if (bge) bg = lower(ctx, bge);
    cgctx_flush_prelude(ctx, o, indent);
    emit_color_slot(o, indent, "bg", "bg", bg.text, &sc, tr_bg);

    CE bdr = { .text = "0u", .type = T_COLOR };
    if (bord) bdr = lower(ctx, bord);
    cgctx_flush_prelude(ctx, o, indent);
    emit_color_slot(o, indent, "bdr", "border", bdr.text, &sc, tr_bdr);

    /* Step 6.3: apply the reveal factor to the alpha byte of each colour. */
    if (has_ve) {
        fprintf(o, "%s#ifdef WISP_HAS_ANIM\n", indent);
        fprintf(o, "%s{ double __r = rev; if (__r < 0) __r = 0; else if (__r > 1) __r = 1;\n", indent);
        fprintf(o, "%s  uint32_t __ab = (uint32_t)(((bg  >> 24) & 0xffu) * __r);\n", indent);
        fprintf(o, "%s  uint32_t __af = (uint32_t)(((fg  >> 24) & 0xffu) * __r);\n", indent);
        fprintf(o, "%s  uint32_t __ad = (uint32_t)(((bdr >> 24) & 0xffu) * __r);\n", indent);
        fprintf(o, "%s  bg  = (bg  & 0x00ffffffu) | (__ab << 24);\n", indent);
        fprintf(o, "%s  fg  = (fg  & 0x00ffffffu) | (__af << 24);\n", indent);
        fprintf(o, "%s  bdr = (bdr & 0x00ffffffu) | (__ad << 24); }\n", indent);
        fprintf(o, "%s#endif\n", indent);
    }

    /* body_lines lowers as an expression, not a constant: an OSD slab's line
     * count is per-slab ($nbody), and a static max would center a one-line
     * notification as if it were the tallest one. */
    if (bline) { CE cb = lower(ctx, bline); cb = coerce_to_int(ctx, cb);
                 cgctx_flush_prelude(ctx, o, indent);
                 fprintf(o, "%sint __bl = %s; if (__bl < 1) __bl = 1;\n", indent, cb.text); }
    else fprintf(o, "%sint __bl = 1;\n", indent);

    if (widthe) {
        CE wd_ce = lower(ctx, widthe); wd_ce = coerce_to_int(ctx, wd_ce);
        cgctx_flush_prelude(ctx, o, indent);
        fprintf(o, "%sint tw = %s;\n", indent, wd_ce.text);
    } else if (vertical) {
        fprintf(o, "%sint tw = f->line_h * __bl;\n", indent);
    } else {
        fprintf(o, "%sint tw = 0;\n", indent);
        fprintf(o, "%sif (cp || pms)  tw += cp_width(f, cp, pm, pms);\n", indent);
        fprintf(o, "%sif ((cp || pms) && txt && txt[0]) tw += 2;\n", indent);
        fprintf(o, "%sif (txt) {\n", indent);
        fprintf(o, "%s    const char *__p = txt; int __w = 0;\n", indent);
        fprintf(o, "%s    while (__p && *__p) {\n", indent);
        fprintf(o, "%s        const char *__nl = __p; while (*__nl && *__nl != '\\n') __nl++;\n", indent);
        fprintf(o, "%s        char __tmp[256]; int __L = (int)(__nl - __p); if (__L > 255) __L = 255;\n", indent);
        fprintf(o, "%s        memcpy(__tmp, __p, __L); __tmp[__L] = 0;\n", indent);
        fprintf(o, "%s        int __lw = text_width(f, __tmp); if (__lw > __w) __w = __lw;\n", indent);
        fprintf(o, "%s        if (!*__nl) break; __p = __nl + 1;\n", indent);
        fprintf(o, "%s    }\n", indent);
        fprintf(o, "%s    tw += __w;\n", indent);
        fprintf(o, "%s}\n", indent);
        /* Inner horizontal padding grows the content-fit slab so the bg slab
         * and layout advance both account for the breathing room. */
        if (pad_xm > 0) fprintf(o, "%stw += %d;\n", indent, 2 * pad_xm);
    }
    /* h: multi-line slab height when body_lines > 1. Default 0 → use tw advance. */
    fprintf(o, "%sint __h = __bl > 1 ? (__bl * f->line_h) : 0;\n", indent);

    /* press_bg: optional widget prop. Renders in place of bg while this st
     * index is the surface's __pressed_st (pointer pressed-and-still-over). */
    Expr *pbge = widget_prop(wd, "press_bg");
    if (pbge) {
        CE pbg = lower(ctx, pbge);
        cgctx_flush_prelude(ctx, o, indent);
        fprintf(o, "%suint32_t press_bg = (uint32_t)(%s);\n", indent, pbg.text);
    } else {
        fprintf(o, "%suint32_t press_bg = 0u;\n", indent);
    }

    fprintf(o, "%sst[%s].vis = vis;\n", indent, idx_expr);
    fprintf(o, "%sst[%s].txt = txt;\n", indent, idx_expr);
    fprintf(o, "%sst[%s].cp  = cp;\n", indent, idx_expr);
    fprintf(o, "%sst[%s].pm  = pm;\n", indent, idx_expr);
    fprintf(o, "%sst[%s].pms = pms;\n", indent, idx_expr);
    fprintf(o, "%sst[%s].fg  = fg;\n", indent, idx_expr);
    fprintf(o, "%sst[%s].bg  = bg;\n", indent, idx_expr);
    fprintf(o, "%sst[%s].press_bg = press_bg;\n", indent, idx_expr);
    fprintf(o, "%sst[%s].border = bdr;\n", indent, idx_expr);
    /* Step 6.4: `transition_size` tweens the *input* sizes, then the normal
     * layout runs from them each tick — neighbours slide instead of snapping.
     * The cross-axis size is quantised to even pixels: it is centered as
     * integer `(box - ch) / 2`, so an odd intermediate value hops the item half
     * a pixel. The main axis must NOT be quantised — items advance left to
     * right, and independent rounding of a growing and a shrinking neighbour
     * stops cancelling, wobbling everything after them by 2px. */
    int tr_sz = transition_dur(wd, "size");
    if (tr_sz > 0) emit_size_slot(o, indent, "tw", "tw", &sc, tr_sz, 0);
    /* Step 6.3b: the reveal factor also scales geometry, so an entering item
     * grows from nothing and an exiting one collapses — not just alpha. Runs
     * after the size slot: the slot owns steady-state resizes (e.g. 28↔34),
     * rev owns appear/disappear; layering them keeps both animations smooth. */
    if (has_ve)
        fprintf(o, "%sif (rev < 1.0) tw = (int)(tw * rev);\n", indent);
    fprintf(o, "%sst[%s].tw  = tw;\n", indent, idx_expr);
    fprintf(o, "%sst[%s].h   = __h;\n", indent, idx_expr);
    /* Cross-axis size (horizontal: height, vertical: width) lowered as an expr
     * so `tag.active ? 34 : 30` resizes per-item; 0 = fill the region. */
    Expr *che = widget_prop(wd, vertical ? "width" : "height");
    if (che) { CE cc = lower(ctx, che); cc = coerce_to_int(ctx, cc);
               fprintf(o, "%sint __ch = %s;\n", indent, cc.text);
               if (tr_sz > 0) emit_size_slot(o, indent, "__ch", "ch", &sc, tr_sz, 1);
               /* keep the even quantisation — see the cross-axis note above */
               if (has_ve)
                   fprintf(o, "%sif (rev < 1.0) __ch = ((int)(__ch * rev)) & ~1;\n", indent);
               fprintf(o, "%sst[%s].ch = __ch;\n", indent, idx_expr); }
    if (has_ve)
        fprintf(o, "%sst[%s].pad = rev < 1.0 ? (int)(%d * rev) : %d;\n",
                indent, idx_expr, padE, padE);
    else
        fprintf(o, "%sst[%s].pad = %d;\n", indent, idx_expr, padE);
    fprintf(o, "%sst[%s].align = %d;\n", indent, idx_expr, (int)al);
    fprintf(o, "%sst[%s].body_lines = __bl;\n", indent, idx_expr);
    if (al == ALIGN_CENTER) {
        fprintf(o, "%sif (vis) { center_total += (__h > 0 ? __h : tw) + %d; __center_trail_pad = %d; }\n", indent, padE, padE);
    }
    fputs("    }\n", o);

    if (it->is_for_cell || it->is_runtime_for_cell) pop_local(ctx);
}

/* Color CE: lower an expression (const/mut/ternary/literal) to a uint32_t-typed
 * C expression, or fall back to a default literal when the prop is absent. */
static CE color_ce(CGCtx *ctx, Expr *e, const char *dflt) {
    CE r;
    if (e) return lower(ctx, e);
    r.type = T_COLOR;
    snprintf(r.text, sizeof r.text, "%s", dflt);
    return r;
}

/* Emit draw block for one item. `vertical` selects axis. */
void emit_item_draw(FILE *o, BarItem *it, CGCtx *ctx, int vertical, const char *nm) {
    Widget *wd = it->w;
    if (it->slider_idx >= 0) {
        Expr *ve  = widget_prop(wd, "value");
        char vmut[128]; vmut[0] = 0;
        if (ve && ve->kind == EX_IDENT)
            snprintf(vmut, sizeof vmut, "%s", sname(ve->ident.s, ve->ident.n));
        int thumb    = eval_int(widget_prop(wd, "thumb_size"), 0);
        int shape    = widget_thumb_shape(wd);
        int thumb_r  = eval_int(widget_prop(wd, "thumb_radius"), 0);
        int thbw     = eval_int(widget_prop(wd, "thumb_border_width"), 0);
        int track_r  = eval_int(widget_prop(wd, "track_radius"), 0);
        int shx      = eval_int(widget_prop(wd, "shadow_x"), 0);
        int shy      = eval_int(widget_prop(wd, "shadow_y"), 2);
        int shblur   = eval_int(widget_prop(wd, "shadow_blur"), 0);
        int sl_vert  = widget_is_vertical(wd);
        const char *indent = "        ";
        /* Colors are lowered as expressions so consts / muts / ternaries work
         * (e.g. a track that recolors when muted). draw_slider gates each on its
         * alpha byte at runtime, so a 0 color = "off". */
        CE c_tbg = color_ce(ctx, widget_prop(wd, "track_bg"),     "0xff202020u");
        CE c_tfg = color_ce(ctx, widget_prop(wd, "track_fg"),     "0xff808080u");
        CE c_thc = color_ce(ctx, widget_prop(wd, "thumb_color"),  "0u");
        CE c_thb = color_ce(ctx, widget_prop(wd, "thumb_border"), "0u");
        CE c_shc = color_ce(ctx, widget_prop(wd, "shadow"),       "0u");
        fprintf(o, "    if (st[%d].vis) {\n", it->st_base);
        fprintf(o, "%sint tw = st[%d].tw, pad = st[%d].pad;\n", indent, it->st_base, it->st_base);
        fprintf(o, "%sint pos;\n", indent);
        fprintf(o, "%sswitch (st[%d].align) {\n", indent, it->st_base);
        fprintf(o, "%s    case 0:  pos = start_pos; start_pos += tw + pad; break;\n", indent);
        fprintf(o, "%s    case 1:  end_pos -= tw + pad; pos = end_pos; break;\n", indent);
        fprintf(o, "%s    default: pos = center_pos; center_pos += tw + pad; break;\n", indent);
        fprintf(o, "%s}\n", indent);
        /* Track rect: main-axis extent = tw (the packed `width`/`height`);
         * cross-axis extent honors the OTHER dimension prop when given, centered
         * in the region (so labels and margins have room) — else fills it. */
        int cross_ext = eval_int(widget_prop(wd, vertical ? "width" : "height"), 0);
        if (vertical) {
            if (cross_ext > 0)
                fprintf(o, "%sint rw = %d, rx = __reg_x + (__reg_w - %d) / 2, ry = pos, rh = tw;\n",
                        indent, cross_ext, cross_ext);
            else
                fprintf(o, "%sint rx = __reg_x, ry = pos, rw = __reg_w, rh = tw;\n", indent);
        } else {
            if (cross_ext > 0)
                fprintf(o, "%sint rh = %d, ry = __reg_y + (__reg_h - %d) / 2, rx = pos, rw = tw;\n",
                        indent, cross_ext, cross_ext);
            else
                fprintf(o, "%sint rx = pos, ry = __reg_y, rw = tw, rh = __reg_h;\n", indent);
        }
        cgctx_flush_prelude(ctx, o, indent);
        /* `value` is any expression in 0..value_max — a mut (0..1, the default
         * max) or a $-binding like an OSD slab's 0..100 progress. */
        double vmax = eval_double(widget_prop(wd, "value_max"), 1.0);
        if (vmax <= 0) vmax = 1.0;
        char vexpr[256];
        if (vmut[0]) {
            snprintf(vexpr, sizeof vexpr, "mut_%s", vmut);
        } else if (ve) {
            CE v = coerce_to_int(ctx, lower(ctx, ve));
            snprintf(vexpr, sizeof vexpr, "%s", v.text);
            cgctx_flush_prelude(ctx, o, indent);
        } else {
            snprintf(vexpr, sizeof vexpr, "0");
        }
        fprintf(o, "%sdouble __val = (double)(%s) / %f;\n", indent, vexpr, vmax);
        fprintf(o, "%sdraw_slider(sl->px, w->w, w->h, rx, ry, rw, rh, %d, __val, (uint32_t)(%s), (uint32_t)(%s),\n",
                indent, sl_vert, c_tbg.text, c_tfg.text);
        fprintf(o, "%s    &(SliderStyle){ .thumb_size=%d, .thumb_shape=%d, .thumb_radius=%d,"
                   " .thumb_color=(uint32_t)(%s), .thumb_border=(uint32_t)(%s), .thumb_border_w=%d, .track_radius=%d,"
                   " .shadow_color=(uint32_t)(%s), .shadow_x=%d, .shadow_y=%d, .shadow_blur=%d });\n",
                indent, thumb, shape, thumb_r, c_thc.text, c_thb.text, thbw, track_r, c_shc.text, shx, shy, shblur);
        /* Live value label: format mut * scale and place at start/end/center of
         * the slider's main (fill) axis. */
        if (widget_flag(wd, "show_value") && vmut[0]) {
            Expr *vf = widget_prop(wd, "value_format");
            char fmt[256];
            if (vf && vf->kind == EX_STRING) {
                /* Emit as an escaped C string literal (one float conversion). */
                size_t fo = 0; fmt[fo++] = '"';
                for (size_t k = 0; k < vf->str.n && fo + 5 < sizeof fmt; k++) {
                    unsigned char ch = (unsigned char)vf->str.s[k];
                    if (ch == '\\' || ch == '"') { fmt[fo++] = '\\'; fmt[fo++] = ch; }
                    else if (ch == '\n')         { fmt[fo++] = '\\'; fmt[fo++] = 'n'; }
                    else if (ch < 32)            { fo += snprintf(fmt + fo, sizeof fmt - fo, "\\x%02x", ch); }
                    else                          { fmt[fo++] = ch; }
                }
                fmt[fo++] = '"'; fmt[fo] = 0;
            } else {
                snprintf(fmt, sizeof fmt, "\"%%.0f\"");
            }
            double scale  = eval_double(widget_prop(wd, "value_scale"), 100.0);
            CE c_vfg      = color_ce(ctx, widget_prop(wd, "value_fg"), "0xffffffffu");
            int vgap      = eval_int(widget_prop(wd, "value_gap"), 6);
            int valign    = widget_value_align(wd);
            cgctx_flush_prelude(ctx, o, indent);
            fprintf(o, "%schar __vb[48]; snprintf(__vb, sizeof __vb, %s, (double)(mut_%s) * %.6f);\n",
                    indent, fmt, vmut, scale);
            fprintf(o, "%sint __vw = text_width(f, __vb); int __vx, __vy;\n", indent);
            if (sl_vert) {
                fprintf(o, "%s__vx = rx + (rw - __vw) / 2;\n", indent);
                if (valign == 0)      fprintf(o, "%s__vy = ry - %d - f->line_h;\n", indent, vgap);
                else if (valign == 2) fprintf(o, "%s__vy = ry + (rh - f->line_h) / 2;\n", indent);
                else                  fprintf(o, "%s__vy = ry + rh + %d;\n", indent, vgap);
            } else {
                fprintf(o, "%s__vy = ry + (rh - f->line_h) / 2;\n", indent);
                if (valign == 0)      fprintf(o, "%s__vx = rx - %d - __vw;\n", indent, vgap);
                else if (valign == 2) fprintf(o, "%s__vx = rx + (rw - __vw) / 2;\n", indent);
                else                  fprintf(o, "%s__vx = rx + rw + %d;\n", indent, vgap);
            }
            fprintf(o, "%sdraw_text(sl->px, w->w, w->h, __vx, __vy, f, __vb, (uint32_t)(%s));\n", indent, c_vfg.text);
        }
        fprintf(o, "%s{ int __i = __%s_nhit++; __%s_hits_buf[__i].x = rx; __%s_hits_buf[__i].y = ry; "
                   "__%s_hits_buf[__i].w = rw; __%s_hits_buf[__i].h = rh; "
                   "__%s_hits_buf[__i].kind = 0; __%s_hits_buf[__i].arg = 0; __%s_hits_buf[__i].slider_idx = %d; __%s_hits_buf[__i].st_idx = %d; }\n",
                indent, nm, nm, nm, nm, nm, nm, nm, nm, it->slider_idx, nm, it->st_base);
        fprintf(o, "    }\n");
        return;
    }
    WBody *clk = widget_onclick(it->w);
    char idx_expr[32];
    const char *indent = "        ";
    int kind;
    if (it->is_runtime_for_cell) {
        snprintf(idx_expr, sizeof idx_expr, "(%d + it)", it->st_base);
        fprintf(o, "    for (int it = 0; it < %s; it++) {\n",
                it->runtime_for_count);
        kind = 2;
    } else {
        snprintf(idx_expr, sizeof idx_expr, "%d", it->st_base);
        fputs("    {\n", o);
        kind = it->is_for_cell ? 1 : 0;
    }

    fprintf(o, "%sif (st[%s].vis) {\n", indent, idx_expr);
    fprintf(o, "%s    int tw = st[%s].tw, pad = st[%s].pad;\n", indent, idx_expr, idx_expr);
    fprintf(o, "%s    int __h = st[%s].h;\n", indent, idx_expr);
    fprintf(o, "%s    int __adv = __h > 0 ? __h : tw;\n", indent);
    fprintf(o, "%s    const char *txt = st[%s].txt; uint32_t cp = st[%s].cp; const uint32_t *pm = st[%s].pm; int pms = st[%s].pms;\n", indent, idx_expr, idx_expr, idx_expr, idx_expr);
    fprintf(o, "%s    uint32_t fg = st[%s].fg, bg = st[%s].bg, bdr = st[%s].border;\n",
            indent, idx_expr, idx_expr, idx_expr);
    /* press_bg override: while this st-index is the surface's pressed_st, swap
     * bg for the widget's press_bg if it has one. */
    fprintf(o, "%s    if (st[%s].press_bg & 0xff000000u && __%s_pressed_st == (%s) && __%s_pressed_w == w) bg = st[%s].press_bg;\n",
            indent, idx_expr, nm, idx_expr, nm, idx_expr);
    fprintf(o, "%s    int body_lines = st[%s].body_lines;\n", indent, idx_expr);
    fprintf(o, "%s    int pos;\n", indent);
    fprintf(o, "%s    switch (st[%s].align) {\n", indent, idx_expr);
    fprintf(o, "%s        case 0:  pos = start_pos; start_pos += __adv + pad; break;\n", indent);
    fprintf(o, "%s        case 1:  end_pos -= __adv + pad; pos = end_pos; break;\n", indent);
    fprintf(o, "%s        default: pos = center_pos; center_pos += __adv + pad; break;\n", indent);
    fprintf(o, "%s    }\n", indent);
    if (vertical) {
        /* main axis = Y; bg + border span the region cross-axis (__reg_w)
         * over the item's height (= __adv). */
        int radius_w = eval_int(widget_prop(wd, "radius"), 0);
        int r_tl = eval_int(widget_prop(wd, "radius_tl"), radius_w);
        int r_tr = eval_int(widget_prop(wd, "radius_tr"), radius_w);
        int r_br = eval_int(widget_prop(wd, "radius_br"), radius_w);
        int r_bl = eval_int(widget_prop(wd, "radius_bl"), radius_w);
        int any_round = r_tl || r_tr || r_br || r_bl;
        int maxr = r_tl; if (r_tr > maxr) maxr = r_tr; if (r_br > maxr) maxr = r_br; if (r_bl > maxr) maxr = r_bl;
        int vbw = eval_int(widget_prop(wd, "border_width"), 1);
        uint32_t shc = eval_color_ctx(ctx, widget_prop(wd, "shadow"), 0);
        int shx = eval_int(widget_prop(wd, "shadow_x"), 0);
        int shy = eval_int(widget_prop(wd, "shadow_y"), 2);
        int shblur = eval_int(widget_prop(wd, "shadow_blur"), 0);
        int shspread = eval_int(widget_prop(wd, "shadow_spread"), 0);
        fprintf(o, "%s    int cx = __reg_x + pad;\n", indent);
        fprintf(o, "%s    int cy = pos + (__adv - f->line_h * body_lines) / 2; if (cy < pos) cy = pos;\n", indent);
        if (shc & 0xff000000u)
            fprintf(o, "%s    fill_rounded_shadow(sl->px, w->w, w->h, __reg_x + %d, pos + %d, __reg_w + %d, __adv + %d, %d, %d, 0x%08xu);\n",
                    indent, shx - shspread, shy - shspread, 2 * shspread, 2 * shspread,
                    maxr + shspread, shblur > 0 ? shblur : 8, shc);
        if (any_round) {
            fprintf(o, "%s    if (bg  & 0xff000000u) fill_rect_rounded(sl->px, w->w, w->h, __reg_x, pos, __reg_w, __adv, %d, %d, %d, %d, bg);\n",
                    indent, r_tl, r_tr, r_br, r_bl);
            fprintf(o, "%s    if (bdr & 0xff000000u) fill_rect_rounded_border(sl->px, w->w, w->h, __reg_x, pos, __reg_w, __adv, %d, %d, %d, %d, %d, 1, 1, 1, 1, 0, bdr);\n",
                    indent, r_tl, r_tr, r_br, r_bl, vbw);
        } else {
            fprintf(o, "%s    if (bg  & 0xff000000u) fill_rect(sl->px, w->w, w->h, __reg_x, pos, __reg_w, __adv, bg);\n", indent);
            fprintf(o, "%s    if (bdr & 0xff000000u) {\n", indent);
            fprintf(o, "%s        fill_rect(sl->px, w->w, w->h, __reg_x, pos, __reg_w, %d, bdr);\n", indent, vbw);
            fprintf(o, "%s        fill_rect(sl->px, w->w, w->h, __reg_x, pos + __adv - %d, __reg_w, %d, bdr);\n", indent, vbw, vbw);
            fprintf(o, "%s        fill_rect(sl->px, w->w, w->h, __reg_x, pos, %d, __adv, bdr);\n", indent, vbw);
            fprintf(o, "%s        fill_rect(sl->px, w->w, w->h, __reg_x + __reg_w - %d, pos, %d, __adv, bdr);\n", indent, vbw, vbw);
            fprintf(o, "%s    }\n", indent);
        }
        fprintf(o, "%s    if (cp || pms) { cx += draw_cp(sl->px, w->w, w->h, cx, cy, f, cp, fg, pm, pms); if (txt && txt[0]) { draw_text(sl->px, w->w, w->h, cx, cy, f, \" \", fg); cx += 2; } }\n", indent);
        fprintf(o, "%s    if (txt) {\n", indent);
        fprintf(o, "%s        const char *__p = txt; int __ln = 0;\n", indent);
        fprintf(o, "%s        while (__p && *__p && __ln < body_lines) {\n", indent);
        fprintf(o, "%s            const char *__nl = __p; while (*__nl && *__nl != '\\n') __nl++;\n", indent);
        fprintf(o, "%s            char __tmp[256]; int __L = (int)(__nl - __p); if (__L > 255) __L = 255;\n", indent);
        fprintf(o, "%s            memcpy(__tmp, __p, __L); __tmp[__L] = 0;\n", indent);
        fprintf(o, "%s            draw_text(sl->px, w->w, w->h, cx, cy + __ln * f->line_h, f, __tmp, fg);\n", indent);
        fprintf(o, "%s            __ln++; if (!*__nl) break; __p = __nl + 1;\n", indent);
        fprintf(o, "%s        }\n", indent);
        fprintf(o, "%s    }\n", indent);
    } else {
        int radius_w = eval_int(widget_prop(wd, "radius"), 0);
        int r_tl = eval_int(widget_prop(wd, "radius_tl"), radius_w);
        int r_tr = eval_int(widget_prop(wd, "radius_tr"), radius_w);
        int r_br = eval_int(widget_prop(wd, "radius_br"), radius_w);
        int r_bl = eval_int(widget_prop(wd, "radius_bl"), radius_w);
        int any_round = r_tl || r_tr || r_br || r_bl;
        int maxr = r_tl; if (r_tr > maxr) maxr = r_tr; if (r_br > maxr) maxr = r_br; if (r_bl > maxr) maxr = r_bl;
        int has_fixed_w = widget_prop(wd, "width") != NULL;
        Expr *heighte = widget_prop(wd, "height");
        /* y_offset shifts the cell vertically post-centering. Useful for cells
         * that should overlap a sibling (e.g. HUD buttons sitting halfway on
         * the bar — negative offset pulls them up out of the centered slot). */
        /* Lowers as an expression: an OSD slab's text block shifts by whether
         * that slab has a progress band, which no constant can express. */
        Expr *yoffe = widget_prop(wd, "y_offset");
        int x_off = eval_int(widget_prop(wd, "x_offset"), 0);
        int pad_x = eval_int(widget_prop(wd, "pad_x"), 0);
        int pad_y = eval_int(widget_prop(wd, "pad_y"), 0);
        uint32_t shc = eval_color_ctx(ctx, widget_prop(wd, "shadow"), 0);
        int shx = eval_int(widget_prop(wd, "shadow_x"), 0);
        int shy = eval_int(widget_prop(wd, "shadow_y"), 2);
        int shblur = eval_int(widget_prop(wd, "shadow_blur"), 0);
        int shspread = eval_int(widget_prop(wd, "shadow_spread"), 0);
        fprintf(o, "%s    int x = pos + %d;\n", indent, x_off);
        if (yoffe) { CE cy = lower(ctx, yoffe); cy = coerce_to_int(ctx, cy);
                     cgctx_flush_prelude(ctx, o, indent);
                     fprintf(o, "%s    int __yoff = %s;\n", indent, cy.text); }
        else fprintf(o, "%s    int __yoff = 0;\n", indent);
        if (heighte) {
            /* Cross-axis height computed in the measure pass (st.ch), so a
             * `tag.active ? 34 : 30` sizes per-item; centered in the region. */
            fprintf(o, "%s    int __ch = st[%s].ch;\n", indent, idx_expr);
            fprintf(o, "%s    int __by = __reg_y + (__reg_h - __ch) / 2 + __yoff;\n", indent);
            fprintf(o, "%s    int __bh = __ch;\n", indent);
        } else {
            fprintf(o, "%s    int __by = __reg_y + __yoff;  /* slab y origin */\n", indent);
            fprintf(o, "%s    int __bh = __h > 0 ? __h : __reg_h;\n", indent);
        }
        /* Slab x/width: explicit `width` or inner `pad_x` span the full cell;
         * otherwise keep the legacy 4-px halo around the content extent. */
        if (has_fixed_w || pad_x > 0) {
            fprintf(o, "%s    int __bx = x;\n", indent);
            fprintf(o, "%s    int __bw = __adv;\n", indent);
        } else {
            fprintf(o, "%s    int __bx = x - 2;\n", indent);
            fprintf(o, "%s    int __bw = __adv + 4;\n", indent);
        }
        /* Free-width content sits inside the inner pad_x gutter. */
        if (pad_x > 0 && !has_fixed_w) fprintf(o, "%s    x += %d;\n", indent, pad_x);
        /* Soft drop shadow behind the slab (offset by shadow_x/y, grown by
         * shadow_spread, softened over shadow_blur px). Drawn before the bg. */
        if (shc & 0xff000000u)
            fprintf(o, "%s    fill_rounded_shadow(sl->px, w->w, w->h, __bx + %d, __by + %d, __bw + %d, __bh + %d, %d, %d, 0x%08xu);\n",
                    indent, shx - shspread, shy - shspread, 2 * shspread, 2 * shspread,
                    maxr + shspread, shblur > 0 ? shblur : 8, shc);
        if (any_round) {
            fprintf(o, "%s    if (bg  & 0xff000000u) fill_rect_rounded(sl->px, w->w, w->h, __bx, __by, __bw, __bh, %d, %d, %d, %d, bg);\n",
                    indent, r_tl, r_tr, r_br, r_bl);
        } else {
            fprintf(o, "%s    if (bg  & 0xff000000u) fill_rect(sl->px, w->w, w->h, __bx, __by, __bw, __bh, bg);\n", indent);
        }
        int bw_px = eval_int(widget_prop(wd, "border_width"), 1);
        /* Per-side suppression (default all on): `border_bottom` alone gives a
         * typographic underline. side order matches fill_rect_rounded_border. */
        int bs_t = eval_int(widget_prop(wd, "border_top"),    1);
        int bs_r = eval_int(widget_prop(wd, "border_right"),  1);
        int bs_b = eval_int(widget_prop(wd, "border_bottom"), 1);
        int bs_l = eval_int(widget_prop(wd, "border_left"),   1);
        fprintf(o, "%s    if (bdr & 0xff000000u) {\n", indent);
        if (any_round) {
            fprintf(o, "%s        fill_rect_rounded_border(sl->px, w->w, w->h, __bx, __by, __bw, __bh, %d, %d, %d, %d, %d, %d, %d, %d, %d, 0, bdr);\n",
                    indent, r_tl, r_tr, r_br, r_bl, bw_px, bs_t, bs_r, bs_b, bs_l);
        } else {
            if (bs_t) fprintf(o, "%s        fill_rect(sl->px, w->w, w->h, __bx, __by, __bw, %d, bdr);\n", indent, bw_px);
            if (bs_b) fprintf(o, "%s        fill_rect(sl->px, w->w, w->h, __bx, __by + __bh - %d, __bw, %d, bdr);\n", indent, bw_px, bw_px);
            if (bs_l) fprintf(o, "%s        fill_rect(sl->px, w->w, w->h, __bx, __by, %d, __bh, bdr);\n", indent, bw_px);
            if (bs_r) fprintf(o, "%s        fill_rect(sl->px, w->w, w->h, __bx + __bw - %d, __by, %d, __bh, bdr);\n", indent, bw_px, bw_px);
        }
        fprintf(o, "%s    }\n", indent);
        /* Fixed-height widget: vertically center the glyph row inside the cell.
         * Free-height widget keeps the surface-row baseline (`y`). pad_y nudges
         * the content row down inside the slab. */
        /* Center the whole line BLOCK, not one line: a multi-line widget
         * (body_lines > 1) otherwise hangs half its text below the cell. */
        if (heighte)
            fprintf(o, "%s    int __ty = __by + (__bh - f->line_h * body_lines) / 2 + %d;\n", indent, pad_y);
        else
            fprintf(o, "%s    int __ty = (__h > 0 ? __by + (__reg_h - __h) / 2 : y) + %d;\n", indent, pad_y);
        fprintf(o, "%s    if (__ty < __reg_y) __ty = __reg_y;\n", indent);
        /* Centering: when width is fixed, compute the actual content width and
         * offset x so icon+text sits in the middle of the cell (within pad_x). */
        if (has_fixed_w) {
            fprintf(o, "%s    int __cw = 0;\n", indent);
            fprintf(o, "%s    if (cp || pms) __cw += cp_width(f, cp, pm, pms);\n", indent);
            fprintf(o, "%s    if ((cp || pms) && txt && txt[0]) __cw += 2;\n", indent);
            fprintf(o, "%s    if (txt) { const char *__p = txt; while (*__p && *__p != '\\n') __p++; char __t2[256]; int __L = (int)(__p - txt); if (__L > 255) __L = 255; memcpy(__t2, txt, __L); __t2[__L] = 0; __cw += text_width(f, __t2); }\n", indent);
            fprintf(o, "%s    if (__cw < tw) x += (tw - __cw) / 2;\n", indent);
        }
        /* Icon-only path uses pixel-bbox centering inside the cell bg box;
         * icon+text falls through to advance-based layout so text kerns. */
        fprintf(o, "%s    if ((cp || pms) && (!txt || !txt[0])) {\n", indent);
        fprintf(o, "%s        draw_cp_centered(sl->px, w->w, w->h, __bx, __by, __bw, __bh, f, cp, fg, pm, pms);\n", indent);
        fprintf(o, "%s    } else if (cp || pms) { x += draw_cp(sl->px, w->w, w->h, x, __ty, f, cp, fg, pm, pms); if (txt && txt[0]) { draw_text(sl->px, w->w, w->h, x, __ty, f, \" \", fg); x += 2; } }\n", indent);
        fprintf(o, "%s    if (txt) {\n", indent);
        fprintf(o, "%s        const char *__p = txt; int __ln = 0;\n", indent);
        fprintf(o, "%s        while (__p && *__p && __ln < body_lines) {\n", indent);
        fprintf(o, "%s            const char *__nl = __p; while (*__nl && *__nl != '\\n') __nl++;\n", indent);
        fprintf(o, "%s            char __tmp[256]; int __L = (int)(__nl - __p); if (__L > 255) __L = 255;\n", indent);
        fprintf(o, "%s            memcpy(__tmp, __p, __L); __tmp[__L] = 0;\n", indent);
        /* `elide` clamps each line to whatever room is left in the region —
         * without it a long summary just runs off the slab. */
        if (widget_flag(wd, "elide"))
            fprintf(o, "%s            draw_text_elided(sl->px, w->w, w->h, x, __ty + __ln * f->line_h, f, __tmp, __reg_x + __reg_w - x - %d, fg);\n",
                    indent, pad_x > 0 ? pad_x : 0);
        else
            fprintf(o, "%s            draw_text(sl->px, w->w, w->h, x, __ty + __ln * f->line_h, f, __tmp, fg);\n", indent);
        fprintf(o, "%s            __ln++; if (!*__nl) break; __p = __nl + 1;\n", indent);
        fprintf(o, "%s        }\n", indent);
        fprintf(o, "%s    }\n", indent);
    }
    if (clk) {
        int arg_val = it->is_for_cell ? it->cell_idx : it->handler_idx;
        /* Hit rect in compound/surface-local coords. For vertical (main=Y) the
         * cross-axis spans the region's X extent; for horizontal it spans the
         * region's Y extent. Origin offsets come from __reg_x/__reg_y so
         * compound regions translate correctly. */
        const char *hx = vertical ? "__reg_x" : "pos";
        const char *hy = vertical ? "pos"     : "__reg_y";
        const char *hw = vertical ? "__reg_w" : "(__adv + pad)";
        const char *hh = vertical ? "(__adv + pad)" : "__reg_h";
        if (it->is_runtime_for_cell) {
            fprintf(o, "%s    { int __i = __%s_nhit++; __%s_hits_buf[__i].x = %s; __%s_hits_buf[__i].y = %s; "
                       "__%s_hits_buf[__i].w = %s; __%s_hits_buf[__i].h = %s; "
                       "__%s_hits_buf[__i].kind = %d; __%s_hits_buf[__i].arg = it; __%s_hits_buf[__i].slider_idx = -1; __%s_hits_buf[__i].st_idx = (%d + it); }\n",
                    indent, nm, nm, hx, nm, hy, nm, hw, nm, hh, nm, kind, nm, nm, nm, it->st_base);
        } else {
            fprintf(o, "%s    { int __i = __%s_nhit++; __%s_hits_buf[__i].x = %s; __%s_hits_buf[__i].y = %s; "
                       "__%s_hits_buf[__i].w = %s; __%s_hits_buf[__i].h = %s; "
                       "__%s_hits_buf[__i].kind = %d; __%s_hits_buf[__i].arg = %d; __%s_hits_buf[__i].slider_idx = -1; __%s_hits_buf[__i].st_idx = %d; }\n",
                    indent, nm, nm, hx, nm, hy, nm, hw, nm, hh, nm, kind, nm, arg_val, nm, nm, it->st_base);
        }
    }
    fprintf(o, "%s}\n", indent);
    fputs("    }\n", o);
}

/* True iff any ST_SET in the body targets the named identifier. Used to skip
 * auto-refresh on sources the click handler already optimistically set(). */
static int stmt_sets_name(Stmt *st, const char *nm) {
    if (!st) return 0;
    if (st->kind == ST_BLOCK) {
        for (int i = 0; i < st->block.n; i++)
            if (stmt_sets_name(st->block.list[i], nm)) return 1;
        return 0;
    }
    if (st->kind == ST_SET) {
        size_t L = strlen(nm);
        return st->set.nlen == L && memcmp(st->set.name, nm, L) == 0;
    }
    return 0;
}

/* Recursive statement emitter used from event handlers (on_click bodies).
 * Handles ST_EXEC / ST_SET / ST_EMIT / ST_BLOCK. */
void emit_stmt(FILE *o, CGCtx *ctx, Stmt *st, const char *indent,
                      SemaResult *r) {
    if (!st) return;
    switch (st->kind) {
    case ST_BLOCK:
        for (int i = 0; i < st->block.n; i++)
            emit_stmt(o, ctx, st->block.list[i], indent, r);
        return;
    case ST_EXEC: {
        CE ce = lower(ctx, st->exec.arg);
        if (ce.type != T_STR) ce = coerce_to_str(ctx, ce, st->loc);
        cgctx_flush_prelude(ctx, o, indent);
        fprintf(o, "%sexec_cmd(%s);\n", indent, ce.text);
        return;
    }
    case ST_SET: {
        const char *nm = sname(st->set.name, st->set.nlen);
        Decl *k = find_konst(ctx, st->set.name, st->set.nlen);
        /* Allow `set <exec_line_source> = <expr>` to override the source's
         * line buffer optimistically. Used so a toggle button can paint the
         * post-click state in the same frame, instead of waiting for the
         * spawned probe to re-poll over a pipe. */
        SrcInst *si = find_inst(ctx->srcs, ctx->nsrc, st->set.name, st->set.nlen);
        CE val = lower(ctx, st->set.val);
        cgctx_flush_prelude(ctx, o, indent);
        if (si && si->drv->drv == DRV_EXEC) {
            if (val.type != T_STR) val = coerce_to_str(ctx, val, st->loc);
            cgctx_flush_prelude(ctx, o, indent);
            fprintf(o, "%ssnprintf(src_%s_line, sizeof src_%s_line, \"%%s\", %s);\n",
                    indent, nm, nm, val.text);
        } else if (k && k->konst.val && k->konst.val->kind == EX_STRING) {
            /* String mut: copy with bounded snprintf. */
            if (val.type != T_STR) val = coerce_to_str(ctx, val, st->loc);
            cgctx_flush_prelude(ctx, o, indent);
            fprintf(o, "%ssnprintf(mut_%s, sizeof mut_%s, \"%%s\", %s);\n",
                    indent, nm, nm, val.text);
        } else {
            fprintf(o, "%smut_%s = (%s);\n", indent, nm, val.text);
        }
        /* Mark every surface that reads this mut/source dirty. sema records
         * deps uniformly for both source and mut names. */
        if (r) {
            for (int i = 0; i < r->nsurfaces; i++) {
                const char **deps = r->surface_deps[i];
                for (int j = 0; deps && deps[j]; j++) {
                    if (strcmp(deps[j], nm) == 0) {
                        fprintf(o, "%sdirty_%s = 1;\n", indent, r->surface_names[i]);
                        break;
                    }
                }
            }
        }
        return;
    }
    case ST_ANIMATE: {
        const char *nm = sname(st->anim.name, st->anim.nlen);
        Decl *k = find_konst(ctx, st->anim.name, st->anim.nlen);
        const char *easing_id = "EASE_LINEAR";
        const char *bez_arg = "NULL";
        char bez_buf[160] = "";
        if (st->anim.easing) {
            Expr *e = st->anim.easing;
            if (e->kind == EX_IDENT) {
                const char *en = e->ident.s; size_t eL = e->ident.n;
                if      (eL == 6 && !memcmp(en, "linear",      6)) easing_id = "EASE_LINEAR";
                else if (eL == 7 && !memcmp(en, "ease_in",     7)) easing_id = "EASE_IN";
                else if (eL == 8 && !memcmp(en, "ease_out",    8)) easing_id = "EASE_OUT";
                else if (eL == 11 && !memcmp(en, "ease_in_out",11)) easing_id = "EASE_IN_OUT";
                else diag_error(e->loc, "unknown easing '%.*s'", (int)eL, en);
            } else if (e->kind == EX_CALL && e->call.nlen == 13 &&
                       !memcmp(e->call.name, "cubic_bezier", 12)) {
                /* fall-through below */
            } else if (e->kind == EX_CALL && e->call.nlen == 12 &&
                       !memcmp(e->call.name, "cubic_bezier", 12) && e->call.nargs == 4) {
                easing_id = "EASE_CUBIC_BEZIER";
                CE a = lower(ctx, e->call.args[0]);
                CE b = lower(ctx, e->call.args[1]);
                CE c = lower(ctx, e->call.args[2]);
                CE d = lower(ctx, e->call.args[3]);
                snprintf(bez_buf, sizeof bez_buf,
                         "(double[]){(double)(%s),(double)(%s),(double)(%s),(double)(%s)}",
                         a.text, b.text, c.text, d.text);
                bez_arg = bez_buf;
            } else {
                diag_error(e->loc, "easing must be ident or cubic_bezier(a,b,c,d)");
            }
        }
        CE to = lower(ctx, st->anim.to);
        CE dur = lower(ctx, st->anim.duration);
        cgctx_flush_prelude(ctx, o, indent);
        if (!k || k->kind != D_MUT) {
            fprintf(o, "%s/* animate: target '%s' not a mut */\n", indent, nm);
            return;
        }
        CE init = lower(ctx, k->konst.val);
        const char *fn = "anim_start_num";
        const char *type_id = "ANIM_T_INT";
        if (init.type == T_FLOAT) type_id = "ANIM_T_FLOAT";
        else if (init.type == T_COLOR) { fn = "anim_start_color"; type_id = NULL; }
        else if (init.type == T_STR) {
            diag_error(st->loc, "cannot animate a string mut");
            return;
        }
        /* Owner widget: `w` is in scope inside <surf>_on_click handlers.
         * Outside click context the codegen-emitted ctx doesn't bind `w` yet
         * — for now pass NULL (acceptable: bar_redraw_all() on the next status
         * tick will catch up). */
        const char *owner = "w";
        if (type_id) {
            fprintf(o, "%s%s(&mut_%s, %s, (double)(mut_%s), (double)(%s), (int)(%s), %s, %s, %s, NULL, NULL);\n",
                    indent, fn, nm, type_id, nm, to.text, dur.text, easing_id, bez_arg, owner);
        } else {
            fprintf(o, "%s%s(&mut_%s, mut_%s, (uint32_t)(%s), (int)(%s), %s, %s, %s, NULL, NULL);\n",
                    indent, fn, nm, nm, to.text, dur.text, easing_id, bez_arg, owner);
        }
        /* Mark surfaces that read this mut dirty so any side-effects (visible/
         * static layout) recompute on next tick. */
        if (r) {
            for (int i = 0; i < r->nsurfaces; i++) {
                const char **deps = r->surface_deps[i];
                for (int j = 0; deps && deps[j]; j++) {
                    if (strcmp(deps[j], nm) == 0) {
                        fprintf(o, "%sdirty_%s = 1;\n", indent, r->surface_names[i]);
                        break;
                    }
                }
            }
        }
        return;
    }
    case ST_EMIT: {
        const char *nm = sname(st->emit.name, st->emit.nlen);
        /* Lower each positional value into a comma-separated arg list. */
        char args[1024]; size_t off = 0; args[0] = 0;
        for (int i = 0; i < st->emit.n; i++) {
            CE ce = lower(ctx, st->emit.val[i]);
            if (off && off + 2 < sizeof args) { args[off++] = ','; args[off++] = ' '; args[off] = 0; }
            int wrote = snprintf(args + off, sizeof args - off, "(%s)", ce.text);
            if (wrote > 0) off += (size_t)wrote;
        }
        cgctx_flush_prelude(ctx, o, indent);
        /* Forward decl + call. Only spawn_osd() is emitted (gen_spawn.c); any
         * other template deliberately fails at link until a generic slot
         * allocator exists. */
        fprintf(o, "%sextern void spawn_%s();\n", indent, nm);
        fprintf(o, "%sspawn_%s(%s);\n", indent, nm, args);
        return;
    }
    }
}

/* Emit per-surface click dispatch. Each item that has on_click contributes a
 * record to a static hit array; pointer dispatch walks it.
 *
 * Surface-agnostic: items come from collect_bar_items (which accepts either
 * a surface body or a compound region body), `nm` namespaces the emitted
 * symbols (__<nm>_hit_*, <nm>_slider_<idx>_set_from, etc.). */
int emit_surface_click_dispatch(FILE *o, BarItem *items, int nitems,
                                       CGCtx *ctx, SemaResult *r, const char *nm) {
    /* Press: route to slider thunk for slider hits, record pressed idx for
     * the synthesized click-on-release path. */
    fprintf(o, "void %s_on_press(Widget *w, int wx, int wy, int btn) {\n", nm);
    fputs("    (void)btn;\n", o);
    fprintf(o, "    __%s_pressed_idx = -1; __%s_pressed_slider = -1; __%s_pressed_st = -1; __%s_pressed_w = 0;\n", nm, nm, nm, nm);
    fprintf(o, "    int __wi = __%s_slot(w); if (__wi < 0) { (void)wx; (void)wy; return; }\n", nm);
    fprintf(o, "    for (int i = 0; i < __%s_hit_n[__wi]; i++) {\n", nm);
    fprintf(o, "        if (wx < __%s_hit[__wi][i].x || wx >= __%s_hit[__wi][i].x + __%s_hit[__wi][i].w) continue;\n", nm, nm, nm);
    fprintf(o, "        if (wy < __%s_hit[__wi][i].y || wy >= __%s_hit[__wi][i].y + __%s_hit[__wi][i].h) continue;\n", nm, nm, nm);
    fprintf(o, "        __%s_pressed_idx = i;\n", nm);
    fprintf(o, "        __%s_pressed_st = __%s_hit[__wi][i].st_idx;\n", nm, nm);
    fprintf(o, "        __%s_pressed_w = w;\n", nm);
    fprintf(o, "        int sidx = __%s_hit[__wi][i].slider_idx;\n", nm);
    fprintf(o, "        if (sidx >= 0) {\n");
    fprintf(o, "            __%s_pressed_slider = sidx;\n", nm);
    /* Dispatch by slider idx to the right thunk. */
    int any_slider = 0;
    for (int it = 0; it < nitems; it++) if (items[it].slider_idx >= 0) any_slider = 1;
    if (any_slider) {
        fprintf(o, "            switch (sidx) {\n");
        for (int it = 0; it < nitems; it++) {
            if (items[it].slider_idx < 0) continue;
            fprintf(o, "                case %d: %s_slider_%d_set_from(w, __%s_hit[__wi][i].x, __%s_hit[__wi][i].y, __%s_hit[__wi][i].w, __%s_hit[__wi][i].h, wx, wy); break;\n",
                    items[it].slider_idx, nm, items[it].slider_idx, nm, nm, nm, nm);
        }
        fprintf(o, "            }\n");
    }
    fprintf(o, "        }\n");
    fprintf(o, "        return;\n");
    fprintf(o, "    }\n");
    fputs("    (void)w;\n}\n\n", o);

    /* Release: clear pressed state (click semantics for non-slider hits are
     * preserved by the existing on_click path called from wisp.c on state==1). */
    fprintf(o, "void %s_on_release(Widget *w, int wx, int wy, int btn) {\n", nm);
    fputs("    (void)w; (void)wx; (void)wy; (void)btn;\n", o);
    fprintf(o, "    __%s_pressed_idx = -1; __%s_pressed_slider = -1; __%s_pressed_st = -1; __%s_pressed_w = 0;\n", nm, nm, nm, nm);
    fputs("}\n\n", o);

    /* Motion: while a slider is pressed, recompute value from pointer. Bound to
     * the widget that received the press so a drag stays on its own surface. */
    fprintf(o, "void %s_on_motion(Widget *w, int wx, int wy) {\n", nm);
    fprintf(o, "    int sidx = __%s_pressed_slider;\n", nm);
    fprintf(o, "    if (sidx < 0 || w != __%s_pressed_w) { (void)w; (void)wx; (void)wy; return; }\n", nm);
    fprintf(o, "    int __wi = __%s_slot(w); if (__wi < 0) return;\n", nm);
    fprintf(o, "    int i = __%s_pressed_idx; if (i < 0 || i >= __%s_hit_n[__wi]) return;\n", nm, nm);
    if (any_slider) {
        fprintf(o, "    switch (sidx) {\n");
        for (int it = 0; it < nitems; it++) {
            if (items[it].slider_idx < 0) continue;
            fprintf(o, "        case %d: %s_slider_%d_set_from(w, __%s_hit[__wi][i].x, __%s_hit[__wi][i].y, __%s_hit[__wi][i].w, __%s_hit[__wi][i].h, wx, wy); break;\n",
                    items[it].slider_idx, nm, items[it].slider_idx, nm, nm, nm, nm);
        }
        fprintf(o, "    }\n");
    }
    fputs("}\n\n", o);

    fprintf(o, "void %s_on_click(Widget *w, int wx, int wy, int btn) {\n", nm);
    fputs("    (void)wy; (void)btn;\n", o);
    fprintf(o, "    int __wi = __%s_slot(w); if (__wi < 0) { (void)wx; return; }\n", nm);
    fprintf(o, "    for (int i = 0; i < __%s_hit_n[__wi]; i++) {\n", nm);
    fprintf(o, "        if (wx < __%s_hit[__wi][i].x || wx >= __%s_hit[__wi][i].x + __%s_hit[__wi][i].w) continue;\n", nm, nm, nm);
    fprintf(o, "        if (__%s_hit[__wi][i].slider_idx >= 0) continue;  /* sliders handle press, not click */\n", nm);
    fprintf(o, "        int kind = __%s_hit[__wi][i].kind; int arg = __%s_hit[__wi][i].arg;\n", nm, nm);
    /* Find each unique (widget, is_for) and emit a branch. */
    int handler_idx = 0;
    for (int it = 0; it < nitems; it++) {
        Widget *wd = items[it].w;
        WBody *clk = widget_onclick(wd);
        if (!clk) continue;
        if (items[it].is_for_cell && items[it].cell_idx > 0) continue;  /* one handler per for */
        int kind_val = items[it].is_runtime_for_cell ? 2 :
                       items[it].is_for_cell ? 1 : 0;
        fprintf(o, "        if (kind == %d", kind_val);
        if (!items[it].is_for_cell && !items[it].is_runtime_for_cell)
            fprintf(o, " && arg == %d", handler_idx);
        fputs(") {\n", o);
        /* Push local for param */
        if (items[it].is_for_cell) {
            push_local(ctx, items[it].for_var, items[it].for_var_n, LB_TAG_IDX, "arg", items[it].for_src);
        } else if (items[it].is_runtime_for_cell) {
            push_local(ctx, items[it].for_var, items[it].for_var_n,
                       items[it].runtime_for_kind, "arg", items[it].runtime_for_src);
        }
        if (clk->click.param && clk->click.plen) {
            push_local(ctx, clk->click.param, clk->click.plen, LB_CLICK_PARAM, "((const char*)0)", NULL);
            /* No real string param plumbing yet — keep as TODO. */
        }
        /* Lower stmt — exec/set/emit/block. */
        emit_stmt(o, ctx, clk->click.body, "            ", r);
        if (clk->click.param && clk->click.plen) pop_local(ctx);
        if (items[it].is_for_cell || items[it].is_runtime_for_cell) pop_local(ctx);
        /* Auto-refresh polled exec_line sources after a click — but ONLY for
         * sources the handler didn't already set() directly. An optimistic
         * set(src, ...) writes the predicted value into src_<n>_line; a parallel
         * refresh would race the user's exec() and frequently overwrite the
         * optimistic value with a stale probe read. */
        for (int si = 0; si < ctx->nsrc; si++) {
            if (ctx->srcs[si].drv->drv != DRV_EXEC) continue;
            const char *sn = sname(ctx->srcs[si].decl->name, ctx->srcs[si].decl->nlen);
            if (stmt_sets_name(clk->click.body, sn)) continue;
            fprintf(o, "            src_%s_refresh();\n", sn);
        }
        fputs("            return;\n        }\n", o);
        if (!items[it].is_for_cell && !items[it].is_runtime_for_cell) handler_idx++;
    }
    fputs("        (void)kind; (void)arg;\n", o);
    fputs("    }\n", o);
    fputs("    (void)w;\n", o);
    fputs("}\n\n", o);
    return 0;
}

/* ============================================================ */
/* Groups — container + contiguous members as one flex slot      */
/* ============================================================ */

static Expr *group_prop(Group *g, const char *name) {
    size_t L = strlen(name);
    for (int i = 0; i < g->nprops; i++)
        if (g->props[i]->nlen == L && memcmp(g->props[i]->name, name, L) == 0)
            return g->props[i]->val;
    return NULL;
}

/* One group member: read its measured st[] (dynamic fg/bg/text already
 * resolved), draw optional member bg then icon+text vertically centered in the
 * container height, push its click rect. Advances the local cursor __gx. */
static void emit_group_member(FILE *o, BarItem *it, const char *nm, int gap) {
    Widget *wd = it->w;
    int sb = it->st_base;
    int mr = eval_int(widget_prop(wd, "radius"), 0);
    int mbw = eval_int(widget_prop(wd, "border_width"), 1);
    int any_round = mr > 0;
    WBody *clk = widget_onclick(wd);
    fprintf(o, "        if (st[%d].vis) {\n", sb);
    fprintf(o, "            int __ma = (st[%d].h>0?st[%d].h:st[%d].tw);\n", sb, sb, sb);
    fprintf(o, "            const char *txt = st[%d].txt; uint32_t cp = st[%d].cp; const uint32_t *pm = st[%d].pm; int pms = st[%d].pms;\n", sb, sb, sb, sb);
    fprintf(o, "            uint32_t fg = st[%d].fg, bg = st[%d].bg, bdr = st[%d].border;\n", sb, sb, sb);
    fprintf(o, "            if (st[%d].press_bg & 0xff000000u && __%s_pressed_st == %d && __%s_pressed_w == w) bg = st[%d].press_bg;\n",
            sb, nm, sb, nm, sb);
    if (any_round) {
        fprintf(o, "            if (bg  & 0xff000000u) fill_rect_rounded(sl->px,w->w,w->h, __gx,__gy,__ma,__gh, %d,%d,%d,%d, bg);\n", mr, mr, mr, mr);
        fprintf(o, "            if (bdr & 0xff000000u) fill_rect_rounded_border(sl->px,w->w,w->h, __gx,__gy,__ma,__gh, %d,%d,%d,%d, %d,1,1,1,1,0, bdr);\n", mr, mr, mr, mr, mbw);
    } else {
        fprintf(o, "            if (bg  & 0xff000000u) fill_rect(sl->px,w->w,w->h, __gx,__gy,__ma,__gh, bg);\n");
    }
    fprintf(o, "            int __ty = __gy + (__gh - f->line_h)/2;\n");
    fprintf(o, "            int __cw = 0; if (cp || pms) __cw += cp_width(f, cp, pm, pms); if ((cp || pms) && txt && txt[0]) __cw += 2;\n");
    fprintf(o, "            if (txt) { const char *__p=txt; while(*__p&&*__p!='\\n')__p++; char __t2[256]; int __L=(int)(__p-txt); if(__L>255)__L=255; memcpy(__t2,txt,__L); __t2[__L]=0; __cw += text_width(f,__t2); }\n");
    fprintf(o, "            int __cx = __gx + (__ma - __cw)/2; if (__cx < __gx) __cx = __gx;\n");
    fprintf(o, "            if ((cp || pms) && (!txt || !txt[0])) draw_cp_centered(sl->px,w->w,w->h,__gx,__gy,__ma,__gh,f,cp,fg,pm,pms);\n");
    fprintf(o, "            else { if (cp || pms) { __cx += draw_cp(sl->px,w->w,w->h,__cx,__ty,f,cp,fg,pm,pms); if (txt&&txt[0]) { draw_text(sl->px,w->w,w->h,__cx,__ty,f,\" \",fg); __cx+=2; } } if (txt) draw_text(sl->px,w->w,w->h,__cx,__ty,f,txt,fg); }\n");
    if (clk)
        fprintf(o, "            { int __i = __%s_nhit++; __%s_hits_buf[__i].x=__gx; __%s_hits_buf[__i].y=__gy; __%s_hits_buf[__i].w=__ma; __%s_hits_buf[__i].h=__gh; __%s_hits_buf[__i].kind=0; __%s_hits_buf[__i].arg=%d; __%s_hits_buf[__i].slider_idx=-1; __%s_hits_buf[__i].st_idx=%d; }\n",
                nm, nm, nm, nm, nm, nm, nm, it->handler_idx, nm, nm, sb);
    fprintf(o, "            if (__ma) __gx += __ma + %d;\n", gap);
    fputs("        }\n", o);
}

/* On a vertical surface a group is a *band*: it takes one row of the stack
 * (its `height`), and its members lay out left-to-right inside that row. That
 * is the only way to get two differently-styled texts on one line, which a
 * menu's prompt + query row needs. */
int emit_group_draw(FILE *o, BarItem *items, int first, int nitems,
                    CGCtx *ctx, const char *nm, int vertical) {
    Group *g = items[first].grp;
    int gid = items[first].group_id;
    int cnt = 1;
    while (first + cnt < nitems && items[first + cnt].group_id == gid) cnt++;

    Align al = eval_align(group_prop(g, "align"));
    int pad  = eval_int(group_prop(g, "pad"), 0);
    int padx = eval_int(group_prop(g, "pad_x"), 0);
    int gap  = eval_int(group_prop(g, "gap"), 0);
    int ch   = eval_int(group_prop(g, "height"), 0);
    int r    = eval_int(group_prop(g, "radius"), 0);
    int bw   = eval_int(group_prop(g, "border_width"), 1);
    uint32_t cbg  = eval_color_ctx(ctx, group_prop(g, "bg"), 0);
    uint32_t cbor = eval_color_ctx(ctx, group_prop(g, "border"), 0);

    fputs("    {\n", o);
    fprintf(o, "        int __gw = %d, __gn = 0;\n", 2 * padx);
    for (int k = 0; k < cnt; k++) {
        int sb = items[first + k].st_base;
        /* zero-extent members (e.g. an empty menu.query cell) consume no gap */
        fprintf(o, "        if (st[%d].vis && (st[%d].h>0?st[%d].h:st[%d].tw) > 0) { __gw += (st[%d].h>0?st[%d].h:st[%d].tw); if (__gn) __gw += %d; __gn++; }\n",
                sb, sb, sb, sb, sb, sb, sb, gap);
    }
    /* Vertical: the band advances the stack by its own height and spans the
     * region's width; horizontal: it advances by the members' total width. */
    fprintf(o, "        int __adv = %s, pos;\n", vertical ? (ch > 0 ? "0" : "f->line_h") : "__gw");
    if (vertical && ch > 0) fprintf(o, "        __adv = %d;\n", ch);
    fprintf(o, "        switch (%d) {\n", (int)al);
    fprintf(o, "            case 1:  end_pos -= __adv + %d; pos = end_pos; break;\n", pad);
    fprintf(o, "            default: pos = start_pos; start_pos += __adv + %d; break;\n", pad);
    fprintf(o, "        }\n");
    if (vertical)
        fprintf(o, "        int __gy = pos, __gh = __adv, __bx = __reg_x, __bw = __reg_w;\n");
    else if (ch > 0)
        fprintf(o, "        int __gy = __reg_y + (__reg_h - %d)/2, __gh = %d, __bx = pos, __bw = __gw;\n", ch, ch);
    else
        fprintf(o, "        int __gy = __reg_y, __gh = __reg_h, __bx = pos, __bw = __gw;\n");
    if (r > 0) {
        if (cbg  & 0xff000000u) fprintf(o, "        fill_rect_rounded(sl->px,w->w,w->h, __bx,__gy,__bw,__gh, %d,%d,%d,%d, 0x%08xu);\n", r, r, r, r, cbg);
        if (cbor & 0xff000000u) fprintf(o, "        fill_rect_rounded_border(sl->px,w->w,w->h, __bx,__gy,__bw,__gh, %d,%d,%d,%d, %d,1,1,1,1,0, 0x%08xu);\n", r, r, r, r, bw, cbor);
    } else {
        if (cbg & 0xff000000u) fprintf(o, "        fill_rect(sl->px,w->w,w->h, __bx,__gy,__bw,__gh, 0x%08xu);\n", cbg);
    }
    fprintf(o, "        int __gx = __bx + %d; (void)__gw;\n", padx);
    for (int k = 0; k < cnt; k++)
        emit_group_member(o, &items[first + k], nm, gap);
    fputs("    }\n", o);
    return cnt;
}


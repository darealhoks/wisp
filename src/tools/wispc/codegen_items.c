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
        if (!widget_clickable(items[i].w)) continue;
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
/* Lower one for-block into items, appending at out[n]; returns the new n. */
static int collect_for_items(ForBlock *f, BarItem *out, int n, int max,
                             CGCtx *ctx, int *err) {
        {
            /* iter must be IDENT.list (tags) or IDENT.history (dbus_signal). */
            const char *tags_src = NULL, *dbus_src = NULL, *tray_src = NULL;
            int notif_hist = 0;
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
                /* tray rides DRV_WISP alongside dnd/gamma_warm, so the driver
                 * kind can't tell them apart — match the driver name. */
                else if (si && f->iter->member.flen == 5 &&
                         memcmp(f->iter->member.field, "items", 5) == 0 &&
                         strcmp(si->drv->name, "tray") == 0)
                    tray_src = sname(si->decl->name, si->decl->nlen);
                /* notifications() rides DRV_WISP too — match on the driver name
                 * for the same reason tray does. */
                else if (si && f->iter->member.flen == 7 &&
                         memcmp(f->iter->member.field, "history", 7) == 0 &&
                         strcmp(si->drv->name, "notifications") == 0)
                    notif_hist = 1;
            }
            if (!tags_src && !dbus_src && !menu_rows && !tray_src && !notif_hist) {
                diag_error(f->loc, "codegen: for-iter must be `rows`, <tags-src>.list, <dbus_signal-src>.history or <tray-src>.items");
                *err = 1; return n;
            }
            if (f->ncells != 1) {
                diag_error(f->loc, "codegen: for-block must contain exactly one cell { … }");
                *err = 1; return n;
            }
            if (menu_rows) {
                if (n >= max) { *err = 1; return n; }
                out[n] = (BarItem){0}; out[n].slider_idx = -1; out[n].graph_idx = -1; out[n].group_id = -1;
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
            } else if (notif_hist) {
                if (n >= max) { *err = 1; return n; }
                out[n] = (BarItem){0}; out[n].slider_idx = -1; out[n].graph_idx = -1; out[n].group_id = -1;
                out[n].w = f->cells[0];
                out[n].is_runtime_for_cell = true;
                out[n].runtime_for_count = "notif_count()";
                out[n].runtime_for_iter = "it";
                out[n].runtime_for_kind = LB_NOTIF_IT;
                out[n].runtime_for_cap = notif_hist_cap;
                out[n].for_var = f->var; out[n].for_var_n = f->vlen;
                n++;
            } else if (tray_src) {
                if (n >= max) { *err = 1; return n; }
                out[n] = (BarItem){0}; out[n].slider_idx = -1; out[n].graph_idx = -1; out[n].group_id = -1;
                out[n].w = f->cells[0];
                out[n].is_runtime_for_cell = true;
                out[n].runtime_for_src = strdup(tray_src);
                out[n].runtime_for_count = "tray_count()";
                out[n].runtime_for_iter = "it";
                out[n].runtime_for_kind = LB_TRAY_IT;
                out[n].runtime_for_cap = TRAY_ITEMS_CAP;
                out[n].for_var = f->var; out[n].for_var_n = f->vlen;
                n++;
            } else if (tags_src) {
                char *src_dup = strdup(tags_src);
                for (int k = 0; k < 9 /* MAX_TAGS */; k++) {
                    if (n >= max) { *err = 1; return n; }
                    out[n] = (BarItem){0}; out[n].slider_idx = -1; out[n].graph_idx = -1; out[n].group_id = -1;
                    out[n].w = f->cells[0]; out[n].is_for_cell = true; out[n].cell_idx = k;
                    out[n].for_var = f->var; out[n].for_var_n = f->vlen;
                    out[n].for_src = src_dup;
                    n++;
                }
            } else {
                if (n >= max) { *err = 1; return n; }
                char *src_dup = strdup(dbus_src);
                out[n] = (BarItem){0}; out[n].slider_idx = -1; out[n].graph_idx = -1; out[n].group_id = -1;
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
        }
    return n;
}

int collect_bar_items(SBody *body, int nbody, BarItem *out, int max,
                             CGCtx *ctx, int *err) {
    int n = 0;
    int gid_counter = 0;
    for (int i = 0; i < nbody; i++) {
        SBody *b = &body[i];
        if (b->kind == SB_WIDGET) {
            if (n >= max) { *err = 1; return n; }
            out[n] = (BarItem){0}; out[n].slider_idx = -1; out[n].graph_idx = -1; out[n].group_id = -1;
            out[n].w = b->widget; n++;
        } else if (b->kind == SB_GROUP) {
            Group *g = b->group;
            if (g->nmembers == 0) continue;
            int gid = gid_counter++;
            int gstart = n;
            for (int k = 0; k < g->nmembers; k++) {
                ForBlock *f = g->fors ? g->fors[k] : NULL;
                if (f) {
                    n = collect_for_items(f, out, n, max, ctx, err);
                    if (*err) return n;
                } else {
                    if (n >= max) { *err = 1; return n; }
                    out[n] = (BarItem){0}; out[n].slider_idx = -1; out[n].graph_idx = -1;
                    out[n].w = g->members[k];
                    n++;
                }
            }
            /* Stamp membership after the fact: a for-block member expands to
             * several items and they all belong to this container. */
            for (int k = gstart; k < n; k++) {
                out[k].group_id = gid;
                out[k].group_first = (k == gstart);
                out[k].grp = (k == gstart) ? g : NULL;
            }
        } else if (b->kind == SB_FOR) {
            n = collect_for_items(b->forb, out, n, max, ctx, err);
            if (*err) return n;
        } /* SB_PROP handled separately */
    }
    return n;
}

/* Main-axis room already eaten by the start-aligned items before this one —
 * on a horizontal surface that is the icon column and friends. Emitted
 * identically into the measure and draw passes: both must wrap at the same
 * width or the measured line count won't match what is drawn, and a column
 * wider than the drawable room gets every line elided (an OSD body wrapping
 * to the full slab width, then losing its tail under the icon's offset). */
static void emit_wrap_left(FILE *o, const char *indent, const char *idx_expr) {
    fprintf(o, "%sint __wleft = 0;\n", indent);
    fprintf(o, "%sfor (int __k = 0; __k < (%s); __k++)\n", indent, idx_expr);
    fprintf(o, "%s    if (st[__k].vis && st[__k].align == 0) __wleft += st[__k].tw + st[__k].pad;\n",
            indent);
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
    /* Slider/graph measure: claims an item slot with its main-axis extent
     * (from `width` / `height` prop, or surface-axis fallback). Cross axis
     * fills the surface. Pad/align reuse the bar-item flex pack. */
    if (it->slider_idx >= 0 || it->graph_idx >= 0) {
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
    Expr *imge  = widget_prop(wd, "image");
    /* Square side for `image`; unset tracks the font so a cell stays row-tall. */
    char img_sz[32] = "f->line_h";
    { int n = eval_int(widget_prop(wd, "image_size"), 0);
      if (n > 0) snprintf(img_sz, sizeof img_sz, "%d", n); }
    Expr *fge   = widget_prop(wd, "fg");
    Expr *bge   = widget_prop(wd, "bg");
    Expr *bord  = widget_prop(wd, "border");
    Expr *vise  = widget_prop(wd, "visible");
    Expr *widthe= widget_prop(wd, vertical ? "height" : "width");
    int   padE  = eval_int(widget_prop(wd, "pad"), 0);
    Expr *bline = widget_prop(wd, "body_lines");
    int   pad_xm= eval_int(widget_prop(wd, "pad_x"), 0);
    int   pad_ym= eval_int(widget_prop(wd, "pad_y"), 0);
    /* On a vertical surface the main axis IS the height, so pad_y has to enter
     * the measured extent — on a horizontal one it only nudges content inside a
     * slab the region already sizes. Skipped when `height` is declared: that is
     * the author's final row height, same rule as the horizontal path. */
    int   vpad_y = (vertical && !widthe) ? pad_ym : 0;
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
        fprintf(o, "    for (int it = 0; it < %s && it < %d; it++) {\n",
                it->runtime_for_count, it->runtime_for_cap);
        ctx->loop_cap = it->runtime_for_cap;
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
        fprintf(o, "%s  else if (!__s->prev && vis) { anim_start_num(&__s->rev, ANIM_T_FLOAT, __s->rev, 1.0, %d, %s, NULL, w, NULL, NULL, 1, 0); __s->prev = 1; }\n",
                indent, ve_in > 0 ? ve_in : 1, widget_easing_id(wd, "enter_easing"));
        fprintf(o, "%s  else if (__s->prev && !vis) { anim_start_num(&__s->rev, ANIM_T_FLOAT, __s->rev, 0.0, %d, %s, NULL, w, NULL, NULL, 1, 0); __s->prev = 0; }\n",
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
    if (imge) {
        /* `image` wins over `icon`, which stays as the fallback glyph when the
         * path is empty or undecodable — same rule as the OSD's $image. */
        CE ip = lower(ctx, imge);
        if (ip.type != T_STR) ip = coerce_to_str(ctx, ip, imge->loc);
        CE ic = (cicon.type == T_PIXMAP) ? (CE){ .text = "0", .type = T_INT }
                                         : coerce_to_int(ctx, cicon);
        cgctx_flush_prelude(ctx, o, indent);
        fprintf(o, "%suint32_t cp = (uint32_t)(%s);\n", indent, ic.text);
        fprintf(o, "%sint pms = %s;\n", indent, img_sz);
        fprintf(o, "%sconst uint32_t *pm = image_cell(%s, pms);\n", indent, ip.text);
        fprintf(o, "%sif (!pm) pms = 0;\n", indent);
    } else if (cicon.type == T_PIXMAP) {
        cgctx_flush_prelude(ctx, o, indent);
        fprintf(o, "%suint32_t cp = (uint32_t)(%s);\n", indent,
                cicon.pm_cp ? cicon.pm_cp : "0");
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

    /* icon_fg: alpha 0 means "absent" — the draw pass then falls back to fg,
     * so configs that never set it keep byte-identical output. No TransSlot:
     * `transition_fg` covers the text colour only. */
    Expr *ifge = widget_prop(wd, "icon_fg");
    CE ifg = { .text = "0u", .type = T_COLOR };
    if (ifge) ifg = lower(ctx, ifge);
    cgctx_flush_prelude(ctx, o, indent);
    fprintf(o, "%suint32_t icon_fg = (uint32_t)(%s);\n", indent, ifg.text);

    /* body_fg: same absent-means-fg contract as icon_fg, applied to the text
     * lines AFTER the first — a notification card dims its body without
     * needing a second widget (a `for` block only ever holds one cell). */
    Expr *bfge = widget_prop(wd, "body_fg");
    CE bfg = { .text = "0u", .type = T_COLOR };
    if (bfge) bfg = lower(ctx, bfge);
    cgctx_flush_prelude(ctx, o, indent);
    fprintf(o, "%suint32_t body_fg = (uint32_t)(%s);\n", indent, bfg.text);

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
        fprintf(o, "%s  uint32_t __ai = (uint32_t)(((icon_fg >> 24) & 0xffu) * __r);\n", indent);
        fprintf(o, "%s  icon_fg = (icon_fg & 0x00ffffffu) | (__ai << 24);\n", indent);
        fprintf(o, "%s  uint32_t __ay = (uint32_t)(((body_fg >> 24) & 0xffu) * __r);\n", indent);
        fprintf(o, "%s  body_fg = (body_fg & 0x00ffffffu) | (__ay << 24);\n", indent);
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
    /* `wrap`: measure against the WRAPPED text, so the content-fit width and
     * body_fit's line count see the lines that will actually be drawn. The draw
     * pass re-wraps — text_wrapped()'s scratch is shared, so the pointer can't
     * be held across items. Column = the region room minus pad_x, same budget
     * `elide` clamps to. */
    fprintf(o, "%sconst char *__mtxt = txt;\n", indent);
    fprintf(o, "%s(void)__mtxt;\n", indent);
    if (widget_flag(wd, "wrap")) {
        fprintf(o, "%sif (txt && txt[0]) {\n", indent);
        /* Vertical draw insets content by `pad + pad_x` and the trailing pad_x
         * stays free, so the column must lose `pad + 2*pad_x` — measuring with
         * only 2*pad_x overshoots the region and clips the last glyph. */
        if (vertical) {
            { Expr *vw = widget_prop(wd, "width");   /* same row width the draw uses */
              if (vw) fprintf(o, "%s    int __ww = %d - %d;\n", indent, eval_int(vw, 0), padE + 2 * pad_xm);
              else    fprintf(o, "%s    int __ww = __reg_w - %d;\n", indent, padE + 2 * pad_xm); }
        } else {
            emit_wrap_left(o, "            ", idx_expr);
            fprintf(o, "%s    int __ww = __reg_w - __wleft - %d;\n", indent, 2 * pad_xm);
        }
        fprintf(o, "%s    __mtxt = text_wrapped(f, txt, __ww, __bl, (int *)0);\n", indent);
        fprintf(o, "%s}\n", indent);
    }
    /* body_fit: shrink the reserved slab to the text's real line count, so
     * body_lines is only a ceiling — stacked cells on a vertical surface sit
     * flush instead of padding out to the tallest possible one. */
    if (widget_flag(wd, "body_fit")) {
        fprintf(o, "%s{ int __fl = 0;\n", indent);
        fprintf(o, "%s  if (__mtxt && __mtxt[0]) { __fl = 1; for (const char *__q = __mtxt; *__q; __q++) if (*__q == '\\n' && __q[1]) __fl++; }\n", indent);
        fprintf(o, "%s  if (__fl < 1) __fl = 1;\n", indent);
        fprintf(o, "%s  if (__fl < __bl) __bl = __fl; }\n", indent);
    }

    /* Icon column: `icon_box` reserves a fixed-width box, 0 = auto (the
     * glyph's ink-aware cp_width). `icon_gap` is the explicit gap between
     * that column and the text (replaces the old magic \" \"+2px). */
    int icon_box = eval_int(widget_prop(wd, "icon_box"), 0);
    int icon_gap = eval_int(widget_prop(wd, "icon_gap"), 2);
    char icw[40];
    if (icon_box > 0) snprintf(icw, sizeof icw, "%d", icon_box);
    else snprintf(icw, sizeof icw, "cp_width(f, cp, pm, pms)");
    if (widthe) {
        CE wd_ce = lower(ctx, widthe); wd_ce = coerce_to_int(ctx, wd_ce);
        cgctx_flush_prelude(ctx, o, indent);
        fprintf(o, "%sint tw = %s;\n", indent, wd_ce.text);
    } else if (vertical) {
        fprintf(o, "%sint tw = f->line_h * __bl + %d;\n", indent, 2 * vpad_y);
    } else {
        fprintf(o, "%sint tw = 0;\n", indent);
        fprintf(o, "%sif (cp || pms)  tw += %s;\n", indent, icw);
        fprintf(o, "%sif ((cp || pms) && txt && txt[0]) tw += %d;\n", indent, icon_gap);
        fprintf(o, "%sif (__mtxt) {\n", indent);
        fprintf(o, "%s    const char *__p = __mtxt; int __w = 0;\n", indent);
        fprintf(o, "%s    while (__p && *__p) {\n", indent);
        fprintf(o, "%s        const char *__nl = __p; while (*__nl && *__nl != '\\n') __nl++;\n", indent);
        fprintf(o, "%s        char __tmp[256]; int __L = (int)(__nl - __p); if (__L > 255) __L = 255;\n", indent);
        fprintf(o, "%s        memcpy(__tmp, __p, __L); __tmp[__L] = 0;\n", indent);
        fprintf(o, "%s        int __lw = text_width(f, __tmp); if (__lw > __w) __w = __lw;\n", indent);
        fprintf(o, "%s        if (!*__nl) break;\n%s        __p = __nl + 1;\n", indent, indent);
        fprintf(o, "%s    }\n", indent);
        fprintf(o, "%s    tw += __w;\n", indent);
        fprintf(o, "%s}\n", indent);
        /* Inner horizontal padding grows the content-fit slab so the bg slab
         * and layout advance both account for the breathing room. */
        if (pad_xm > 0) fprintf(o, "%stw += %d;\n", indent, 2 * pad_xm);
    }
    /* h: multi-line slab height when body_lines > 1. Default 0 → use tw advance. */
    fprintf(o, "%sint __h = __bl > 1 ? (__bl * f->line_h + %d) : 0;\n", indent, 2 * vpad_y);

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
    /* hover_bg: same, for the cell the pointer is over. */
    Expr *hbge = widget_prop(wd, "hover_bg");
    if (hbge) {
        CE hbg = lower(ctx, hbge);
        cgctx_flush_prelude(ctx, o, indent);
        fprintf(o, "%suint32_t hover_bg = (uint32_t)(%s);\n", indent, hbg.text);
    } else {
        fprintf(o, "%suint32_t hover_bg = 0u;\n", indent);
    }

    fprintf(o, "%sst[%s].vis = vis;\n", indent, idx_expr);
    fprintf(o, "%sst[%s].txt = txt;\n", indent, idx_expr);
    fprintf(o, "%sst[%s].cp  = cp;\n", indent, idx_expr);
    fprintf(o, "%sst[%s].pm  = pm;\n", indent, idx_expr);
    fprintf(o, "%sst[%s].pms = pms;\n", indent, idx_expr);
    fprintf(o, "%sst[%s].fg  = fg;\n", indent, idx_expr);
    fprintf(o, "%sst[%s].icon_fg = icon_fg;\n", indent, idx_expr);
    fprintf(o, "%sst[%s].body_fg = body_fg;\n", indent, idx_expr);
    fprintf(o, "%sst[%s].bg  = bg;\n", indent, idx_expr);
    fprintf(o, "%sst[%s].press_bg = press_bg;\n", indent, idx_expr);
    fprintf(o, "%sst[%s].hover_bg = hover_bg;\n", indent, idx_expr);
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
    ctx->loop_cap = 0;
}

/* Color CE: lower an expression (const/mut/ternary/literal) to a uint32_t-typed
 * C expression, or fall back to a default literal when the prop is absent. */
/* text_align pins the multi-line text BLOCK inside the cell across the text
 * axis: 0 center (default, today's behaviour), 1 top, 2 bottom. Compile-time
 * only — the choice is per widget declaration, never per item. */
static int widget_text_align(Widget *w) {
    Expr *e = widget_prop(w, "text_align");
    if (!e || e->kind != EX_IDENT) return 0;
    const char *s = e->ident.s; size_t n = e->ident.n;
    if (n == 3 && !memcmp(s, "top",   3)) return 1;
    if (n == 5 && !memcmp(s, "start", 5)) return 1;
    if (n == 6 && !memcmp(s, "bottom",6)) return 2;
    if (n == 3 && !memcmp(s, "end",   3)) return 2;
    return 0;
}

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
    if (it->graph_idx >= 0) {
        const char *indent = "        ";
        /* ponytail: the slot-rect prologue mirrors the slider block below;
         * factor into a helper if a third slot-widget kind ever appears. */
        fprintf(o, "    if (st[%d].vis) {\n", it->st_base);
        fprintf(o, "%sint tw = st[%d].tw, pad = st[%d].pad;\n", indent, it->st_base, it->st_base);
        fprintf(o, "%sint pos;\n", indent);
        fprintf(o, "%sswitch (st[%d].align) {\n", indent, it->st_base);
        fprintf(o, "%s    case 0:  pos = start_pos; start_pos += tw + pad; break;\n", indent);
        fprintf(o, "%s    case 1:  pos = end_pos - tw; end_pos -= tw + pad; break;\n", indent);
        fprintf(o, "%s    default: pos = center_pos; center_pos += tw + pad; break;\n", indent);
        fprintf(o, "%s}\n", indent);
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
        CE c_bg = color_ce(ctx, widget_prop(wd, "bg"),      "0u");
        CE c_fg = color_ce(ctx, widget_prop(wd, "graph_fg"), "0xffffffffu");
        int rad = eval_int(widget_prop(wd, "radius"), 0);
        double vmax = eval_double(widget_prop(wd, "graph_max"), 100.0);
        if (vmax <= 0) vmax = 100.0;
        const GraphReg *gr = graph_reg_at(it->graph_idx);
        cgctx_flush_prelude(ctx, o, indent);
        fprintf(o, "%sif ((uint32_t)(%s) & 0xff000000u) fill_rect_rounded(sl->px, w->w, w->h,"
                   " rx, ry, rw, rh, %d, %d, %d, %d, (uint32_t)(%s));\n",
                indent, c_bg.text, rad, rad, rad, rad, c_bg.text);
        fprintf(o, "%sextern float wg_ring_%d[]; extern int wg_head_%d, wg_len_%d;\n",
                indent, it->graph_idx, it->graph_idx, it->graph_idx);
        fprintf(o, "%sdraw_sparkline(sl->px, w->w, w->h, rx, ry, rw, rh, wg_ring_%d, wg_len_%d,"
                   " wg_head_%d, %d, %f, (uint32_t)(%s));\n",
                indent, it->graph_idx, it->graph_idx, it->graph_idx, gr->cap, vmax, c_fg.text);
        fprintf(o, "    }\n");
        return;
    }
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
        fprintf(o, "%s    case 1:  pos = end_pos - tw; end_pos -= tw + pad; break;\n", indent);
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
            fprintf(o, "%s(void)__vw;   /* some align branches place by edge, not width */\n", indent);
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
        /* 64 == the __<nm>_hits_buf[] size emitted by codegen_surface.c; a
         * config with more clickable cells than that drops the overflow
         * instead of writing past the array. */
        fprintf(o, "%s{ int __i = __%s_nhit; if (__i < 64) { __%s_nhit++; __%s_hits_buf[__i].x = rx; __%s_hits_buf[__i].y = ry; "
                   "__%s_hits_buf[__i].w = rw; __%s_hits_buf[__i].h = rh; "
                   "__%s_hits_buf[__i].kind = 0; __%s_hits_buf[__i].arg = 0; __%s_hits_buf[__i].slider_idx = %d; __%s_hits_buf[__i].st_idx = %d; "
                   "__%s_hits_buf[__i].tip = %s; } }\n",
                indent, nm, nm, nm, nm, nm, nm, nm, nm, nm, it->slider_idx, nm, it->st_base,
                nm, widget_tip_lit(it->w));
        fprintf(o, "    }\n");
        return;
    }
    int clk = widget_clickable(it->w);
    char idx_expr[32];
    const char *indent = "        ";
    int kind;
    if (it->is_runtime_for_cell) {
        snprintf(idx_expr, sizeof idx_expr, "(%d + it)", it->st_base);
        fprintf(o, "    for (int it = 0; it < %s && it < %d; it++) {\n",
                it->runtime_for_count, it->runtime_for_cap);
        ctx->loop_cap = it->runtime_for_cap;
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
    /* Separator rows: menu.c gives them their own slot height, so the draw has
     * to advance by the same amount or the hit grid and the pixels diverge. */
    int sep = vertical && it->is_runtime_for_cell && ctx->menu_sep_h > 0
              && (ctx->menu_sep_col & 0xff000000u);
    if (sep) {
        fprintf(o, "%s    int __sep = w->s.menu.row_flags && "
                   "(w->s.menu.row_flags[w->s.menu.filtered[it]] & MENU_ROW_SEPARATOR);\n", indent);
        fprintf(o, "%s    if (__sep) __adv = %d;\n", indent, ctx->menu_sep_h);
    }
    fprintf(o, "%s    const char *txt = st[%s].txt; uint32_t cp = st[%s].cp; const uint32_t *pm = st[%s].pm; int pms = st[%s].pms;\n", indent, idx_expr, idx_expr, idx_expr, idx_expr);
    fprintf(o, "%s    uint32_t fg = st[%s].fg, bg = st[%s].bg, bdr = st[%s].border; (void)bdr;\n",
            indent, idx_expr, idx_expr, idx_expr);
    fprintf(o, "%s    uint32_t ifg = st[%s].icon_fg; if (!(ifg & 0xff000000u)) ifg = fg; (void)ifg;\n",
            indent, idx_expr);
    fprintf(o, "%s    uint32_t bfg = st[%s].body_fg; if (!(bfg & 0xff000000u)) bfg = fg; (void)bfg;\n",
            indent, idx_expr);
    /* press_bg override: while this st-index is the surface's pressed_st, swap
     * bg for the widget's press_bg if it has one. */
    fprintf(o, "%s    if (st[%s].hover_bg & 0xff000000u && __%s_hover_st == (%s) && __%s_hover_w == w) bg = st[%s].hover_bg;\n",
            indent, idx_expr, nm, idx_expr, nm, idx_expr);
    fprintf(o, "%s    if (st[%s].press_bg & 0xff000000u && __%s_pressed_st == (%s) && __%s_pressed_w == w) bg = st[%s].press_bg;\n",
            indent, idx_expr, nm, idx_expr, nm, idx_expr);
    fprintf(o, "%s    int body_lines = st[%s].body_lines;\n", indent, idx_expr);
    /* A vertical row spans the region cross-axis unless the cell declares an
     * explicit `width` — then it is that wide and centred, so a panel's border
     * and corner radius stay clear of it instead of being painted over. */
    if (vertical) {
        Expr *vw = widget_prop(wd, "width");
        if (vw) fprintf(o, "%s    int __rw = %d, __rx = __reg_x + (__reg_w - %d) / 2;\n",
                        indent, eval_int(vw, 0), eval_int(vw, 0));
        else    fprintf(o, "%s    int __rw = __reg_w, __rx = __reg_x;\n", indent);
    }
    /* Re-wrap for the draw: the measure pass's scratch has been reused by every
     * item measured after this one. body_lines is the (already body_fit-clamped)
     * ceiling, so both passes produce the same lines. */
    if (widget_flag(wd, "wrap")) {
        /* Same column the measure pass used — the declared `pad` constant, not
         * st[].pad, so a reveal tween can't re-break the lines mid-animation. */
        int wrap_inset = 2 * eval_int(widget_prop(wd, "pad_x"), 0);
        if (vertical) wrap_inset += eval_int(widget_prop(wd, "pad"), 0);
        if (!vertical) emit_wrap_left(o, "            ", idx_expr);
        fprintf(o, "%s    if (txt && txt[0]) txt = text_wrapped(f, txt, %s - %s%d, body_lines, (int *)0);\n",
                indent, vertical ? "__rw" : "__reg_w", vertical ? "" : "__wleft - ", wrap_inset);
    }
    fprintf(o, "%s    int pos;\n", indent);
    fprintf(o, "%s    switch (st[%s].align) {\n", indent, idx_expr);
    fprintf(o, "%s        case 0:  pos = start_pos; start_pos += __adv + pad; break;\n", indent);
    fprintf(o, "%s        case 1:  pos = end_pos - __adv; end_pos -= __adv + pad; break;\n", indent);
    fprintf(o, "%s        default: pos = center_pos; center_pos += __adv + pad; break;\n", indent);
    fprintf(o, "%s    }\n", indent);
    /* Scrollable container: this is one scrolled row. Recorded in content
     * coordinates so the wheel can snap to the next row top and the arrow keys
     * can walk rows of unequal height. */
    if (ctx->scroll_rows && vertical)
        fprintf(o, "%s    if (st[%s].align == 0) __%s_rowrec(__wi, pos + w->scroll_off - __rowbase0, %s);\n",
                indent, idx_expr, nm, idx_expr);
    /* align=end mirrors: the pad band sits BEFORE pos, so clears/hits start
     * there rather than trailing past the content. */
    fprintf(o, "%s    int __bs = st[%s].align == 1 ? pos - pad : pos;\n", indent, idx_expr);
    fprintf(o, "%s    (void)__bs;\n", indent);
    if (sep) {
        fprintf(o, "%s    if (__sep) {\n", indent);
        fprintf(o, "%s        int __sw = __reg_w * %d / 100;\n", indent, ctx->menu_sep_frac);
        fprintf(o, "%s        fill_rect(sl->px, w->w, w->h, __reg_x + (__reg_w - __sw) / 2,"
                   " pos + (__adv - 1) / 2, __sw, 1, 0x%08xu);\n",
                indent, ctx->menu_sep_col);
        fprintf(o, "%s        continue;\n%s    }\n", indent, indent);
    }
    /* Partial repaint gate: skip a cell whose sources didn't tick (its pixels
     * survive in the copied-forward buffer); overwrite a dirty cell with the
     * flat surface bg before its content redraws, and grow the damage span. The
     * layout counters above always run so neighbours still get their positions.
     * partial_ok is set only for flat horizontal bars — main axis is X here. */
    if (ctx->partial_ok) {
        fprintf(o, "%s    if (!__partial || (0x%016llxull & __wds)) {\n",
                indent, (unsigned long long)it->dep_mask);
        fprintf(o, "%s    if (__partial) { fill_rect(sl->px, w->w, w->h, __bs, __reg_y, __adv + pad, __reg_h, 0x%08xu);\n",
                indent, ctx->surface_bg);
        fprintf(o, "%s        if (__bs < __dmg_x0) __dmg_x0 = __bs;\n", indent);
        fprintf(o, "%s        if (__bs + __adv + pad > __dmg_x1) __dmg_x1 = __bs + __adv + pad; }\n", indent);
    }
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
        /* Main axis is Y, so the slab already spans the region width and
         * pad_x can't grow it — it insets the content instead (both sides,
         * the elide clamp below takes the trailing one). */
        fprintf(o, "%s    int cx = __rx + pad + %d;\n",
                indent, eval_int(widget_prop(wd, "pad_x"), 0));
        /* pad_y is inside the measured row height (see vpad_y), so start/end
         * alignment must inset by it or the text sits on the card's edge. */
        int v_pady = eval_int(widget_prop(wd, "pad_y"), 0);
        switch (widget_text_align(wd)) {
        case 1:  fprintf(o, "%s    int cy = pos + %d;\n", indent, v_pady); break;
        case 2:  fprintf(o, "%s    int cy = pos + __adv - %d - f->line_h * body_lines; if (cy < pos) cy = pos;\n", indent, v_pady); break;
        default: fprintf(o, "%s    int cy = pos + (__adv - f->line_h * body_lines) / 2; if (cy < pos) cy = pos;\n", indent); break;
        }
        if (shc & 0xff000000u)
            fprintf(o, "%s    fill_rounded_shadow(sl->px, w->w, w->h, __rx + %d, pos + %d, __rw + %d, __adv + %d, %d, %d, 0x%08xu);\n",
                    indent, shx - shspread, shy - shspread, 2 * shspread, 2 * shspread,
                    maxr + shspread, shblur > 0 ? shblur : 8, shc);
        if (any_round) {
            fprintf(o, "%s    if (bg  & 0xff000000u) fill_rect_rounded(sl->px, w->w, w->h, __rx, pos, __rw, __adv, %d, %d, %d, %d, bg);\n",
                    indent, r_tl, r_tr, r_br, r_bl);
            fprintf(o, "%s    if (bdr & 0xff000000u) fill_rect_rounded_border(sl->px, w->w, w->h, __rx, pos, __rw, __adv, %d, %d, %d, %d, %d, 1, 1, 1, 1, 0, bdr);\n",
                    indent, r_tl, r_tr, r_br, r_bl, vbw);
        } else {
            fprintf(o, "%s    if (bg  & 0xff000000u) fill_rect(sl->px, w->w, w->h, __rx, pos, __rw, __adv, bg);\n", indent);
            fprintf(o, "%s    if (bdr & 0xff000000u) {\n", indent);
            fprintf(o, "%s        fill_rect(sl->px, w->w, w->h, __rx, pos, __rw, %d, bdr);\n", indent, vbw);
            fprintf(o, "%s        fill_rect(sl->px, w->w, w->h, __rx, pos + __adv - %d, __rw, %d, bdr);\n", indent, vbw, vbw);
            fprintf(o, "%s        fill_rect(sl->px, w->w, w->h, __rx, pos, %d, __adv, bdr);\n", indent, vbw);
            fprintf(o, "%s        fill_rect(sl->px, w->w, w->h, __rx + __rw - %d, pos, %d, __adv, bdr);\n", indent, vbw, vbw);
            fprintf(o, "%s    }\n", indent);
        }
        int v_icb = eval_int(widget_prop(wd, "icon_box"), 0);
        int v_icg = eval_int(widget_prop(wd, "icon_gap"), 2);
        char v_icw[40];
        if (v_icb > 0) snprintf(v_icw, sizeof v_icw, "%d", v_icb);
        else snprintf(v_icw, sizeof v_icw, "cp_width(f, cp, pm, pms)");
        /* Cross-axis centering, same rule as the horizontal path: a cell that
         * declares its own `width` centers icon+text inside it (measured on the
         * first line) instead of hugging the left inset. A row without `width`
         * spans the region and stays start-aligned — that is what a wrapped
         * notification card wants. */
        if (widget_prop(wd, "width")) {
            int v_padx = eval_int(widget_prop(wd, "pad_x"), 0);
            fprintf(o, "%s    { int __cw = 0;\n", indent);
            fprintf(o, "%s      if (cp || pms) __cw += %s;\n", indent, v_icw);
            fprintf(o, "%s      if ((cp || pms) && txt && txt[0]) __cw += %d;\n", indent, v_icg);
            fprintf(o, "%s      if (txt) { const char *__q = txt; while (*__q && *__q != '\\n') __q++; char __t3[256]; int __L3 = (int)(__q - txt); if (__L3 > 255) __L3 = 255; memcpy(__t3, txt, __L3); __t3[__L3] = 0; __cw += text_width(f, __t3); }\n", indent);
            fprintf(o, "%s      int __av = __rw - pad - %d;\n", indent, 2 * v_padx);
            fprintf(o, "%s      if (__cw < __av) cx += (__av - __cw) / 2; }\n", indent);
        }
        fprintf(o, "%s    if (cp || pms) { int __icw = %s; draw_cp_centered(sl->px, w->w, w->h, cx, cy, __icw, f->line_h, f, cp, ifg, pm, pms); cx += __icw; if (txt && txt[0]) cx += %d; }\n",
                indent, v_icw, v_icg);
        fprintf(o, "%s    if (txt) {\n", indent);
        fprintf(o, "%s        const char *__p = txt; int __ln = 0;\n", indent);
        fprintf(o, "%s        while (__p && *__p && __ln < body_lines) {\n", indent);
        fprintf(o, "%s            const char *__nl = __p; while (*__nl && *__nl != '\\n') __nl++;\n", indent);
        fprintf(o, "%s            char __tmp[256]; int __L = (int)(__nl - __p); if (__L > 255) __L = 255;\n", indent);
        fprintf(o, "%s            memcpy(__tmp, __p, __L); __tmp[__L] = 0;\n", indent);
        /* Same as the horizontal path: `elide` clamps to the room left in the
         * row and appends '…', instead of running past the slab. */
        if (widget_flag(wd, "elide"))
            fprintf(o, "%s            draw_text_elided(sl->px, w->w, w->h, cx, cy + __ln * f->line_h, f, __tmp, __rx + __rw - cx - %d, __ln ? bfg : fg);\n",
                    indent, eval_int(widget_prop(wd, "pad_x"), 0));
        else
            fprintf(o, "%s            draw_text(sl->px, w->w, w->h, cx, cy + __ln * f->line_h, f, __tmp, __ln ? bfg : fg);\n", indent);
        fprintf(o, "%s            __ln++;\n%s            if (!*__nl) break;\n%s            __p = __nl + 1;\n", indent, indent, indent);
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
        int ta = widget_text_align(wd);
        if (heighte) {
            if (ta == 1)      fprintf(o, "%s    int __ty = __by + %d;\n", indent, pad_y);
            else if (ta == 2) fprintf(o, "%s    int __ty = __by + __bh - f->line_h * body_lines + %d;\n", indent, pad_y);
            else              fprintf(o, "%s    int __ty = __by + (__bh - f->line_h * body_lines) / 2 + %d;\n", indent, pad_y);
        } else {
            if (ta == 1)      fprintf(o, "%s    int __ty = (__h > 0 ? __by : y) + %d;\n", indent, pad_y);
            else if (ta == 2) fprintf(o, "%s    int __ty = (__h > 0 ? __by + (__reg_h - __h) : y) + %d;\n", indent, pad_y);
            else              fprintf(o, "%s    int __ty = (__h > 0 ? __by + (__reg_h - __h) / 2 : y) + %d;\n", indent, pad_y);
        }
        fprintf(o, "%s    if (__ty < __reg_y) __ty = __reg_y;\n", indent);
        /* Centering: when width is fixed, compute the actual content width and
         * offset x so icon+text sits in the middle of the cell (within pad_x). */
        int h_icb = eval_int(widget_prop(wd, "icon_box"), 0);
        int h_icg = eval_int(widget_prop(wd, "icon_gap"), 2);
        char h_icw[40];
        if (h_icb > 0) snprintf(h_icw, sizeof h_icw, "%d", h_icb);
        else snprintf(h_icw, sizeof h_icw, "cp_width(f, cp, pm, pms)");
        if (has_fixed_w) {
            fprintf(o, "%s    int __cw = 0;\n", indent);
            fprintf(o, "%s    if (cp || pms) __cw += %s;\n", indent, h_icw);
            fprintf(o, "%s    if ((cp || pms) && txt && txt[0]) __cw += %d;\n", indent, h_icg);
            fprintf(o, "%s    if (txt) { const char *__p = txt; while (*__p && *__p != '\\n') __p++; char __t2[256]; int __L = (int)(__p - txt); if (__L > 255) __L = 255; memcpy(__t2, txt, __L); __t2[__L] = 0; __cw += text_width(f, __t2); }\n", indent);
            fprintf(o, "%s    if (__cw < tw) x += (tw - __cw) / 2;\n", indent);
        }
        /* Icon-only path bbox-centers inside the whole cell bg box; icon+text
         * bbox-centers inside its reserved column, then advances column+gap. */
        fprintf(o, "%s    if ((cp || pms) && (!txt || !txt[0])) {\n", indent);
        fprintf(o, "%s        draw_cp_centered(sl->px, w->w, w->h, __bx, __by, __bw, __bh, f, cp, ifg, pm, pms);\n", indent);
        fprintf(o, "%s    } else if (cp || pms) { int __icw = %s; draw_cp_centered(sl->px, w->w, w->h, x, __ty, __icw, f->line_h, f, cp, ifg, pm, pms); x += __icw; if (txt && txt[0]) x += %d; }\n",
                indent, h_icw, h_icg);
        fprintf(o, "%s    if (txt) {\n", indent);
        fprintf(o, "%s        const char *__p = txt; int __ln = 0;\n", indent);
        fprintf(o, "%s        while (__p && *__p && __ln < body_lines) {\n", indent);
        fprintf(o, "%s            const char *__nl = __p; while (*__nl && *__nl != '\\n') __nl++;\n", indent);
        fprintf(o, "%s            char __tmp[256]; int __L = (int)(__nl - __p); if (__L > 255) __L = 255;\n", indent);
        fprintf(o, "%s            memcpy(__tmp, __p, __L); __tmp[__L] = 0;\n", indent);
        /* `elide` clamps each line to whatever room is left in the region —
         * without it a long summary just runs off the slab. The room stops at
         * the first end-aligned item's column, not at the region edge: those
         * are drawn after this one, so end_pos hasn't been walked back yet and
         * an OSD summary would elide straight over the "42%". */
        if (widget_flag(wd, "elide")) {
            fprintf(o, "%s            int __rsv = 0;\n", indent);
            fprintf(o, "%s            for (int __k = 0; __k < (int)(sizeof st / sizeof st[0]); __k++)\n", indent);
            fprintf(o, "%s                if (st[__k].vis && st[__k].align == 1 && __k != (%s)) __rsv += st[__k].tw + st[__k].pad;\n",
                    indent, idx_expr);
            fprintf(o, "%s            draw_text_elided(sl->px, w->w, w->h, x, __ty + __ln * f->line_h, f, __tmp, __reg_x + __reg_w - __rsv - x - %d, __ln ? bfg : fg);\n",
                    indent, pad_x > 0 ? pad_x : 0);
        }
        else
            fprintf(o, "%s            draw_text(sl->px, w->w, w->h, x, __ty + __ln * f->line_h, f, __tmp, __ln ? bfg : fg);\n", indent);
        fprintf(o, "%s            __ln++;\n%s            if (!*__nl) break;\n%s            __p = __nl + 1;\n", indent, indent, indent);
        fprintf(o, "%s        }\n", indent);
        fprintf(o, "%s    }\n", indent);
    }
    if (ctx->partial_ok) fprintf(o, "%s    }\n", indent);  /* close partial gate */
    /* A `tooltip`-only cell still needs a rect to hover-test against, but must
     * never match a click branch — kind -1 is matched by no dispatch case. */
    if (!clk && widget_prop(it->w, "tooltip")) kind = -1;
    if (clk || kind == -1) {
        int arg_val = it->is_for_cell ? it->cell_idx : it->handler_idx;
        /* Hit rect in compound/surface-local coords. For vertical (main=Y) the
         * cross-axis spans the region's X extent; for horizontal it spans the
         * region's Y extent. Origin offsets come from __reg_x/__reg_y so
         * compound regions translate correctly. */
        const char *hx = vertical ? "__rx" : "__bs";
        const char *hy = vertical ? "__bs"    : "__reg_y";
        const char *hw = vertical ? "__rw" : "(__adv + pad)";
        const char *hh = vertical ? "(__adv + pad)" : "__reg_h";
        if (it->is_runtime_for_cell) {
            fprintf(o, "%s    { int __i = __%s_nhit; if (__i < 64) { __%s_nhit++; __%s_hits_buf[__i].x = %s; __%s_hits_buf[__i].y = %s; "
                       "__%s_hits_buf[__i].w = %s; __%s_hits_buf[__i].h = %s; "
                       "__%s_hits_buf[__i].kind = %d; __%s_hits_buf[__i].arg = it; __%s_hits_buf[__i].slider_idx = -1; __%s_hits_buf[__i].st_idx = (%d + it); "
                       "__%s_hits_buf[__i].tip = %s; } }\n",
                    indent, nm, nm, nm, hx, nm, hy, nm, hw, nm, hh, nm, kind, nm, nm, nm, it->st_base,
                    nm, widget_tip_lit(it->w));
        } else {
            fprintf(o, "%s    { int __i = __%s_nhit; if (__i < 64) { __%s_nhit++; __%s_hits_buf[__i].x = %s; __%s_hits_buf[__i].y = %s; "
                       "__%s_hits_buf[__i].w = %s; __%s_hits_buf[__i].h = %s; "
                       "__%s_hits_buf[__i].kind = %d; __%s_hits_buf[__i].arg = %d; __%s_hits_buf[__i].slider_idx = -1; __%s_hits_buf[__i].st_idx = %d; "
                       "__%s_hits_buf[__i].tip = %s; } }\n",
                    indent, nm, nm, nm, hx, nm, hy, nm, hw, nm, hh, nm, kind, nm, arg_val, nm, nm, it->st_base,
                    nm, widget_tip_lit(it->w));
        }
    }
    fprintf(o, "%s}\n", indent);
    fputs("    }\n", o);
    ctx->loop_cap = 0;
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

/* Emit per-surface click dispatch. Each item that has on_click contributes a
 * record to a static hit array; pointer dispatch walks it.
 *
 * Surface-agnostic: items come from collect_bar_items (which accepts either
 * a surface body or a compound region body), `nm` namespaces the emitted
 * symbols (__<nm>_hit_*, <nm>_slider_<idx>_set_from, etc.). */
/* Damage band = union of the rects of the st index leaving the hover tint and of
 * the one gaining it (`new_st`). Emitted only for scrollable surfaces, where
 * render_<nm> honours w->dmg_y0/y1; anywhere else a hover change repaints whole.
 * Assumes __hw (this widget's hit-table slot) is already in scope. */
static void emit_hover_band(FILE *o, const char *nm, const char *new_st,
                            const char *ind) {
    fprintf(o, "%s{ int __b0 = 1 << 30, __b1 = 0;\n", ind);
    fprintf(o, "%s  int __ob = __%s_hover_w == w ? __%s_hover_st : -1;\n", ind, nm, nm);
    fprintf(o, "%s  if (__hw >= 0) for (int __k = 0; __k < __%s_hit_n[__hw]; __k++) {\n", ind, nm);
    fprintf(o, "%s      int __s = __%s_hit[__hw][__k].st_idx;\n", ind, nm);
    fprintf(o, "%s      if (__s != __ob && __s != (%s)) continue;\n", ind, new_st);
    fprintf(o, "%s      if (__%s_hit[__hw][__k].y < __b0) __b0 = __%s_hit[__hw][__k].y;\n", ind, nm, nm);
    fprintf(o, "%s      int __e = __%s_hit[__hw][__k].y + __%s_hit[__hw][__k].h;\n", ind, nm, nm);
    fprintf(o, "%s      if (__e > __b1) __b1 = __e;\n", ind);
    fprintf(o, "%s  }\n", ind);
    fprintf(o, "%s  if (__b1 > __b0) { w->dmg_y0 = __b0; w->dmg_y1 = __b1; } }\n", ind);
}

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
    /* Only surfaces that actually declare hover_bg pay for hover tracking —
     * otherwise motion over an ordinary bar must cost nothing (idle-zero). */
    int any_hover = 0, any_tip = 0;
    for (int it = 0; it < nitems; it++) {
        if (widget_prop(items[it].w, "hover_bg")) any_hover = 1;
        if (widget_prop(items[it].w, "tooltip"))  any_tip = 1;
    }

    /* Hit-test + hover state, no repaint: the wheel path needs to re-aim hover
     * *before* the scroll's single render, or the highlight visibly retargets
     * a frame late. Returns 1 when the hovered cell changed. */
    fprintf(o, "int %s_hover_set(Widget *w, int wx, int wy) {\n", nm);
    if (any_hover || any_tip) {
        fprintf(o, "    { int __hs = -1; const char *__ht = 0; int __hx = 0, __hcw = 0;\n");
        fprintf(o, "      int __hw = __%s_slot(w);\n", nm);
        fprintf(o, "      if (__hw >= 0) for (int i = 0; i < __%s_hit_n[__hw]; i++) {\n", nm);
        fprintf(o, "          if (wx < __%s_hit[__hw][i].x || wx >= __%s_hit[__hw][i].x + __%s_hit[__hw][i].w) continue;\n", nm, nm, nm);
        fprintf(o, "          if (wy < __%s_hit[__hw][i].y || wy >= __%s_hit[__hw][i].y + __%s_hit[__hw][i].h) continue;\n", nm, nm, nm);
        fprintf(o, "          __hs = __%s_hit[__hw][i].st_idx;\n", nm);
        fprintf(o, "          __ht = __%s_hit[__hw][i].tip;\n", nm);
        fprintf(o, "          __hx = __%s_hit[__hw][i].x;\n", nm);
        fprintf(o, "          __hcw = __%s_hit[__hw][i].w;\n", nm);
        fprintf(o, "          break;\n");
        fprintf(o, "      }\n");
        fprintf(o, "      (void)__ht; (void)__hx; (void)__hcw;\n");
        fprintf(o, "      if (__hs != __%s_hover_st || (__hs >= 0 && w != __%s_hover_w)) {\n", nm, nm);
        /* Only the row losing the tint and the row gaining it change pixels, so
         * hand the render a damage band spanning exactly those two rects. */
        if (ctx->scroll_rows && any_hover) emit_hover_band(o, nm, "__hs", "          ");
        fprintf(o, "          __%s_hover_st = __hs;\n", nm);
        fprintf(o, "          __%s_hover_w = __hs >= 0 ? w : 0;\n", nm);
        if (any_tip) {
            /* Anchor mirrors widget_note_click: cell rect + the bar's own top
             * margin, so a floating bar's tooltip clears it. */
            fprintf(o, "          if (__ht) { TipAnchor __ta = { w->output, __hx, __hcw, w->margin_top + w->h }; tooltip_arm(__ht, &__ta); }\n");
            fprintf(o, "          else tooltip_hide();\n");
        }
        fprintf(o, "          return 1;\n");
        fprintf(o, "      } }\n");
    } else {
        fputs("    (void)w; (void)wx; (void)wy;\n", o);
    }
    fputs("    return 0;\n}\n\n", o);

    fprintf(o, "void %s_on_motion(Widget *w, int wx, int wy) {\n", nm);
    /* Repaint (and re-arm the dwell) only when the hovered st index (or
     * widget) actually changes; doing it per motion event would be a frame
     * — and a timer restart — per pointer sample. */
    if (any_hover) fprintf(o, "    if (%s_hover_set(w, wx, wy)) render_%s(w);\n", nm, nm);
    else           fprintf(o, "    %s_hover_set(w, wx, wy);\n", nm);
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

    /* Leave: drop the hover tint. Without this the last hovered cell stays lit
     * after the pointer walks off the surface. */
    fprintf(o, "void %s_on_leave(Widget *w) {\n", nm);
    if (any_hover || any_tip) {
        fprintf(o, "    if (__%s_hover_w != w) return;\n", nm);
        if (ctx->scroll_rows && any_hover) {
            fprintf(o, "    int __hw = __%s_slot(w);\n", nm);
            emit_hover_band(o, nm, "-1", "    ");
        }
        fprintf(o, "    __%s_hover_st = -1; __%s_hover_w = 0;\n", nm, nm);
        if (any_tip)   fputs("    tooltip_hide();\n", o);
        if (any_hover) fprintf(o, "    render_%s(w);\n", nm);
    } else {
        fputs("    (void)w;\n", o);
    }
    fputs("}\n\n", o);

    fprintf(o, "void %s_on_click(Widget *w, int wx, int wy, int btn) {\n", nm);
    fputs("    (void)btn;\n", o);
    fprintf(o, "    int __wi = __%s_slot(w); if (__wi < 0) { (void)wx; (void)wy; return; }\n", nm);
    fprintf(o, "    for (int i = 0; i < __%s_hit_n[__wi]; i++) {\n", nm);
    fprintf(o, "        if (wx < __%s_hit[__wi][i].x || wx >= __%s_hit[__wi][i].x + __%s_hit[__wi][i].w) continue;\n", nm, nm, nm);
    /* Y matters as soon as items stack (vertical surface, wrapped rows): without
     * it every row of a column shares the X span and row 0 always wins. */
    fprintf(o, "        if (wy < __%s_hit[__wi][i].y || wy >= __%s_hit[__wi][i].y + __%s_hit[__wi][i].h) continue;\n", nm, nm, nm);
    fprintf(o, "        if (__%s_hit[__wi][i].slider_idx >= 0) continue;  /* sliders handle press, not click */\n", nm);
    fprintf(o, "        int kind = __%s_hit[__wi][i].kind; int arg = __%s_hit[__wi][i].arg;\n", nm, nm);
    /* Remember the clicked cell so a popup this handler opens (possibly via
     * exec → wispctl → ctl) can anchor under it. */
    fprintf(o, "        widget_note_click(w, __%s_hit[__wi][i].x, __%s_hit[__wi][i].w);\n", nm, nm);
    /* Find each unique (widget, is_for) and emit a branch. */
    int handler_idx = 0;
    for (int it = 0; it < nitems; it++) {
        Widget *wd = items[it].w;
        WBody *clk  = widget_onclick(wd);
        WBody *rclk = widget_handler(wd, WB_ONRCLICK);
        WBody *mclk = widget_handler(wd, WB_ONMCLICK);
        if (!clk && !rclk && !mclk) continue;
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
        /* Right button (BTN_RIGHT) splits off first; a widget with only
         * on_click must not fire it on a right-click. */
        if (rclk) {
            fputs("            if (btn == 0x111) {\n", o);
            if (rclk->click.param && rclk->click.plen)
                push_local(ctx, rclk->click.param, rclk->click.plen, LB_CLICK_PARAM, "((const char*)0)", NULL);
            emit_stmt(o, ctx, rclk->click.body, "                ", r);
            if (rclk->click.param && rclk->click.plen) pop_local(ctx);
            fputs("                return;\n            }\n", o);
        } else {
            fputs("            if (btn == 0x111) return;\n", o);
        }
        if (mclk) {
            fputs("            if (btn == 0x112) {\n", o);
            if (mclk->click.param && mclk->click.plen)
                push_local(ctx, mclk->click.param, mclk->click.plen, LB_CLICK_PARAM, "((const char*)0)", NULL);
            emit_stmt(o, ctx, mclk->click.body, "                ", r);
            if (mclk->click.param && mclk->click.plen) pop_local(ctx);
            fputs("                return;\n            }\n", o);
        } else {
            fputs("            if (btn == 0x112) return;\n", o);
        }
        if (clk) {
            if (clk->click.param && clk->click.plen) {
                push_local(ctx, clk->click.param, clk->click.plen, LB_CLICK_PARAM, "((const char*)0)", NULL);
                /* No real string param plumbing yet — keep as TODO. */
            }
            /* Lower stmt — exec/set/emit/block. */
            emit_stmt(o, ctx, clk->click.body, "            ", r);
            if (clk->click.param && clk->click.plen) pop_local(ctx);
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
        }
        if (items[it].is_for_cell || items[it].is_runtime_for_cell) pop_local(ctx);
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
    char sbuf[32]; const char *sb = sbuf;
    /* A for-block member is one cell drawn N times: loop the runtime count and
     * slide the st[] index, exactly like the top-level runtime-for draw does. */
    if (it->is_runtime_for_cell) {
        snprintf(sbuf, sizeof sbuf, "(%d + it)", it->st_base);
        fprintf(o, "        for (int it = 0; it < %s && it < %d; it++) {\n",
                it->runtime_for_count, it->runtime_for_cap);
    } else {
        snprintf(sbuf, sizeof sbuf, "%d", it->st_base);
    }
    int mr = eval_int(widget_prop(wd, "radius"), 0);
    int mbw = eval_int(widget_prop(wd, "border_width"), 1);
    int any_round = mr > 0;
    int clk = widget_clickable(wd);
    fprintf(o, "        if (st[%s].vis) {\n", sb);
    fprintf(o, "            int __ma = (st[%s].h>0?st[%s].h:st[%s].tw);\n", sb, sb, sb);
    /* Paint is gated (a partial frame may skip this group), but the hit rect
     * and cursor advance below are NOT: click regions must survive frames
     * that don't repaint the group, or the cell goes click-dead. */
    fprintf(o, "            if (__gdraw) {\n");
    fprintf(o, "            const char *txt = st[%s].txt; uint32_t cp = st[%s].cp; const uint32_t *pm = st[%s].pm; int pms = st[%s].pms;\n", sb, sb, sb, sb);
    fprintf(o, "            uint32_t fg = st[%s].fg, bg = st[%s].bg, bdr = st[%s].border; (void)bdr;\n", sb, sb, sb);
    fprintf(o, "            uint32_t ifg = st[%s].icon_fg; if (!(ifg & 0xff000000u)) ifg = fg;\n", sb);
    fprintf(o, "            if (st[%s].hover_bg & 0xff000000u && __%s_hover_st == (%s) && __%s_hover_w == w) bg = st[%s].hover_bg;\n",
            sb, nm, sb, nm, sb);
    fprintf(o, "            if (st[%s].press_bg & 0xff000000u && __%s_pressed_st == (%s) && __%s_pressed_w == w) bg = st[%s].press_bg;\n",
            sb, nm, sb, nm, sb);
    /* A member's declared height sizes its own bg/border; without one it fills
     * the group band. Otherwise a 20px icon cell got a full-band-tall pill. */
    fprintf(o, "            int __mh = st[%s].ch > 0 ? st[%s].ch : __gh, __my = __gy + (__gh - __mh)/2;\n", sb, sb);
    if (any_round)
        fprintf(o, "            if (bg  & 0xff000000u) fill_rect_rounded(sl->px,w->w,w->h, __gx,__my,__ma,__mh, %d,%d,%d,%d, bg);\n", mr, mr, mr, mr);
    else
        fprintf(o, "            if (bg  & 0xff000000u) fill_rect(sl->px,w->w,w->h, __gx,__my,__ma,__mh, bg);\n");
    /* Border is radius-independent — the SDF handles r=0 — so a member with
     * `border` and no `radius` paints instead of silently resolving a colour. */
    fprintf(o, "            if (bdr & 0xff000000u) fill_rect_rounded_border(sl->px,w->w,w->h, __gx,__my,__ma,__mh, %d,%d,%d,%d, %d,1,1,1,1,0, bdr);\n", mr, mr, mr, mr, mbw);
    int m_icb = eval_int(widget_prop(wd, "icon_box"), 0);
    int m_icg = eval_int(widget_prop(wd, "icon_gap"), 2);
    char m_icw[40];
    if (m_icb > 0) snprintf(m_icw, sizeof m_icw, "%d", m_icb);
    else snprintf(m_icw, sizeof m_icw, "cp_width(f, cp, pm, pms)");
    fprintf(o, "            int __ty = __gy + (__gh - f->line_h)/2;\n");
    fprintf(o, "            int __cw = 0; if (cp || pms) __cw += %s; if ((cp || pms) && txt && txt[0]) __cw += %d;\n", m_icw, m_icg);
    fprintf(o, "            if (txt) { const char *__p=txt; while(*__p&&*__p!='\\n')__p++; char __t2[256]; int __L=(int)(__p-txt); if(__L>255)__L=255; memcpy(__t2,txt,__L); __t2[__L]=0; __cw += text_width(f,__t2); }\n");
    fprintf(o, "            int __cx = __gx + (__ma - __cw)/2; if (__cx < __gx) __cx = __gx;\n");
    fprintf(o, "            if ((cp || pms) && (!txt || !txt[0])) draw_cp_centered(sl->px,w->w,w->h,__gx,__gy,__ma,__gh,f,cp,ifg,pm,pms);\n");
    fprintf(o, "            else { if (cp || pms) { int __icw = %s; draw_cp_centered(sl->px,w->w,w->h,__cx,__ty,__icw,f->line_h,f,cp,ifg,pm,pms); __cx += __icw; if (txt&&txt[0]) __cx += %d; } if (txt) draw_text(sl->px,w->w,w->h,__cx,__ty,f,txt,fg); }\n",
            m_icw, m_icg);
    fprintf(o, "            }\n");
    /* Same widened gate as the top-level path: kind -1 = hover-only rect. */
    int has_tip = widget_prop(wd, "tooltip") != NULL;
    if (clk || has_tip) {
        int kind = !clk ? -1 : it->is_runtime_for_cell ? 2 : it->is_for_cell ? 1 : 0;
        char arg[16];
        if (it->is_runtime_for_cell) snprintf(arg, sizeof arg, "it");
        else snprintf(arg, sizeof arg, "%d", it->is_for_cell ? it->cell_idx : it->handler_idx);
        fprintf(o, "            { int __i = __%s_nhit; if (__i < 64) { __%s_nhit++; __%s_hits_buf[__i].x=__gx; __%s_hits_buf[__i].y=__gy; __%s_hits_buf[__i].w=__ma; __%s_hits_buf[__i].h=__gh; __%s_hits_buf[__i].kind=%d; __%s_hits_buf[__i].arg=%s; __%s_hits_buf[__i].slider_idx=-1; __%s_hits_buf[__i].st_idx=%s; __%s_hits_buf[__i].tip=%s; } }\n",
                nm, nm, nm, nm, nm, nm, nm, kind, nm, arg, nm, nm, sb, nm, widget_tip_lit(wd));
    }
    fprintf(o, "            if (__ma) __gx += __ma + %d;\n", gap);
    fputs("        }\n", o);
    if (it->is_runtime_for_cell) fputs("        }\n", o);
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
        BarItem *it = &items[first + k];
        char sbuf[32]; const char *sb = sbuf;
        if (it->is_runtime_for_cell) {
            snprintf(sbuf, sizeof sbuf, "(%d + it)", it->st_base);
            fprintf(o, "        for (int it = 0; it < %s && it < %d; it++)\n",
                    it->runtime_for_count, it->runtime_for_cap);
        } else {
            snprintf(sbuf, sizeof sbuf, "%d", it->st_base);
        }
        /* zero-extent members (e.g. an empty menu.query cell) consume no gap */
        fprintf(o, "        if (st[%s].vis && (st[%s].h>0?st[%s].h:st[%s].tw) > 0) { __gw += (st[%s].h>0?st[%s].h:st[%s].tw); if (__gn) __gw += %d; __gn++; }\n",
                sb, sb, sb, sb, sb, sb, sb, gap);
    }
    /* Vertical: the band advances the stack by its own height and spans the
     * region's width; horizontal: it advances by the members' total width. */
    /* A horizontal group with nothing visible (an empty tray) collapses whole —
     * no pill, no pad. Vertical bands keep their row: a menu's prompt line must
     * hold its place even when the query is empty. */
    if (!vertical) fprintf(o, "        if (!__gn) __gw = 0;\n");
    fprintf(o, "        int __adv = %s, pos;\n", vertical ? (ch > 0 ? "0" : "f->line_h") : "__gw");
    if (vertical && ch > 0) fprintf(o, "        __adv = %d;\n", ch);
    fprintf(o, "        int __gpad = %s ? %d : 0;\n", vertical ? "1" : "__gn", pad);
    fprintf(o, "        switch (%d) {\n", (int)al);
    fprintf(o, "            case 1:  pos = end_pos - __adv; end_pos -= __adv + __gpad; break;\n");
    fprintf(o, "            default: pos = start_pos; start_pos += __adv + __gpad; break;\n");
    fprintf(o, "        }\n");
    /* A start-aligned band is one scrolled row too — see emit_item_draw. */
    if (ctx->scroll_rows && vertical && al != ALIGN_END)
        fprintf(o, "        __%s_rowrec(__wi, pos + w->scroll_off - __rowbase0, %d);\n",
                nm, items[first].st_base);
    if (vertical)
        fprintf(o, "        int __gy = pos, __gh = __adv, __bx = __reg_x, __bw = __reg_w; (void)__bw;\n");
    else if (ch > 0)
        fprintf(o, "        int __gy = __reg_y + (__reg_h - %d)/2, __gh = %d, __bx = pos, __bw = __gw; (void)__bw;\n", ch, ch);
    else
        fprintf(o, "        int __gy = __reg_y, __gh = __reg_h, __bx = pos, __bw = __gw; (void)__bw;\n");
    /* Partial repaint gate at group granularity: a group's pill spans several
     * cells (its corners are rounded at the ends), so it repaints as one unit —
     * clear its whole box back to the flat surface bg, then redraw pill + every
     * member. When no member's source ticked, only the PAINT is skipped
     * (__gdraw=0): layout and hit-rect registration still run, so later groups
     * keep their positions and skipped cells stay clickable. */
    if (ctx->partial_ok) {
        uint64_t gmask = 0;
        for (int k = 0; k < cnt; k++) gmask |= items[first + k].dep_mask;
        fprintf(o, "        int __gdraw = !__partial || (0x%016llxull & __wds);\n", (unsigned long long)gmask);
        fprintf(o, "        if (__gdraw && __partial) { fill_rect(sl->px,w->w,w->h, __bx,__gy,__bw,__gh, 0x%08xu);\n",
                ctx->surface_bg);
        fprintf(o, "            if (__bx < __dmg_x0) __dmg_x0 = __bx;\n");
        fprintf(o, "            if (__bx + __bw > __dmg_x1) __dmg_x1 = __bx + __bw; }\n");
    } else {
        fputs("        int __gdraw = 1; (void)__gdraw;\n", o);
    }
    const char *gg = vertical ? "if (__gdraw) " : "if (__gdraw && __gn) ";
    if (r > 0) {
        if (cbg  & 0xff000000u) fprintf(o, "        %sfill_rect_rounded(sl->px,w->w,w->h, __bx,__gy,__bw,__gh, %d,%d,%d,%d, 0x%08xu);\n", gg, r, r, r, r, cbg);
        if (cbor & 0xff000000u) fprintf(o, "        %sfill_rect_rounded_border(sl->px,w->w,w->h, __bx,__gy,__bw,__gh, %d,%d,%d,%d, %d,1,1,1,1,0, 0x%08xu);\n", gg, r, r, r, r, bw, cbor);
    } else {
        if (cbg & 0xff000000u) fprintf(o, "        %sfill_rect(sl->px,w->w,w->h, __bx,__gy,__bw,__gh, 0x%08xu);\n", gg, cbg);
    }
    fprintf(o, "        int __gx = __bx + %d; (void)__gw;\n", padx);
    for (int k = 0; k < cnt; k++)
        emit_group_member(o, &items[first + k], nm, gap);
    fputs("    }\n", o);
    return cnt;
}


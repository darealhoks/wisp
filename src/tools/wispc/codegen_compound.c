/* wispc codegen — compound (L-shaped) surfaces + the emit_surfaces driver. */
#include "codegen_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Does this surface declare anything to draw (as opposed to only props)? */
static int surface_has_body(Decl *d) {
    for (int i = 0; i < d->surface.n; i++)
        if (d->surface.items[i].kind != SB_PROP) return 1;
    return 0;
}
/* Compound emit — L-shaped (and friends) merged layer surfaces  */
/* ============================================================ */

/* LS_ANCHOR_* mirror the values in src/proto.h. wispc doesn't link against the
 * runtime headers so we redeclare the bitmasks here; sema.c uses the same
 * mapping in resolve_edge_ident. */
#define CG_ANCHOR_TOP    1
#define CG_ANCHOR_BOTTOM 2
#define CG_ANCHOR_LEFT   4
#define CG_ANCHOR_RIGHT  8

/* edge-as-mask is LS_ANCHOR_*; vertical iff items stack along Y, which is
 * true for left/right sidebars (edge LEFT|RIGHT). */
static int region_is_vertical_edge(int edge) {
    return (edge == CG_ANCHOR_LEFT || edge == CG_ANCHOR_RIGHT);
}

/* Subtract rect B from rect A; emit up to 4 axis-aligned sub-rects covering
 * A \ B. Used to paint compound regions without overlapping earlier ones (so
 * alpha-bg compounds don't double-blend along the shared corner). Returns the
 * number of sub-rects written; 1 with A itself if A and B don't overlap. */
/* Per-region rect inside the compound bbox, derived from edge + size. */
static void region_rect(Region *rg, int bbox_w, int bbox_h,
                        int *rx, int *ry, int *rw, int *rh) {
    int sz = rg->size;
    switch (rg->edge) {
    case CG_ANCHOR_TOP:    *rx = 0;            *ry = 0;            *rw = bbox_w; *rh = sz;     break;
    case CG_ANCHOR_BOTTOM: *rx = 0;            *ry = bbox_h - sz;  *rw = bbox_w; *rh = sz;     break;
    case CG_ANCHOR_LEFT:   *rx = 0;            *ry = 0;            *rw = sz;     *rh = bbox_h; break;
    case CG_ANCHOR_RIGHT:  *rx = bbox_w - sz;  *ry = 0;            *rw = sz;     *rh = bbox_h; break;
    default:               *rx = 0; *ry = 0; *rw = 0; *rh = 0; break;
    }
}

int emit_generated_compound(FILE *o, Decl *cmp, CGCtx *ctx, const char *nm) {
    int bbox_w   = eval_int  (surface_prop(cmp, "width"),   0);
    int bbox_h   = eval_int  (surface_prop(cmp, "height"),  0);
    int margin   = eval_int  (surface_prop(cmp, "margin"),  0);
    uint32_t bg  = eval_color_ctx(ctx, surface_prop(cmp, "bg"), 0xff000000u);
    int layer    = eval_layer(surface_prop(cmp, "layer")); if (layer < 0) layer = 2;
    int anchor   = eval_anchor(surface_prop(cmp, "anchor"));
    if (anchor < 0) anchor = CG_ANCHOR_TOP | CG_ANCHOR_LEFT;
    int radius   = eval_int  (surface_prop(cmp, "radius"),       0);
    int r_inner  = eval_int  (surface_prop(cmp, "radius_inner"), 0);
    int has_excl = surface_prop(cmp, "exclusive_zone") != NULL;
    int excl_def = 0;

    if (bbox_w <= 0 || bbox_h <= 0) {
        diag_error(cmp->loc, "compound '%s' needs width and height", nm);
        return 1;
    }

    /* Walk regions, collect items per-region, compute rects, pick default
     * exclusive_zone as largest region size on a primary anchor edge. */
    Region *regs[8]; int nregs = 0;
    for (int i = 0; i < cmp->surface.n && nregs < 8; i++) {
        if (cmp->surface.items[i].kind == SB_REGION)
            regs[nregs++] = cmp->surface.items[i].region;
    }
    int rrx[8], rry[8], rrw[8], rrh[8];
    for (int i = 0; i < nregs; i++) {
        region_rect(regs[i], bbox_w, bbox_h, &rrx[i], &rry[i], &rrw[i], &rrh[i]);
        if (regs[i]->size > excl_def) excl_def = regs[i]->size;
    }
    int excl_zone = has_excl ? eval_int(surface_prop(cmp, "exclusive_zone"), excl_def) : excl_def;

    /* Detect which edges this compound has (drives fillet placement and
     * cross-region margin adjustments). */
    int has_top = 0, has_bot = 0, has_left = 0, has_right = 0;
    int top_sz = 0, bot_sz = 0, left_sz = 0, right_sz = 0;
    for (int r = 0; r < nregs; r++) {
        switch (regs[r]->edge) {
        case CG_ANCHOR_TOP:    has_top = 1;   top_sz   = regs[r]->size; break;
        case CG_ANCHOR_BOTTOM: has_bot = 1;   bot_sz   = regs[r]->size; break;
        case CG_ANCHOR_LEFT:   has_left = 1;  left_sz  = regs[r]->size; break;
        case CG_ANCHOR_RIGHT:  has_right = 1; right_sz = regs[r]->size; break;
        }
    }
    (void)excl_zone; (void)anchor; (void)bbox_w; (void)bbox_h; (void)margin;
    (void)rrx; (void)rry; (void)rrw; (void)rrh; (void)radius;
    (void)top_sz; (void)bot_sz; (void)left_sz; (void)right_sz;
    (void)has_top; (void)has_bot;

    /* Combined widget array exposed to bar_render's fanout. Holds every
     * per-region widget across all regions. */
    fprintf(o, "Widget *__%s_widgets[32]; int __%s_nw;\n\n", nm, nm);
    reg_collect(nm);

    /* Per-region: items, hit table, render fn, click dispatchers, create_on.
     * Each region becomes an independent thin layer surface — much smaller
     * SHM pool than one full-screen compound buffer. */
    ctx->widget_var = "w";
    BarItem all_items[256]; int n_all = 0;

    for (int r = 0; r < nregs; r++) {
        int err = 0;
        int got = collect_bar_items(regs[r]->items, regs[r]->nitems,
                                    all_items + n_all, 256 - n_all, ctx, &err);
        if (err) return 1;
        /* Per-region st[] slots; assign handler_idx + slider_idx region-local. */
        int arr = 0;
        for (int i = 0; i < got; i++) {
            BarItem *it = &all_items[n_all + i];
            it->st_base = arr;
            arr += it->is_runtime_for_cell ? it->runtime_for_cap : 1;
        }
        if (arr == 0) arr = 1;
        int rn_sliders = 0;
        for (int i = 0; i < got; i++)
            if (widget_is_slider(all_items[n_all + i].w))
                all_items[n_all + i].slider_idx = rn_sliders++;
        assign_handler_idx(all_items + n_all, got);

        const char *regname = sname(regs[r]->name, regs[r]->nlen);
        char rnm[128]; snprintf(rnm, sizeof rnm, "%s_%s", nm, regname);
        int e = regs[r]->edge;
        int vertical = region_is_vertical_edge(e);

        /* Geometry: each region is its own thin strip. Vertical strips use
         * margins so they tuck between horizontal strips. Horizontal strips
         * (top/bottom) extend by r_inner into the interior so a quarter-disc
         * bulge can be filled in the empty notch, rounding the L's inner
         * corner smoothly. Only the horizontal strips own these bulges; the
         * vertical strips stay rectangular. */
        int ext_h = 0;
        if (r_inner > 0 && !vertical &&
            ((e == CG_ANCHOR_TOP    && (has_left || has_right)) ||
             (e == CG_ANCHOR_BOTTOM && (has_left || has_right))))
            ext_h = r_inner;

        int surf_w = 0, surf_h = 0, surf_anchor = 0, surf_ez = regs[r]->size;
        int mt = 0, mb = 0;          /* margins for vertical surfaces */
        int content_x = 0, content_y = 0, content_w = 0, content_h = 0;
        switch (e) {
        case CG_ANCHOR_TOP:
            surf_w = 0; surf_h = regs[r]->size + ext_h;
            surf_anchor = CG_ANCHOR_TOP|CG_ANCHOR_LEFT|CG_ANCHOR_RIGHT;
            content_x = 0; content_y = 0; content_w = -1; content_h = regs[r]->size;
            break;
        case CG_ANCHOR_BOTTOM:
            surf_w = 0; surf_h = regs[r]->size + ext_h;
            surf_anchor = CG_ANCHOR_BOTTOM|CG_ANCHOR_LEFT|CG_ANCHOR_RIGHT;
            /* Content sits in the lower half (extension above it). */
            content_x = 0; content_y = ext_h; content_w = -1; content_h = regs[r]->size;
            break;
        case CG_ANCHOR_LEFT:
            surf_w = regs[r]->size; surf_h = 0;
            surf_anchor = CG_ANCHOR_LEFT|CG_ANCHOR_TOP|CG_ANCHOR_BOTTOM;
            /* No margins: left/right overlap with top/bottom at the corners
             * so the corner area is always backed by a solid strip. */
            content_x = 0; content_y = 0; content_w = regs[r]->size; content_h = -1;
            break;
        case CG_ANCHOR_RIGHT:
            surf_w = regs[r]->size; surf_h = 0;
            surf_anchor = CG_ANCHOR_RIGHT|CG_ANCHOR_TOP|CG_ANCHOR_BOTTOM;
            content_x = 0; content_y = 0; content_w = regs[r]->size; content_h = -1;
            break;
        }

        /* Per-region slider thunks. */
        for (int i = 0; i < got; i++) {
            BarItem *it = &all_items[n_all + i];
            if (it->slider_idx < 0) continue;
            Widget *wd = it->w;
            Expr *ve = widget_prop(wd, "value");
            if (!ve || ve->kind != EX_IDENT) {
                diag_error(wd->loc, "codegen: slider widget needs `value = <mut>;`");
                ctx->failed = 1; return 1;
            }
            const char *mn = sname(ve->ident.s, ve->ident.n);
            int swvert = widget_is_vertical(wd);
            WBody *onch = widget_handler(wd, WB_ONCHANGE);
            fprintf(o, "static void %s_slider_%d_set_from(Widget *w, int rx, int ry, int rw, int rh, int wx, int wy) {\n",
                    rnm, it->slider_idx);
            fputs("    (void)w;\n", o);
            int ts_drag2 = eval_int(widget_prop(wd, "thumb_size"), 0);
            if (ts_drag2 < 0) ts_drag2 = 0;
            if (swvert)
                fprintf(o, "    double __v = (rh > %d) ? 1.0 - (double)(wy - (ry + %d)) / (double)(rh - %d) : 0.0;\n",
                        ts_drag2, ts_drag2/2, ts_drag2);
            else
                fprintf(o, "    double __v = (rw > %d) ? (double)(wx - (rx + %d)) / (double)(rw - %d) : 0.0;\n",
                        ts_drag2, ts_drag2/2, ts_drag2);
            fputs("    if (__v < 0) __v = 0;\n    if (__v > 1) __v = 1;\n", o);
            fputs("    (void)rx; (void)ry; (void)rw; (void)rh; (void)wx; (void)wy;\n", o);
            fprintf(o, "    mut_%s = __v;\n", mn);
            fprintf(o, "    dirty_%s = 1;\n", nm);
            if (onch) emit_stmt(o, ctx, onch->click.body, "    ", ctx->r);
            fputs("}\n", o);
        }

        /* Per-region hit table + dispatch state. */
        fprintf(o, "typedef struct { int x, y, w, h; int kind; int arg; int slider_idx; int st_idx; } %s_Hit;\n", rnm);
        fprintf(o, "static %s_Hit __%s_hits_buf[64];\n", rnm, rnm);
        fprintf(o, "static int __%s_nhit;\n", rnm);
        fprintf(o, "static int __%s_pressed_idx = -1;\n", rnm);
        fprintf(o, "static int __%s_pressed_slider = -1;\n", rnm);
        fprintf(o, "static int __%s_pressed_st = -1;\n", rnm);
        fprintf(o, "static Widget *__%s_pressed_w;\n", rnm);
        fprintf(o, "static Widget *__%s_widgets[4]; static int __%s_nw;\n", rnm, rnm);
        emit_hit_store(o, rnm, 4);
        fputs("\n", o);
        reg_collect(rnm);

        /* Step 6.1: transition slot storage for this region's items. */
        fputs("#ifdef WISP_HAS_ANIM\n", o);
        for (int i = 0; i < got; i++) {
            BarItem *it = &all_items[n_all + i];
            if (!item_has_any_transition(it->w)) continue;
            int slots = it->is_runtime_for_cell ? it->runtime_for_cap : 1;
            emit_item_slot_decls(o, it->w, rnm, n_all + i, slots, 4);
        }
        for (int i = 0; i < got; i++) {
            BarItem *it = &all_items[n_all + i];
            if (!widget_has_vis_anim(it->w)) continue;
            int slots = it->is_runtime_for_cell ? it->runtime_for_cap : 1;
            fprintf(o, "static VisSlot %s_vis%d[4][%d];\n", rnm, n_all + i, slots);
        }
        fputs("#endif\n", o);

        /* render_<rnm>(Widget *w): paint just this region. */
        fprintf(o, "void render_%s(Widget *w) {\n", rnm);
        fputs("    if (!w->configured || w->w <= 0 || w->h <= 0) return;\n", o);
        fprintf(o, "    int __wi = __%s_slot(w); if (__wi < 0) __wi = 0; (void)__wi;\n", rnm);
        fputs("    widget_ensure_pool(w, 1);\n", o);
        fputs("    BufSlot *sl = widget_free_slot(w);\n", o);
        fputs("    if (!sl) return;\n", o);
        fputs("    clear_buf(sl->px, w->w, w->h, 0);\n", o);
        fprintf(o, "    const Font *f = &font_%d;\n",
                eval_int(surface_prop(cmp, "font_size"), 14));
        fprintf(o, "    int __clip_top = %d; (void)__clip_top;\n",
                eval_int(surface_prop(cmp, "clip_top"), 0));
        fprintf(o, "    __%s_nhit = 0;\n", rnm);
        /* Resolve content width/height (-1 = full surface). */
        const char *cw_expr = (content_w < 0) ? "w->w" : NULL;
        const char *ch_expr = (content_h < 0) ? "w->h" : NULL;
        fprintf(o, "    int __cx = %d, __cy = %d;\n", content_x, content_y);
        if (cw_expr) fprintf(o, "    int __cw = %s;\n", cw_expr);
        else         fprintf(o, "    int __cw = %d;\n", content_w);
        if (ch_expr) fprintf(o, "    int __ch = %s;\n", ch_expr);
        else         fprintf(o, "    int __ch = %d;\n", content_h);
        fprintf(o, "    fill_rect(sl->px, w->w, w->h, __cx, __cy, __cw, __ch, 0x%08xu);\n", bg);

        /* Convex bulge into the empty notch on horizontal strips only.
         * Fills a quarter-disc of bg color outside the disc, creating a
         * smooth concave rounded corner where the L's inner edges meet.
         * Vertical strips stay rectangular (margins keep them clear of the
         * corner area). */
        if (r_inner > 0 && !vertical) {
            /* Shift the fillet outward by the perpendicular strip's size so
             * the rounded inner corner sits at the inside edge of that strip
             * (instead of in the empty notch). LEFT/RIGHT fill the corner
             * overlap with solid red, so only TOP/BOTTOM cut. */
            switch (e) {
            case CG_ANCHOR_TOP:
                if (has_left)
                    fprintf(o, "    fill_inner_fillet(sl->px, w->w, w->h, %d, %d, %d, 2, 0x%08xu);\n",
                            r_inner, top_sz + r_inner, r_inner, bg);
                if (has_right)
                    fprintf(o, "    fill_inner_fillet(sl->px, w->w, w->h, w->w - %d, %d, %d, 3, 0x%08xu);\n",
                            r_inner, top_sz + r_inner, r_inner, bg);
                break;
            case CG_ANCHOR_BOTTOM:
                if (has_left)
                    fprintf(o, "    fill_inner_fillet(sl->px, w->w, w->h, %d, %d, %d, 1, 0x%08xu);\n",
                            r_inner, ext_h - r_inner, r_inner, bg);
                if (has_right)
                    fprintf(o, "    fill_inner_fillet(sl->px, w->w, w->h, w->w - %d, %d, %d, 0, 0x%08xu);\n",
                            r_inner, ext_h - r_inner, r_inner, bg);
                break;
            }
        }

        /* Items: flex-pack within the content rect. */
        fputs("    int __reg_x = __cx, __reg_y = __cy;\n", o);
        fputs("    int __reg_w = __cw, __reg_h = __ch;\n", o);
        fputs("    (void)__reg_x; (void)__reg_y; (void)__reg_w; (void)__reg_h;\n", o);
        if (vertical) fputs("    int y = __reg_y; (void)y;\n", o);
        else          fputs("    int y = __reg_y + (__reg_h - f->line_h) / 2;\n", o);
        fprintf(o, "    struct { int tw, vis; uint32_t cp, fg, bg, border, press_bg; const uint32_t *pm; int pms; const char *txt; int pad, align; int h; int ch; int body_lines; } st[%d];\n", arr);
        fprintf(o, "    for (int __i = 0; __i < %d; __i++) { st[__i].vis = 0; st[__i].h = 0; st[__i].ch = 0; st[__i].body_lines = 1; st[__i].border = 0; st[__i].press_bg = 0; }\n", arr);
        fputs("    (void)st;\n", o);
        fputs("    int center_total = 0;\n", o);
        fprintf(o, "    int end_extent = %s;\n", vertical ? "__reg_h" : "__reg_w");
        for (int i = 0; i < got; i++) {
            emit_item_measure(o, &all_items[n_all + i], ctx, vertical, rnm, n_all + i);
            if (ctx->failed) return 1;
        }
        fprintf(o, "    int start_pos = %s;\n", vertical ? "__reg_y" : "__reg_x");
        fprintf(o, "    int end_pos = end_extent + %s;\n", vertical ? "__reg_y" : "__reg_x");
        fprintf(o, "    int center_pos = (end_extent - center_total) / 2 + %s;\n", vertical ? "__reg_y" : "__reg_x");
        fputs("    (void)start_pos; (void)end_pos; (void)center_pos;\n", o);
        for (int i = 0; i < got; i++)
            emit_item_draw(o, &all_items[n_all + i], ctx, vertical, rnm);

        /* Snapshot hits — into this widget's per-slot row. */
        emit_hit_snapshot(o, rnm);
        /* Re-assert input region each frame so it tracks the configured size. */
        fputs("    { Rect __ir[1] = { { __cx, __cy, __cw, __ch } };\n", o);
        fputs("      widget_set_input_region_multi(w, __ir, 1); }\n", o);
        fprintf(o, "    cutout_apply(\"%s\", w->output, sl->px, w->w, w->h);\n", rnm);
        fputs("    widget_attach(w, sl, 0);\n", o);
        fputs("}\n\n", o);

        /* Per-region click dispatchers. */
        emit_surface_click_dispatch(o, all_items + n_all, got, ctx, ctx->r, rnm);

        /* Per-region create_on. */
        fprintf(o, "static void %s_create_on(Output *o) {\n", rnm);
        fputs("    if (!o) return;\n", o);
        fprintf(o, "    if (__%s_nw >= 4) return;\n", rnm);
        fputs("    Widget *w = widget_alloc(W_BAR);\n", o);
        fprintf(o, "    if (!w) { msg(\"wisp: no widget slot for %s\"); return; }\n", rnm);
        fputs("    w->output = o;\n", o);
        fprintf(o, "    __%s_widgets[__%s_nw++] = w;\n", rnm, rnm);
        fprintf(o, "    if (__%s_nw < 32) __%s_widgets[__%s_nw++] = w;\n", nm, nm, nm);
        fprintf(o, "    widget_setup_surface(w, %d, \"wisp-%s\", o);\n", layer, rnm);
        fprintf(o, "    widget_set_size(w, %d, %d);\n", surf_w, surf_h);
        fprintf(o, "    widget_set_anchor(w, %d);\n", surf_anchor);
        fprintf(o, "    widget_set_margin(w, %d, 0, %d, 0);\n", mt, mb);
        fprintf(o, "    widget_set_exclusive_zone(w, %d);\n", surf_ez);
        fputs("    widget_set_kbd_interactive(w, 0);\n", o);
        /* Input region is set per-frame in render_<rnm> because we need
         * runtime w->w/h for stretched axes (unknown until configure). */
        fputs("    wl_req(w->surface, SURFACE_REQ_COMMIT, NULL, 0, -1);\n", o);
        fputs("}\n\n", o);

        n_all += got;
    }

    /* Compound-level dispatchers: route by widget identity to per-region fn. */
    fprintf(o, "void render_%s(Widget *w) {\n", nm);
    for (int r = 0; r < nregs; r++) {
        const char *regname = sname(regs[r]->name, regs[r]->nlen);
        fprintf(o, "    for (int i = 0; i < __%s_%s_nw; i++) if (__%s_%s_widgets[i] == w) { render_%s_%s(w); return; }\n",
                nm, regname, nm, regname, nm, regname);
    }
    fputs("    (void)w;\n}\n\n", o);

    const char *fns[4] = { "on_press", "on_release", "on_motion", "on_click" };
    const char *sigs[4] = {
        "Widget *w, int wx, int wy, int btn",
        "Widget *w, int wx, int wy, int btn",
        "Widget *w, int wx, int wy",
        "Widget *w, int wx, int wy, int btn",
    };
    const char *args[4] = { "w, wx, wy, btn", "w, wx, wy, btn", "w, wx, wy", "w, wx, wy, btn" };
    for (int k = 0; k < 4; k++) {
        fprintf(o, "void %s_%s(%s) {\n", nm, fns[k], sigs[k]);
        for (int r = 0; r < nregs; r++) {
            const char *regname = sname(regs[r]->name, regs[r]->nlen);
            fprintf(o, "    for (int i = 0; i < __%s_%s_nw; i++) if (__%s_%s_widgets[i] == w) { %s_%s_%s(%s); return; }\n",
                    nm, regname, nm, regname, nm, regname, fns[k], args[k]);
        }
        if (k == 2) fputs("    (void)w; (void)wx; (void)wy;\n", o);
        else        fputs("    (void)w; (void)wx; (void)wy; (void)btn;\n", o);
        fputs("}\n\n", o);
    }

    fprintf(o, "void %s_redraw(void) {\n", nm);
    fprintf(o, "    for (int i = 0; i < __%s_nw; i++) render_%s(__%s_widgets[i]);\n", nm, nm, nm);
    fputs("}\n\n", o);

    fprintf(o, "int %s_is_visible(void) { return 1; }\n", nm);
    fprintf(o, "void %s_apply_visibility(void) {\n", nm);
    fputs("    int want = 1;\n    int have = 0;\n", o);
    fprintf(o, "    have = __%s_nw > 0;\n", nm);
    fputs("    if (want && !have) {\n", o);
    fputs("        for (int i = 0; i < MAX_OUTPUTS; i++)\n", o);
    fprintf(o, "            if (outputs[i].active) %s_create_on(&outputs[i]);\n", nm);
    fputs("    }\n}\n\n", o);

    fprintf(o, "void %s_create_on(Output *o) {\n", nm);
    for (int r = 0; r < nregs; r++) {
        const char *regname = sname(regs[r]->name, regs[r]->nlen);
        fprintf(o, "    %s_%s_create_on(o);\n", nm, regname);
    }
    fputs("}\n\n", o);

    return 0;
}

int emit_surfaces(FILE *o, Unit *u, CGCtx *ctx) {
    reg_reset();
    fputs("/* Generated by wispc. Do not edit. */\n", o);
    fputs("#include \"wisp.h\"\n#include <stdio.h>\n#include <string.h>\n#include <stdlib.h>\n#include <unistd.h>\n#include <signal.h>\n\n", o);
    /* tags(labels=, pinned=) override the config.h defaults per-preset. */
    {
        const char *labels = NULL; size_t llen = 0;
        const char *pinned = NULL; size_t plen = 0;
        for (int i = 0; i < ctx->nsrc; i++) {
            if (ctx->srcs[i].drv->drv != DRV_TAGS) continue;
            labels = ctx->srcs[i].fmt;  llen = ctx->srcs[i].flen;
            pinned = ctx->srcs[i].arg2; plen = ctx->srcs[i].a2len;
        }
        if (labels) {
            static const char *const def[9] = { "1","2","3","4","5","6","7","8","9" };
            fputs("__attribute__((unused)) static const char *const TAG_LABEL[] = { ", o);
            const char *p = labels, *end = labels + llen;
            for (int t = 0; t < 9; t++) {
                while (p < end && *p == ' ') p++;
                const char *s = p;
                while (p < end && *p != ' ') p++;
                if (p > s) fprintf(o, "\"%.*s\", ", (int)(p - s), s);
                else       fprintf(o, "\"%s\", ", def[t]);
            }
            fputs("};\n", o);
        } else {
            fputs("__attribute__((unused)) static const char *const TAG_LABEL[] = TAG_LABELS;\n", o);
        }
        uint32_t pin = 0;
        for (size_t i = 0; pinned && i < plen; i++) {
            if (pinned[i] < '0' || pinned[i] > '9') continue;
            int v = 0;
            while (i < plen && pinned[i] >= '0' && pinned[i] <= '9') v = v * 10 + (pinned[i++] - '0');
            if (v >= 1 && v <= 32) pin |= 1u << (v - 1);
        }
        fprintf(o, "__attribute__((unused)) static const uint32_t TAG_PINNED = 0x%xu;\n", pin);
    }

    /* extern decls for source value buffers (clocks + exec + dbus) + status helpers. */
    for (int i = 0; i < ctx->nsrc; i++) {
        const char *snm = sname(ctx->srcs[i].decl->name, ctx->srcs[i].decl->nlen);
        if (ctx->srcs[i].drv->drv == DRV_CLOCK)
            fprintf(o, "extern char src_%s_value[];\n", snm);
        else if (ctx->srcs[i].drv->drv == DRV_EXEC) {
            /* Sized extern so `sizeof src_<n>_line` is usable inside on_click
             * bodies (set(<src>, ...) lowers to snprintf with sizeof). Must
             * match the definition in gen_sources.c (256). */
            fprintf(o, "extern char src_%s_line[256];\n", snm);
            fprintf(o, "void src_%s_refresh(void);\n", snm);
        }
        else if (ctx->srcs[i].drv->drv == DRV_DBUS) {
            fprintf(o, "extern char src_%s_value[];\n", snm);
            fprintf(o, "typedef struct { char summary[128]; char body[256]; char url[256]; uint8_t urgent; } src_%s_hist_t;\n", snm);
            fprintf(o, "extern src_%s_hist_t src_%s_hist[];\n", snm, snm);
            fprintf(o, "extern int src_%s_hist_n;\n", snm);
        }
    }
    int need_mem = 0, need_vpn = 0, need_wifi = 0;
    for (int i = 0; i < ctx->nsrc; i++) {
        const char *nm = ctx->srcs[i].drv->name;
        if (!strcmp(nm, "mem"))  need_mem = 1;
        if (!strcmp(nm, "vpn"))  need_vpn = 1;
        if (!strcmp(nm, "wifi")) need_wifi = 1;
    }
    if (need_mem)  fputs("int wispgen_mem_pct(void);\n", o);
    if (need_vpn)  fputs("const char *wispgen_vpn_state_s(void);\n", o);
    if (need_wifi) fputs("const char *wispgen_wifi_ssid(void);\n", o);

    /* extern decls for every mut (lives in gen_bindings.c). Reads in render
     * exprs / writes in set() need the symbol visible in this TU. */
    for (int i = 0; i < ctx->nkonst; i++) {
        Decl *k = ctx->konst[i];
        if (k->kind != D_MUT) continue;
        const char *mnm = sname(k->name, k->nlen);
        CE init = lower(ctx, k->konst.val);
        if (ctx->prelude) {
            fclose(ctx->prelude); ctx->prelude = NULL;
            free(ctx->prelude_buf); ctx->prelude_buf = NULL; ctx->prelude_sz = 0;
        }
        const char *cty = "int";
        switch (init.type) {
        case T_FLOAT: cty = "double"; break;
        case T_COLOR: cty = "uint32_t"; break;
        case T_STR:   cty = NULL; break;
        default:      cty = "int"; break;
        }
        if (cty) fprintf(o, "extern %s mut_%s;\n", cty, mnm);
        else     fprintf(o, "extern char mut_%s[128];\n", mnm);
    }

    /* Forward decls for every per-surface create_on (apply_visibility uses
     * them; declaration order isn't otherwise guaranteed) + dirty flag (set()
     * in click handlers writes to it). */
    for (int i = 0; i < ctx->r->nsurfaces; i++)
        fprintf(o, "extern int dirty_%s;\n", ctx->r->surface_names[i]);
    for (int i = 0; i < u->n; i++) {
        Decl *d = u->decls[i];
        if (d->kind != D_SURFACE && d->kind != D_COMPOUND) continue;
        if (d->kind == D_SURFACE && surface_prop(d, "spawned_by")) continue;
        /* menu.c owns a menu's lifecycle: no auto-create on output add. */
        if (d->is_menu) continue;
        fprintf(o, "void %.*s_create_on(Output *);\n", (int)d->nlen, d->name);
    }

    /* Helpers we use: cp_width, draw_cp, fill_rect, draw_text, text_width,
     * exec_cmd. Provide local statics that bar.c had; we lost those when we
     * replaced bar.c, so re-emit minimal ones. */
    fprintf(o, "\n#define MENU_ROWS_CAP %d\n", MENU_ROWS_CAP);
    /* An icon is a codepoint or a pixmap (a decoded app icon); pm wins. */
    fputs("\nstatic int cp_width(const Font *f, uint32_t cp, const uint32_t *pm, int pms) {\n"
          "    if (pms) return pms;\n"
          "    const Glyph *g = font_find(f, cp);\n"
          "    return g ? g->adv : f->px_size / 2;\n"
          "}\n", o);
    fputs("static int draw_cp(uint32_t *px, int sw, int sh, int x, int y,\n"
          "                   const Font *f, uint32_t cp, uint32_t fg,\n"
          "                   const uint32_t *pm, int pms) {\n"
          "    if (pms) { if (pm) blit_argb(px, sw, sh, x, y + (f->line_h - pms) / 2, pm, pms); return pms; }\n"
          "    const Glyph *g = font_find(f, cp); if (!g) return 0;\n"
          "    draw_glyph(px, sw, sh, x, y + f->baseline, f, g, fg);\n"
          "    return g->adv;\n"
          "}\n", o);
    /* fill_rect clipped to y >= min_y. Used for border draws when a surface
     * declares `clip_top`: borders above the threshold are hidden so e.g. a
     * HUD button can visually extend out of the bar without an outline showing
     * across the bar. Bg fills don't go through this — they intentionally
     * flow above the threshold. */
    fputs("__attribute__((unused))\n"
          "static void fill_rect_clipped(uint32_t *px, int sw, int sh,\n"
          "                              int x, int y, int w, int h, uint32_t c, int min_y) {\n"
          "    if (y < min_y) { int d = min_y - y; y += d; h -= d; }\n"
          "    if (w > 0 && h > 0) fill_rect(px, sw, sh, x, y, w, h, c);\n"
          "}\n", o);
    /* Vertical gradient bg: per-row lerp of ARGB from c_top (at y) to c_bot
     * (at y+h-1), clipped to y >= min_y. Drawn with fill_rect one row at a
     * time — cheap for a ~34px bar, and only used when `bg_bottom` is set. */
    fputs("__attribute__((unused))\n"
          "static void fill_rect_vgrad(uint32_t *px, int sw, int sh,\n"
          "                            int x, int y, int w, int h,\n"
          "                            uint32_t c_top, uint32_t c_bot, int min_y) {\n"
          "    if (w <= 0 || h <= 0) return;\n"
          "    int ta=(c_top>>24)&0xff, tr=(c_top>>16)&0xff, tg=(c_top>>8)&0xff, tb=c_top&0xff;\n"
          "    int ba=(c_bot>>24)&0xff, br=(c_bot>>16)&0xff, bg=(c_bot>>8)&0xff, bb=c_bot&0xff;\n"
          "    for (int j = 0; j < h; j++) {\n"
          "        int yy = y + j; if (yy < min_y) continue;\n"
          "        int num = (h > 1) ? j : 0, den = (h > 1) ? (h - 1) : 1;\n"
          "        uint32_t a = ta + (ba - ta) * num / den;\n"
          "        uint32_t r = tr + (br - tr) * num / den;\n"
          "        uint32_t g = tg + (bg - tg) * num / den;\n"
          "        uint32_t b = tb + (bb - tb) * num / den;\n"
          "        fill_rect(px, sw, sh, x, yy, w, 1, (a<<24)|(r<<16)|(g<<8)|b);\n"
          "    }\n"
          "}\n", o);
    /* Pixel-bbox centering for icon-only cells. Nerd Font advance widths are
     * monospace-cell-sized but the visible glyph often sits off-center inside
     * that cell. We center the actual rasterized bbox (g->w, g->h, g->bx, g->by)
     * inside the destination box, ignoring advance entirely. */
    fputs("__attribute__((unused))\n"
          "static void draw_cp_centered(uint32_t *px, int sw, int sh,\n"
          "                             int bx, int by, int bw, int bh,\n"
          "                             const Font *f, uint32_t cp, uint32_t fg,\n"
          "                             const uint32_t *pm, int pms) {\n"
          "    if (pms) { if (pm) blit_argb(px, sw, sh, bx + (bw - pms) / 2, by + (bh - pms) / 2, pm, pms); return; }\n"
          "    const Glyph *g = font_find(f, cp); if (!g) return;\n"
          "    int x = bx + (bw - g->w) / 2 - g->bx;\n"
          "    int baseline = by + (bh - g->h) / 2 + g->by;\n"
          "    draw_glyph(px, sw, sh, x, baseline, f, g, fg);\n"
          "}\n", o);
    fputs("__attribute__((unused)) static void exec_cmd(const char *cmd) {\n"
          "    if (!cmd || !*cmd) return;\n"
          "    pid_t p = fork(); if (p == 0) { setsid(); signal(SIGCHLD, SIG_DFL); execl(\"/bin/sh\", \"sh\", \"-c\", cmd, (char*)0); _exit(127); }\n"
          "}\n\n", o);

    /* First pass: emit each surface's render/click/create_on/redraw_all. */
    const char *surf_names[16]; int nsurf = 0;
    /* Menus join the render/input/redraw dispatchers but not the tags or
     * title fanouts — they have no bar state and no visibility lifecycle. */
    const char *menu_names[8]; int nmenu = 0;
    int has_tags_src = 0;
    for (int i = 0; i < ctx->nsrc; i++)
        if (ctx->srcs[i].drv->drv == DRV_TAGS) has_tags_src = 1;

    for (int i = 0; i < u->n; i++) {
        Decl *d = u->decls[i];
        if (d->kind != D_SURFACE && d->kind != D_COMPOUND) continue;
        /* Skip spawned_by templates (Session 5) — no auto-create lifecycle.
         * reveal_on_hover surfaces lower the same way as bars; the only
         * differences (W_HUD widget kind, hud_register, no exclusive zone)
         * are handled inside emit_generated_surface. */
        if (d->kind == D_SURFACE && surface_prop(d, "spawned_by")) {
            fprintf(o, "void %.*s_apply_visibility(void) {}\n", (int)d->nlen, d->name);
            /* `spawned_by = osd` lowers to render_<name>(): walks the slab
             * ring, sets up the per-slab $-bindings, runs the normal widget
             * measure/draw pipeline. osd_render() calls straight into it. */
            Expr *sb = surface_prop(d, "spawned_by");
            int is_osd = sb && sb->kind == EX_IDENT
                      && sb->ident.n == 3 && memcmp(sb->ident.s, "osd", 3) == 0;
            /* `spawned_by = osd_pill` is the same lowering with a one-slab,
             * fixed-height ring — the progress-only pill osd_post routes to. */
            int is_pill = sb && sb->kind == EX_IDENT
                       && sb->ident.n == 8 && memcmp(sb->ident.s, "osd_pill", 8) == 0;
            if (is_osd || is_pill) {
                char *nm = strndup0(d->name, d->nlen);
                int rc = emit_spawned_osd_skeleton(o, d, ctx, nm, is_pill);
                free(nm);
                if (rc) return 1;
            }
            /* `spawned_by = menu` is the default look every menu gets unless
             * its own decl carries a body. */
            int is_menu_tmpl = sb && sb->kind == EX_IDENT
                            && sb->ident.n == 4 && memcmp(sb->ident.s, "menu", 4) == 0;
            if (is_menu_tmpl && surface_has_body(d)
                && emit_menu_render(o, d, ctx, "menu_default")) return 1;
            continue;
        }
        if (d->is_menu) {
            /* No widget body: the `spawned_by = menu` template's renderer
             * (render_menu_default) covers it — nothing to emit here. */
            if (!surface_has_body(d)) continue;
            if (nmenu >= 8) { diag_error(d->loc, "codegen: too many menus"); return 1; }
            char *mn = malloc(d->nlen + 8);
            sprintf(mn, "menu_%.*s", (int)d->nlen, d->name);
            menu_names[nmenu++] = mn;
            if (emit_menu_render(o, d, ctx, mn)) return 1;
            continue;
        }
        char *nm_dup = strndup0(d->name, d->nlen);
        if (nsurf >= 16) { diag_error(d->loc, "codegen: too many surfaces"); free(nm_dup); return 1; }
        surf_names[nsurf++] = nm_dup;
        if (d->kind == D_COMPOUND) {
            if (emit_generated_compound(o, d, ctx, nm_dup)) return 1;
        } else {
            if (emit_generated_surface(o, d, ctx, nm_dup)) return 1;
        }
    }

    /* Dispatchers — these provide the symbols the hand-written runtime calls
     * (bar_render / bar_input_click / bar_redraw_all). They walk every
     * generated surface's owned-widget array and route to the matching
     * render_<n>/<n>_input_click. */
    const char *disp_names[24]; int ndisp = 0;
    for (int i = 0; i < nsurf; i++) disp_names[ndisp++] = surf_names[i];
    for (int i = 0; i < nmenu; i++) disp_names[ndisp++] = menu_names[i];
    fputs("void bar_render(Widget *w) {\n", o);
    for (int i = 0; i < ndisp; i++) {
        fprintf(o, "    for (int i = 0; i < __%s_nw; i++) if (__%s_widgets[i] == w) { render_%s(w); return; }\n",
                disp_names[i], disp_names[i], disp_names[i]);
    }
    fputs("    (void)w;\n}\n\n", o);

    fputs("void bar_input_click(Widget *w, int wx, int wy, int btn) {\n", o);
    for (int i = 0; i < ndisp; i++) {
        fprintf(o, "    for (int i = 0; i < __%s_nw; i++) if (__%s_widgets[i] == w) { %s_on_click(w, wx, wy, btn); return; }\n",
                disp_names[i], disp_names[i], disp_names[i]);
    }
    fputs("    (void)w; (void)wx; (void)wy; (void)btn;\n}\n\n", o);

    fputs("void bar_input_press(Widget *w, int wx, int wy, int btn) {\n", o);
    for (int i = 0; i < ndisp; i++) {
        fprintf(o, "    for (int i = 0; i < __%s_nw; i++) if (__%s_widgets[i] == w) { %s_on_press(w, wx, wy, btn); return; }\n",
                disp_names[i], disp_names[i], disp_names[i]);
    }
    fputs("    (void)w; (void)wx; (void)wy; (void)btn;\n}\n\n", o);

    fputs("void bar_input_release(Widget *w, int wx, int wy, int btn) {\n", o);
    for (int i = 0; i < ndisp; i++) {
        fprintf(o, "    for (int i = 0; i < __%s_nw; i++) if (__%s_widgets[i] == w) { %s_on_release(w, wx, wy, btn); return; }\n",
                disp_names[i], disp_names[i], disp_names[i]);
    }
    fputs("    (void)w; (void)wx; (void)wy; (void)btn;\n}\n\n", o);

    fputs("void bar_input_motion(Widget *w, int wx, int wy) {\n", o);
    for (int i = 0; i < ndisp; i++) {
        fprintf(o, "    for (int i = 0; i < __%s_nw; i++) if (__%s_widgets[i] == w) { %s_on_motion(w, wx, wy); return; }\n",
                disp_names[i], disp_names[i], disp_names[i]);
    }
    fputs("    (void)w; (void)wx; (void)wy;\n}\n\n", o);

    fputs("void bar_redraw_all(void) {\n", o);
    for (int i = 0; i < ndisp; i++)
        fprintf(o, "    %s_redraw();\n", disp_names[i]);
    fputs("}\n\n", o);

    /* tags setters: always emit so wl.c/ctl.c link cleanly under
     * WISP_HAS_BAR. When no tags source exists, the fanout still writes
     * the per-widget state but wispgen_tags_changed() is a no-op stub. */
    (void)has_tags_src;
    fputs("extern void wispgen_tags_changed(void);\n", o);
    fputs("static void __apply_tags(Widget *w, uint32_t m, uint32_t a, uint32_t u) {\n"
          "    w->s.bar.tag_mask = m; w->s.bar.active_mask = a; w->s.bar.urgent_mask = u; w->s.bar.have_tags = 1;\n"
          "}\n", o);
    fputs("void bar_set_tags(uint32_t m, uint32_t a, uint32_t u) {\n", o);
    for (int i = 0; i < nsurf; i++)
        fprintf(o, "    for (int i = 0; i < __%s_nw; i++) __apply_tags(__%s_widgets[i], m, a, u);\n",
                surf_names[i], surf_names[i]);
    fputs("    wispgen_tags_changed();\n}\n", o);
    fputs("void bar_set_tags_on(Output *o, uint32_t m, uint32_t a, uint32_t u) {\n"
          "    if (o && o->bar) __apply_tags(o->bar, m, a, u);\n"
          "    wispgen_tags_changed();\n}\n", o);
    fputs("void bar_set_title(const char *s) {\n", o);
    for (int i = 0; i < nsurf; i++)
        fprintf(o, "    for (int i = 0; i < __%s_nw; i++) snprintf(__%s_widgets[i]->s.bar.title, MAX_TEXT, \"%%s\", s ? s : \"\");\n",
                surf_names[i], surf_names[i]);
    fputs("    wispgen_tags_changed();\n}\n", o);
    fputs("void bar_set_title_on(Output *o, const char *s) {\n"
          "    if (o && o->bar) snprintf(o->bar->s.bar.title, MAX_TEXT, \"%s\", s ? s : \"\");\n"
          "    wispgen_tags_changed();\n}\n\n", o);

    emit_reg_destroyed(o);
    return 0;
}

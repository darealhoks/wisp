/* wispc codegen — render_<nm> emit for a declared surface. */
#include "codegen_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int emit_generated_surface(FILE *o, Decl *sur, CGCtx *ctx, const char *nm) {
    int height        = eval_int   (surface_prop(sur, "height"),         24);
    int width         = eval_int   (surface_prop(sur, "width"),          0);
    int margin        = eval_int   (surface_prop(sur, "margin"),         0);
    /* HUD surfaces with `reveal_on_hover = N` get an N-px trigger strip on the
     * slide edge (top for anchor=top, etc) that receives pointer-enter to drive
     * reveal. `reveal_gutter` is the UNPAINTED band on that same edge — bar/
     * desktop shows through it — and the DSL `height` is the content stacked
     * below it, so total surface size adds the gutter.
     *
     * The two are independent: the trigger is an input region, the gutter is
     * paint. They default equal (a flush bar wants a trigger the width of the
     * band it shows through — see dwlarp), but a preset whose bar FLOATS has no
     * bar to fill the gutter, so every gutter px is bare desktop above the body.
     * Such a preset sets `reveal_gutter = 0` to paint from the anchored edge
     * while keeping a fat, easy-to-hit trigger. */
    int reveal_g      = eval_int   (surface_prop(sur, "reveal_on_hover"), 0);
    int gut_g         = eval_int   (surface_prop(sur, "reveal_gutter"), reveal_g);
    /* Hoisted: the fillet emit (further down) needs these to outline its arcs
     * and carve the body side-border where a wedge attaches. eval_color_ctx so
     * a `const` resolves — see the border block below. */
    uint32_t sur_bord   = eval_color_ctx(ctx, surface_prop(sur, "border"), 0);
    int      sur_bord_w = eval_int(surface_prop(sur, "border_width"), 1);
    int      has_bord   = (sur_bord & 0xff000000u) && sur_bord_w > 0;
    /* Default exclusive_zone: only meaningful for an edge-anchored bar (height
     * with a width-stretching anchor); a floating panel should be 0. */
    int has_excl      = surface_prop(sur, "exclusive_zone") != NULL;
    int excl_zone     = eval_int   (surface_prop(sur, "exclusive_zone"), height);
    /* eval_color_ctx (not eval_color) so a `const NAME = #color` used as the
     * surface bg resolves instead of silently lowering to the default black. */
    uint32_t bg       = eval_color_ctx(ctx, surface_prop(sur, "bg"), 0xff000000);
    int layer         = eval_layer (surface_prop(sur, "layer"));
    if (layer < 0) layer = 2;
    int anchor        = eval_anchor(surface_prop(sur, "anchor"));
    if (anchor < 0) anchor = 1 | 4 | 8;
    int vertical      = surface_is_vertical(sur);
    if (!has_excl && width > 0) excl_zone = 0;
    (void)has_excl;

    /* Top-gutter clearance: any BOTTOM-anchored HUD (incl. bottom-corner
     * widgets) gets gutter on top of the buffer. Top-corner fillets (TL/TR)
     * extend UPWARD into the gutter (cid 2/3 wedges) — so the gutter must
     * fit max(reveal_g, ftl, ftr). Corner anchors also exclude `fillet_outer_*`
     * remap (no consistent "outer" for corners). */
    /* Codegen-time cid values per anchor (see runtime emission at the cid
     * table below for the actual wedge direction semantics). Pre-computed
     * here so pad-size calcs downstream can be cid-aware. */
    int cg_cid_tl, cg_cid_tr, cg_cid_br, cg_cid_bl;
    switch (anchor) {
    case 5:  cg_cid_tl=0; cg_cid_tr=1; cg_cid_br=2; cg_cid_bl=1; break;
    case 6:  cg_cid_tl=2; cg_cid_tr=1; cg_cid_br=2; cg_cid_bl=3; break;
    case 9:  cg_cid_tl=0; cg_cid_tr=1; cg_cid_br=0; cg_cid_bl=3; break;
    case 10: cg_cid_tl=3; cg_cid_tr=3; cg_cid_br=2; cg_cid_bl=3; break;
    case 2:  cg_cid_tl=3; cg_cid_tr=2; cg_cid_br=2; cg_cid_bl=3; break;
    default: cg_cid_tl=0; cg_cid_tr=1; cg_cid_br=2; cg_cid_bl=3; break;
    }
    int gutter_top = gut_g;
    if (anchor & 2) {
        Expr *p_tl = surface_prop(sur, "fillet_tl");
        if (!p_tl && anchor == 2) p_tl = surface_prop(sur, "fillet_outer_left");
        Expr *p_tr = surface_prop(sur, "fillet_tr");
        if (!p_tr && anchor == 2) p_tr = surface_prop(sur, "fillet_outer_right");
        int lo, hi;
        eval_int_range(p_tl, &lo, &hi, 0);
        int r_tl = lo > hi ? lo : hi;
        eval_int_range(p_tr, &lo, &hi, 0);
        int r_tr = lo > hi ? lo : hi;
        if (r_tl > gutter_top) gutter_top = r_tl;
        if (r_tr > gutter_top) gutter_top = r_tr;
    }
    /* Symmetric mirror of gutter_top: TOP-anchored corners whose BL/BR fillets
     * extend DOWN past body-bottom (cid USES_BOT) need extra buffer height
     * BELOW the body. Otherwise the wedge gets clipped at the buffer's bottom
     * edge in the fully-revealed state. */
    int gutter_bottom = 0;
    if (anchor & 1) {
        Expr *p_bl = surface_prop(sur, "fillet_bl");
        Expr *p_br = surface_prop(sur, "fillet_br");
        int lo, hi;
        eval_int_range(p_bl, &lo, &hi, 0);
        int r_bl = lo > hi ? lo : hi;
        eval_int_range(p_br, &lo, &hi, 0);
        int r_br = lo > hi ? lo : hi;
        if (USES_BOT(cg_cid_bl) && r_bl > gutter_bottom) gutter_bottom = r_bl;
        if (USES_BOT(cg_cid_br) && r_br > gutter_bottom) gutter_bottom = r_br;
    }

    /* Concave armpit corners (CONCAVE quarter-arc, fill_inner_fillet) — distinct
     * from the convex `fillet_*` arcs (fill_corner_fillet) that extend a HUD body
     * outward. Two roles:
     *   INNER (`armpit_inner`) — the two DESKTOP-FACING corners. Grows the buffer
     *     by the radius into the wallpaper and curls the body's underside down
     *     the screen-side edges (the bar "feet").
     *   OUTER (`armpit_outer`) — the two corners ON the anchored screen edge,
     *     rounded in place (no buffer growth). A thin click-through strip uses
     *     this to round the screen's own corners.
     * Per-anchor corner mapping (absolute `armpit_{bl,br,tl,tr}` override the
     * role that owns that corner):
     *   TOP:    inner = bl/br (extend DOWN),  outer = tl/tr
     *   BOTTOM: inner = tl/tr (extend UP),    outer = bl/br
     * `armpit_color` paints the arcs (default = surface bg, so the bar feet keep
     * matching the body without extra config). */
    int arm_in_bl = 0, arm_in_br = 0, arm_in_tl = 0, arm_in_tr = 0;
    int arm_out_tl = 0, arm_out_tr = 0, arm_out_bl = 0, arm_out_br = 0;
    int armpit = 0;  /* desktop-side buffer extension = max inner radius */
    uint32_t armpit_col = eval_color_ctx(ctx, surface_prop(sur, "armpit_color"), bg);
    if (reveal_g == 0 && !vertical && ((anchor & 1) || (anchor & 2))) {
        int ai = eval_int(surface_prop(sur, "armpit_inner"), 0);
        int ao = eval_int(surface_prop(sur, "armpit_outer"), 0);
        if (anchor & 1) {                 /* TOP */
            arm_in_bl  = eval_int(surface_prop(sur, "armpit_bl"), ai);
            arm_in_br  = eval_int(surface_prop(sur, "armpit_br"), ai);
            arm_out_tl = eval_int(surface_prop(sur, "armpit_tl"), ao);
            arm_out_tr = eval_int(surface_prop(sur, "armpit_tr"), ao);
        } else {                          /* BOTTOM */
            arm_in_tl  = eval_int(surface_prop(sur, "armpit_tl"), ai);
            arm_in_tr  = eval_int(surface_prop(sur, "armpit_tr"), ai);
            arm_out_bl = eval_int(surface_prop(sur, "armpit_bl"), ao);
            arm_out_br = eval_int(surface_prop(sur, "armpit_br"), ao);
        }
        armpit = arm_in_bl;
        if (arm_in_br > armpit) armpit = arm_in_br;
        if (arm_in_tl > armpit) armpit = arm_in_tl;
        if (arm_in_tr > armpit) armpit = arm_in_tr;
    }
    int armpit_any_outer = arm_out_tl | arm_out_tr | arm_out_bl | arm_out_br;

    /* Collect items (widgets + unrolled for-cells). */
    BarItem items[128]; int err = 0;
    int nitems = collect_bar_items(sur->surface.items, sur->surface.n,
                                   items, 128, ctx, &err);
    if (err) return 1;
    /* Assign st_base: static items occupy 1 slot, runtime-for items occupy cap. */
    int n_arr = 0;
    for (int i = 0; i < nitems; i++) {
        items[i].st_base = n_arr;
        n_arr += items[i].is_runtime_for_cell ? items[i].runtime_for_cap : 1;
    }
    if (n_arr == 0) n_arr = 1;  /* avoid zero-length array */

    /* Assign slider_idx to slider widgets so the drag dispatcher can route
     * to per-slider thunks <nm>_slider_<idx>_set_from. */
    int n_sliders = 0;
    for (int i = 0; i < nitems; i++)
        if (widget_is_slider(items[i].w)) items[i].slider_idx = n_sliders++;
    assign_handler_idx(items, nitems);
    if (n_sliders > 0) {
        /* Emit thunks before render_<nm> so they're in scope. */
        for (int i = 0; i < nitems; i++) {
            if (items[i].slider_idx < 0) continue;
            Widget *wd = items[i].w;
            Expr *ve = widget_prop(wd, "value");
            if (!ve || ve->kind != EX_IDENT) {
                diag_error(wd->loc, "codegen: slider widget needs `value = <mut>;`");
                ctx->failed = 1; return 1;
            }
            const char *mn = sname(ve->ident.s, ve->ident.n);
            int vertical = widget_is_vertical(wd);
            WBody *onch = widget_handler(wd, WB_ONCHANGE);
            fprintf(o, "static void %s_slider_%d_set_from(Widget *w, int rx, int ry, int rw, int rh, int wx, int wy) {\n",
                    nm, items[i].slider_idx);
            fputs("    (void)w;\n", o);
            /* Knob travel is inset by half the thumb on each end so the knob
             * stays fully inside the track. Map cursor onto the same inset
             * range so dragging to the physical track end gives value 0/1. */
            int ts_drag = eval_int(widget_prop(wd, "thumb_size"), 0);
            if (ts_drag < 0) ts_drag = 0;
            if (vertical)
                fprintf(o, "    double __v = (rh > %d) ? 1.0 - (double)(wy - (ry + %d)) / (double)(rh - %d) : 0.0;\n",
                        ts_drag, ts_drag/2, ts_drag);
            else
                fprintf(o, "    double __v = (rw > %d) ? (double)(wx - (rx + %d)) / (double)(rw - %d) : 0.0;\n",
                        ts_drag, ts_drag/2, ts_drag);
            fputs("    if (__v < 0) __v = 0;\n    if (__v > 1) __v = 1;\n", o);
            fputs("    (void)rx; (void)ry; (void)rw; (void)rh; (void)wx; (void)wy;\n", o);
            fprintf(o, "    mut_%s = __v;\n", mn);
            fprintf(o, "    dirty_%s = 1;\n", nm);
            if (onch) {
                emit_stmt(o, ctx, onch->click.body, "    ", ctx->r);
            }
            fputs("}\n", o);
        }
    }

    /* Per-surface hit + widget arrays (used by render and click dispatch).
     * slider_idx is -1 for ordinary click hits, >=0 to identify a slider
     * widget within the surface (drives the drag thunk). */
    fprintf(o, "typedef struct { int x, y, w, h; int kind; int arg; int slider_idx; int st_idx; } %s_Hit;\n", nm);
    fprintf(o, "static %s_Hit __%s_hits_buf[64];\n", nm, nm);
    fprintf(o, "static int __%s_nhit;\n", nm);
    fprintf(o, "static int __%s_pressed_idx = -1;\n", nm);
    fprintf(o, "static int __%s_pressed_slider = -1;\n", nm);
    /* press_bg: which st[] index is currently held by the pointer (-1 = none).
     * Draw pass swaps bg → press_bg when this matches the widget's st index,
     * gated on pressed_w so a press on one output doesn't tint another. */
    fprintf(o, "static int __%s_pressed_st = -1;\n", nm);
    fprintf(o, "static Widget *__%s_pressed_w;\n", nm);
    fprintf(o, "Widget *__%s_widgets[8]; int __%s_nw;\n", nm, nm);
    emit_hit_store(o, nm, 8);
    fputs("\n", o);
    reg_collect(nm);

    ctx->widget_var = "w";

    /* Step 6.1: per-(item,prop) transition slot storage. Sized 8 for runtime-for
     * cells (so each dbus-history ring slot tweens independently), 1 otherwise. */
    fputs("#ifdef WISP_HAS_ANIM\n", o);
    for (int i = 0; i < nitems; i++) {
        if (!item_has_any_transition(items[i].w)) continue;
        int slots = items[i].is_runtime_for_cell ? items[i].runtime_for_cap : 1;
        emit_item_slot_decls(o, items[i].w, nm, i, slots, 8);
    }
    /* Step 6.3: per-item VisSlot for enter/exit animations on `visible`. */
    for (int i = 0; i < nitems; i++) {
        if (!widget_has_vis_anim(items[i].w)) continue;
        int slots = items[i].is_runtime_for_cell ? items[i].runtime_for_cap : 1;
        fprintf(o, "static VisSlot %s_vis%d[8][%d];\n", nm, i, slots);
    }
    fputs("#endif\n", o);

    fprintf(o, "void render_%s(Widget *w) {\n", nm);
    fputs("    if (!w->configured || w->w <= 0 || w->h <= 0) return;\n", o);
    /* Per-instance tween state: the surface exists once per output. */
    fprintf(o, "    int __wi = __%s_slot(w); if (__wi < 0) __wi = 0; (void)__wi;\n", nm);
    /* Skip render for HUD-class surfaces parked at full slide-out: their body
     * isn't visible, and allocating a pool here just to attach a buffer that
     * never gets used (until reveal) is the main idle-RAM regression. The
     * trigger input region stays addressable without a buffer because it was
     * set on the surface at hud_register time. */
    if (eval_int(surface_prop(sur, "reveal_on_hover"), 0) > 0) {
        fputs("    if (w->kind == W_HUD && !w->s.hud.visible && !w->s.hud.animating) return;\n", o);
    }
    fputs("    widget_ensure_pool(w, 2);\n", o);
    fputs("    BufSlot *sl = widget_free_slot(w);\n", o);
    fputs("    if (!sl) return;\n", o);
    /* HUD widgets translate the render output by cur_off along the slide edge
     * to produce the slide-in animation. Non-HUD surfaces use (__ox,__oy)=(0,0). */
    fputs("    int __ox = 0, __oy = 0;\n", o);
    /* Corner anchors slide DIAGONALLY: cur_off ticks along slide_edge (vertical
     * for bottom/top corners), and the perpendicular axis offset is derived
     * proportionally so both axes stay in lockstep. The body holds its shape
     * and translates as a unit toward the screen corner during hide. */
    int __corner_anchor = (anchor == 5 || anchor == 6 || anchor == 9 || anchor == 10);
    if (__corner_anchor) {
        fprintf(o,
                "    if (w->kind == W_HUD) {\n"
                "        int __off = (int)w->s.hud.cur_off;\n"
                "        int __sx = (w->w > 0 && w->h > 0) ? __off * (int)w->w / (int)w->h : 0;\n"
                "        __oy = %s __off;\n"
                "        __ox = %s __sx;\n"
                "    }\n",
                (anchor & 2) ? " " : "-",
                (anchor & 8) ? " " : "-");
    } else {
        fputs("    if (w->kind == W_HUD) {\n"
              "        int __off = (int)w->s.hud.cur_off;\n"
              "        switch (w->s.hud.slide_edge) {\n"
              "        case LS_ANCHOR_TOP:    __oy = -__off; break;\n"
              "        case LS_ANCHOR_BOTTOM: __oy =  __off; break;\n"
              "        case LS_ANCHOR_LEFT:   __ox = -__off; break;\n"
              "        case LS_ANCHOR_RIGHT:  __ox =  __off; break;\n"
              "        }\n"
              "    }\n", o);
    }
    /* Content-region origin/size. For HUD surfaces with a `reveal_gutter`, the
     * top N (anchor=top) pixels are unpainted; bg, border, and widget layout all
     * happen inside the content region. A HUD with `reveal_gutter = 0` has no
     * band to skip and falls through to the flush case — the trigger region is
     * input-only and never displaces paint. */
    if (gut_g > 0 || gutter_top > 0 || gutter_bottom > 0 || armpit > 0) {
        fputs("    int __cox = __ox, __coy = __oy;\n", o);
        fputs("    int __cws = w->w,  __chs = w->h;\n", o);
        if (armpit > 0) {
            /* Non-HUD concave feet (mutually exclusive with the HUD gutter:
             * armpit is only computed when reveal_g == 0). The painted body
             * keeps height `__chs - armpit`; the extra strip on the desktop
             * side holds the feet, drawn below after the border. */
            if (anchor & 1) fprintf(o, "    __chs -= %d;\n", armpit);
            else            fprintf(o, "    __coy += %d; __chs -= %d;\n", armpit, armpit);
        } else {
            /* TOP/BOTTOM gutter sits at the buffer's top (`__coy += gutter_top`
             * shifts content down past it, regardless of slide direction). LEFT/RIGHT
             * panels keep bg flush to the anchored edge — the trigger input region
             * still lives there but is logical-only. `gutter_bottom` (TOP-anchored
             * corners with bottom-extending fillets) carves off buffer rows past
             * body-bottom so __chs ends up equal to the body's painted height. */
            fprintf(o,
                    "    switch (w->s.hud.slide_edge) {\n"
                    "    case LS_ANCHOR_TOP:    __coy += %d; __chs -= %d; break;\n"
                    "    case LS_ANCHOR_BOTTOM: __coy += %d; __chs -= %d; break;\n"
                    "    case LS_ANCHOR_LEFT:   __cws -= %d;               break;\n"
                    "    case LS_ANCHOR_RIGHT:  __cox += %d; __cws -= %d; break;\n"
                    "    default: break;\n"
                    "    }\n",
                    gutter_top, gutter_top + gutter_bottom,
                    gutter_top, gutter_top,
                    gut_g, gut_g, gut_g);
        }
    } else {
        fputs("    int __cox = __ox, __coy = __oy;\n", o);
        fputs("    int __cws = w->w,  __chs = w->h;\n", o);
    }
    /* Per-corner outward fillets. Absolute form: `fillet_tl/tr/bl/br = N;`.
     * Anchor-relative aliases (resolved at codegen time from the surface's
     * `anchor`) place the fillet at the corners ON the anchored edge — i.e.
     * where the body emerges from the screen — for "inner", and the opposite
     * edge for "outer". Same DSL spelling works for any anchor.
     *   anchor=TOP    inner_left → TL, inner_right → TR (top corners)
     *   anchor=BOTTOM inner_left → BL, inner_right → BR (bottom corners)
     *   anchor=LEFT   inner_top  → TL, inner_bottom → BL (left corners)
     *   anchor=RIGHT  inner_top  → TR, inner_bottom → BR (right corners)
     * Absolute names always win when both are present.
     *
     * Each fillet may be a single int (static radius) or `lo..hi` (animated:
     * radius tracks the body's emerged extent past the gutter along the slide
     * axis, capped at `hi`). Horizontal pads grow the surface and shift content
     * origin; vertical fillets share body rows and don't change size. The
     * buffer is sized using max(lo,hi) so the larger radius never overflows.
     *
     * `fillet_offset_y = -N;` lifts/lowers the fillet's junction line relative
     * to the body edge along the slide axis (negative = into the gutter). */
    Expr *_fp_tl = surface_prop(sur, "fillet_tl");
    Expr *_fp_tr = surface_prop(sur, "fillet_tr");
    Expr *_fp_bl = surface_prop(sur, "fillet_bl");
    Expr *_fp_br = surface_prop(sur, "fillet_br");
    /* For TOP/BOTTOM anchors the alias slots match absolute corner_id geometry
     * so we fold inner_left/right + outer_left/right into _fp_tl/tr/bl/br.
     * For LEFT/RIGHT anchors the inner/outer abstract slots need DIFFERENT
     * corner_id geometry than the corresponding absolute slot at the same
     * body corner — they're tracked separately and emitted with explicit
     * corner_ids further down. */
    Expr *_fp_lr_it = NULL, *_fp_lr_ib = NULL, *_fp_lr_ot = NULL, *_fp_lr_ob = NULL;
    int  _fp_lr_it_cid = 0, _fp_lr_ib_cid = 0, _fp_lr_ot_cid = 0, _fp_lr_ob_cid = 0;
    const char *_fp_lr_it_xa = "0", *_fp_lr_it_ya = "0", *_fp_lr_it_xa_anim = "0";
    const char *_fp_lr_ib_xa = "0", *_fp_lr_ib_ya = "0", *_fp_lr_ib_xa_anim = "0";
    const char *_fp_lr_ot_xa = "0", *_fp_lr_ot_ya = "0", *_fp_lr_ot_xa_anim = "0";
    const char *_fp_lr_ob_xa = "0", *_fp_lr_ob_ya = "0", *_fp_lr_ob_xa_anim = "0";
    {
        Expr *it  = surface_prop(sur, "fillet_inner_top");
        Expr *ib  = surface_prop(sur, "fillet_inner_bottom");
        Expr *ot  = surface_prop(sur, "fillet_outer_top");
        Expr *ob  = surface_prop(sur, "fillet_outer_bottom");
        Expr *il  = surface_prop(sur, "fillet_inner_left");
        Expr *ir  = surface_prop(sur, "fillet_inner_right");
        Expr *ol  = surface_prop(sur, "fillet_outer_left");
        Expr *orp = surface_prop(sur, "fillet_outer_right");
        switch (anchor) {
        case 1: /* TOP: anchored = top → inner = TL/TR, outer = BL/BR */
            if (!_fp_tl && il)  _fp_tl = il;
            if (!_fp_tr && ir)  _fp_tr = ir;
            if (!_fp_bl && ol)  _fp_bl = ol;
            if (!_fp_br && orp) _fp_br = orp;
            break;
        case 2: /* BOTTOM: anchored = bottom → inner = BL/BR, outer = TL/TR */
            if (!_fp_bl && il)  _fp_bl = il;
            if (!_fp_br && ir)  _fp_br = ir;
            if (!_fp_tl && ol)  _fp_tl = ol;
            if (!_fp_tr && orp) _fp_tr = orp;
            break;
        case 4: /* LEFT: anchored edge = left → body-TL/BL are anchored corners.
                 * Mirror of the TOP-anchored HUD pattern (tabs at TL/TR sticking
                 * UP into the bar). Here tabs stick UP and DOWN at body-TL/BL,
                 * sitting in the body's leftmost r columns, extending into the
                 * wallpaper above/below body — i.e. flush with the screen-left
                 * edge.
                 *   inner_top    → tab UP   from body-TL (corner_id=2)
                 *   inner_bottom → tab DOWN from body-BL (corner_id=1)
                 *   outer_top    → tab UP   from body-TR (corner_id=3)
                 *   outer_bottom → tab DOWN from body-BR (corner_id=0) */
            _fp_lr_it = it;  _fp_lr_it_cid = 2; _fp_lr_it_xa = "__fx_left";  _fp_lr_it_ya = "__fy_top"; _fp_lr_it_xa_anim = "__ax_left";
            _fp_lr_ib = ib;  _fp_lr_ib_cid = 1; _fp_lr_ib_xa = "__fx_left";  _fp_lr_ib_ya = "__fy_bot"; _fp_lr_ib_xa_anim = "__ax_left";
            _fp_lr_ot = ot;  _fp_lr_ot_cid = 3; _fp_lr_ot_xa = "__fx_right"; _fp_lr_ot_ya = "__fy_top"; _fp_lr_ot_xa_anim = "__fx_right";
            _fp_lr_ob = ob;  _fp_lr_ob_cid = 0; _fp_lr_ob_xa = "__fx_right"; _fp_lr_ob_ya = "__fy_bot"; _fp_lr_ob_xa_anim = "__fx_right";
            break;
        case 8: /* RIGHT: anchored edge = right → body-TR/BR are anchored corners.
                 * Tabs stick UP and DOWN at body-TR/BR, in the body's rightmost
                 * r columns, extending into the wallpaper above/below body —
                 * flush with the screen-right edge.
                 *   inner_top    → tab UP   from body-TR (corner_id=3)
                 *   inner_bottom → tab DOWN from body-BR (corner_id=0)
                 *   outer_top    → tab UP   from body-TL (corner_id=2)
                 *   outer_bottom → tab DOWN from body-BL (corner_id=1)
                 * Animated path pins the inner (anchored-edge) tabs' x to the
                 * screen-edge gutter (__ax_right) so the arc latches at the
                 * screen edge while body emerges past it — mirror of how the
                 * TOP-anchored HUD pins TL/TR's y to __ay_top. */
            _fp_lr_it = it;  _fp_lr_it_cid = 3; _fp_lr_it_xa = "__fx_right"; _fp_lr_it_ya = "__fy_top"; _fp_lr_it_xa_anim = "__ax_right";
            _fp_lr_ib = ib;  _fp_lr_ib_cid = 0; _fp_lr_ib_xa = "__fx_right"; _fp_lr_ib_ya = "__fy_bot"; _fp_lr_ib_xa_anim = "__ax_right";
            _fp_lr_ot = ot;  _fp_lr_ot_cid = 2; _fp_lr_ot_xa = "__fx_left";  _fp_lr_ot_ya = "__fy_top"; _fp_lr_ot_xa_anim = "__fx_left";
            _fp_lr_ob = ob;  _fp_lr_ob_cid = 1; _fp_lr_ob_xa = "__fx_left";  _fp_lr_ob_ya = "__fy_bot"; _fp_lr_ob_xa_anim = "__fx_left";
            break;
        default: break;
        }
    }
    int __lr_it_lo, __lr_it_hi; int __lr_it_anim = eval_int_range(_fp_lr_it, &__lr_it_lo, &__lr_it_hi, 0);
    int __lr_ib_lo, __lr_ib_hi; int __lr_ib_anim = eval_int_range(_fp_lr_ib, &__lr_ib_lo, &__lr_ib_hi, 0);
    int __lr_ot_lo, __lr_ot_hi; int __lr_ot_anim = eval_int_range(_fp_lr_ot, &__lr_ot_lo, &__lr_ot_hi, 0);
    int __lr_ob_lo, __lr_ob_hi; int __lr_ob_anim = eval_int_range(_fp_lr_ob, &__lr_ob_lo, &__lr_ob_hi, 0);
#define __LRMAX(a,b) ((a)>(b)?(a):(b))
    int __lr_it = __LRMAX(__lr_it_lo, __lr_it_hi);
    int __lr_ib = __LRMAX(__lr_ib_lo, __lr_ib_hi);
    int __lr_ot = __LRMAX(__lr_ot_lo, __lr_ot_hi);
    int __lr_ob = __LRMAX(__lr_ob_lo, __lr_ob_hi);
#undef __LRMAX
    int __sur_f_tl_lo, __sur_f_tl_hi; int __sur_f_tl_anim = eval_int_range(_fp_tl, &__sur_f_tl_lo, &__sur_f_tl_hi, 0);
    int __sur_f_tr_lo, __sur_f_tr_hi; int __sur_f_tr_anim = eval_int_range(_fp_tr, &__sur_f_tr_lo, &__sur_f_tr_hi, 0);
    int __sur_f_bl_lo, __sur_f_bl_hi; int __sur_f_bl_anim = eval_int_range(_fp_bl, &__sur_f_bl_lo, &__sur_f_bl_hi, 0);
    int __sur_f_br_lo, __sur_f_br_hi; int __sur_f_br_anim = eval_int_range(_fp_br, &__sur_f_br_lo, &__sur_f_br_hi, 0);
#define __FMAX(a,b) ((a)>(b)?(a):(b))
    int __sur_f_tl = __FMAX(__sur_f_tl_lo, __sur_f_tl_hi);
    int __sur_f_tr = __FMAX(__sur_f_tr_lo, __sur_f_tr_hi);
    int __sur_f_bl = __FMAX(__sur_f_bl_lo, __sur_f_bl_hi);
    int __sur_f_br = __FMAX(__sur_f_br_lo, __sur_f_br_hi);
    int __sur_f_oy = eval_int(surface_prop(sur, "fillet_offset_y"), 0);
    {
        /* LR wedges sit inside the body's x range (at the body's leftmost or
         * rightmost r columns) and extend only vertically (UP/DOWN into the
         * wallpaper above/below body). No horizontal padding required. */
        int lr_pad_l = 0, lr_pad_r = 0;
        /* cid-aware: only fillets whose wedge box actually extends into a
         * pad strip contribute to that strip's size. Corner-anchored armpits
         * may have x_range inside body (cid 0/3 on TR, 1/2 on TL etc.) and
         * therefore must NOT inflate left/right pad. */
        int tl_l = USES_LEFT(cg_cid_tl)  ? __sur_f_tl : 0;
        int bl_l = USES_LEFT(cg_cid_bl)  ? __sur_f_bl : 0;
        int tr_r = USES_RIGHT(cg_cid_tr) ? __sur_f_tr : 0;
        int br_r = USES_RIGHT(cg_cid_br) ? __sur_f_br : 0;
        int pad_l_abs = tl_l > bl_l ? tl_l : bl_l;
        int pad_r_abs = tr_r > br_r ? tr_r : br_r;
        int pad_l = pad_l_abs > lr_pad_l ? pad_l_abs : lr_pad_l;
        int pad_r = pad_r_abs > lr_pad_r ? pad_r_abs : lr_pad_r;
        int lr_pad_top = 0, lr_pad_bot = 0;
        if (anchor == 4 || anchor == 8) {
            int it = __lr_it > __lr_ot ? __lr_it : __lr_ot;
            int ib = __lr_ib > __lr_ob ? __lr_ib : __lr_ob;
            lr_pad_top = it; lr_pad_bot = ib;
        }
        if (pad_l > 0) fprintf(o, "    __cox += %d; __cws -= %d;\n", pad_l, pad_l);
        if (pad_r > 0) fprintf(o, "    __cws -= %d;\n", pad_r);
        if (lr_pad_top > 0) fprintf(o, "    __coy += %d; __chs -= %d;\n", lr_pad_top, lr_pad_top);
        if (lr_pad_bot > 0) fprintf(o, "    __chs -= %d;\n", lr_pad_bot);
    }
    /* Animated fillets on a HUD: radius tracks the body's emerged extent past
     * the gutter line along the slide axis, not a time-based lerp. At any
     * frame the fillet's straight edge lines up with the visible portion of
     * the body's side edge, so the disc never overhangs where the body
     * actually is. `hi` caps the maximum radius once fully emerged.
     *
     * Axis-aware: slide_edge picks w->h (TOP/BOTTOM) or w->w (LEFT/RIGHT) as
     * the extent dimension so the same codegen handles vertical and horizontal
     * slides. */
    {
        /* Pixel-tracking fillet radius (axis-aware HUD-classic):
         *   emerged = body_dim_along_slide_axis - cur_off
         *   r       = min(hi, max(0, emerged))
         *
         * `body_dim` is the body's extent along the slide axis (the DSL `height`
         * for TOP/BOTTOM, `width` for LEFT/RIGHT). `cur_off` is the slide offset
         * (0 = fully revealed, full_dim = fully hidden). emerged equals the
         * body's protrusion past its anchored edge in buffer coords — at any
         * frame, r is exactly how far the body has come out from behind the
         * screen-edge (or bar gutter for TOP). When the body is still fully
         * tucked behind the gutter/edge, emerged <= 0 and the fillet doesn't
         * paint; once the body crosses the line, r grows pixel-for-pixel with
         * the protrusion until it caps at `hi`. No time term — the fillet
         * latches to the body's actual visible edge, so widgets sliding from
         * any anchor (and at any easing/duration) get the same look.
         *
         * `__sur_body_visible` mirrors emerged but stays full at rest (no
         * animation, or non-HUD widgets). Body inner-corner radii clamp to it
         * so the rounded inner corner never carves into the visible body's
         * anchored-side edge when the body is narrower/shorter than 2r — that
         * carve is what makes the armpit fillet look detached during slide. */
        int __body_dim_axis = (anchor == 4 || anchor == 8) ? width : height;
        fprintf(o, "    int __sur_f_body_dim = %d; (void)__sur_f_body_dim;\n", __body_dim_axis ? __body_dim_axis : 1);
        fputs("    int __sur_f_emerged = 0; (void)__sur_f_emerged;\n", o);
        fputs("    int __sur_body_visible = __sur_f_body_dim; (void)__sur_body_visible;\n", o);
        fputs("    if (w->kind == W_HUD && w->s.hud.full_h > 0) {\n", o);
        fputs("        int __e = __sur_f_body_dim - (int)w->s.hud.cur_off;\n", o);
        fputs("        if (__e < 0) __e = 0;\n", o);
        fputs("        if (__e > __sur_f_body_dim) __e = __sur_f_body_dim;\n", o);
        fputs("        __sur_f_emerged = __e;\n", o);
        fputs("        __sur_body_visible = __e;\n", o);
        fputs("    }\n", o);
    }
    fputs("    (void)__cox; (void)__coy; (void)__cws; (void)__chs;\n", o);
    /* Region origin/extent for emit_item_draw cross-axis geometry. Surface
     * case: content area (HUD-gutter-stripped). Compound regions re-bind these
     * per-region inside scope blocks. */
    fputs("    int __reg_x = __cox, __reg_y = __coy;\n", o);
    fputs("    int __reg_w = __cws, __reg_h = __chs;\n", o);
    fputs("    (void)__reg_x; (void)__reg_y; (void)__reg_w; (void)__reg_h;\n", o);
    fputs("    clear_buf(sl->px, w->w, w->h, 0);\n", o);
    /* Optional `clip_top = N;` surface prop: hide any pixel of the surface
     * body (bg + surface border) above y=N. Widget bg/borders/text/icons are
     * NOT clipped — those flow freely so e.g. a HUD button can stick out of
     * the bar while the container body only shows below. 0 = no clipping. */
    fprintf(o, "    int __clip_top = %d; (void)__clip_top;\n",
            eval_int(surface_prop(sur, "clip_top"), 0));
    /* Per-surface rounded corners. Uniform `radius = N;` sets all four; the
     * per-corner overrides `radius_tl/tr/br/bl` win when present. We picked
     * the per-corner shorthand instead of an EX_TUPLE expression kind because
     * the expr grammar already accepts plain ints — zero parser surgery. */
    int __sur_r_tl = 0, __sur_r_tr = 0, __sur_r_br = 0, __sur_r_bl = 0;
    {
        int ru = eval_int(surface_prop(sur, "radius"), 0);
        /* Anchor-relative round-over aliases. `radius_inner = N` rounds both
         * desktop-facing corners; `radius_outer = N` rounds both anchored-edge
         * corners. Absolute `radius_tl/tr/bl/br` wins when both are present. */
        int r_inner = eval_int(surface_prop(sur, "radius_inner"), ru);
        int r_outer = eval_int(surface_prop(sur, "radius_outer"), ru);
        int r_tl_def = ru, r_tr_def = ru, r_bl_def = ru, r_br_def = ru;
        switch (anchor) {
        case 1: /* TOP    */ r_tl_def = r_outer; r_tr_def = r_outer; r_bl_def = r_inner; r_br_def = r_inner; break;
        case 2: /* BOTTOM */ r_bl_def = r_outer; r_br_def = r_outer; r_tl_def = r_inner; r_tr_def = r_inner; break;
        case 4: /* LEFT   */ r_tl_def = r_outer; r_bl_def = r_outer; r_tr_def = r_inner; r_br_def = r_inner; break;
        case 8: /* RIGHT  */ r_tr_def = r_outer; r_br_def = r_outer; r_tl_def = r_inner; r_bl_def = r_inner; break;
        default: break;
        }
        int r_tl = eval_int(surface_prop(sur, "radius_tl"), r_tl_def);
        int r_tr = eval_int(surface_prop(sur, "radius_tr"), r_tr_def);
        int r_br = eval_int(surface_prop(sur, "radius_br"), r_br_def);
        int r_bl = eval_int(surface_prop(sur, "radius_bl"), r_bl_def);
        __sur_r_tl = r_tl; __sur_r_tr = r_tr; __sur_r_br = r_br; __sur_r_bl = r_bl;
        if (r_tl | r_tr | r_br | r_bl) {
            /* clip_top is a RASTER clip, not geometry: shortening the rect here
             * would shrink the radius half-extent clamp and square the corners
             * as a sliding body crosses the clip line. Draw full, wipe after —
             * the body is the first paint after clear_buf, so the band is
             * otherwise empty. */
            fprintf(o, "    { int __by0 = __coy, __bh0 = __chs;\n");
            /* Inner-side radii clamp to __sur_body_visible so the rounded inner
             * corner never carves the visible body's anchored-side edge during
             * slide. Only needed when armpit fillets exist — the clamp keeps the
             * fillet attached to the body edge. Without fillets it's actively
             * wrong: it squares off the fully-visible inner corners as the body
             * retracts (radius shrinks with emerged extent). */
            fprintf(o, "      int __rtl=%d, __rtr=%d, __rbr=%d, __rbl=%d;\n", r_tl, r_tr, r_br, r_bl);
            int cg_has_fillet = __sur_f_tl | __sur_f_tr | __sur_f_bl | __sur_f_br |
                                __lr_it | __lr_ib | __lr_ot | __lr_ob;
            switch (cg_has_fillet ? anchor : 0) {
            case 1: /* TOP    — inner = bl, br */
                fputs("      if (__rbl > __sur_body_visible) __rbl = __sur_body_visible;\n"
                      "      if (__rbr > __sur_body_visible) __rbr = __sur_body_visible;\n", o);
                break;
            case 2: /* BOTTOM — inner = tl, tr */
                fputs("      if (__rtl > __sur_body_visible) __rtl = __sur_body_visible;\n"
                      "      if (__rtr > __sur_body_visible) __rtr = __sur_body_visible;\n", o);
                break;
            case 4: /* LEFT   — inner = tr, br */
                fputs("      if (__rtr > __sur_body_visible) __rtr = __sur_body_visible;\n"
                      "      if (__rbr > __sur_body_visible) __rbr = __sur_body_visible;\n", o);
                break;
            case 8: /* RIGHT  — inner = tl, bl */
                fputs("      if (__rtl > __sur_body_visible) __rtl = __sur_body_visible;\n"
                      "      if (__rbl > __sur_body_visible) __rbl = __sur_body_visible;\n", o);
                break;
            default: break;
            }
            fprintf(o, "      if (__bh0 > 0) fill_rect_rounded(sl->px, w->w, w->h, __cox, __by0, __cws, __bh0, __rtl, __rtr, __rbr, __rbl, 0x%08xu);\n", bg);
            fprintf(o, "      if (__clip_top > 0) { int __ct = __clip_top > (int)w->h ? (int)w->h : __clip_top; memset(sl->px, 0, (size_t)w->w * (size_t)__ct * 4); } }\n");
        } else {
            Expr *bg_bot_e = surface_prop(sur, "bg_bottom");
            if (bg_bot_e) {
                uint32_t bg_bot = eval_color_ctx(ctx, bg_bot_e, bg);
                fprintf(o, "    fill_rect_vgrad(sl->px, w->w, w->h, __cox, __coy, __cws, __chs, 0x%08xu, 0x%08xu, __clip_top);\n", bg, bg_bot);
            } else {
                fprintf(o, "    fill_rect_clipped(sl->px, w->w, w->h, __cox, __coy, __cws, __chs, 0x%08xu, __clip_top);\n", bg);
            }
        }
    }
    /* Surface border. `border = #color;` enables all 4 sides; `border_top = 0;`
     * (or _bottom/_left/_right) suppresses individual sides. `border_width = N;`
     * controls thickness (default 1). Sides default to ON when `border` is set.
     * When any radius is set, the border is drawn as an antialiased annular
     * outline that follows the rounded outer shape. */
    {
        /* eval_color_ctx (not eval_color): a `const NAME = #color` used as the
         * surface border must resolve, else it lowers to 0 and the alpha test
         * below silently drops the border entirely. Same trap as `bg` above. */
        uint32_t sb = sur_bord;
        if (sb & 0xff000000u) {
            int sbw = sur_bord_w;
            int sbt = eval_int(surface_prop(sur, "border_top"),    1);
            int sbb = eval_int(surface_prop(sur, "border_bottom"), 1);
            int sbl = eval_int(surface_prop(sur, "border_left"),   1);
            int sbr = eval_int(surface_prop(sur, "border_right"),  1);
            int rounded = __sur_r_tl | __sur_r_tr | __sur_r_br | __sur_r_bl;
            if (rounded) {
                fprintf(o, "    fill_rect_rounded_border(sl->px, w->w, w->h, __cox, __coy, __cws, __chs, %d, %d, %d, %d, %d, %d, %d, %d, %d, __clip_top, 0x%08xu);\n",
                        __sur_r_tl, __sur_r_tr, __sur_r_br, __sur_r_bl,
                        sbw, sbt, sbr, sbb, sbl, sb);
            } else {
                /* Side-border carve-out: when a corner has a fillet, the
                 * adjacent side border starts past the fillet's end so the
                 * straight border doesn't poke up alongside the arc. */
                int top_l = __sur_f_tl, top_r = __sur_f_tr;
                int bot_l = __sur_f_bl, bot_r = __sur_f_br;
                if (sbt) fprintf(o, "    fill_rect_clipped(sl->px, w->w, w->h, __cox + %d, __coy, __cws - %d, %d, 0x%08xu, __clip_top);\n",
                                 top_l, top_l + top_r, sbw, sb);
                if (sbb) fprintf(o, "    fill_rect_clipped(sl->px, w->w, w->h, __cox + %d, __coy + __chs - %d, __cws - %d, %d, 0x%08xu, __clip_top);\n",
                                 bot_l, sbw, bot_l + bot_r, sbw, sb);
                if (sbl) fprintf(o, "    fill_rect_clipped(sl->px, w->w, w->h, __cox, __coy + %d, %d, __chs - %d, 0x%08xu, __clip_top);\n",
                                 top_l, sbw, top_l + bot_l, sb);
                if (sbr) fprintf(o, "    fill_rect_clipped(sl->px, w->w, w->h, __cox + __cws - %d, __coy + %d, %d, __chs - %d, 0x%08xu, __clip_top);\n",
                                 sbw, top_r, sbw, top_r + bot_r, sb);
            }
        }
    }
    /* Per-corner fillet draws — bg-only quarter-arc extensions of the body
     * into each requested corner's armpit. Drawn after the border so they
     * overwrite any residual edge pixels at the carve-out boundary.
     *
     * Animated path radius = min(hi, __sur_f_emerged) so the arc tracks the
     * body's emerged extent. Anchor on the SLIDE AXIS is pinned to the screen
     * position of the bar/body junction *only* for corners that lie on the
     * anchored edge (HUD/bar-overlap pattern: arc sits at a fixed gutter line
     * while the body slides past). Corners on the inner edge use a body-
     * relative anchor — the wedge moves with the body, growing into wallpaper
     * as the panel emerges. Cross-axis anchor is always body-relative. */
    if (__sur_f_tl_lo | __sur_f_tl_hi | __sur_f_tr_lo | __sur_f_tr_hi |
        __sur_f_bl_lo | __sur_f_bl_hi | __sur_f_br_lo | __sur_f_br_hi |
        __lr_it | __lr_ib | __lr_ot | __lr_ob) {
        fprintf(o,
            "    int __fy_top   = __coy + %d;\n"
            "    int __fy_bot   = __coy + __chs - %d;\n"
            "    int __fx_left  = __cox;\n"
            "    int __fx_right = __cox + __cws;\n",
            __sur_f_oy, __sur_f_oy);
        /* Anchored-axis pin for animated fillets: the body's RESTING edge
         * position on the anchored axis, in buffer coords. TOP/BOTTOM put the
         * gutter on the ANCHORED side (HUD-classic, gutter overlaps bar) so
         * body's rest top = reveal_g, body's rest bottom = h - reveal_g.
         * LEFT/RIGHT put the gutter on the DESKTOP side (no bar-equivalent on
         * vertical screen edges; gutter sticks into desktop) so body's rest
         * left = 0 and body's rest right = w. The fillet hinge clings to this
         * line — the body's screen-edge attachment — while the body slides
         * past behind it. */
        /* Body's RESTING top/bottom in buffer coords. With the gutter-on-desktop
         * shift (`__coy += reveal_g`) applied uniformly for TOP and BOTTOM
         * slides, the body always rests at buffer y in [reveal_g, h]. The pin
         * coords are therefore symmetric and fire for either vertical slide
         * direction — mirrors how __ax_left/__ax_right work for LEFT/RIGHT. */
        /* Pin per-edge based on whether that edge is anchored to the screen.
         * Anchored edges keep their fillet pinned at the screen-edge line
         * (so the wedge stays flush with the edge while body slides past).
         * Non-anchored edges' fillets follow the body — important for corner
         * anchors where the body slides diagonally and the non-anchored edges
         * carry their fillets along. */
        fprintf(o,
            "    int __ay_top   = (%s) ? (%d)             : __fy_top;\n"
            "    int __ay_bot   = (%s) ? ((int)w->h - %d) : __fy_bot;\n"
            "    int __ax_left  = (%s) ? (%d)             : __fx_left;\n"
            "    int __ax_right = (%s) ? ((int)w->w - %d) : __fx_right;\n",
            (anchor & 1) ? "w->kind == W_HUD" : "0", gutter_top + __sur_f_oy,
            (anchor & 2) ? "w->kind == W_HUD" : "0", __sur_f_oy,
            (anchor & 4) ? "w->kind == W_HUD" : "0", __sur_f_oy,
            (anchor & 8) ? "w->kind == W_HUD" : "0", __sur_f_oy);
        fputs("    (void)__ay_top; (void)__ay_bot; (void)__ax_left; (void)__ax_right;\n", o);
        fputs("    (void)__fy_top; (void)__fy_bot; (void)__fx_left; (void)__fx_right;\n", o);
    }
    /* Proportional armpit growth: r = hi * emerged / body_dim (clamped 0..hi).
     * With this, r reaches `hi` only at full reveal (cur_off=0) and grows
     * continuously through the whole post-emergence slide, rather than capping
     * once emerged exceeds hi (which under ease-out compresses the visible
     * growth into the first ~12% of real time). Matches the user-described
     * HUD feel: "arc angle small at start, gets bigger as it progresses". */
    /* Outline a fillet wedge, given its radius expression R (a runtime var when
     * animated, a literal otherwise). The wedge extends horizontally off
     * x_corner, so it abuts a VERTICAL body side — left for cid 0/3, right for
     * 1/2 — over the wedge's span along Y (downward from y_corner for cid 0/1,
     * upward for 2/3). Two steps, and the order matters: repaint that span of
     * the straight side border with bg (the wedge continues the body there, so
     * a border line across the junction reads as a seam), then lay the arc band
     * down as the real outline. The arc meets the body side tangentially, so it
     * joins the surviving straight border cleanly at the wedge's far end. */
#define __FILLET_BORDER(IND, X, Y, R, CORNER) do {                                   \
        if (has_bord) {                                                              \
            int __cid = (CORNER);                                                    \
            char __cvx[64], __cvy[96];                                               \
            if (__cid == 0 || __cid == 3) snprintf(__cvx, sizeof __cvx, "__cox");    \
            else snprintf(__cvx, sizeof __cvx, "__cox + __cws - %d", sur_bord_w);    \
            if (__cid == 0 || __cid == 1) snprintf(__cvy, sizeof __cvy, "%s", (Y));  \
            else snprintf(__cvy, sizeof __cvy, "%s - (%s)", (Y), (R));               \
            fprintf(o, "%sfill_rect(sl->px, w->w, w->h, %s, %s, %d, %s, 0x%08xu);\n",\
                    (IND), __cvx, __cvy, sur_bord_w, (R), bg);                       \
            fprintf(o, "%sfill_corner_fillet_border(sl->px, w->w, w->h, %s, %s, %s, %d, %d, 0x%08xu);\n", \
                    (IND), (X), (Y), (R), __cid, sur_bord_w, sur_bord);              \
        }                                                                            \
    } while (0)
#define __EMIT_FILLET(LO, HI, ANIM, XSTATIC, YSTATIC, XANIM, YANIM, CORNER) do {     \
        if ((LO) > 0 || (HI) > 0) {                                                  \
            if ((ANIM)) {                                                            \
                fprintf(o, "    { int __fr = (__sur_f_emerged <= 0) ? 0 : (%d) * __sur_f_emerged / __sur_f_body_dim;\n", (HI)); \
                fprintf(o, "      if (__fr > (%d)) __fr = (%d);\n", (HI), (HI)); \
                fprintf(o, "      if (__fr > 0) { fill_corner_fillet(sl->px, w->w, w->h, %s, %s, __fr, %d, 0x%08xu);\n", XANIM, YANIM, CORNER, bg); \
                __FILLET_BORDER("        ", XANIM, YANIM, "__fr", CORNER);           \
                fprintf(o, "      } }\n");                                           \
            } else {                                                                 \
                char __rlit[16]; snprintf(__rlit, sizeof __rlit, "%d", (HI));        \
                fprintf(o, "    fill_corner_fillet(sl->px, w->w, w->h, %s, %s, %d, %d, 0x%08xu);\n", XSTATIC, YSTATIC, (HI), CORNER, bg); \
                __FILLET_BORDER("    ", XSTATIC, YSTATIC, __rlit, CORNER);           \
            }                                                                        \
        }                                                                            \
    } while (0)
    /* Wedge direction (corner_id) per anchor: armpit wedge orientation
     * follows which body edges are anchored vs facing wallpaper. Pre-computed
     * at codegen time (see cg_cid_* above). */
    __EMIT_FILLET(__sur_f_tl_lo, __sur_f_tl_hi, __sur_f_tl_anim, "__fx_left",  "__fy_top", "__ax_left",  "__ay_top",  cg_cid_tl);
    __EMIT_FILLET(__sur_f_tr_lo, __sur_f_tr_hi, __sur_f_tr_anim, "__fx_right", "__fy_top", "__ax_right", "__ay_top",  cg_cid_tr);
    __EMIT_FILLET(__sur_f_br_lo, __sur_f_br_hi, __sur_f_br_anim, "__fx_right", "__fy_bot", "__ax_right", "__ay_bot",  cg_cid_br);
    __EMIT_FILLET(__sur_f_bl_lo, __sur_f_bl_hi, __sur_f_bl_anim, "__fx_left",  "__fy_bot", "__ax_left",  "__ay_bot",  cg_cid_bl);
    __EMIT_FILLET(__lr_it_lo, __lr_it_hi, __lr_it_anim, _fp_lr_it_xa, _fp_lr_it_ya, _fp_lr_it_xa_anim, _fp_lr_it_ya, _fp_lr_it_cid);
    __EMIT_FILLET(__lr_ib_lo, __lr_ib_hi, __lr_ib_anim, _fp_lr_ib_xa, _fp_lr_ib_ya, _fp_lr_ib_xa_anim, _fp_lr_ib_ya, _fp_lr_ib_cid);
    __EMIT_FILLET(__lr_ot_lo, __lr_ot_hi, __lr_ot_anim, _fp_lr_ot_xa, _fp_lr_ot_ya, _fp_lr_ot_xa_anim, _fp_lr_ot_ya, _fp_lr_ot_cid);
    __EMIT_FILLET(__lr_ob_lo, __lr_ob_hi, __lr_ob_anim, _fp_lr_ob_xa, _fp_lr_ob_ya, _fp_lr_ob_xa_anim, _fp_lr_ob_ya, _fp_lr_ob_cid);
#undef __EMIT_FILLET
#undef __FILLET_BORDER
#undef __FMAX
    /* Concave armpit feet (INNER) — the arc curls into the two desktop-facing
     * corners. Disc center sits at the inner end of each foot's bbox; pixels
     * OUTSIDE the disc are filled, leaving a concave arc that sweeps from the
     * body edge down the screen-side edge. cx/cy track the runtime content edge
     * (__coy/__chs) so the feet stay flush with the painted body. */
    if (armpit > 0) {
        if (anchor & 1) {   /* TOP: feet hang below at BL/BR (cid 2/3) */
            if (arm_in_bl > 0)
                fprintf(o, "    fill_inner_fillet(sl->px, w->w, w->h, %d, __coy + __chs + %d, %d, 2, 0x%08xu);\n",
                        arm_in_bl, arm_in_bl, arm_in_bl, armpit_col);
            if (arm_in_br > 0)
                fprintf(o, "    fill_inner_fillet(sl->px, w->w, w->h, w->w - %d, __coy + __chs + %d, %d, 3, 0x%08xu);\n",
                        arm_in_br, arm_in_br, arm_in_br, armpit_col);
        } else {            /* BOTTOM: feet rise above at TL/TR (cid 1/0) */
            if (arm_in_tl > 0)
                fprintf(o, "    fill_inner_fillet(sl->px, w->w, w->h, %d, __coy - %d, %d, 1, 0x%08xu);\n",
                        arm_in_tl, arm_in_tl, arm_in_tl, armpit_col);
            if (arm_in_tr > 0)
                fprintf(o, "    fill_inner_fillet(sl->px, w->w, w->h, w->w - %d, __coy - %d, %d, 0, 0x%08xu);\n",
                        arm_in_tr, arm_in_tr, arm_in_tr, armpit_col);
        }
    }
    /* Concave round-overs (OUTER) — round the two corners on the anchored screen
     * edge in place. Disc center sits at each corner's inner diagonal; the filled
     * wedge masks the square corner while the disc area stays clear, so the
     * display corner reads as rounded. Uses the full buffer (w->h), independent
     * of the inner feet's content trim. */
    if (armpit_any_outer) {
        if (anchor & 1) {   /* TOP: round screen TL/TR (cid 2/3) */
            if (arm_out_tl > 0)
                fprintf(o, "    fill_inner_fillet(sl->px, w->w, w->h, %d, %d, %d, 2, 0x%08xu);\n",
                        arm_out_tl, arm_out_tl, arm_out_tl, armpit_col);
            if (arm_out_tr > 0)
                fprintf(o, "    fill_inner_fillet(sl->px, w->w, w->h, w->w - %d, %d, %d, 3, 0x%08xu);\n",
                        arm_out_tr, arm_out_tr, arm_out_tr, armpit_col);
        } else {            /* BOTTOM: round screen BL/BR (cid 1/0) */
            if (arm_out_bl > 0)
                fprintf(o, "    fill_inner_fillet(sl->px, w->w, w->h, %d, w->h - %d, %d, 1, 0x%08xu);\n",
                        arm_out_bl, arm_out_bl, arm_out_bl, armpit_col);
            if (arm_out_br > 0)
                fprintf(o, "    fill_inner_fillet(sl->px, w->w, w->h, w->w - %d, w->h - %d, %d, 0, 0x%08xu);\n",
                        arm_out_br, arm_out_br, arm_out_br, armpit_col);
        }
    }
    fprintf(o, "    const Font *f = &font_%d;\n",
            eval_int(surface_prop(sur, "font_size"), 14));
    if (vertical)
        fputs("    int y = __coy; (void)y;\n", o);
    else
        fputs("    int y = (__chs - f->line_h) / 2 + __coy; (void)y;\n", o);
    fprintf(o,
        "    struct { int tw, vis; uint32_t cp, fg, bg, border, press_bg; const uint32_t *pm; int pms; const char *txt; int pad, align; int h; int ch; int body_lines; } st[%d];\n",
        n_arr);
    fprintf(o, "    for (int __i = 0; __i < %d; __i++) { st[__i].vis = 0; st[__i].h = 0; st[__i].ch = 0; st[__i].body_lines = 1; st[__i].border = 0; st[__i].press_bg = 0; }\n", n_arr);
    fputs("    (void)st;\n", o);
    fputs("    int center_total = 0;\n", o);
    /* Trailing-pad correction: every center-aligned item adds `tw + pad` to
     * center_total during measure, but only N-1 gaps exist between N items.
     * Track the last contributor's pad and subtract it after measure. */
    fputs("    int __center_trail_pad = 0;\n", o);
    fprintf(o, "    int end_extent = %s;\n", vertical ? "__chs" : "__cws");
    fprintf(o, "    __%s_nhit = 0;\n\n", nm);
    fputs("    /* --- measure pass --- */\n", o);

    for (int i = 0; i < nitems; i++) {
        /* A group's members always pack along the band's own axis. */
        emit_item_measure(o, &items[i], ctx, items[i].group_id >= 0 ? 0 : vertical, nm, i);
        if (ctx->failed) return 1;
    }

    fputs("    if (center_total > __center_trail_pad) center_total -= __center_trail_pad;\n", o);
    fprintf(o, "    int start_pos = %s;\n", vertical ? "__coy" : "__cox");
    fprintf(o, "    int end_pos = end_extent + %s;\n", vertical ? "__coy" : "__cox");
    fprintf(o, "    int center_pos = (end_extent - center_total) / 2 + %s;\n", vertical ? "__coy" : "__cox");
    fputs("    if (center_pos < 0 && __cox == 0 && __coy == 0) center_pos = 0;\n", o);
    fputs("    (void)start_pos; (void)end_pos; (void)center_pos;\n\n", o);
    fputs("    /* --- draw pass --- */\n", o);
    for (int i = 0; i < nitems; i++) {
        if (items[i].group_id >= 0) {
            if (items[i].group_first)
                i += emit_group_draw(o, items, i, nitems, ctx, nm, vertical) - 1;
            continue;
        }
        emit_item_draw(o, &items[i], ctx, vertical, nm);
    }

    /* Snapshot hits for click dispatch — into this widget's per-slot row. */
    emit_hit_snapshot(o, nm);

    /* `cutout_into = <surface>`: this surface punches a transparent rect
     * through the named target's pixels. cutout_width/cutout_height are
     * expressions (e.g. `anim.emerged_h`); cutout_x/cutout_y default to
     * (centered, 0). The register-then-clear pattern keeps the target's
     * cutout in sync with this surface's render cadence. */
    {
        Expr *ci = surface_prop(sur, "cutout_into");
        if (ci && ci->kind == EX_IDENT) {
            char tgt[64]; int tl = ci->ident.n < 63 ? (int)ci->ident.n : 63;
            memcpy(tgt, ci->ident.s, tl); tgt[tl] = 0;
            ctx->widget_var = "w";
            Expr *cw_e = surface_prop(sur, "cutout_width");
            Expr *ch_e = surface_prop(sur, "cutout_height");
            Expr *cx_e = surface_prop(sur, "cutout_x");
            Expr *cy_e = surface_prop(sur, "cutout_y");
            CE cw_ce = cw_e ? coerce_to_int(ctx, lower(ctx, cw_e)) : (CE){ .text = "(int)w->w", .type = T_INT };
            CE ch_ce = ch_e ? coerce_to_int(ctx, lower(ctx, ch_e)) : (CE){ .text = "(int)w->h", .type = T_INT };
            CE cx_ce = cx_e ? coerce_to_int(ctx, lower(ctx, cx_e)) : (CE){ .text = "CUTOUT_X_CENTER", .type = T_INT };
            CE cy_ce = cy_e ? coerce_to_int(ctx, lower(ctx, cy_e)) : (CE){ .text = "0", .type = T_INT };
            cgctx_flush_prelude(ctx, o, "    ");
            fprintf(o,
                "    { int __cw = (%s), __ch = (%s);\n"
                "      if (__cw > 0 && __ch > 0) cutout_set(\"%s\", w->output, (%s), (%s), __cw, __ch);\n"
                "      else                      cutout_clear(\"%s\", w->output); }\n",
                cw_ce.text, ch_ce.text, tgt, cx_ce.text, cy_ce.text, tgt);
        }
    }
    /* Apply any cutouts registered against this surface (e.g. an OSD slab
     * punching a transparent rect through the bar strip beneath it). */
    fprintf(o, "    cutout_apply(\"%s\", w->output, sl->px, w->w, w->h);\n", nm);
    /* Optional `clip_widgets = true`: in addition to `clip_top` clipping the
     * body bg/border, wipe ALL pixels above the clip line at the very end
     * (after widget content + fillets render). Use this when the surface is
     * meant to be strictly bounded by clip_top — e.g. a corner widget whose
     * slide animation moves the body through an adjacent surface's territory
     * and must not bleed icons/text upward. Without it, widget glyphs stick
     * out above clip_top (HUD-classic, where icons straddle the bar). */
    {
        Expr *cw_e = surface_prop(sur, "clip_widgets");
        int cw_on = 0;
        if (cw_e) {
            if (cw_e->kind == EX_BOOL) cw_on = cw_e->b ? 1 : 0;
            else if (cw_e->kind == EX_INT) cw_on = cw_e->i ? 1 : 0;
            else if (cw_e->kind == EX_IDENT
                     && cw_e->ident.n == 4
                     && memcmp(cw_e->ident.s, "true", 4) == 0) cw_on = 1;
        }
        if (cw_on) {
            fputs("    if (__clip_top > 0) {\n", o);
            fputs("        int __ct = __clip_top > (int)w->h ? (int)w->h : __clip_top;\n", o);
            fputs("        memset(sl->px, 0, (size_t)w->w * (size_t)__ct * 4);\n", o);
            fputs("    }\n", o);
        }
    }
    fputs("    widget_attach(w, sl, 0);\n", o);
    fputs("}\n\n", o);

    SurGeom __g = { anchor, layer, margin, width, height, excl_zone,
                    gut_g, gutter_top, gutter_bottom, armpit, reveal_g,
                    cg_cid_tl, cg_cid_tr, cg_cid_br, cg_cid_bl };
    return emit_surface_life(o, sur, ctx, nm, items, nitems, &__g);
}

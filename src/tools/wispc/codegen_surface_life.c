/* wispc codegen — the per-surface lifecycle functions: click dispatch,
 * <nm>_redraw, visibility, and <nm>_create_on. Paired with codegen_surface.c,
 * which emits render_<nm>; the buffer sizing here must match what that paints,
 * hence the shared SurGeom. */
#include "codegen_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int emit_surface_life(FILE *o, Decl *sur, CGCtx *ctx, const char *nm,
                      BarItem *items, int nitems, const SurGeom *g) {
    emit_surface_click_dispatch(o, items, nitems, ctx, ctx->r, nm);

    fprintf(o, "void %s_redraw(void) {\n", nm);
    fprintf(o, "    for (int i = 0; i < __%s_nw; i++) render_%s(__%s_widgets[i]);\n", nm, nm, nm);
    fputs("}\n\n", o);

    /* Visibility lifecycle: surfaces declaring `visible = <expr>` (typically a
     * mut bool) get an is_visible()/apply_visibility() pair. apply_visibility
     * is called from the main loop after each dirty pass; on a false→true
     * transition it creates widgets on every active output, on true→false it
     * destroys them all. Surfaces without `visible` get no-op stubs so the
     * caller doesn't need to special-case them. */
    Expr *vis_prop = surface_prop(sur, "visible");
    if (vis_prop) {
        ctx->widget_var = NULL;
        CE ve = lower(ctx, vis_prop);
        ve = coerce_to_int(ctx, ve);
        fprintf(o, "int %s_is_visible(void) {\n", nm);
        cgctx_flush_prelude(ctx, o, "    ");
        fprintf(o, "    return %s;\n", ve.text);
        fputs("}\n", o);
        ctx->widget_var = "w";
    } else {
        fprintf(o, "int %s_is_visible(void) { return 1; }\n", nm);
    }
    fprintf(o, "void %s_apply_visibility(void) {\n", nm);
    if (vis_prop) {
        fprintf(o, "    int want = %s_is_visible();\n", nm);
        fprintf(o, "    int have = __%s_nw > 0;\n", nm);
        fputs("    if (want && !have) {\n", o);
        fputs("        for (int i = 0; i < MAX_OUTPUTS; i++)\n", o);
        fprintf(o, "            if (outputs[i].active) %s_create_on(&outputs[i]);\n", nm);
        fputs("    } else if (!want && have) {\n", o);
        /* Destroy from the tail: widget_destroy() swap-removes each widget from
         * __<nm>_widgets via wispgen_widget_destroyed(), so always destroy the
         * current last entry until the registry is empty. (A forward for-loop
         * would skip the element swapped into the freed slot.) */
        fprintf(o, "        while (__%s_nw > 0) widget_destroy(__%s_widgets[__%s_nw - 1]);\n", nm, nm, nm);
        fputs("    }\n", o);
    }
    fputs("}\n\n", o);

    int reveal = eval_int(surface_prop(sur, "reveal_on_hover"), 0);
    int is_hud = reveal > 0;

    fprintf(o, "void %s_create_on(Output *o) {\n", nm);
    fputs("    if (!o) return;\n", o);
    fprintf(o, "    if (__%s_nw >= 8) return;\n", nm);
    if (is_hud) fputs("    Widget *w = widget_alloc(W_HUD);\n", o);
    else        fputs("    Widget *w = widget_alloc(W_BAR);\n", o);
    fprintf(o, "    if (!w) { msg(\"wisp: no widget slot for %s\"); return; }\n", nm);
    fputs("    w->output = o;\n", o);
    /* Legacy convenience: a surface named "bar" populates o->bar so the rest
     * of the runtime (dwl-ipc tag accumulator, lock-on-output) keeps working. */
    if (strcmp(nm, "bar") == 0) fputs("    o->bar = w;\n", o);
    fprintf(o, "    __%s_widgets[__%s_nw++] = w;\n", nm, nm);
    fprintf(o, "    widget_setup_surface(w, %d, \"wisp-%s\", o);\n", g->layer, nm);
    int __buf_w = 0, __buf_h = 0; /* exported to hud_register below */
    {
        /* HUD reserves the `reveal_gutter` band ON THE SLIDE AXIS, above/beside
         * the body. The trigger strip needs no buffer of its own: the slide is
         * render-only (the layer surface stays anchored), so the input region is
         * addressable whether or not the gutter reserves pixels for it. */
        int axis_is_x = (is_hud && (g->anchor == 4 || g->anchor == 8));
        int total_w = axis_is_x ? (g->width + g->gut_g) : g->width;
        int total_h = (is_hud && !axis_is_x) ? (g->height + g->gutter_top + g->gutter_bottom) : (g->height + g->armpit);
        Expr *pf_tl = surface_prop(sur, "fillet_tl");
        Expr *pf_tr = surface_prop(sur, "fillet_tr");
        Expr *pf_bl = surface_prop(sur, "fillet_bl");
        Expr *pf_br = surface_prop(sur, "fillet_br");
        /* TOP/BOTTOM aliases fold into absolute tl/tr/bl/br (same corner_id). */
        Expr *lr_it = NULL, *lr_ib = NULL, *lr_ot = NULL, *lr_ob = NULL;
        {
            Expr *it  = surface_prop(sur, "fillet_inner_top");
            Expr *ib  = surface_prop(sur, "fillet_inner_bottom");
            Expr *ot  = surface_prop(sur, "fillet_outer_top");
            Expr *ob  = surface_prop(sur, "fillet_outer_bottom");
            Expr *il  = surface_prop(sur, "fillet_inner_left");
            Expr *ir  = surface_prop(sur, "fillet_inner_right");
            Expr *ol  = surface_prop(sur, "fillet_outer_left");
            Expr *orp = surface_prop(sur, "fillet_outer_right");
            switch (g->anchor) {
            case 1: if (!pf_tl && il)  pf_tl = il;  if (!pf_tr && ir)  pf_tr = ir;  if (!pf_bl && ol)  pf_bl = ol;  if (!pf_br && orp) pf_br = orp; break;
            case 2: if (!pf_bl && il)  pf_bl = il;  if (!pf_br && ir)  pf_br = ir;  if (!pf_tl && ol)  pf_tl = ol;  if (!pf_tr && orp) pf_tr = orp; break;
            case 4: lr_ot = ot; lr_ob = ob; lr_it = it; lr_ib = ib; break;
            case 8: lr_ot = ot; lr_ob = ob; lr_it = it; lr_ib = ib; break;
            default: break;
            }
        }
        int ftl_lo, ftl_hi; eval_int_range(pf_tl, &ftl_lo, &ftl_hi, 0);
        int ftr_lo, ftr_hi; eval_int_range(pf_tr, &ftr_lo, &ftr_hi, 0);
        int fbl_lo, fbl_hi; eval_int_range(pf_bl, &fbl_lo, &fbl_hi, 0);
        int fbr_lo, fbr_hi; eval_int_range(pf_br, &fbr_lo, &fbr_hi, 0);
        int lit_lo, lit_hi; eval_int_range(lr_it, &lit_lo, &lit_hi, 0);
        int lib_lo, lib_hi; eval_int_range(lr_ib, &lib_lo, &lib_hi, 0);
        int lot_lo, lot_hi; eval_int_range(lr_ot, &lot_lo, &lot_hi, 0);
        int lob_lo, lob_hi; eval_int_range(lr_ob, &lob_lo, &lob_hi, 0);
#define __MX(a,b) ((a)>(b)?(a):(b))
        int ftl = __MX(ftl_lo, ftl_hi), ftr = __MX(ftr_lo, ftr_hi);
        int fbl = __MX(fbl_lo, fbl_hi), fbr = __MX(fbr_lo, fbr_hi);
        int lit = __MX(lit_lo, lit_hi), lib = __MX(lib_lo, lib_hi);
        int lot = __MX(lot_lo, lot_hi), lob = __MX(lob_lo, lob_hi);
        int tl_l = USES_LEFT(g->cid_tl)  ? ftl : 0;
        int bl_l = USES_LEFT(g->cid_bl)  ? fbl : 0;
        int tr_r = USES_RIGHT(g->cid_tr) ? ftr : 0;
        int br_r = USES_RIGHT(g->cid_br) ? fbr : 0;
        int pad_l_abs = tl_l > bl_l ? tl_l : bl_l;
        int pad_r_abs = tr_r > br_r ? tr_r : br_r;
        int lr_pad_l = 0, lr_pad_r = 0;
        (void)lit; (void)lib; (void)lot; (void)lob;
        int pad_l = __MX(pad_l_abs, lr_pad_l);
        int pad_r = __MX(pad_r_abs, lr_pad_r);
        /* LR aliases also extend vertically: inner_top + outer_top wedges
         * stick UP above body-top; inner_bottom + outer_bottom stick DOWN
         * below body-bottom. Y-padding is symmetric to X-padding for the
         * X-anchored case. */
        int lr_pad_top = (g->anchor == 4 || g->anchor == 8) ? __MX(lit, lot) : 0;
        int lr_pad_bot = (g->anchor == 4 || g->anchor == 8) ? __MX(lib, lob) : 0;
#undef __MX
        __buf_w = total_w + pad_l + pad_r;
        __buf_h = total_h + lr_pad_top + lr_pad_bot;
        fprintf(o, "    widget_set_size(w, %d, %d);\n", __buf_w, __buf_h);
    }
    fprintf(o, "    widget_set_anchor(w, %d);\n", g->anchor);
    if (is_hud) {
        /* HUD: default exclusive_zone=-1 (ignore other surfaces' exclusive
         * zones so the trigger gutter overlaps the bar). DSL can override
         * with `exclusive_zone = 0` to instead respect other zones — useful
         * for top-corner widgets that should sit *below* the bar. */
        int hud_excl = surface_prop(sur, "exclusive_zone") ? g->excl_zone : -1;
        fprintf(o, "    widget_set_exclusive_zone(w, %d);\n", hud_excl);
        fputs("    widget_set_kbd_interactive(w, 0);\n", o);
        /* hud_register's full_w/full_h must match the SHM buffer dimensions
         * so slide_extent, trigger input region, and the emerged calc all
         * agree with the rendered geometry. */
        fprintf(o, "    hud_register(w, %d, %d, %d, %d);\n", g->anchor, g->reveal_g, __buf_w, __buf_h);
        int reveal_ms = eval_int(surface_prop(sur, "reveal_anim_ms"), 0);
        if (reveal_ms > 0)
            fprintf(o, "    hud_set_reveal_anim(w, %d, %s);\n",
                    reveal_ms, surface_easing_id(sur, "reveal_easing"));
        fputs("    wl_req(w->surface, SURFACE_REQ_COMMIT, NULL, 0, -1);\n", o);
    } else {
        /* Always set, even at 0: a reload adopts the old process's layer
         * surface, which may carry the previous preset's margin. */
        fprintf(o, "    widget_set_margin(w, %d, %d, %d, %d);\n", g->margin, g->margin, g->margin, g->margin);
        fprintf(o, "    widget_set_exclusive_zone(w, %d);\n", g->excl_zone);
        fputs("    widget_set_kbd_interactive(w, 0);\n", o);
        Expr *inp = surface_prop(sur, "input");
        int input_none = inp && inp->kind == EX_IDENT && inp->ident.n == 4
                         && memcmp(inp->ident.s, "none", 4) == 0;
        if (input_none) {
            /* `input = none;` → empty input region: pointer events fall through
             * to whatever is beneath (used by the decorative screen-corner
             * strip so it never steals clicks from windows/desktop). */
            fputs("    widget_set_input_region_rect(w, 0, 0, 0, 0);\n", o);
        } else if (g->armpit > 0) {
            /* Constrain pointer input to the painted body. Without this the
             * default full-surface region would let the transparent feet strip
             * (the extra `armpit` rows on the desktop side) intercept clicks
             * across the whole width. A region wider than the surface is clipped
             * by the compositor, so 1<<15 safely spans any output. */
            fprintf(o, "    widget_set_input_region_rect(w, 0, %d, %d, %d);\n",
                    (g->anchor & 1) ? 0 : g->armpit, 1 << 15, g->height);
        }
        fputs("    wl_req(w->surface, SURFACE_REQ_COMMIT, NULL, 0, -1);\n", o);
    }
    fputs("}\n\n", o);

    return 0;
}


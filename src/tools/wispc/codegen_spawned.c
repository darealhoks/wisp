/* wispc codegen — surfaces spawned at runtime: OSD slabs and menu popups.
 * Both render into a pool the runtime owns (osd.c / menu.c), so neither
 * emits the create_on/visibility lifecycle a declared surface gets. */
#include "codegen_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The renderer for a `spawned_by = osd` template: iterates the slab ring,
 * pushes the per-slab $-bindings (summary/body/icon/pct/muted/...) and runs
 * each declared widget through emit_item_measure/emit_item_draw. Emitted as
 * `render_<nm>(Widget *w)`; osd_render() calls it. The runtime owns time
 * (osd_slab_geom) and the bar junction (osd_bar_split); everything else about
 * where a pixel lands comes from the declaration.
 *
 * `pill` selects the one-slab progress pill (`spawned_by = osd_pill`): same
 * pipeline, but the ring is a single fixed-height item, the top margin is
 * baked into the buffer instead of offsetting the body (a layer-surface
 * margin would clip the slide), and a negative margin straddles the bar edge
 * — body inside the bar row, all four corners round, fillets still on the
 * junction line. */
int emit_spawned_osd_skeleton(FILE *o, Decl *sur, CGCtx *ctx, const char *nm, int pill) {
    BarItem items[64]; int err = 0;
    int nitems = collect_bar_items(sur->surface.items, sur->surface.n,
                                   items, 64, ctx, &err);
    if (err) return 1;
    int n_arr = 0;
    for (int i = 0; i < nitems; i++) {
        items[i].st_base = n_arr;
        n_arr += items[i].is_runtime_for_cell ? items[i].runtime_for_cap : 1;
    }
    if (n_arr == 0) n_arr = 1;
    int n_sliders = 0;
    for (int i = 0; i < nitems; i++)
        if (widget_is_slider(items[i].w)) items[i].slider_idx = n_sliders++;
    assign_handler_idx(items, nitems);

    /* Per-surface hit table — sized but unused (no click dispatch wired;
     * osd.c's dismiss-on-click owns the whole slab). */
    fprintf(o, "static int __%s_nhit;\n", nm);
    fprintf(o, "typedef struct { int x, y, w, h; int kind; int arg; int slider_idx; int st_idx; } %s_Hit;\n", nm);
    fprintf(o, "static %s_Hit __%s_hits_buf[64];\n", nm, nm);
    fprintf(o, "static int __%s_pressed_st = -1;\n", nm);
    fprintf(o, "static Widget *__%s_pressed_w;\n", nm);

    /* Fallbacks only — a bg/border that is an expression (a `:warn` overlay)
     * lowers per-slab instead; evaluating it here would demand a literal. */
    uint32_t slab_bg   = surface_prop(sur, "bg")     ? 0 : 0xff000000;
    uint32_t slab_bord = eval_color_ctx(ctx, surface_prop(sur, "border"), 0);
    int slab_bord_w    = eval_int(surface_prop(sur, "border_width"), 0);
    int slab_radius    = eval_int(surface_prop(sur, "radius"), 0);
    int slab_margin    = eval_int(surface_prop(sur, "margin"), 0);
    int slab_w         = eval_int(surface_prop(sur, "width"), 0);
    int sep_frac       = eval_int(surface_prop(sur, "separator_frac"), 0);
    uint32_t sep_col   = eval_color_ctx(ctx, surface_prop(sur, "separator"), 0);
    int fillet_r       = eval_int(surface_prop(sur, "fillet_r"), 0);

    /* Anchor drives the whole stack layout. An edge in the mask with margin 0
     * means the chain sits FLUSH against that screen edge: no round-over, no
     * border, and (for a two-edge corner) a fillet flaring into it. A margin
     * lifts the chain off every edge, so nothing is flush. */
    int amask = eval_anchor(surface_prop(sur, "anchor"));
    if (amask < 0) amask = 1;
    int flush = slab_margin > 0 ? 0 : amask;
    int fl_t = !!(flush & 1), fl_b = !!(flush & 2),
        fl_l = !!(flush & 4), fl_r = !!(flush & 8);
    int up = !!(amask & 2);   /* bottom-anchored: chain grows upward */
    /* A straddling pill sits *over* the bar (overlay layer), so there is no
     * junction to clip against and its top corners round like any other. */
    int straddle = pill && slab_margin < 0;
    /* Corner rounds only where the chain faces the desktop on both its edges. */
    int r_tl = (straddle || (!fl_t && !fl_l)) ? slab_radius : 0;
    int r_tr = (straddle || (!fl_t && !fl_r)) ? slab_radius : 0;
    int r_br = (!fl_b && !fl_r) ? slab_radius : 0;
    int r_bl = (!fl_b && !fl_l) ? slab_radius : 0;

    fprintf(o, "void render_%s(Widget *w) {\n", nm);
    fputs("    if (!w->configured || w->w <= 0 || w->h <= 0) return;\n", o);
    fputs("    int __wi = 0; (void)__wi;\n", o);   /* OSD template: singleton widget */
    /* Nothing left in the ring: hand back a transparent buffer (never NULL —
     * see osd_attach_empty) and let the pool go, so idle costs nothing. */
    fputs("    if (!w->s.osd.items[0].active) {\n"
          "        if (w->s.osd.has_pixels) osd_stack_attach_empty(w);\n"
          "        return;\n    }\n", o);
    fprintf(o, "    int __n = %s;\n",
            pill ? "osd_pill_layout(w)" : "osd_slab_layout(w)");
    fputs("    osd_stack_input_region(w);\n", o);
    fputs("    widget_ensure_pool(w, 2);\n", o);
    fputs("    BufSlot *sl = widget_free_slot(w);\n", o);
    fputs("    if (!sl) return;\n", o);
    fputs("    clear_buf(sl->px, w->w, w->h, 0);\n", o);
    fprintf(o, "    const Font *f = &font_%d; (void)f;\n",
            eval_int(surface_prop(sur, "font_size"), 14));
    fprintf(o, "    int __slab_w = %d; if (__slab_w <= 0) __slab_w = w->w;\n", slab_w);
    /* First/last *visible* slab: the chain rounds only its outer corners.
     * __chain_h is the whole chain's current extent — a bottom-anchored stack
     * is laid out from it, and both anchors use it to ramp the fillets. */
    fputs("    int __first = -1, __last = -1, __chain_h = 0;\n", o);
    fputs("    for (int i = 0; i < __n; i++) { OsdSlabGeom g; osd_slab_geom(w, i, &g);\n"
          "        if (g.vh > 0) { if (__first < 0) __first = i; __last = i;\n"
          "            __chain_h = g.y + g.vh; } }\n"
          "    (void)__chain_h;\n", o);
    fprintf(o, "    int __chain_x = %s;\n",
            (amask & 8) ? "w->w - __slab_w" : (amask & 4) ? "0" : "(w->w - __slab_w) / 2");
    /* __jsplit is the bar junction line (where the fillets sit); __split is
     * how far down the body fill must stay clear of it. They differ only for
     * a straddling pill, which paints over the bar row. */
    fprintf(o, "    int __jsplit = %s; (void)__jsplit;\n", fl_t ? "osd_bar_split()" : "0");
    fprintf(o, "    int __split = %s;\n", straddle ? "0" : "__jsplit");
    fputs("    int __ctop = w->h, __cbot = 0;\n", o);
    fputs("    uint32_t __ctop_bg = 0, __cbot_bg = 0;\n", o);
    fputs("    (void)__ctop_bg; (void)__cbot_bg;\n", o);
    /* Slab loop. MAX_OSDS comes from src/wisp.h via the included wisp.h header. */
    fputs("    for (int __sl = 0; __sl < __n; __sl++) {\n", o);
    fputs("        OsdSlabGeom __g; osd_slab_geom(w, __sl, &__g);\n", o);
    fputs("        if (__g.vh <= 0) continue;\n", o);
    /* The leading slab slides: drawn at full settled height but offset up by
     * the not-yet-emerged remainder, so body and content slide as one and the
     * overflow clips against the buffer top. Lower slabs grow in place. */
    if (up)
        /* Bottom-anchored: the chain is pinned to its bottom edge, so every
         * slab keeps its top at the running offset measured up from there and
         * grows in place; the emerging slab's overflow clips off-screen. */
        fprintf(o, "        int __reg_x = __chain_x, __reg_y = w->h - %d - __chain_h + __g.y,"
                   " __reg_w = __slab_w, __reg_h = __g.vh;\n", slab_margin);
    else {
        fputs("        int __slide = (__sl == __first) ? __g.vh - __g.sh : 0;\n", o);
        /* A pill's margin is already inside the buffer height its vh sweeps,
         * so adding it here would rest the body twice as far down. */
        fprintf(o, "        int __reg_x = __chain_x, __reg_y = %d + __g.y + __slide,"
                   " __reg_w = __slab_w, __reg_h = (__sl == __first) ? __g.sh : __g.vh;\n",
                pill ? 0 : slab_margin);
    }
    fputs("        (void)__reg_x; (void)__reg_y; (void)__reg_w; (void)__reg_h;\n", o);
    /* Per-slab transient buffers for $pct → "42" string interpolation. */
    fputs("        char __pct_buf[16]; snprintf(__pct_buf, sizeof __pct_buf, \"%d\", w->s.osd.items[__sl].progress);\n", o);

    /* Push $-bindings. src_name slot is the type-tag: "str" → T_STR, "int" → T_INT. */
    push_local(ctx, "summary", 7, LB_DOLLAR_BIND,
               "w->s.osd.items[__sl].summary", "str");
    /* Wrapped, not raw: osd_slab_layout already broke the body to the slab's
     * text column, and a widget's body_lines renderer splits on "\n". */
    push_local(ctx, "body", 4, LB_DOLLAR_BIND,
               "osd_slab_body(__sl)", "str");
    push_local(ctx, "nbody", 5, LB_DOLLAR_BIND,
               "osd_slab_nbody(__sl)", "int");
    push_local(ctx, "icon", 4, LB_DOLLAR_BIND,
               "w->s.osd.items[__sl].icon_cp", "int");
    push_local(ctx, "pct", 3, LB_DOLLAR_BIND,
               "__pct_buf", "str");
    push_local(ctx, "muted", 5, LB_DOLLAR_BIND,
               "w->s.osd.items[__sl].muted", "int");
    push_local(ctx, "urgency", 7, LB_DOLLAR_BIND,
               "w->s.osd.items[__sl].urgency", "int");
    /* $pct is the display string, $progress the 0..100 number a slider wants. */
    push_local(ctx, "progress", 8, LB_DOLLAR_BIND,
               "w->s.osd.items[__sl].progress", "int");
    /* Truthy derivations of `muted`, so `:mute` / `:warn` work with the same
     * boolean-field mechanism the for-cell pseudos use. */
    push_local(ctx, "mute", 4, LB_DOLLAR_BIND,
               "(w->s.osd.items[__sl].muted == 1)", "int");
    push_local(ctx, "warn", 4, LB_DOLLAR_BIND,
               "(w->s.osd.items[__sl].muted == 2)", "int");

    /* Slab body, drawn before the widgets that sit on it. Colors lower as
     * expressions, not constants — a `:warn` pseudo makes them per-slab. */
    {
        CE cbg = {0};
        Expr *bge = surface_prop(sur, "bg");
        if (bge) cbg = lower(ctx, bge);
        Expr *bde = surface_prop(sur, "border");
        CE cbd = {0};
        if (bde) cbd = lower(ctx, bde);
        cgctx_flush_prelude(ctx, o, "        ");
        char bgtxt[512];
        if (bge) snprintf(bgtxt, sizeof bgtxt, "(uint32_t)(%s)", cbg.text);
        else          snprintf(bgtxt, sizeof bgtxt, "0x%08xu", slab_bg);
        /* The chain reads as one body: only its outer corners round, and the
         * outline is a single pass after the loop over the accumulated extent
         * (a per-slab border would draw seams across every join). */
        fprintf(o, "        { int __rtl = (__sl == __first) ? %d : 0, __rtr = (__sl == __first) ? %d : 0;\n",
                r_tl, r_tr);
        fprintf(o, "          int __rbr = (__sl == __last) ? %d : 0, __rbl = (__sl == __last) ? %d : 0;\n",
                r_br, r_bl);
        fprintf(o, "          uint32_t __bg = %s;\n", bgtxt);
        /* Flush under a bar: the round-over must not bite into the rows the
         * bar cutout already nulled (wallpaper would show through the bar),
         * so it only applies past the junction. __split is 0 everywhere else,
         * which makes the call plain fill_rect_rounded. */
        fputs("          fill_rect_rounded_split(sl->px, w->w, w->h, __reg_x, __reg_y, __reg_w,"
              " __reg_h, __rtl, __rtr, __rbr, __rbl, __split, __bg);\n", o);
        fputs("          if (__reg_y < __ctop) { __ctop = __reg_y; __ctop_bg = __bg; }\n", o);
        fputs("          if (__reg_y + __reg_h > __cbot) { __cbot = __reg_y + __reg_h; __cbot_bg = __bg; }\n", o);
        fputs("        }\n", o);
        (void)cbd; (void)bde;
    }
    /* Hairline between two slabs that have both finished animating — drawn
     * mid-slide it would slice across a body that is still moving. */
    if (sep_frac > 0 && (sep_col & 0xff000000u)) {
        fputs("        if (__sl > __first) { OsdSlabGeom __pg; osd_slab_geom(w, __sl - 1, &__pg);\n", o);
        fprintf(o, "            if (__g.settled && __pg.settled) {\n"
                   "                int __sw = __reg_w * %d / 100;\n"
                   "                fill_rect(sl->px, w->w, w->h, __reg_x + (__reg_w - __sw) / 2,"
                   " __reg_y, __sw, 1, 0x%08xu);\n            } }\n", sep_frac, sep_col);
    }
    /* `y` is the text baseline cursor for horizontally-laid items; matches
     * the formula in emit_generated_surface so emit_item_draw can reference
     * it as a free variable. */
    fputs("        int y = (__reg_h - f->line_h) / 2 + __reg_y; (void)y;\n", o);
    fputs("        int __cox = __reg_x, __coy = __reg_y, __cws = __reg_w, __chs = __reg_h;\n", o);
    fputs("        (void)__cox; (void)__coy; (void)__cws; (void)__chs;\n", o);

    /* Measure + draw passes — same scaffolding as a real surface, just inside
     * the slab loop. center_pos math uses __reg_w as the extent. */
    fprintf(o, "        struct { int tw, vis; uint32_t cp, fg, bg, border, press_bg; const uint32_t *pm; int pms; const char *txt; int pad, align; int h; int ch; int body_lines; } st[%d];\n", n_arr);
    fprintf(o, "        for (int __i = 0; __i < %d; __i++) { st[__i].vis = 0; st[__i].h = 0; st[__i].ch = 0; st[__i].body_lines = 1; st[__i].border = 0; st[__i].press_bg = 0; }\n", n_arr);
    fputs("        (void)st;\n", o);
    fputs("        int center_total = 0;\n", o);
    fputs("        int __center_trail_pad = 0; (void)__center_trail_pad;\n", o);
    fputs("        int end_extent = __reg_w;\n", o);
    /* Re-purpose nhit for this slab — overwritten each iteration. */
    fprintf(o, "        __%s_nhit = 0;\n", nm);


    fputs("        /* --- measure pass --- */\n", o);
    for (int i = 0; i < nitems; i++) {
        emit_item_measure(o, &items[i], ctx, 0 /*horizontal*/, nm, i);
        if (ctx->failed) { for (int k = 0; k < 10; k++) pop_local(ctx); return 1; }
    }
    fputs("        if (center_total > __center_trail_pad) center_total -= __center_trail_pad;\n", o);
    fputs("        int start_pos = __reg_x;\n", o);
    fputs("        int end_pos = end_extent + __reg_x;\n", o);
    fputs("        int center_pos = (end_extent - center_total) / 2 + __reg_x;\n", o);
    fputs("        (void)start_pos; (void)end_pos; (void)center_pos;\n", o);
    fputs("        /* --- draw pass --- */\n", o);
    for (int i = 0; i < nitems; i++)
        emit_item_draw(o, &items[i], ctx, 0 /*horizontal*/, nm);

    fputs("    }\n", o);

    /* Outline the whole chain as one shape; sides on a flush screen edge stay
     * open. Mid-slide overflow past the buffer is clipped by the primitives. */
    if (slab_bord_w > 0 && (slab_bord & 0xff000000u))
        fprintf(o, "    if (__cbot > __ctop)\n"
                   "        fill_rect_rounded_border(sl->px, w->w, w->h, __chain_x, __ctop, __slab_w,"
                   " __cbot - __ctop, %d, %d, %d, %d, %d, %d, %d, %d, %d, 0, 0x%08xu);\n",
                r_tl, r_tr, r_br, r_bl, slab_bord_w,
                !fl_t, !fl_r, !fl_b, !fl_l, slab_bord);

    /* Fillets flare the chain into the two screen edges it is flush against,
     * ramping with the emerged height so they never outsize the body. Only the
     * bottom-right corner stack needs them today; the top-flush-under-a-bar
     * variant's junction split is still osd.c's (see the plan).
     * ponytail: hardcoded to the bottom|right armpits — generalize when a
     * preset anchors bottom|left. */
    if (pill && fillet_r > 0 && fl_t) {
        /* One slab, one regime: the arc ramps with how far the body has
         * emerged past the junction, reaching the declared radius exactly at
         * rest (a negative margin shortens that travel by however far the
         * body finally tucks into the bar). */
        int bd = eval_int(surface_prop(sur, "height"), 0)
               + (slab_margin < 0 ? slab_margin : 0);
        if (bd < 1) bd = 1;
        fprintf(o, "    { int __em = __cbot - __jsplit; if (__em < 0) __em = 0;\n"
                   "      int __fr = %d * __em / %d; if (__fr > %d) __fr = %d;\n"
                   "      if (__fr > 0) {\n"
                   "          fill_corner_fillet(sl->px, w->w, w->h, __chain_x, __jsplit, __fr, 0, __ctop_bg);\n"
                   "          fill_corner_fillet(sl->px, w->w, w->h, __chain_x + __slab_w, __jsplit, __fr, 1, __ctop_bg);\n"
                   "      } }\n",
                fillet_r, bd, fillet_r, fillet_r);
    } else if (fillet_r > 0 && fl_t && !fl_l && !fl_r) {
        /* Flush under a bar: the armpits sit on the junction line and grow
         * with the body's emerged-past-bar extent, so the chain reads as one
         * shape with the bar instead of butting into it. Two regimes — a
         * spawning chain ramps the arc proportionally across the whole reveal,
         * a closing one holds it at full R until the chain has less than R
         * left, so slab[0] sliding away over a full slab[1] doesn't snap.
         * ponytail: emerged is the chain extent incl. gaps; both presets run
         * gap = 0. Subtract the gaps if one ever doesn't. */
        fprintf(o, "    if (__first >= 0) { int __em = __cbot - __jsplit; if (__em < 0) __em = 0;\n"
                   "        OsdSlabGeom __lg; osd_slab_geom(w, __first, &__lg);\n"
                   "        int __fr;\n"
                   "        if (__lg.closing) { __fr = __em < %d ? __em : %d; }\n"
                   "        else { int __bd = __lg.sh - __jsplit; if (__bd < 1) __bd = 1;\n"
                   "               __fr = %d * __em / __bd; if (__fr > %d) __fr = %d; }\n"
                   "        if (__fr > 0) {\n"
                   "            fill_corner_fillet(sl->px, w->w, w->h, __chain_x, __jsplit, __fr, 0, __ctop_bg);\n"
                   "            fill_corner_fillet(sl->px, w->w, w->h, __chain_x + __slab_w, __jsplit, __fr, 1, __ctop_bg);\n"
                   "        } }\n",
                fillet_r, fillet_r, fillet_r, fillet_r, fillet_r);
    } else if (fillet_r > 0 && fl_b && fl_r) {
        fprintf(o, "    { int __fr = (__cbot - __ctop) < %d ? (__cbot - __ctop) : %d;\n",
                fillet_r, fillet_r);
        fputs("      if (__fr > 0) {\n"
              "          fill_corner_fillet(sl->px, w->w, w->h, w->w, __ctop, __fr, 3, __ctop_bg);\n"
              "          fill_corner_fillet(sl->px, w->w, w->h, __chain_x, w->h, __fr, 3, __cbot_bg);\n", o);
        if (slab_bord_w > 0 && (slab_bord & 0xff000000u))
            fprintf(o, "          fill_corner_fillet_border(sl->px, w->w, w->h, w->w, __ctop, __fr, 3, %d, 0x%08xu);\n"
                       "          fill_corner_fillet_border(sl->px, w->w, w->h, __chain_x, w->h, __fr, 3, %d, 0x%08xu);\n",
                    slab_bord_w, slab_bord, slab_bord_w, slab_bord);
        fputs("      } }\n", o);
    }

    fputs("    w->s.osd.has_pixels = 1;\n", o);
    fputs("    widget_attach(w, sl, osd_slab_anim_pending(w));\n", o);
    fputs("}\n\n", o);

    for (int k = 0; k < 10; k++) pop_local(ctx);
    return 0;
}

/* The renderer for a `menu NAME {}` decl. A menu is a vertical surface whose
 * rows are one runtime-for cell, so the measure/draw/hit machinery is exactly
 * a bar's — all this adds is the body fill and the pool handling that menu.c
 * used to open-code. Lifecycle (create, size, filter, keys, scroll, reply)
 * stays in menu.c, which is why this emits no create_on/apply_visibility. */
int emit_menu_render(FILE *o, Decl *sur, Decl *tmpl, CGCtx *ctx, const char *nm) {
    BarItem items[64]; int err = 0;
    /* `menu.prompt` falls back to the surface's declared prompt when the
     * caller passed no title. */
    Expr *pr = surface_prop(sur, "prompt");
    char prompt_lit[128];
    if (pr && pr->kind == EX_STRING)
        snprintf(prompt_lit, sizeof prompt_lit, "\"%.*s\"", (int)pr->str.n, pr->str.s);
    else
        snprintf(prompt_lit, sizeof prompt_lit, "\"\"");
    push_local(ctx, "menu", 4, LB_MENU_SELF, prompt_lit, NULL);
    int nitems = collect_bar_items(sur->surface.items, sur->surface.n,
                                   items, 64, ctx, &err);
    if (err) return 1;
    int n_arr = 0;
    for (int i = 0; i < nitems; i++) {
        items[i].st_base = n_arr;
        n_arr += items[i].is_runtime_for_cell ? items[i].runtime_for_cap : 1;
    }
    if (n_arr == 0) n_arr = 1;
    int n_sliders = 0;
    for (int i = 0; i < nitems; i++)
        if (widget_is_slider(items[i].w)) items[i].slider_idx = n_sliders++;
    assign_handler_idx(items, nitems);

    fprintf(o, "typedef struct { int x, y, w, h; int kind; int arg; int slider_idx; int st_idx; } %s_Hit;\n", nm);
    fprintf(o, "static %s_Hit __%s_hits_buf[64];\n", nm, nm);
    fprintf(o, "static int __%s_nhit;\n", nm);
    fprintf(o, "static int __%s_pressed_idx = -1;\n", nm);
    fprintf(o, "static int __%s_pressed_slider = -1;\n", nm);
    fprintf(o, "static int __%s_pressed_st = -1;\n", nm);
    fprintf(o, "static Widget *__%s_pressed_w;\n", nm);
    /* One menu widget exists at a time, but the hit store is per-slot, so the
     * widget registers itself on first render instead of via a create_on. */
    fprintf(o, "Widget *__%s_widgets[1]; int __%s_nw;\n", nm, nm);
    emit_hit_store(o, nm, 1);
    fputs("\n", o);

    ctx->widget_var = "w";
    fputs("#ifdef WISP_HAS_ANIM\n", o);
    for (int i = 0; i < nitems; i++) {
        if (!item_has_any_transition(items[i].w)) continue;
        emit_item_slot_decls(o, items[i].w, nm, i,
                             items[i].is_runtime_for_cell ? items[i].runtime_for_cap : 1, 1);
    }
    for (int i = 0; i < nitems; i++) {
        if (!widget_has_vis_anim(items[i].w)) continue;
        fprintf(o, "static VisSlot %s_vis%d[1][%d];\n", nm, i,
                items[i].is_runtime_for_cell ? items[i].runtime_for_cap : 1);
    }
    fputs("#endif\n", o);

    /* A `menu NAME {}` body inherits the template's axis unless it names its
     * own: `axis` is what menu.c's sizing/scroll assume, and it is declared
     * once on the template. `tmpl` is NULL when sur IS the template. */
    Expr *ax = surface_prop(sur, "axis");
    /* menu.c sizes and scrolls every menu from the template's axis alone
     * (MENU_VERTICAL), so a per-menu axis that disagrees would draw rows on one
     * axis and size the surface on the other. Reject it instead. */
    if (ax && tmpl && surface_is_vertical(sur) != surface_is_vertical(tmpl))
        diag_error(sur->loc, "menu '%s': axis must match the `spawned_by = menu` "
                             "template's axis", sur->name);
    int vert        = surface_is_vertical(!ax && tmpl ? tmpl : sur);
    uint32_t bg     = eval_color_ctx(ctx, surface_prop(sur, "bg"), 0xff000000);
    uint32_t bord   = eval_color_ctx(ctx, surface_prop(sur, "border"), 0);
    int bord_w      = eval_int(surface_prop(sur, "border_width"), 0);
    int radius      = eval_int(surface_prop(sur, "radius"), 0);
    int pad         = eval_int(surface_prop(sur, "pad"), 0);
    int pad_x       = eval_int(surface_prop(sur, "pad_x"), pad);
    int pad_y       = eval_int(surface_prop(sur, "pad_y"), pad);

    fprintf(o, "void render_%s(Widget *w) {\n", nm);
    fputs("    if (!w->configured || w->w <= 0 || w->h <= 0) return;\n", o);
    fprintf(o, "    if (__%s_nw == 0 || __%s_widgets[0] != w) { __%s_widgets[0] = w; __%s_nw = 1; }\n",
            nm, nm, nm, nm);
    fputs("    int __wi = 0; (void)__wi;\n", o);   /* menu registry cap is 1 */
    fputs("    widget_ensure_pool(w, 2);\n", o);
    fputs("    BufSlot *sl = widget_free_slot(w);\n    if (!sl) return;\n", o);
    fputs("    clear_buf(sl->px, w->w, w->h, 0);\n", o);
    fprintf(o, "    fill_rect_rounded(sl->px, w->w, w->h, 0, 0, w->w, w->h, %d, %d, %d, %d, 0x%08xu);\n",
            radius, radius, radius, radius, bg);
    if (bord_w > 0 && (bord & 0xff000000u))
        fprintf(o, "    fill_rect_rounded_border(sl->px, w->w, w->h, 0, 0, w->w, w->h,"
                   " %d, %d, %d, %d, %d, 1, 1, 1, 1, 0, 0x%08xu);\n",
                radius, radius, radius, radius, bord_w, bord);
    fprintf(o, "    const Font *f = &font_%d;\n",
            eval_int(surface_prop(sur, "font_size"), 14));
    fprintf(o, "    int __cox = %d, __coy = %d, __cws = w->w - %d, __chs = w->h - %d;\n",
            pad_x, pad_y, 2 * pad_x, 2 * pad_y);
    fputs("    (void)__cox; (void)__coy; (void)__cws; (void)__chs;\n", o);
    fputs("    int __clip_top = 0; (void)__clip_top;\n", o);
    /* Vertical draw uses the row band's x extent, horizontal the y extent;
     * for a menu both are the padded content box. */
    fputs("    int __reg_x = __cox, __reg_w = __cws; (void)__reg_x; (void)__reg_w;\n", o);
    fputs("    int __reg_y = __coy, __reg_h = __chs; (void)__reg_y; (void)__reg_h;\n", o);
    if (vert)
        fputs("    int y = __coy; (void)y;\n", o);
    else /* text baseline cursor for horizontally-laid items */
        fputs("    int y = (__chs - f->line_h) / 2 + __coy; (void)y;\n", o);
    fprintf(o,
        "    struct { int tw, vis; uint32_t cp, fg, bg, border, press_bg; const uint32_t *pm; int pms; const char *txt; int pad, align; int h; int ch; int body_lines; } st[%d];\n",
        n_arr);
    fprintf(o, "    for (int __i = 0; __i < %d; __i++) { st[__i].vis = 0; st[__i].h = 0; st[__i].ch = 0; st[__i].body_lines = 1; st[__i].border = 0; st[__i].press_bg = 0; }\n", n_arr);
    fputs("    (void)st;\n", o);
    fputs("    int center_total = 0;\n", o);
    fputs("    int __center_trail_pad = 0; (void)__center_trail_pad;\n", o);
    fprintf(o, "    int end_extent = %s;\n", vert ? "__chs" : "__cws");
    fprintf(o, "    __%s_nhit = 0;\n\n", nm);

    fputs("    /* --- measure pass --- */\n", o);
    for (int i = 0; i < nitems; i++) {
        emit_item_measure(o, &items[i], ctx, items[i].group_id >= 0 ? 0 : vert, nm, i);
        if (ctx->failed) return 1;
    }
    const char *org = vert ? "__coy" : "__cox";
    fputs("    if (center_total > __center_trail_pad) center_total -= __center_trail_pad;\n", o);
    fprintf(o, "    int start_pos = %s;\n", org);
    fprintf(o, "    int end_pos = end_extent + %s;\n", org);
    fprintf(o, "    int center_pos = (end_extent - center_total) / 2 + %s;\n", org);
    fputs("    (void)start_pos; (void)end_pos; (void)center_pos;\n", o);
    fputs("    /* --- draw pass --- */\n", o);
    for (int i = 0; i < nitems; i++) {
        if (items[i].group_id >= 0) {
            if (items[i].group_first)
                i += emit_group_draw(o, items, i, nitems, ctx, nm, vert) - 1;
            continue;
        }
        emit_item_draw(o, &items[i], ctx, vert, nm);
        if (ctx->failed) return 1;
    }
    emit_hit_snapshot(o, nm);
    fputs("    widget_attach(w, sl, 0);\n", o);
    fputs("}\n\n", o);

    pop_local(ctx);
    emit_surface_click_dispatch(o, items, nitems, ctx, ctx->r, nm);
    fprintf(o, "void %s_redraw(void) {\n"
               "    for (int i = 0; i < __%s_nw; i++) render_%s(__%s_widgets[i]);\n"
               "}\n\n", nm, nm, nm, nm);
    return 0;
}

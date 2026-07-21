/* wispc codegen — surface and compound emit (split from codegen.c). */
#include "codegen_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static void reg_collect(const char *base) {
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
static void emit_hit_store(FILE *o, const char *nm, int maxw) {
    fprintf(o, "static %s_Hit __%s_hit[%d][64]; static int __%s_hit_n[%d];\n",
            nm, nm, maxw, nm, maxw);
    fprintf(o, "static int __%s_slot(Widget *w) {\n"
               "    for (int i = 0; i < __%s_nw; i++) if (__%s_widgets[i] == w) return i;\n"
               "    return -1;\n"
               "}\n", nm, nm, nm);
}

/* Snapshot the render working buffer (__<nm>_hits_buf / __<nm>_nhit) into the
 * clicked widget's per-slot row. Emitted at the end of render_<nm>. */
static void emit_hit_snapshot(FILE *o, const char *nm) {
    fprintf(o, "    { int __wi = __%s_slot(w);\n"
               "      if (__wi >= 0) {\n"
               "          __%s_hit_n[__wi] = __%s_nhit;\n"
               "          for (int i = 0; i < __%s_nhit; i++) __%s_hit[__wi][i] = __%s_hits_buf[i];\n"
               "      } }\n",
            nm, nm, nm, nm, nm, nm);
}

/* The renderer for a `spawned_by = osd` template: iterates the slab ring,
 * pushes the per-slab $-bindings (summary/body/icon/pct/muted/...) and runs
 * each declared widget through emit_item_measure/emit_item_draw. Emitted as
 * `render_<nm>(Widget *w)`; osd_render() calls it. The runtime owns time
 * (osd_slab_geom) and the bar junction (osd_bar_split); everything else about
 * where a pixel lands comes from the declaration. */
int emit_spawned_osd_skeleton(FILE *o, Decl *sur, CGCtx *ctx, const char *nm) {
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
    /* Corner rounds only where the chain faces the desktop on both its edges. */
    int r_tl = (!fl_t && !fl_l) ? slab_radius : 0;
    int r_tr = (!fl_t && !fl_r) ? slab_radius : 0;
    int r_br = (!fl_b && !fl_r) ? slab_radius : 0;
    int r_bl = (!fl_b && !fl_l) ? slab_radius : 0;

    fprintf(o, "void render_%s(Widget *w) {\n", nm);
    fputs("    if (!w->configured || w->w <= 0 || w->h <= 0) return;\n", o);
    /* Nothing left in the ring: hand back a transparent buffer (never NULL —
     * see osd_attach_empty) and let the pool go, so idle costs nothing. */
    fputs("    if (!w->s.osd.items[0].active) {\n"
          "        if (w->s.osd.has_pixels) osd_stack_attach_empty(w);\n"
          "        return;\n    }\n", o);
    fputs("    int __n = osd_slab_layout(w);\n", o);
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
    fprintf(o, "    int __split = %s;\n", fl_t ? "osd_bar_split()" : "0");
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
        fprintf(o, "        int __reg_x = __chain_x, __reg_y = %d + __g.y + __slide,"
                   " __reg_w = __slab_w, __reg_h = (__sl == __first) ? __g.sh : __g.vh;\n", slab_margin);
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
    if (fillet_r > 0 && fl_t && !fl_l && !fl_r) {
        /* Flush under a bar: the armpits sit on the junction line and grow
         * with the body's emerged-past-bar extent, so the chain reads as one
         * shape with the bar instead of butting into it. Two regimes — a
         * spawning chain ramps the arc proportionally across the whole reveal,
         * a closing one holds it at full R until the chain has less than R
         * left, so slab[0] sliding away over a full slab[1] doesn't snap.
         * ponytail: emerged is the chain extent incl. gaps; both presets run
         * gap = 0. Subtract the gaps if one ever doesn't. */
        fprintf(o, "    if (__first >= 0) { int __em = __cbot - __split; if (__em < 0) __em = 0;\n"
                   "        OsdSlabGeom __lg; osd_slab_geom(w, __first, &__lg);\n"
                   "        int __fr;\n"
                   "        if (__lg.closing) { __fr = __em < %d ? __em : %d; }\n"
                   "        else { int __bd = __lg.sh - __split; if (__bd < 1) __bd = 1;\n"
                   "               __fr = %d * __em / __bd; if (__fr > %d) __fr = %d; }\n"
                   "        if (__fr > 0) {\n"
                   "            fill_corner_fillet(sl->px, w->w, w->h, __chain_x, __split, __fr, 0, __ctop_bg);\n"
                   "            fill_corner_fillet(sl->px, w->w, w->h, __chain_x + __slab_w, __split, __fr, 1, __ctop_bg);\n"
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
/* Does this surface declare anything to draw (as opposed to only props)? */
static int surface_has_body(Decl *d) {
    for (int i = 0; i < d->surface.n; i++)
        if (d->surface.items[i].kind != SB_PROP) return 1;
    return 0;
}

int emit_menu_render(FILE *o, Decl *sur, CGCtx *ctx, const char *nm) {
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
                             items[i].is_runtime_for_cell ? items[i].runtime_for_cap : 1);
    }
    for (int i = 0; i < nitems; i++) {
        if (!widget_has_vis_anim(items[i].w)) continue;
        fprintf(o, "static VisSlot %s_vis%d[%d];\n", nm, i,
                items[i].is_runtime_for_cell ? items[i].runtime_for_cap : 1);
    }
    fputs("#endif\n", o);

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
    /* Vertical draw uses the row band's x extent; for a menu that is the
     * padded content box. */
    fputs("    int __reg_x = __cox, __reg_w = __cws; (void)__reg_x; (void)__reg_w;\n", o);
    fputs("    int y = __coy; (void)y;\n", o);
    fprintf(o,
        "    struct { int tw, vis; uint32_t cp, fg, bg, border, press_bg; const uint32_t *pm; int pms; const char *txt; int pad, align; int h; int ch; int body_lines; } st[%d];\n",
        n_arr);
    fprintf(o, "    for (int __i = 0; __i < %d; __i++) { st[__i].vis = 0; st[__i].h = 0; st[__i].ch = 0; st[__i].body_lines = 1; st[__i].border = 0; st[__i].press_bg = 0; }\n", n_arr);
    fputs("    (void)st;\n", o);
    fputs("    int center_total = 0;\n", o);
    fputs("    int __center_trail_pad = 0; (void)__center_trail_pad;\n", o);
    fputs("    int end_extent = __chs;\n", o);
    fprintf(o, "    __%s_nhit = 0;\n\n", nm);

    fputs("    /* --- measure pass --- */\n", o);
    for (int i = 0; i < nitems; i++) {
        emit_item_measure(o, &items[i], ctx, items[i].group_id >= 0 ? 0 : 1, nm, i);
        if (ctx->failed) return 1;
    }
    fputs("    if (center_total > __center_trail_pad) center_total -= __center_trail_pad;\n", o);
    fputs("    int start_pos = __coy;\n", o);
    fputs("    int end_pos = end_extent + __coy;\n", o);
    fputs("    int center_pos = (end_extent - center_total) / 2 + __coy;\n", o);
    fputs("    (void)start_pos; (void)end_pos; (void)center_pos;\n", o);
    fputs("    /* --- draw pass --- */\n", o);
    for (int i = 0; i < nitems; i++) {
        if (items[i].group_id >= 0) {
            if (items[i].group_first)
                i += emit_group_draw(o, items, i, nitems, ctx, nm, 1) - 1;
            continue;
        }
        emit_item_draw(o, &items[i], ctx, 1 /*vertical*/, nm);
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
    /* For each fillet at corner (TL/TR/BR/BL) we check whether its wedge box
     * uses the LEFT/RIGHT/TOP/BOTTOM pad strip — depends on cid AND corner.
     *   LEFT  pad iff TL or BL with cid in {0,3}
     *   RIGHT pad iff TR or BR with cid in {1,2}
     *   TOP   pad iff TL or TR with cid in {2,3}
     *   BOT   pad iff BL or BR with cid in {0,1} */
    #define USES_LEFT(cid)  ((cid)==0 || (cid)==3)
    #define USES_RIGHT(cid) ((cid)==1 || (cid)==2)
    #define USES_TOP(cid)   ((cid)==2 || (cid)==3)
    #define USES_BOT(cid)   ((cid)==0 || (cid)==1)

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
        emit_item_slot_decls(o, items[i].w, nm, i, slots);
    }
    /* Step 6.3: per-item VisSlot for enter/exit animations on `visible`. */
    for (int i = 0; i < nitems; i++) {
        if (!widget_has_vis_anim(items[i].w)) continue;
        int slots = items[i].is_runtime_for_cell ? items[i].runtime_for_cap : 1;
        fprintf(o, "static VisSlot %s_vis%d[%d];\n", nm, i, slots);
    }
    fputs("#endif\n", o);

    fprintf(o, "void render_%s(Widget *w) {\n", nm);
    fputs("    if (!w->configured || w->w <= 0 || w->h <= 0) return;\n", o);
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
        fputs("    int y = (__chs - f->line_h) / 2 + __coy;\n", o);
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
    fprintf(o, "    widget_setup_surface(w, %d, \"wisp-%s\", o);\n", layer, nm);
    int __buf_w = 0, __buf_h = 0; /* exported to hud_register below */
    {
        /* HUD reserves the `reveal_gutter` band ON THE SLIDE AXIS, above/beside
         * the body. The trigger strip needs no buffer of its own: the slide is
         * render-only (the layer surface stays anchored), so the input region is
         * addressable whether or not the gutter reserves pixels for it. */
        int axis_is_x = (is_hud && (anchor == 4 || anchor == 8));
        int total_w = axis_is_x ? (width + gut_g) : width;
        int total_h = (is_hud && !axis_is_x) ? (height + gutter_top + gutter_bottom) : (height + armpit);
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
            switch (anchor) {
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
        int tl_l = USES_LEFT(cg_cid_tl)  ? ftl : 0;
        int bl_l = USES_LEFT(cg_cid_bl)  ? fbl : 0;
        int tr_r = USES_RIGHT(cg_cid_tr) ? ftr : 0;
        int br_r = USES_RIGHT(cg_cid_br) ? fbr : 0;
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
        int lr_pad_top = (anchor == 4 || anchor == 8) ? __MX(lit, lot) : 0;
        int lr_pad_bot = (anchor == 4 || anchor == 8) ? __MX(lib, lob) : 0;
#undef __MX
        __buf_w = total_w + pad_l + pad_r;
        __buf_h = total_h + lr_pad_top + lr_pad_bot;
        fprintf(o, "    widget_set_size(w, %d, %d);\n", __buf_w, __buf_h);
    }
    fprintf(o, "    widget_set_anchor(w, %d);\n", anchor);
    if (is_hud) {
        /* HUD: default exclusive_zone=-1 (ignore other surfaces' exclusive
         * zones so the trigger gutter overlaps the bar). DSL can override
         * with `exclusive_zone = 0` to instead respect other zones — useful
         * for top-corner widgets that should sit *below* the bar. */
        int hud_excl = surface_prop(sur, "exclusive_zone") ? excl_zone : -1;
        fprintf(o, "    widget_set_exclusive_zone(w, %d);\n", hud_excl);
        fputs("    widget_set_kbd_interactive(w, 0);\n", o);
        /* hud_register's full_w/full_h must match the SHM buffer dimensions
         * so slide_extent, trigger input region, and the emerged calc all
         * agree with the rendered geometry. */
        fprintf(o, "    hud_register(w, %d, %d, %d, %d);\n", anchor, reveal_g, __buf_w, __buf_h);
        int reveal_ms = eval_int(surface_prop(sur, "reveal_anim_ms"), 0);
        if (reveal_ms > 0)
            fprintf(o, "    hud_set_reveal_anim(w, %d, %s);\n",
                    reveal_ms, surface_easing_id(sur, "reveal_easing"));
        fputs("    wl_req(w->surface, SURFACE_REQ_COMMIT, NULL, 0, -1);\n", o);
    } else {
        if (margin > 0)
            fprintf(o, "    widget_set_margin(w, %d, %d, %d, %d);\n", margin, margin, margin, margin);
        fprintf(o, "    widget_set_exclusive_zone(w, %d);\n", excl_zone);
        fputs("    widget_set_kbd_interactive(w, 0);\n", o);
        Expr *inp = surface_prop(sur, "input");
        int input_none = inp && inp->kind == EX_IDENT && inp->ident.n == 4
                         && memcmp(inp->ident.s, "none", 4) == 0;
        if (input_none) {
            /* `input = none;` → empty input region: pointer events fall through
             * to whatever is beneath (used by the decorative screen-corner
             * strip so it never steals clicks from windows/desktop). */
            fputs("    widget_set_input_region_rect(w, 0, 0, 0, 0);\n", o);
        } else if (armpit > 0) {
            /* Constrain pointer input to the painted body. Without this the
             * default full-surface region would let the transparent feet strip
             * (the extra `armpit` rows on the desktop side) intercept clicks
             * across the whole width. A region wider than the surface is clipped
             * by the compositor, so 1<<15 safely spans any output. */
            fprintf(o, "    widget_set_input_region_rect(w, 0, %d, %d, %d);\n",
                    (anchor & 1) ? 0 : armpit, 1 << 15, height);
        }
        fputs("    wl_req(w->surface, SURFACE_REQ_COMMIT, NULL, 0, -1);\n", o);
    }
    fputs("}\n\n", o);

    return 0;
}

/* ============================================================ */
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
            emit_item_slot_decls(o, it->w, rnm, n_all + i, slots);
        }
        for (int i = 0; i < got; i++) {
            BarItem *it = &all_items[n_all + i];
            if (!widget_has_vis_anim(it->w)) continue;
            int slots = it->is_runtime_for_cell ? it->runtime_for_cap : 1;
            fprintf(o, "static VisSlot %s_vis%d[%d];\n", rnm, n_all + i, slots);
        }
        fputs("#endif\n", o);

        /* render_<rnm>(Widget *w): paint just this region. */
        fprintf(o, "void render_%s(Widget *w) {\n", rnm);
        fputs("    if (!w->configured || w->w <= 0 || w->h <= 0) return;\n", o);
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
        if (mt || mb)
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
    n_reg_names = 0;
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
            if (is_osd) {
                char *nm = strndup0(d->name, d->nlen);
                emit_spawned_osd_skeleton(o, d, ctx, nm);
                free(nm);
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
            /* An item-only menu (no widget body) stays on the legacy
             * menu.c render path until every preset is ported. */
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

    /* Prune a destroyed widget out of every surface/region registry. Called by
     * widget.c:widget_destroy() (weak symbol). Swap-remove keeps each array
     * dense so the cap (8/32/4) is never hit by stale entries on monitor
     * hotplug, and so redraw/tag fanouts never touch a freed widget. */
    fputs("void wispgen_widget_destroyed(Widget *w) {\n", o);
    for (int i = 0; i < n_reg_names; i++)
        fprintf(o, "    for (int i = 0; i < __%s_nw; i++) if (__%s_widgets[i] == w) { __%s_widgets[i] = __%s_widgets[--__%s_nw]; break; }\n",
                reg_names[i], reg_names[i], reg_names[i], reg_names[i], reg_names[i]);
    fputs("}\n", o);
    return 0;
}


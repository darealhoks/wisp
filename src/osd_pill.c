/* Pct pill — minimal {icon}{slider} OSD for progress-only posts (volume /
 * brightness), top-centered so it reads as a bar module. #included at the end
 * of osd.c (single TU); enabled by `pill_w > 0` on the DSL osd surface. */

#if OSD_PILL_W > 0

static Widget *pill_widget(void) {
    for (int i = 0; i < MAX_WIDGETS; i++)
        if (widgets[i].kind == W_OSD && widgets[i].s.osd.is_pill)
            return &widgets[i];
    return NULL;
}

static Widget *pill_make_on(Output *o) {
    Widget *w = widget_alloc(W_OSD);
    if (!w) { msg("wisp: no widget slot for OSD pill"); return NULL; }
    w->s.osd.is_pill = 1;
    w->w = OSD_PILL_W;
    /* pill_margin is baked into the buffer (body drawn OSD_PILL_MARGIN below
     * the buffer top) instead of a layer-surface margin: a margin puts the
     * buffer top BELOW the screen edge, so the slide tween clipped there —
     * the pill visibly vanished pill_margin px before reaching the edge. */
    w->h = OSD_PILL_H + OSD_PILL_MARGIN;
    widget_setup_surface(w, LAYER_OVERLAY, "wisp-osd-pill", o);
    widget_set_size(w, w->w, w->h);
    /* Anchor TOP → compositor centers horizontally. exclusive_zone=-1 ignores
     * the bar's zone so the pill sits IN the bar row. */
    widget_set_anchor(w, LS_ANCHOR_TOP);
    widget_set_margin(w, 0, 0, 0, 0);
    widget_set_exclusive_zone(w, -1);
    widget_set_kbd_interactive(w, 0);
    widget_set_input_region_rect(w, 0, 0, 0, 0);
    wl_req(w->surface, SURFACE_REQ_COMMIT, NULL, 0, -1);
    return w;
}

static void osd_pill_render(Widget *w) {
    if (!w->configured) return;
    Osd *o = &w->s.osd.items[0];
    if (!o->active && !w->s.osd.has_pixels) return;
    if (!o->active) { osd_attach_empty(w); return; }

    int64_t now = now_ms();
    double p = slab_anim_p(o, now);
    int W = w->w, H = w->h;
    int vh = (int)((double)H * p);
    if (vh < 0) vh = 0;
    if (vh > H) vh = H;

    update_input_region(w);
    widget_ensure_pool(w, 2);
    BufSlot *s = widget_free_slot(w);
    if (!s) return;
    memset(s->px, 0, (size_t)W * H * 4);

    /* Body slides down into place: drawn at its full size, offset up by the
     * not-yet-emerged remainder and clipped by the buffer top. vh sweeps the
     * whole buffer (body + baked-in top margin) so p=1 rests the body at
     * y=OSD_PILL_MARGIN and p=0 hides it fully above the screen edge. */
    int BH = OSD_PILL_H;
    int y = vh - BH;
    uint32_t bg  = o->muted == 2 ? OSD_BG_WARN : OSD_BG;
    uint32_t fg  = o->muted == 2 ? OSD_FG_WARN : OSD_FG;
    uint32_t pfg = o->muted == 2 ? OSD_PROG_FG_WARN
                 : o->muted == 1 ? OSD_PROG_FG_MUTE : OSD_PROG_FG;
    int r = OSD_PILL_RADIUS;
    fill_rect_rounded(s->px, W, H, 0, y, W, BH, r, r, r, r, bg);
    if (OSD_BORDER_W > 0)
        fill_rect_rounded_border(s->px, W, H, 0, y, W, BH, r, r, r, r,
                                 OSD_BORDER_W, 1, 1, 1, 1, 0, OSD_BORDER);

    int tx = OSD_PAD_X;
    if (o->icon_cp) {
        /* Pill icon: a step smaller than font_large + extra gap, else the
         * glyph's right edge crowds the slider track. */
        const Glyph *g = font_find(&font_20, o->icon_cp);
        if (g) {
            int gy = y + (BH - g->h) / 2 + g->by;
            draw_glyph(s->px, W, H, tx - g->bx, gy, &font_20, g, fg);
            tx += g->adv + OSD_ICON_GAP + 6;
        }
    }

    /* Slider: rounded track filling the rest of the row. */
    int bx = tx, bw = W - OSD_PAD_X - bx;
    if (bw > 0) {
        int by = y + (BH - OSD_PROG_H) / 2;
        int pr = OSD_PROG_H / 2;
        fill_rounded_clipped(s->px, W, H, bx, by, bw, OSD_PROG_H,
                             pr, pr, pr, pr, bx, by, bw, OSD_PROG_H,
                             pr, pr, pr, pr, OSD_PROG_TRACK_BG);
        int pmax = o->progress > 100 ? o->progress : 100;
        int pw = bw * o->progress / pmax;
        if (pw > bw) pw = bw;
        if (pw > 0)
            fill_rounded_clipped(s->px, W, H, bx, by, pw, OSD_PROG_H,
                                 pr, pr, pr, pr, bx, by, bw, OSD_PROG_H,
                                 pr, pr, pr, pr, pfg);
    }

    w->s.osd.has_pixels = 1;
    int anim_pending =
        (o->closing_at_ms > 0 && now < o->closing_at_ms + OSD_SLIDE_MS) ||
        (o->closing_at_ms == 0 && o->spawn_ms > 0 && now < o->spawn_ms + OSD_SLIDE_MS);
    widget_attach(w, s, anim_pending);
}

static uint32_t pill_post(uint32_t icon_cp, int progress, int muted, int timeout_ms) {
    Output *target = focused_output;
    if (!target)
        for (int i = 0; i < MAX_OUTPUTS; i++)
            if (outputs[i].active) { target = &outputs[i]; break; }
    if (!target) return 0;
    Widget *w = pill_widget();
    if (w && OSD_FOCUS_FOLLOW && w->output != target) { widget_destroy(w); w = NULL; }
    if (!w) w = pill_make_on(target);
    if (!w) return 0;

    Osd *o = &w->s.osd.items[0];
    int64_t now = now_ms();
    /* Same hammering rule as the stack: a replace while settled must not
     * restart the slide-in. */
    int64_t prev_spawn = (o->active && !o->closing_at_ms) ? o->spawn_ms : 0;
    memset(o, 0, sizeof *o);
    o->active     = 1;
    o->spawn_ms   = prev_spawn ? prev_spawn : now;
    o->replace_id = ++w->s.osd.next_id;
    o->icon_cp    = icon_cp;
    o->progress   = progress;
    o->muted      = muted;
    o->expires_at_ms = now + (timeout_ms > 0 ? timeout_ms : OSD_TIMEOUT_OSD);
    osd_pill_render(w);
    return o->replace_id;
}

#endif /* OSD_PILL_W > 0 */

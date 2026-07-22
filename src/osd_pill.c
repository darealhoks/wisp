/* Pct pill — the minimal OSD progress-only posts (volume / brightness) route
 * to instead of the notification stack. What it looks like is declared: a
 * `surface pill { spawned_by = osd_pill; … }` lowers to render_pill(). This
 * file owns only what the DSL can't say — the widget, its buffer geometry and
 * the post/replace rule. #included at the end of osd.c (single TU); enabled by
 * OSD_PILL_W > 0, i.e. by the pill surface existing. */

#if OSD_PILL_W > 0

extern void render_pill(Widget *w);

/* pill_margin <= 0 → flush against the bar like the stack: buffer grows
 * fillet_r either side plus the bar row on top, and fillet wedges claw into
 * the bar at its bottom edge. 0 rests the body exactly at the junction
 * (square top, emerges from under the bar); negative rests it that many px
 * INSIDE the bar row — straddling the edge to conserve screen space. */
#define PILL_FR (OSD_PILL_MARGIN <= 0 ? OSD_PILL_FILLET_R : 0)

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
    w->w = OSD_PILL_W + 2 * PILL_FR;
    /* pill_margin is baked into the buffer (osd_pill_layout's vh sweeps the
     * whole height) instead of being a layer-surface margin: a margin puts the
     * buffer top BELOW the screen edge, so the slide tween clipped there —
     * the pill visibly vanished pill_margin px before reaching the edge.
     * Flush mode swaps that inset for the bar row the fillets claw into
     * (minus however far a negative margin tucks the body into the bar). */
    w->h = OSD_PILL_H + OSD_PILL_MARGIN + (PILL_FR ? osd_bar_split() : 0);
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
    render_pill(w);
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

/* Floating top-center OSD notification stack — #included at the end of
 * osd.c (same TU). Selected by OSD_FLOAT (margin > 0, anchor top): the chain
 * rests OSD_TOP_MARGIN below the bar's exclusive zone and slides down out
 * from under the bar, dressed like a bar module (rounded pill + border) —
 * none of the flush-bar fillet/cutout machinery applies. */

#if OSD_FLOAT
static void osd_render_float(Widget *w) {
    if (!w->configured) return;
    int has_items = w->s.osd.items[0].active;
    if (!has_items && !w->s.osd.has_pixels) return;
    if (!has_items) { osd_attach_empty(w); return; }

    const Font *f = &font_small;
    int64_t now = now_ms();
    int W = w->w, H = w->h;

    /* Pass 1: wrap bodies, per-slab heights + tween progress; top-down Y
     * from animated visible heights (mirrors osd_render_top). */
    static char wrapped[MAX_OSDS][OSD_MAX_BODY_LINES][OSD_BODY_MAX];
    int nbody[MAX_OSDS] = {0}, item_y[MAX_OSDS] = {0}, visible_h[MAX_OSDS] = {0};
    double anim_p_arr[MAX_OSDS] = {0};
    int total = 0, n_active = 0;
    for (int i = 0; i < MAX_OSDS; i++) {
        Osd *o = &w->s.osd.items[i];
        if (!o->active) break;
        int tx = slab_text_x(o);
        int body_w = OSD_W - OSD_PAD_X - tx;
        if (body_w < 0) body_w = 0;
        if (o->body[0])
            nbody[i] = wrap_body(f, o->body, body_w, OSD_MAX_BODY_LINES, wrapped[i]);
        o->h = slab_height_for(nbody[i], o->progress >= 0);
        anim_p_arr[i] = slab_anim_p(o, now);
        int vh = (int)((double)o->h * anim_p_arr[i]);
        if (vh < 0) vh = 0;
        if (vh > o->h) vh = o->h;
        visible_h[i] = vh;
        if (n_active > 0 && visible_h[i-1] > 0) total += OSD_GAP;
        item_y[i] = total;
        total += vh;
        n_active++;
    }
    update_input_region(w);

    widget_ensure_pool(w, 2);
    BufSlot *s = widget_free_slot(w);
    if (!s) return;
    memset(s->px, 0, (size_t)W * H * 4);

    int first = -1, last = -1;
    for (int i = 0; i < n_active; i++)
        if (visible_h[i] > 0) { if (first < 0) first = i; last = i; }

    /* Chain extent for the shared border pass; seeded while drawing. */
    int chain_top = H, chain_bot = 0;

    for (int i = 0; i < n_active; i++) {
        Osd *o = &w->s.osd.items[i];
        int sh = o->h, vh = visible_h[i];
        if (vh <= 0) continue;
        uint32_t bg = o->muted == 2 ? OSD_BG_WARN : OSD_BG;
        uint32_t fg = o->muted == 2 ? OSD_FG_WARN : OSD_FG;
        uint32_t pfg = o->muted == 2 ? OSD_PROG_FG_WARN
                     : o->muted == 1 ? OSD_PROG_FG_MUTE : OSD_PROG_FG;

        /* The leading slab slides down: drawn full-height, offset up by the
         * not-yet-emerged remainder, clipped by the buffer top (same trick
         * as the pct pill). Lower slabs grow in place. */
        int slide_off = (i == first) ? (vh - sh) : 0;
        int y  = OSD_TOP_MARGIN + item_y[i] + slide_off;
        int dh = (i == first) ? sh : vh;

        /* One pill: round only the chain's outer corners. */
        int r_t = (i == first) ? OSD_RADIUS : 0;
        int r_b = (i == last)  ? OSD_RADIUS : 0;
        fill_rect_rounded(s->px, W, H, 0, y, OSD_W, dh, r_t, r_t, r_b, r_b, bg);
        if (y < chain_top) chain_top = y;
        if (y + dh > chain_bot) chain_bot = y + dh;

        if (i > first && anim_p_arr[i] >= 1.0 && anim_p_arr[i-1] >= 1.0) {
            int sep_w = OSD_W * OSD_SEPARATOR_FRAC / 100;
            fill_rect(s->px, W, H, (OSD_W - sep_w) / 2, y, sep_w, 1, OSD_SEPARATOR);
        }

        char pct_buf[16] = "";
        int pct_w = 0;
        if (o->progress >= 0) {
            snprintf(pct_buf, sizeof pct_buf, "%d%%", o->progress);
            pct_w = text_width(f, pct_buf);
        }
        int tx = slab_text_x(o);
        int prog_band = (o->progress >= 0) ? OSD_PROG_H + 8 : 0;
        int content_h = sh - prog_band;
        int body_w = OSD_W - OSD_PAD_X - tx;
        if (body_w < 0) body_w = 0;

        if (o->icon_cp) {
            const Glyph *g = font_find(&font_large, o->icon_cp);
            if (g) {
                int gy = y + (content_h - g->h) / 2 + g->by;
                draw_glyph(s->px, W, H, OSD_PAD_X - g->bx, gy, &font_large, g, fg);
            }
        }
        if (nbody[i] > 0) {
            int total_lines = 1 + nbody[i];
            int ty = y + (content_h - total_lines * f->line_h) / 2;
            draw_elided(s->px, W, H, tx, ty, f, o->summary, body_w, fg);
            for (int li = 0; li < nbody[i]; li++)
                draw_text(s->px, W, H, tx, ty + (li + 1) * f->line_h, f, wrapped[i][li], fg);
        } else {
            int ty = y + (content_h - f->line_h) / 2;
            int sum_w = body_w - (pct_w ? pct_w + OSD_ICON_GAP : 0);
            if (sum_w < 0) sum_w = 0;
            draw_elided(s->px, W, H, tx, ty, f, o->summary, sum_w, fg);
            if (pct_w)
                draw_text(s->px, W, H, OSD_W - OSD_PAD_X - pct_w, ty, f, pct_buf, fg);
        }
        if (o->progress >= 0) {
            int by = y + sh - 2 - OSD_PROG_H - 4;
            int bx = OSD_PAD_X;
            int bw = OSD_W - 2 * OSD_PAD_X;
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
    }

    /* Outline the whole chain as one pill (mid-slide overflow past the
     * buffer top is clipped by the primitives). */
    if (OSD_BORDER_W > 0 && chain_bot > chain_top)
        fill_rect_rounded_border(s->px, W, H, 0, chain_top, OSD_W,
                                 chain_bot - chain_top,
                                 OSD_RADIUS, OSD_RADIUS, OSD_RADIUS, OSD_RADIUS,
                                 OSD_BORDER_W, 1, 1, 1, 1, 0, OSD_BORDER);

    w->s.osd.has_pixels = 1;
    int anim_pending = 0;
    for (int i = 0; i < n_active; i++) {
        Osd *o = &w->s.osd.items[i];
        if (o->closing_at_ms > 0 && now < o->closing_at_ms + OSD_SLIDE_MS) { anim_pending = 1; break; }
        if (o->closing_at_ms == 0 && o->spawn_ms > 0 && now < o->spawn_ms + OSD_SLIDE_MS) { anim_pending = 1; break; }
    }
    widget_attach(w, s, anim_pending);
}
#endif /* OSD_FLOAT */

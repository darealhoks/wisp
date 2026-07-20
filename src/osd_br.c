/* Bottom-right OSD notification stack renderer — #included at the end of the
 * shared prologue in osd.c (same TU); see osd.c for the top-anchored variant. */

#if OSD_ANCHOR_BR
/* Bottom-right notification stack: body flush against the bottom and right
 * screen edges, newest slab nearest the corner, older slabs pushed up.
 * Fillets flare the chain into the two screen edges (top-right along the
 * right edge, bottom-left along the bottom edge). No bar cutout, no top
 * inset — none of the flush-bar machinery applies here.
 * ponytail: margin under the chain isn't supported (body assumes flush
 * edges for the fillet geometry); add OSD_BR_MARGIN when a preset wants a
 * floating corner stack. */
static void osd_render_br(Widget *w) {
    if (!w->configured) return;
    int has_items = w->s.osd.items[0].active;
    if (!has_items && !w->s.osd.has_pixels) return;
    if (!has_items) { osd_attach_empty(w); return; }

    const Font *f = &font_small;
    int64_t now = now_ms();
    int W = w->w, H = w->h;
    int body_x = W - OSD_W;

    /* Pass 1: wrap bodies, per-slab heights + tween progress. */
    static char wrapped[MAX_OSDS][OSD_MAX_BODY_LINES][OSD_BODY_MAX];
    int nbody[MAX_OSDS] = {0}, visible_h[MAX_OSDS] = {0};
    double anim_p_arr[MAX_OSDS] = {0};
    int n_active = 0;
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
        n_active++;
    }
    update_input_region(w);

    /* Layout bottom-up: items[n-1] (newest) sits in the corner; each older
     * slab stacks above the one after it. */
    int y_top[MAX_OSDS];
    int off = 0;
    for (int i = n_active - 1; i >= 0; i--) {
        if (visible_h[i] > 0) {
            if (off > 0) off += OSD_GAP;
            off += visible_h[i];
        }
        y_top[i] = H - off;
    }
    int chain_top = H - off, chain_h = off;

    widget_ensure_pool(w, 2);
    BufSlot *s = widget_free_slot(w);
    if (!s) return;
    memset(s->px, 0, (size_t)W * H * 4);

    int topmost = -1;
    for (int i = 0; i < n_active; i++) if (visible_h[i] > 0) { topmost = i; break; }
    for (int i = 0; i < n_active; i++) {
        Osd *o = &w->s.osd.items[i];
        int y = y_top[i], vh = visible_h[i];
        if (vh <= 0) continue;
        uint32_t bg = o->muted == 2 ? OSD_BG_WARN : OSD_BG;
        uint32_t fg = o->muted == 2 ? OSD_FG_WARN : OSD_FG;
        uint32_t pfg = o->muted == 2 ? OSD_PROG_FG_WARN
                     : o->muted == 1 ? OSD_PROG_FG_MUTE : OSD_PROG_FG;

        /* Chain reads as one body: only the topmost slab rounds its
         * desktop-facing top-left corner; right/bottom sit on screen edges. */
        int r_tl = (i == topmost) ? OSD_RADIUS : 0;
        fill_rect_rounded(s->px, W, H, body_x, y, OSD_W, vh, r_tl, 0, 0, 0, bg);

        if (i > topmost && anim_p_arr[i] >= 1.0 && anim_p_arr[i-1] >= 1.0) {
            int sep_w = OSD_W * OSD_SEPARATOR_FRAC / 100;
            int sep_x = body_x + (OSD_W - sep_w) / 2;
            fill_rect(s->px, W, H, sep_x, y, sep_w, 1, OSD_SEPARATOR);
        }

        /* Content rides the slab's rising top edge (y already tracks vh), so
         * mid-slide it emerges from the screen bottom with the body; overflow
         * past the buffer is clipped by the primitives. */
        char pct_buf[16] = "";
        int pct_w = 0;
        if (o->progress >= 0) {
            snprintf(pct_buf, sizeof pct_buf, "%d%%", o->progress);
            pct_w = text_width(f, pct_buf);
        }
        int tx = body_x + slab_text_x(o);
        int prog_band = (o->progress >= 0) ? OSD_PROG_H + 8 : 0;
        int content_h = o->h - prog_band;
        int body_w = OSD_W - OSD_PAD_X - slab_text_x(o);
        if (body_w < 0) body_w = 0;

        if (o->icon_cp) {
            const Glyph *g = font_find(&font_large, o->icon_cp);
            if (g) {
                int gy = y + (content_h - g->h) / 2 + g->by;
                draw_glyph(s->px, W, H, body_x + OSD_PAD_X - g->bx, gy, &font_large, g, fg);
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
                draw_text(s->px, W, H, body_x + OSD_W - OSD_PAD_X - pct_w, ty, f, pct_buf, fg);
        }
        if (o->progress >= 0) {
            int by = y + o->h - 2 - OSD_PROG_H - 4;
            int bx = body_x + OSD_PAD_X;
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

    if (chain_h > 0) {
        uint32_t top_bg, bot_bg;
        {
            Osd *ot = &w->s.osd.items[topmost < 0 ? 0 : topmost];
            Osd *ob = &w->s.osd.items[n_active - 1];
            top_bg = ot->muted == 2 ? OSD_BG_WARN : OSD_BG;
            bot_bg = ob->muted == 2 ? OSD_BG_WARN : OSD_BG;
        }
        /* Outline the whole chain; the screen-edge sides stay open. */
        if (OSD_BORDER_W > 0)
            fill_rect_rounded_border(s->px, W, H, body_x, chain_top, OSD_W, chain_h,
                                     OSD_RADIUS, 0, 0, 0, OSD_BORDER_W,
                                     1, 0, 0, 1, 0, OSD_BORDER);
        /* Fillets flare the chain into the screen edges, ramping with the
         * emerged height so they never outsize the body. Both armpits lie
         * up-left of their corner point → corner_id 3. */
        int fr = chain_h < OSD_FILLET_R ? chain_h : OSD_FILLET_R;
        if (fr > 0) {
            fill_corner_fillet(s->px, W, H, W, chain_top, fr, 3, top_bg);
            fill_corner_fillet(s->px, W, H, body_x, H, fr, 3, bot_bg);
            if (OSD_BORDER_W > 0) {
                fill_corner_fillet_border(s->px, W, H, W, chain_top, fr, 3, OSD_BORDER_W, OSD_BORDER);
                fill_corner_fillet_border(s->px, W, H, body_x, H, fr, 3, OSD_BORDER_W, OSD_BORDER);
            }
        }
    }

    w->s.osd.has_pixels = 1;
    int anim_pending = 0;
    for (int i = 0; i < n_active; i++) {
        Osd *o = &w->s.osd.items[i];
        if (o->closing_at_ms > 0 && now < o->closing_at_ms + OSD_SLIDE_MS) { anim_pending = 1; break; }
        if (o->closing_at_ms == 0 && o->spawn_ms > 0 && now < o->spawn_ms + OSD_SLIDE_MS) { anim_pending = 1; break; }
    }
    widget_attach(w, s, anim_pending);
}
#endif /* OSD_ANCHOR_BR */

/* OSD / notification stack — replaces both mako (app notifications) and
 * dwl-osd (volume / brightness / mic sliders).
 *
 * One W_OSD widget; surface sized to hold MAX_OSDS stacked slabs vertically.
 * Active items pack to the top, inactive rows render transparent so the
 * unused area below is click-through. Replacement-by-id is the same idea as
 * mako's x-canonical-private-synchronous: a fresh post with a matching
 * `replace_id` overwrites the existing slot instead of stacking. */

#include "wisp.h"

#include <stdio.h>
#include <string.h>

/* DSL-driven behavior toggles (set via gen_overrides.h). Defaults preserve
 * the hand-written full preset's behavior. */
#ifndef OSD_FOCUS_FOLLOW
#define OSD_FOCUS_FOLLOW 1
#endif
#ifndef OSD_DBUS_CLOSE
#define OSD_DBUS_CLOSE 1
#endif
#ifndef OSD_DISMISS_ON_CLICK
#define OSD_DISMISS_ON_CLICK 1
#endif
/* Notification-stack anchor: 0 = top-center under a flush bar (fillets claw
 * into the bar, bar cutout). 1 = bottom-right screen corner (fillets flare
 * into the screen edges). Only these two layouts are wired; declare
 * `anchor = bottom | right` on the osd surface for the latter. */
#ifndef OSD_ANCHOR_BR
#define OSD_ANCHOR_BR 0
#endif
/* Slab/pill outline; 0 = no border (legacy presets set `border` color only). */
#ifndef OSD_BORDER_W
#define OSD_BORDER_W 0
#endif
/* Pct pill: minimal {icon}{slider} surface for progress-only posts (volume /
 * brightness), top-centered like a bar module. 0 = disabled → pct posts render
 * as normal slabs in the stack. */
#ifndef OSD_PILL_W
#define OSD_PILL_W 0
#endif
#ifndef OSD_PILL_H
#define OSD_PILL_H 36
#endif
#ifndef OSD_PILL_MARGIN
#define OSD_PILL_MARGIN 0
#endif
#ifndef OSD_PILL_RADIUS
#define OSD_PILL_RADIUS OSD_RADIUS
#endif
/* Floating top-center stack, OSD_TOP_MARGIN px below the bar's exclusive zone.
 * Like the bottom-right anchor it is fully declared now — the margin is what
 * lifts the chain off every screen edge, so nothing is flush and the generated
 * renderer rounds/outlines all four sides. Only selects the render path. */
#define OSD_FLOAT (!OSD_ANCHOR_BR && OSD_TOP_MARGIN > 0)

/* Vertical padding inside a slab (top + bottom) around the text block. */
#define OSD_SLAB_PAD_Y 24
/* Worst-case slab height: 1 summary line + OSD_MAX_BODY_LINES body lines +
 * progress band + padding. Surface is sized for the worst case so we never
 * have to renegotiate size with the compositor at runtime. */
#define OSD_PROG_BAND  (OSD_PROG_H + 8)
#define OSD_MAX_SLAB_H ((1 + OSD_MAX_BODY_LINES) * 17 + OSD_PROG_BAND + OSD_SLAB_PAD_Y)
/* Layout: the OSD surface respects the bar's exclusive zone (so dwl positions
 * the buffer's top at screen y=BAR_HEIGHT, immediately below the bar). Body
 * draws at buffer y=0, so it sits flush against the bar's bottom edge — no
 * gap to wallpaper between them. Horizontal side pad reserves OSD_FILLET_R px
 * for the convex top-corner arcs; anchor TOP centers the wider buffer so the
 * body's screen position is unchanged. */
#ifndef OSD_FILLET_R
#define OSD_FILLET_R   18
#endif
#define OSD_SIDE_PAD   OSD_FILLET_R
#define OSD_TOTAL_H    (MAX_OSDS * OSD_MAX_SLAB_H + (MAX_OSDS - 1) * OSD_GAP)
/* Surface extends UP into the bar's exclusive zone (exclusive_zone=-1) the
 * same way the HUD does — gives the OSD ownership of the bar-strip pixels
 * directly above the body so the fillets visually claw into the bar instead
 * of butting against it from outside the surface. Body content still starts
 * at screen y=BAR_HEIGHT (buffer y=OSD_TOP_INSET); the bar-strip is left
 * transparent except for the fillet wedges. */
#define OSD_TOP_INSET  BAR_HEIGHT
/* Slide-in animation duration (ms). Matches HUD's reveal_anim_ms. DSL knob
 * `slide_ms` on the `osd` surface template (in the .wisp configs) overrides this via
 * gen_overrides.h. */
#ifndef OSD_SLIDE_MS
#define OSD_SLIDE_MS    200
#endif

extern void dbus_emit_closed(uint32_t id, uint32_t reason) __attribute__((weak));

int dnd_on = 0;

static void update_input_region(Widget *w);
static void start_close(Osd *o, uint32_t reason);
#if OSD_PILL_W > 0
static Widget *pill_widget(void);
static uint32_t pill_post(uint32_t icon_cp, int progress, int muted, int timeout_ms);
static void osd_pill_render(Widget *w);
#endif

/* The notification stack's widget. Not widget_first(W_OSD): the pct pill is
 * a second W_OSD widget, distinguished by s.osd.is_pill. */
static Widget *osd_stack(void) {
    for (int i = 0; i < MAX_WIDGETS; i++)
        if (widgets[i].kind == W_OSD && !widgets[i].s.osd.is_pill)
            return &widgets[i];
    return NULL;
}

/* Walk the prefix of `s` whose pixel width fits `budget`, returning byte count. */
static int prefix_fitting(const Font *f, const char *s, int budget) {
    int n = 0, w = 0;
    while (s[n]) {
        uint32_t cp; int k = utf8_decode(s + n, &cp);
        if (!k) break;
        const Glyph *g = font_find(f, cp);
        int gw = g ? g->adv : f->px_size / 2;
        if (w + gw > budget) break;
        n += k; w += gw;
    }
    return n;
}

/* Word-wrap `s` into at most `max_lines` lines, each <= max_w pixels.
 * Final line gets an ellipsis only if input doesn't fit within max_lines. */
static int wrap_body(const Font *f, const char *s, int max_w, int max_lines,
                     char out[][OSD_BODY_MAX]) {
    int line = 0;
    int sp_w = text_width(f, " ");
    char cur[OSD_BODY_MAX] = ""; int cur_len = 0, cur_w = 0;

    while (*s && line < max_lines) {
        while (*s == ' ' || *s == '\t' || *s == '\n') s++;
        if (!*s) break;
        const char *we = s;
        while (*we && *we != ' ') we++;
        int wl = (int)(we - s);
        if (wl >= (int)sizeof cur) wl = sizeof cur - 1;
        char word[OSD_BODY_MAX];
        memcpy(word, s, wl); word[wl] = 0;
        int ww = text_width(f, word);
        int add_sp = cur_len > 0 ? sp_w : 0;
        if (cur_w + add_sp + ww > max_w) {
            if (cur_len == 0) {
                int cut = prefix_fitting(f, word, max_w);
                if (cut == 0) cut = 1;
                memcpy(out[line], word, cut); out[line][cut] = 0;
                line++;
                s += cut;
                continue;
            }
            memcpy(out[line], cur, cur_len); out[line][cur_len] = 0;
            line++; cur[0] = 0; cur_len = 0; cur_w = 0;
            continue;
        }
        if (add_sp) { cur[cur_len++] = ' '; cur[cur_len] = 0; cur_w += sp_w; }
        memcpy(cur + cur_len, word, wl); cur_len += wl; cur[cur_len] = 0;
        cur_w += ww;
        s = we;
    }
    if (cur_len > 0 && line < max_lines) {
        memcpy(out[line], cur, cur_len); out[line][cur_len] = 0;
        line++;
    }
    if (*s && line > 0) {
        char *last = out[line - 1];
        int ell_w = text_width(f, "\xe2\x80\xa6");
        int lw = text_width(f, last);
        while (lw + ell_w > max_w) {
            int len = (int)strlen(last);
            if (!len) break;
            int back = 1;
            while (back < len && (last[len - back] & 0xc0) == 0x80) back++;
            last[len - back] = 0;
            lw = text_width(f, last);
        }
        size_t ll = strlen(last);
        if (ll + 3 < OSD_BODY_MAX) { memcpy(last + ll, "\xe2\x80\xa6", 3); last[ll + 3] = 0; }
    }
    return line;
}

static int find_replace(Widget *w, uint32_t rid) {
    if (!rid) return -1;
    for (int i = 0; i < MAX_OSDS; i++)
        if (w->s.osd.items[i].active && w->s.osd.items[i].replace_id == rid)
            return i;
    return -1;
}
static int find_free(Widget *w) {
    for (int i = 0; i < MAX_OSDS; i++)
        if (!w->s.osd.items[i].active) return i;
    return -1;
}
/* Evict the soonest-to-expire non-critical slot. If the stack is full of
 * critical-urgency items, evict the soonest-to-expire critical only when the
 * incoming notification is itself critical — otherwise refuse (return -1) so
 * we never silently drop a critical alert in favor of a normal/low one.
 * Sticky-critical (expires_at_ms == 0) is never displaced. */
static int evict(Widget *w, int incoming_urgency) {
    int oldest = -1;
    int64_t best = INT64_MAX;
    for (int i = 0; i < MAX_OSDS; i++) {
        if (!w->s.osd.items[i].active || w->s.osd.items[i].urgency >= 2) continue;
        int64_t e = w->s.osd.items[i].expires_at_ms;
        if (e && e < best) { best = e; oldest = i; }
    }
    if (oldest < 0 && incoming_urgency >= 2) {
        for (int i = 0; i < MAX_OSDS; i++) {
            if (!w->s.osd.items[i].active) continue;
            int64_t e = w->s.osd.items[i].expires_at_ms;
            if (e && e < best) { best = e; oldest = i; }
        }
    }
    if (oldest < 0) return -1;
    uint32_t evicted_id = w->s.osd.items[oldest].replace_id;
    w->s.osd.items[oldest].active = 0;
    if (OSD_DBUS_CLOSE && dbus_emit_closed) dbus_emit_closed(evicted_id, 2 /*dismissed*/);
    return oldest;
}

/* Pack active items into a contiguous prefix preserving insertion order. */
static void pack(Widget *w) {
    Osd tmp[MAX_OSDS];
    int n = 0;
    for (int i = 0; i < MAX_OSDS; i++)
        if (w->s.osd.items[i].active) tmp[n++] = w->s.osd.items[i];
    for (int i = 0; i < n; i++) w->s.osd.items[i] = tmp[i];
    for (int i = n; i < MAX_OSDS; i++) w->s.osd.items[i].active = 0;
}

/* Final tx (post-icon) for body/summary text in a slab. */
static int slab_text_x(const Osd *o) {
    int tx = OSD_PAD_X;
    if (o->icon_cp) {
        const Glyph *g = font_find(&font_large, o->icon_cp);
        if (g) tx += g->adv + OSD_ICON_GAP;
    }
    return tx;
}

static int slab_height_for(int nbody, int has_progress) {
    int text_h = (1 + nbody) * font_small.line_h;
    int prog_h = has_progress ? OSD_PROG_H + 8 : 0;
    int h = text_h + prog_h + OSD_SLAB_PAD_Y;
    if (h < OSD_SLAB_H) h = OSD_SLAB_H;
    return h;
}

/* Cutout state: height of the transparent rect we ask the bar to punch over
 * the OSD's column range. Tracks slab[0]'s current visible body height,
 * clamped to BAR_HEIGHT, so the cutout grows/shrinks in lockstep with the
 * OSD slide. Without this, the cutout was punched at full BAR_HEIGHT instantly
 * while OSD's body was still mid-slide, exposing wallpaper through the un-
 * covered bar rows for a few frames — visible as a flash of wallpaper color.
 *
 * Registered against the bar via the generic cutout registry (widget.c); the
 * codegen-emitted bar render invokes cutout_apply("bar", ...) automatically. */
#if !OSD_ANCHOR_BR && !OSD_FLOAT
static int osd_bar_cutout_h = 0;

static Output *osd_cutout_scope = NULL;

static void osd_publish_bar_cutout(Output *scope) {
    /* The OSD lives on exactly one output at a time; bars on other monitors
     * must not punch a transparent strip. Clear the previous scope (in case
     * focus moved) before re-publishing on the new one. */
    if (osd_cutout_scope && osd_cutout_scope != scope)
        cutout_clear("bar", osd_cutout_scope);
    if (osd_bar_cutout_h <= 0) { cutout_clear("bar", scope); osd_cutout_scope = NULL; return; }
    int ch = osd_bar_cutout_h;
    if (ch > BAR_HEIGHT) ch = BAR_HEIGHT;
    cutout_set("bar", scope, CUTOUT_X_CENTER, 0, OSD_W, ch);
    osd_cutout_scope = scope;
}

/* Called from output_destroy: forget cutout state if it was scoped to the
 * output going away, so a later hotplug reusing the slot can't be compared
 * against a dangling Output *. */
void osd_on_output_destroyed(Output *o) {
    if (osd_cutout_scope == o) { osd_bar_cutout_h = 0; osd_cutout_scope = NULL; }
}
#else
void osd_on_output_destroyed(Output *o) { (void)o; }
#endif

/* Empty transition: attach a fully transparent buffer rather than NULL.
 * NULL-attach unmaps the layer surface, which makes wlroots destroy every
 * pending entry in configure_list. Any configure already in flight to us
 * is then unackable — when we receive it and reply with ack_configure,
 * wlroots fires the fatal "wrong configure serial" protocol error. Keeping
 * the surface mapped with a transparent buffer keeps configure_list
 * consistent. Frame callback drives a one-shot SHM-pool free in
 * on_frame_done (hud.c) so the ~2.8 MB pool isn't kept around between
 * notification bursts. */
static void osd_attach_empty(Widget *w) {
    update_input_region(w);
#if !OSD_ANCHOR_BR && !OSD_FLOAT
    if (!w->s.osd.is_pill && osd_bar_cutout_h != 0) {
        osd_bar_cutout_h = 0;
        osd_publish_bar_cutout(w->output);
        extern void bar_redraw_all(void);
        bar_redraw_all();
    }
#endif
    widget_ensure_pool(w, 2);
    BufSlot *s = widget_free_slot(w);
    if (!s) return;
    memset(s->px, 0, (size_t)widget_pw(w) * widget_ph(w) * 4);
    widget_attach(w, s, 1);
    w->s.osd.has_pixels = 0;
    w->want_pool_free = 1;
}

/* Spawn / close tween progress for one slab: 1.0 at rest, ease-out cubic in,
 * cubic decay out. */
static double slab_anim_p(const Osd *o, int64_t now) {
    if (o->closing_at_ms > 0) {
        int64_t e = now - o->closing_at_ms;
        if (e < 0) e = 0;
        if (e >= OSD_SLIDE_MS) return 0.0;
        double t = (double)e / (double)OSD_SLIDE_MS;
        return 1.0 - t * t * t;
    }
    if (o->spawn_ms > 0 && now < o->spawn_ms + OSD_SLIDE_MS) {
        int64_t e = now - o->spawn_ms;
        if (e < 0) e = 0;
        double t = (double)e / (double)OSD_SLIDE_MS;
        return 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
    }
    return 1.0;
}

/* Pass-1 state, shared by every layout backend and by the generated slab
 * renderer via osd_slab_geom(). Valid only for the widget most recently passed
 * to osd_slab_layout() — there is one stack widget at a time. */
static char   osd_wrapped[MAX_OSDS][OSD_MAX_BODY_LINES][OSD_BODY_MAX];
static int    osd_nbody[MAX_OSDS];
static int    osd_item_y[MAX_OSDS];
static int    osd_visible_h[MAX_OSDS];
static double osd_anim_p[MAX_OSDS];

/* Wrap bodies, compute per-slab settled height, tween progress and Y. item_y
 * uses the *animated* visible height of the slabs above, so closing slabs
 * collapse smoothly (slabs below shift up) and spawning slabs push the stack
 * down as they emerge. */
int osd_slab_layout(Widget *w) {
    const Font *f = &font_small;
    int64_t now = now_ms();
    int total = 0, n_active = 0;
    for (int i = 0; i < MAX_OSDS; i++) {
        Osd *o = &w->s.osd.items[i];
        osd_nbody[i] = 0;
        if (!o->active) break;
        int tx = slab_text_x(o);
        int body_w = OSD_W - OSD_PAD_X - tx;
        if (body_w < 0) body_w = 0;
        if (o->body[0])
            osd_nbody[i] = wrap_body(f, o->body, body_w, OSD_MAX_BODY_LINES, osd_wrapped[i]);
        o->h = slab_height_for(osd_nbody[i], o->progress >= 0);

        osd_anim_p[i] = slab_anim_p(o, now);
        int vh = (int)((double)o->h * osd_anim_p[i]);
        if (vh < 0) vh = 0;
        if (vh > o->h) vh = o->h;
        osd_visible_h[i] = vh;

        if (n_active > 0 && osd_visible_h[i-1] > 0) total += OSD_GAP;
        osd_item_y[i] = total;
        total += vh;
        n_active++;
    }
#if !OSD_ANCHOR_BR && !OSD_FLOAT
    /* Sync bar cutout to slab[0]'s emerging body: the bar's transparent rect
     * grows 0 → BAR_HEIGHT with vh, so the translucent slab blends with
     * wallpaper only, never double-blended over the bar. Republish when the
     * scope (output) changed too — a migration can settle slab[0] at the same
     * height, leaving the hole on the old output's bar. */
    int new_cut_h = 0;
    if (n_active > 0)
        new_cut_h = osd_visible_h[0] < BAR_HEIGHT ? osd_visible_h[0] : BAR_HEIGHT;
    if (new_cut_h != osd_bar_cutout_h || osd_cutout_scope != w->output) {
        osd_bar_cutout_h = new_cut_h;
        osd_publish_bar_cutout(w->output);
        extern void bar_redraw_all(void);
        bar_redraw_all();
    }
#endif
    return n_active;
}

int osd_bar_split(void) {
#if !OSD_ANCHOR_BR && !OSD_FLOAT
    return BAR_HEIGHT;
#else
    return 0;
#endif
}

/* Both exist so the generated renderer never re-derives time or input state:
 * it draws, and asks the runtime for the rest. */
/* The wrapped body as one "\n"-joined string, so the DSL's body_lines text
 * renderer draws the same lines osd_slab_layout measured. Rebuilt per call —
 * only two widgets ever ask, and only while a slab is on screen.
 * ponytail: wrap width is still osd_slab_layout's OSD_W-derived column, not
 * the declaring widget's; thread the widget width in if a preset diverges. */
const char *osd_slab_body(int i) {
    static char joined[OSD_MAX_BODY_LINES * OSD_BODY_MAX];
    int n = 0;
    for (int li = 0; li < osd_nbody[i]; li++) {
        if (n && n < (int)sizeof joined - 1) joined[n++] = '\n';
        int k = (int)strlen(osd_wrapped[i][li]);
        if (k > (int)sizeof joined - 1 - n) k = (int)sizeof joined - 1 - n;
        memcpy(joined + n, osd_wrapped[i][li], k);
        n += k;
    }
    joined[n] = 0;
    return joined;
}
int osd_slab_nbody(int i) { return osd_nbody[i]; }

void osd_stack_input_region(Widget *w) { update_input_region(w); }
void osd_stack_attach_empty(Widget *w) { osd_attach_empty(w); }

int osd_slab_anim_pending(Widget *w) {
    int64_t now = now_ms();
    for (int i = 0; i < MAX_OSDS; i++) {
        Osd *o = &w->s.osd.items[i];
        if (!o->active) break;
        if (o->closing_at_ms > 0 && now < o->closing_at_ms + OSD_SLIDE_MS) return 1;
        if (o->closing_at_ms == 0 && o->spawn_ms > 0 && now < o->spawn_ms + OSD_SLIDE_MS) return 1;
    }
    return 0;
}

void osd_slab_geom(Widget *w, int i, OsdSlabGeom *g) {
    g->y  = osd_item_y[i];
    g->vh = osd_visible_h[i];
    g->sh = w->s.osd.items[i].h;
    g->settled = osd_anim_p[i] >= 1.0;
    g->closing = w->s.osd.items[i].closing_at_ms > 0;
}


void osd_render(Widget *w) {
#if OSD_PILL_W > 0
    if (w->s.osd.is_pill) { osd_pill_render(w); return; }
#endif
    /* Every stack layout is a declaration now — anchor + margin pick the
     * shape. osd.c keeps only what the DSL can't say: the ring, the tweens,
     * the bar cutout, and the junction split the renderer asks for. */
    extern void render_osd(Widget *w);
    render_osd(w);
}

/* Create an OSD widget anchored to `o`. Layer-surface output is fixed at
 * creation time, so focus-follows-output requires destroy+recreate. */
static Widget *osd_make_on(Output *o) {
    Widget *w = widget_alloc(W_OSD);
    if (!w) { msg("wisp: no widget slot for OSD"); return NULL; }
#if OSD_ANCHOR_BR
    /* Buffer pads OSD_FILLET_R on the left and top for the armpit wedges;
     * body sits flush in the bottom-right corner. */
    w->w = OSD_W + OSD_FILLET_R;
    w->h = OSD_TOTAL_H + OSD_FILLET_R;
    widget_setup_surface(w, LAYER_OVERLAY, "wisp-osd", o);
    widget_set_size(w, w->w, w->h);
    widget_set_anchor(w, LS_ANCHOR_BOTTOM | LS_ANCHOR_RIGHT);
    widget_set_margin(w, 0, 0, 0, 0);
    widget_set_exclusive_zone(w, 0);
#elif OSD_FLOAT
    w->w = OSD_W;
    w->h = OSD_TOTAL_H + OSD_TOP_MARGIN;
    widget_setup_surface(w, LAYER_OVERLAY, "wisp-osd", o);
    widget_set_size(w, w->w, w->h);
    /* Anchor TOP only -> compositor centers horizontally. exclusive_zone=-1
     * ignores the bar's zone so the buffer top sits at screen y=0: the chain
     * rests OSD_TOP_MARGIN from the true screen edge — on the bar's own row,
     * like a center bar module — and drops in from the screen top (mid-slide
     * rows above the rest position are clipped by the buffer top). */
    widget_set_anchor(w, LS_ANCHOR_TOP);
    widget_set_margin(w, 0, 0, 0, 0);
    widget_set_exclusive_zone(w, -1);
#else
    w->w = OSD_W + 2 * OSD_SIDE_PAD;
    w->h = OSD_TOTAL_H;
    widget_setup_surface(w, LAYER_OVERLAY, "wisp-osd", o);
    widget_set_size(w, w->w, w->h);
    /* Anchor TOP only → compositor centers horizontally. exclusive_zone=-1
     * ignores the bar's zone so the buffer top sits at screen y=0; the
     * top OSD_TOP_INSET rows overlap the bar (transparent except fillets),
     * and body content starts at buffer y=OSD_TOP_INSET (= screen
     * y=BAR_HEIGHT), still flush against the bar's bottom edge. */
    widget_set_anchor(w, LS_ANCHOR_TOP);
    widget_set_margin(w, 0, 0, 0, 0);
    widget_set_exclusive_zone(w, -1);
#endif
    widget_set_kbd_interactive(w, 0);
    /* Empty input region: OSD never steals pointer events. */
    widget_set_input_region_rect(w, 0, 0, 0, 0);
    wl_req(w->surface, SURFACE_REQ_COMMIT, NULL, 0, -1);
    return w;
}

/* Return the OSD widget anchored to the focused output, migrating it (and
 * carrying active slabs across) if focus has moved since the last post. */
static Widget *osd_ensure(void) {
    Output *target = focused_output;
    if (!target) {
        /* Fall back to first connected output. */
        for (int i = 0; i < MAX_OUTPUTS; i++)
            if (outputs[i].active) { target = &outputs[i]; break; }
    }
    if (!target) return NULL;
    Widget *w = osd_stack();
    if (w && w->output == target) return w;
    if (w && !OSD_FOCUS_FOLLOW) return w;
    if (w) {
        /* Migrate: snapshot items, tear down, rebuild on the new output. */
        Osd snap[MAX_OSDS];
        memcpy(snap, w->s.osd.items, sizeof snap);
        uint32_t next_id = w->s.osd.next_id;
        widget_destroy(w);
        /* The cutout statics are scoped to the old output; reset them so the
         * first render on the new output republishes from scratch (otherwise a
         * settled slab[0] makes the height-equal gate skip the republish). */
#if !OSD_ANCHOR_BR && !OSD_FLOAT
        osd_bar_cutout_h = 0;
        osd_cutout_scope = NULL;
#endif
        w = osd_make_on(target);
        if (!w) return NULL;
        memcpy(w->s.osd.items, snap, sizeof snap);
        w->s.osd.next_id = next_id;
        /* Re-render happens at caller (osd_post → osd_render). */
        return w;
    }
    return osd_make_on(target);
}

/* First-configure hook: a slab posted before the layer surface's initial
 * configure couldn't render, and by the time the configure lands the
 * spawn_ms window may have partly (or fully) burned — the slab pops in with
 * a truncated slide (or none). Restart pending spawn tweens so the first
 * paint plays the full slide-in. */
void osd_on_first_configure(Widget *w) {
    int64_t now = now_ms();
    for (int i = 0; i < MAX_OSDS; i++) {
        Osd *o = &w->s.osd.items[i];
        if (o->active && !o->closing_at_ms && o->spawn_ms > 0)
            o->spawn_ms = now;
    }
}

uint32_t osd_post(uint32_t replace_id, const char *summary, const char *body,
                  uint32_t icon_cp, int progress, int urgency, int muted,
                  int timeout_ms) {
#if OSD_PILL_W > 0
    /* Progress-only posts (volume / brightness) get the minimal pill; any
     * post with a body is a real notification and goes to the stack.
     * ponytail: dbus notifications carrying a progress hint but no body also
     * land here — close-by-id on the pill isn't wired; add if a client needs it. */
    if (progress >= 0 && (!body || !body[0]))
        return pill_post(icon_cp, progress, muted, timeout_ms);
#endif
    Widget *w = osd_ensure();
    if (!w) return 0;

    int slot = find_replace(w, replace_id);
    if (slot < 0) slot = find_free(w);
    if (slot < 0) slot = evict(w, urgency);
    if (slot < 0) return 0;  /* refused — incoming would have displaced a critical */

    Osd *o = &w->s.osd.items[slot];
    /* Preserve spawn_ms across content-only updates: when a held key
     * (volume / brightness) hammers replace-id posts, the slab has long
     * since finished its slide-in. Restarting the tween would look like a
     * jarring "refresh". Only seed a new spawn_ms for a genuinely new slab
     * (empty slot) or one that was mid-close at the time of the replace. */
    int64_t now = now_ms();
    int64_t prev_spawn = (o->active && !o->closing_at_ms) ? o->spawn_ms : 0;
    memset(o, 0, sizeof *o);
    o->active = 1;
    o->spawn_ms = prev_spawn ? prev_spawn : now;
    o->replace_id = replace_id ? replace_id : ++w->s.osd.next_id;
    if (summary) snprintf(o->summary, sizeof o->summary, "%s", summary);
    if (body)    snprintf(o->body,    sizeof o->body,    "%s", body);
    o->icon_cp  = icon_cp;
    o->progress = progress;
    o->urgency  = urgency;
    o->muted    = muted;
    if (timeout_ms < 0) {
        o->expires_at_ms = now + (urgency == 0 ? OSD_TIMEOUT_LOW : OSD_TIMEOUT_NORMAL);
    } else if (timeout_ms == 0) {
        o->expires_at_ms = 0;  /* sticky */
    } else {
        o->expires_at_ms = now + timeout_ms;
    }

    pack(w);
    osd_render(w);  /* refreshes input region from computed heights */
    return o->replace_id;
}

/* Begin the slide-out animation on `o`. The slab stays active (so it keeps
 * its slot and renders during the tween); osd_check_expiry reaps it once the
 * animation completes and only then notifies dbus. Idempotent. */
static void start_close(Osd *o, uint32_t reason) {
    if (!o->active || o->closing_at_ms > 0) return;
    o->closing_at_ms = now_ms();
    o->close_reason  = reason;
    o->expires_at_ms = 0;  /* don't double-expire mid-slide */
}

void osd_close(uint32_t id) {
    Widget *w = osd_stack();
    if (!w) return;
    int slot = find_replace(w, id);
    if (slot < 0) return;
    start_close(&w->s.osd.items[slot], 3 /*closed by call*/);
    osd_render(w);
}

void osd_close_all(void) {
    Widget *w = osd_stack();
    if (!w) return;
    for (int i = 0; i < MAX_OSDS; i++) {
        if (w->s.osd.items[i].active)
            start_close(&w->s.osd.items[i], 3);
    }
    osd_render(w);
}

/* Compositor-paced animation tick. Called from on_frame_done while any slab
 * is mid-slide; the next frame's request_frame flag in osd_render decides
 * whether to ask for another callback. */
void osd_tick(Widget *w) {
    if (!w || w->kind != W_OSD) return;
    int64_t now = now_ms();
    int reaped = 0;
    for (int i = 0; i < MAX_OSDS; i++) {
        Osd *o = &w->s.osd.items[i];
        if (!o->active || !o->closing_at_ms) continue;
        if (now >= o->closing_at_ms + OSD_SLIDE_MS) {
            uint32_t id = o->replace_id, reason = o->close_reason ? o->close_reason : 3;
            o->active = 0;
            o->closing_at_ms = 0;
            reaped = 1;
            /* Pill ids are private to the pill widget — never surface them
             * as dbus NotificationClosed. */
            if (OSD_DBUS_CLOSE && dbus_emit_closed && !w->s.osd.is_pill)
                dbus_emit_closed(id, reason);
        }
    }
    if (reaped) pack(w);
    osd_render(w);
}

int osd_check_expiry(int64_t now) {
    int next = -1, redraw = 0;
#if OSD_PILL_W > 0
    Widget *pw = pill_widget();
    if (pw) {
        Osd *o = &pw->s.osd.items[0];
        if (o->active && o->closing_at_ms > 0) {
            if (o->closing_at_ms + OSD_SLIDE_MS - now <= 0) {
                o->active = 0;
                o->closing_at_ms = 0;
                osd_pill_render(pw);
            }
        } else if (o->active && o->expires_at_ms) {
            int64_t left = o->expires_at_ms - now;
            if (left <= 0) { start_close(o, 1); osd_pill_render(pw); }
            else if (next < 0 || left < next) next = (int)left;
        }
    }
#endif
    Widget *w = osd_stack();
    if (!w) return next;
    for (int i = 0; i < MAX_OSDS; i++) {
        Osd *o = &w->s.osd.items[i];
        if (!o->active) continue;
        if (o->closing_at_ms > 0) {
            int64_t left = o->closing_at_ms + OSD_SLIDE_MS - now;
            if (left <= 0) {
                /* Reap timed-out slide-out (frame callbacks normally do this,
                 * but a stalled compositor or hidden output won't deliver
                 * any). dbus_close_reason preserved. */
                uint32_t id = o->replace_id, reason = o->close_reason ? o->close_reason : 3;
                o->active = 0;
                o->closing_at_ms = 0;
                redraw = 1;
                if (OSD_DBUS_CLOSE && dbus_emit_closed) dbus_emit_closed(id, reason);
            }
            continue;
        }
        if (!o->expires_at_ms) continue;
        int64_t left = o->expires_at_ms - now;
        if (left <= 0) {
            start_close(o, 1 /*expired*/);
            redraw = 1;
        } else if (next < 0 || left < next) next = (int)left;
    }
    if (redraw) { pack(w); osd_render(w); }
    return next;
}

/* Y → slab index, or -1 if click is in a gap / past the stack. Walks per-item
 * heights because slabs are now variable-height. */
static int slab_at(Widget *w, int y) {
#if OSD_ANCHOR_BR
    /* Bottom-up: items[n-1] flush with the buffer bottom, older slabs above. */
    int n = 0;
    while (n < MAX_OSDS && w->s.osd.items[n].active) n++;
    int off = w->h;
    for (int i = n - 1; i >= 0; i--) {
        Osd *o = &w->s.osd.items[i];
        int sh = o->h > 0 ? o->h : OSD_SLAB_H;
        if (y >= off - sh && y < off) return i;
        off -= sh + OSD_GAP;
    }
    return -1;
#else
    y -= OSD_FLOAT ? OSD_TOP_MARGIN : 0;
    if (y < 0) return -1;
    int off = 0;
    for (int i = 0; i < MAX_OSDS; i++) {
        Osd *o = &w->s.osd.items[i];
        if (!o->active) break;
        int sh = o->h > 0 ? o->h : OSD_SLAB_H;
        if (y >= off && y < off + sh) return i;
        off += sh + OSD_GAP;
    }
    return -1;
#endif
}

/* Refresh the surface's input region: hit-testable only over active slabs.
 * Composed of one rect per active item so the inter-slab gaps stay
 * click-through (don't steal clicks from windows below). */
static void update_input_region(Widget *w) {
#if OSD_PILL_W > 0
    if (w->s.osd.is_pill) {
        if (w->s.osd.items[0].active)
            widget_set_input_region_rect(w, 0, 0, w->w, w->h);
        else
            widget_set_input_region_rect(w, 0, 0, 0, 0);
        return;
    }
#endif
    int total = 0, n = 0;
    for (int i = 0; i < MAX_OSDS; i++) {
        Osd *o = &w->s.osd.items[i];
        if (!o->active) break;
        if (n > 0) total += OSD_GAP;
        total += o->h > 0 ? o->h : OSD_SLAB_H;
        n++;
    }
    if (n == 0) {
        widget_set_input_region_rect(w, 0, 0, 0, 0);
        return;
    }
    /* Single contiguous rect covering all active slabs — simpler than
     * one-region-per-slab and clicks in gaps just dismiss the slab above.
     * Offset by the bar inset / side pad so clicks above the body (over the
     * bar) and on the fillet gutter fall through. */
#if OSD_ANCHOR_BR
    widget_set_input_region_rect(w, w->w - OSD_W, w->h - total, OSD_W, total);
#elif OSD_FLOAT
    widget_set_input_region_rect(w, 0, OSD_TOP_MARGIN, OSD_W, total);
#else
    widget_set_input_region_rect(w, OSD_SIDE_PAD, 0, OSD_W, total);
#endif
}

void osd_on_click(Widget *w, int x, int y) {
    (void)x;
    if (!OSD_DISMISS_ON_CLICK) return;
#if OSD_PILL_W > 0
    if (w->s.osd.is_pill) {
        if (w->s.osd.items[0].active) {
            start_close(&w->s.osd.items[0], 2 /*dismissed*/);
            osd_pill_render(w);
        }
        return;
    }
#endif
    int idx = slab_at(w, y);
    if (idx < 0) return;
    if (!w->s.osd.items[idx].active) return;
    uint32_t id = w->s.osd.items[idx].replace_id;
    osd_close(id);
}

/* Pct pill lives in its own file (file-size rule); same TU — osd.c itself is
 * #included by gen_spawn.c, so the pill rides along the same way. */
#include "osd_pill.c"


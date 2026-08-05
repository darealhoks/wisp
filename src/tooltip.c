/* Tooltip — a transient, non-interactive label that hangs off a widget rect.
 * What it looks like is declared (`surface tooltip { spawned_by = tooltip; … }`
 * lowers to render_tooltip()); this file owns only the surface: creation,
 * auto-width, placement and teardown. Same division as menu.c/render_menu_*.
 *
 * Hard constraint: a tooltip is decoration, never a focus target. It takes
 * keyboard_interactivity 0 and a zero-area input region, so neither keys nor
 * clicks can ever reach it — this is why it does NOT reuse menu_create. */

#include "wisp.h"

#include <string.h>

extern void render_tooltip(Widget *w);

static Widget *tip_widget(void) {
    for (int i = 0; i < MAX_WIDGETS; i++)
        if (widgets[i].kind == W_TIP) return &widgets[i];
    return NULL;
}

void tooltip_render(Widget *w) { render_tooltip(w); }

/* Logical width of an output — mode_w is physical, layer-shell margins aren't. */
static int out_logical_w(const Output *o) {
    return o->scale120 > 0 ? o->mode_w * 120 / o->scale120 : o->mode_w;
}

/* Hover dwell. No timerfd: the pending deadline just shortens the main loop's
 * epoll timeout (tooltip_check_deferred), and only while something is pending —
 * an unhovered bar keeps blocking forever, which is the idle-zero invariant. */
static const char *pend_text;
static TipAnchor   pend_at;
static int64_t     pend_at_ms;

void tooltip_arm(const char *text, const TipAnchor *at) {
    tooltip_hide();
    if (!text || !*text || !at) return;
    pend_text  = text;   /* .rodata: wispc only lowers literal tooltips */
    pend_at    = *at;
    pend_at_ms = now_ms() + TIP_DELAY_MS;
}

int tooltip_check_deferred(int64_t now) {
    if (!pend_at_ms) return -1;
    int64_t left = pend_at_ms - now;
    if (left > 0) return (int)left;
    const char *t = pend_text;
    pend_at_ms = 0;
    pend_text = NULL;
    tooltip_show(t, &pend_at);
    return -1;
}

void tooltip_show(const char *text, const TipAnchor *at) {
    if (!text || !*text) { tooltip_hide(); return; }

    Output *o = at ? at->out : NULL;
    if (!o) o = focused_output;
    if (!o)
        for (int i = 0; i < MAX_OUTPUTS; i++)
            if (outputs[i].active) { o = &outputs[i]; break; }
    if (!o) return;

    /* An output change means a new surface: a layer surface is bound to its
     * output for life. */
    Widget *w = tip_widget();
    if (w && w->output != o) { widget_destroy(w); w = NULL; }
    int fresh = !w;
    if (!w) {
        w = widget_alloc(W_TIP);
        if (!w) { msg("wisp: no widget slot for tooltip"); return; }
        widget_setup_surface(w, LAYER_OVERLAY, "wisp-tooltip", o);
    }

    size_t n = strnlen(text, sizeof w->s.tip.text - 1);
    memcpy(w->s.tip.text, text, n);
    w->s.tip.text[n] = 0;

    /* ponytail: measured at font_small like menu.c does, not at the surface's
     * declared font_size — font_px() is private to lock.c. Declare
     * `font_size = 14` on the tooltip surface, or lift font_px() to render.c
     * if a preset ever wants another size. */
    int tw = text_width(&font_small, w->s.tip.text) + 2 * TIP_PAD_X;
    if (tw > TIP_MAX_W) tw = TIP_MAX_W;   /* past the clamp the DSL's `elide` cuts */
    if (tw < 1) tw = 1;

    int ax = at ? at->x : 0, aw = at ? at->w : 0;
    int mx = ax + aw / 2 - tw / 2;
    int ow = out_logical_w(o);
    if (mx > ow - tw) mx = ow - tw;
    if (mx < 0) mx = 0;

    /* tw/TIP_H are the BODY; a declared shadow pads the buffer around it and
     * takes the same amount back out of the margin, so the body lands where it
     * would with no shadow. Floored at 0 — a negative layer-shell margin would
     * drag the body itself off-screen. */
    int my = (at ? at->below : 0) + TIP_GAP - TIP_SHADOW_PAD_T;
    mx -= TIP_SHADOW_PAD_L;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    widget_set_size(w, tw + TIP_SHADOW_PAD_L + TIP_SHADOW_PAD_R,
                       TIP_H + TIP_SHADOW_PAD_T + TIP_SHADOW_PAD_B);
    widget_set_anchor(w, LS_ANCHOR_TOP | LS_ANCHOR_LEFT);
    widget_set_margin(w, my, 0, 0, mx);
    widget_set_exclusive_zone(w, -1);
    widget_set_kbd_interactive(w, 0);
    /* Zero-area region: the compositor routes every pointer event straight
     * through to whatever is underneath, so hovering the tooltip can't cancel
     * the hover that spawned it. */
    widget_set_input_region_rect(w, 0, 0, 0, 0);
    wl_req(w->surface, SURFACE_REQ_COMMIT, NULL, 0, -1);
    /* A fresh surface has no configure yet; render_tooltip early-returns on
     * !configured and widget_repaint paints it when the configure lands. */
    if (!fresh) render_tooltip(w);
}

/* Full teardown, not a hide: widget_destroy frees the SHM pool and the layer
 * surface, so a dismissed tooltip leaves nothing mapped and no timer armed. */
void tooltip_hide(void) {
    pend_at_ms = 0;
    pend_text = NULL;
    Widget *w = tip_widget();
    if (w) widget_destroy(w);
}

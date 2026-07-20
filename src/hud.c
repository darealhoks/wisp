/* HUD — generic hover-revealed slide-in shell.
 *
 * Owns nothing about the surface's contents: codegen emits the per-surface
 * render/click. hud.c handles only the trigger input region, slide animation,
 * and idle-pool teardown. Slide is implemented as a layer-surface margin tween
 * on whichever single edge `anchor` picks (see hud_register).
 *
 * State machine: hidden → ptr_enter ⇒ swap to full input region, start margin
 * tween toward 0, render via bar_render. Pointer leave (after grace) ⇒ tween
 * back to -slide_extent; on completion swap input region back to the trigger
 * strip and free the SHM pool. */

#include "wisp.h"

#include <math.h>
#include <string.h>

static int64_t last_btn_ms;

/* Pick a single edge bit from a multi-bit anchor. Prefer the first single
 * anchored edge that has a finite extent dimension we can slide along.
 * For anchor=top|right we slide from the top edge (visually: drops down).
 * Documented: priority is top > bottom > left > right. */
static int pick_slide_edge(int anchor) {
    if (anchor & LS_ANCHOR_TOP)    return LS_ANCHOR_TOP;
    if (anchor & LS_ANCHOR_BOTTOM) return LS_ANCHOR_BOTTOM;
    if (anchor & LS_ANCHOR_LEFT)   return LS_ANCHOR_LEFT;
    if (anchor & LS_ANCHOR_RIGHT)  return LS_ANCHOR_RIGHT;
    return LS_ANCHOR_TOP;
}

static int slide_extent(Widget *w) {
    int e = w->s.hud.slide_edge;
    int dim = (e == LS_ANCHOR_TOP || e == LS_ANCHOR_BOTTOM) ? w->s.hud.full_h
                                                            : w->s.hud.full_w;
    /* Slide is render-only — the Wayland surface stays anchored, so the trigger
     * input region (the unpainted gutter at the top of the surface) is always
     * reachable. Slide the painted content fully out of the surface. */
    return dim > 0 ? dim : 0;
}

static void kick_frame(Widget *w);

void hud_render(Widget *w) {
    /* Dispatcher: codegen-emitted bar_render walks per-surface widget tables
     * and routes to render_<name>. Works for W_HUD because the widget pointer
     * matches what create_on registered. */
    bar_render(w);
}

void hud_tick(Widget *w, int64_t now) {
    if (!w->s.hud.animating) return;
#ifdef WISP_HAS_ANIM
    if (w->s.hud.reveal_ms > 0) {
        /* Step 6.2: fixed-duration reveal with chosen easing. */
        double t = (double)(now - w->s.hud.anim_start_ms) / (double)w->s.hud.reveal_ms;
        if (t < 0) t = 0;
        double u = anim_ease(w->s.hud.reveal_easing, t > 1 ? 1 : t);
        w->s.hud.cur_off = w->s.hud.from_off
            + (w->s.hud.target_off - w->s.hud.from_off) * u;
        if (t >= 1.0) w->s.hud.cur_off = w->s.hud.target_off;
    } else
#endif
    {
        double dt = (double)(now - w->s.hud.anim_last_ms);
        w->s.hud.anim_last_ms = now;
        double k = 1.0 - exp(-dt / HUD_ANIM_TAU_MS);
        w->s.hud.cur_off += (w->s.hud.target_off - w->s.hud.cur_off) * k;
    }
    if (fabs(w->s.hud.cur_off - w->s.hud.target_off) < HUD_ANIM_EPSILON) {
        w->s.hud.cur_off = w->s.hud.target_off;
        w->s.hud.animating = 0;
        if (w->s.hud.target_off >= slide_extent(w)) {
            w->s.hud.visible = 0;
            widget_set_input_region(w, w->s.hud.region_trigger);
            BufSlot *s = widget_free_slot(w);
            if (s) {
                memset(s->px, 0, w->s.hud.full_w * w->s.hud.full_h * 4);
                widget_attach(w, s, 0);
                w->want_pool_free = 1;
            }
            return;
        }
        hud_render(w);
        return;
    }
    hud_render(w);
    kick_frame(w);
}

static void kick_frame(Widget *w) {
    if (w->frame_cb) return;
    w->frame_cb = wl_new_id();
    uint32_t a = w->frame_cb;
    wl_req(w->surface, SURFACE_REQ_FRAME, &a, 1, -1);
    wl_req(w->surface, SURFACE_REQ_COMMIT, NULL, 0, -1);
}

static void show(Widget *w) {
    /* Pointer can re-cross the trigger gutter while a tween toward 0 is already
     * in flight — re-arming anim_start_ms each twitch resets the eased curve to
     * t=0 and kicks a fresh frame, burning CPU in a perpetual restart loop. */
    if (w->s.hud.animating && w->s.hud.target_off == 0) return;
    if (w->s.hud.visible && w->s.hud.target_off == 0) return;
    int was_hidden = !w->s.hud.visible;
    int64_t now = now_ms();
    w->s.hud.visible = 1;
    w->s.hud.target_off = 0;
    w->s.hud.animating = 1;
    w->s.hud.anim_last_ms = now;
    w->s.hud.from_off = w->s.hud.cur_off;
    w->s.hud.anim_start_ms = now;
    if (was_hidden) widget_set_input_region(w, w->s.hud.region_full);
    hud_render(w);
    kick_frame(w);
}

static void hide(Widget *w) {
    int ext = slide_extent(w);
    if (!w->s.hud.visible && w->s.hud.cur_off >= ext) return;
    int64_t now = now_ms();
    w->s.hud.target_off = ext;
    w->s.hud.animating = 1;
    w->s.hud.anim_last_ms = now;
    w->s.hud.from_off = w->s.hud.cur_off;
    w->s.hud.anim_start_ms = now;
    kick_frame(w);
}

void hud_on_pointer_enter(Widget *w, int x, int y) {
    (void)x; (void)y;
    msg("wisp: hud enter w=%p edge=%d off=%d", (void*)w, w->s.hud.slide_edge, (int)w->s.hud.cur_off);
    w->s.hud.ptr_inside = 1;
    w->s.hud.hide_at_ms = 0;
    show(w);
}
void hud_on_pointer_leave(Widget *w) {
    w->s.hud.ptr_inside = 0;
    /* Always schedule the hide — returning early here strands the HUD open,
     * since no further pointer events arrive once the pointer is gone. The
     * click grace only pushes the hide out, it never cancels it. */
    int64_t hide_at = now_ms() + HUD_HIDE_DELAY_MS;
    int64_t grace_until = last_btn_ms + HUD_CLICK_GRACE_MS;
    w->s.hud.hide_at_ms = hide_at > grace_until ? hide_at : grace_until;
}
void hud_on_pointer_motion(Widget *w, int x, int y) {
    (void)w; (void)x; (void)y;
}
void hud_on_pointer_button(Widget *w, uint32_t button, uint32_t state) {
    last_btn_ms = now_ms();
    if (!w->s.hud.visible || button != 0x110 /*BTN_LEFT*/) return;
    if (state == 1) {
        bar_input_press(w, ptr_x, ptr_y, (int)button);
        bar_input_click(w, ptr_x, ptr_y, (int)button);
    } else {
        bar_input_release(w, ptr_x, ptr_y, (int)button);
    }
    hud_render(w);
}

int hud_check_deferred(int64_t now) {
    int timeout = -1;
    for (int i = 0; i < MAX_WIDGETS; i++) {
        Widget *w = &widgets[i];
        if (w->kind != W_HUD) continue;
        if (w->s.hud.hide_at_ms) {
            int64_t left = w->s.hud.hide_at_ms - now;
            if (left <= 0) {
                w->s.hud.hide_at_ms = 0;
                if (w->s.hud.visible && !w->s.hud.ptr_inside) hide(w);
            } else if (timeout < 0 || left < timeout) timeout = (int)left;
        }
    }
    return timeout;
}

/* Codegen calls this from <surf>_create_on after widget_setup_surface +
 * set_size + set_anchor, before the first COMMIT. Sets up the trigger input
 * region geometry and primes the hidden margin. */
void hud_register(Widget *w, int anchor, int trigger_size, int full_w, int full_h) {
    w->s.hud.anchor = anchor;
    w->s.hud.slide_edge = pick_slide_edge(anchor);
    w->s.hud.trigger_size = trigger_size > 0 ? trigger_size : 5;
    w->s.hud.full_w = full_w;
    w->s.hud.full_h = full_h;
    w->w = full_w;
    w->h = full_h;

    int ts = w->s.hud.trigger_size;
    int tx = 0, ty = 0, tw = full_w, th = full_h;
    /* Trigger zone is the intersection of all anchored edge strips: a single
     * anchored edge gives a full-length strip; a corner anchor gives a small
     * square at that screen corner. */
    if (anchor & LS_ANCHOR_TOP)    { ty = 0;          th = ts; }
    if (anchor & LS_ANCHOR_BOTTOM) { ty = full_h - ts; th = ts; }
    if (anchor & LS_ANCHOR_LEFT)   { tx = 0;          tw = ts; }
    if (anchor & LS_ANCHOR_RIGHT)  { tx = full_w - ts; tw = ts; }
    w->s.hud.region_trigger = widget_make_region(tx, ty, tw, th);
    w->s.hud.region_full    = widget_make_region(0, 0, full_w, full_h);
    widget_set_input_region(w, w->s.hud.region_trigger);

    /* Slide is purely a render-translation: the layer surface is always at its
     * anchored position; only the painted content moves. cur_off > 0 = hidden
     * by that many pixels along the slide edge. */
    w->s.hud.cur_off = w->s.hud.target_off = slide_extent(w);
    w->s.hud.visible = 0;
}

/* Step 6.2: codegen calls this when the DSL surface declares reveal_anim_ms /
 * reveal_easing. ms<=0 leaves the exponential-decay default in place. */
void hud_set_reveal_anim(Widget *w, int ms, int easing) {
    w->s.hud.reveal_ms = ms;
    w->s.hud.reveal_easing = easing;
}

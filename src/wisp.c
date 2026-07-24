/* wisp — input dispatchers + small loop helpers.
 *
 * The boot sequence + epoll loop lives in src/gen/<preset>/main.c (hand-written
 * today, codegen'd by `wispc` later). This file holds the always-linked-core
 * primitives the loop and the wl.c dispatch call into:
 *   - now_ms()
 *   - key-repeat timerfd plumbing (wisp emulates Wayland auto-repeat itself)
 *   - on_pointer_event / on_keyboard_event / on_ls_event
 *     (one branch per widget kind, each gated by WISP_HAS_*). */

#include "wisp.h"

#include <stdint.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

int64_t now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ============================================================ */
/* Key-repeat (Wayland never auto-repeats; clients emulate it).  */
/* ============================================================ */

int      key_rep_tfd      = -1;
uint32_t key_rep_key      = 0;
int      key_rep_delay_ms = 400;
int      key_rep_rate_ms  = 35;

/* Only menu and lock consume held keys; without either, nothing arms it. */
#if defined(WISP_HAS_MENU) || defined(WISP_HAS_LOCK)
static void key_rep_arm(uint32_t key) {
    key_rep_key = key;
    if (key_rep_tfd < 0) return;
    struct itimerspec ts = {
        .it_value    = { .tv_sec = key_rep_delay_ms / 1000,
                         .tv_nsec = (key_rep_delay_ms % 1000) * 1000000L },
        .it_interval = { .tv_sec = key_rep_rate_ms / 1000,
                         .tv_nsec = (key_rep_rate_ms % 1000) * 1000000L },
    };
    timerfd_settime(key_rep_tfd, 0, &ts, NULL);
}
#endif
void key_rep_cancel(void) {
    key_rep_key = 0;
    if (key_rep_tfd < 0) return;
    struct itimerspec off = {0};
    timerfd_settime(key_rep_tfd, 0, &off, NULL);
}

/* ============================================================ */
/* Input dispatchers (called from wl.c)                          */
/* ============================================================ */

void on_pointer_event(uint16_t op, uint8_t *body, uint32_t bodylen) {
    switch (op) {
    case 0: {  /* enter: serial, surface, x, y */
        if (bodylen < 16) return;
        uint32_t serial = *(uint32_t *)body;
        ptr_focus = *(uint32_t *)(body + 4);
        int32_t fx = *(int32_t *)(body + 8), fy = *(int32_t *)(body + 12);
        ptr_x = fx >> 8; ptr_y = fy >> 8;
        enter_serial = serial;
        Widget *w = widget_by_surface(ptr_focus);
        (void)w;
#ifdef WISP_HAS_LOCK
        /* Hide the pointer over a lock surface: set_cursor with a null surface
         * removes the cursor image until pointer leave. */
        if (w && w->kind == W_LOCK && id_pointer) {
            uint32_t a[4] = { serial, 0, 0, 0 };
            wl_req(id_pointer, POINTER_REQ_SET_CURSOR, a, 4, -1);
        }
#endif
#ifdef WISP_HAS_HUD
        if (w && w->kind == W_HUD) hud_on_pointer_enter(w, ptr_x, ptr_y);
#endif
#ifdef WISP_HAS_MENU
        /* Seed hover from the enter coords: a stationary cursor gets no
         * motion event when the popup maps beneath it. */
        if (w && w->kind == W_MENU) menu_on_hover(w, ptr_x, ptr_y);
#endif
        break;
    }
    case 1: {  /* leave */
        Widget *w = widget_by_surface(ptr_focus);
        (void)w;
#ifdef WISP_HAS_HUD
        if (w && w->kind == W_HUD) hud_on_pointer_leave(w);
#endif
        ptr_focus = 0;
        break;
    }
    case 2: {  /* motion */
        if (bodylen < 12) return;
        int32_t fx = *(int32_t *)(body + 4), fy = *(int32_t *)(body + 8);
        ptr_x = fx >> 8; ptr_y = fy >> 8;
        Widget *w = widget_by_surface(ptr_focus);
        (void)w;
#ifdef WISP_HAS_HUD
        if (w && w->kind == W_HUD) hud_on_pointer_motion(w, ptr_x, ptr_y);
#endif
#ifdef WISP_HAS_BAR
        if (w && (w->kind == W_BAR || w->kind == W_HUD))
            bar_input_motion(w, ptr_x, ptr_y);
#endif
#ifdef WISP_HAS_MENU
        if (w && w->kind == W_MENU) menu_on_hover(w, ptr_x, ptr_y);
#endif
        break;
    }
    case 3: {  /* button */
        if (bodylen < 16) return;
        uint32_t button = *(uint32_t *)(body + 8);
        uint32_t state  = *(uint32_t *)(body + 12);
        Widget *w = widget_by_surface(ptr_focus);
        if (!w) return;
        (void)button; (void)state;
#ifdef WISP_HAS_MENU
        /* Click-off: a press on any non-menu surface dismisses an open menu. */
        if (state == 1 && w->kind != W_MENU) {
            menu_clickoff = 1;
            menu_cancel_all();
            menu_clickoff = 0;
        }
#endif
#ifdef WISP_HAS_HUD
        if (w->kind == W_HUD) { hud_on_pointer_button(w, button, state); break; }
#endif
#ifdef WISP_HAS_MENU
        if (w->kind == W_MENU && state == 1 && button == 0x110) {
            menu_on_click(w, ptr_x, ptr_y); break;
        }
#endif
#ifdef WISP_HAS_OSD
        if (w->kind == W_OSD && state == 1 && button == 0x110) {
            osd_on_click(w, ptr_x, ptr_y); break;
        }
#endif
#ifdef WISP_HAS_BAR
        /* BTN_RIGHT/BTN_MIDDLE only ever click: no press/release, so they
         * can't start a slider drag or paint press_bg. */
        if (w->kind == W_BAR && (button == 0x111 || button == 0x112)) {
            if (state == 1) bar_input_click(w, ptr_x, ptr_y, (int)button);
            break;
        }
        if (w->kind == W_BAR && button == 0x110) {
            if (state == 1) {
                bar_input_press(w, ptr_x, ptr_y, (int)button);
                bar_input_click(w, ptr_x, ptr_y, (int)button);
            } else {
                bar_input_release(w, ptr_x, ptr_y, (int)button);
            }
            /* Mark bar dirty so press_bg / release transitions paint next frame.
             * Weak ref: configs without a "bar" surface still link cleanly;
             * the branch above gates on W_BAR so this code is dead in that
             * case but the linker still resolves the symbol. */
            extern int dirty_bar __attribute__((weak));
            if (&dirty_bar) dirty_bar = 1;
            break;
        }
#endif
        break;
    }
    case 4: {  /* axis: time(4) axis(4) value(fixed 24.8) */
#ifdef WISP_HAS_MENU
        if (bodylen >= 12) {
            Widget *w = widget_by_surface(ptr_focus);
            if (w && w->kind == W_MENU) {
                int32_t v = *(int32_t *)(body + 8);
                if (v) menu_on_scroll(w, v > 0 ? 1 : -1);
            }
        }
#endif
        break;
    }
    default: break;
    }
}

void on_keyboard_event(uint16_t op, uint8_t *body, uint32_t bodylen) {
    switch (op) {
    case 0: {  /* keymap: format(4) fd(via cmsg) size(4) */
        /* Claim the fd first: a short keymap event must not orphan it in
         * pending_fds where the next fd-bearing event would misattribute it. */
        int fd = wl_take_pending_fd();
        if (bodylen < 8) { if (fd >= 0) close(fd); return; }
        uint32_t format = *(uint32_t *)body;
        uint32_t size   = *(uint32_t *)(body + 4);
        if (fd >= 0) {
            if (format == 1) xkb_load(fd, size);
            close(fd);
        }
        break;
    }
    case 1: if (bodylen < 8) return; kbd_focus = *(uint32_t *)(body + 4); break;
    case 2: {  /* leave: serial, surface */
#ifdef WISP_HAS_MENU
        /* Click-off on another app's window: the compositor moves keyboard
         * focus away (menu is kbd-on_demand where supported) — treat losing
         * focus as dismissal. A menu closing itself is already destroyed by
         * the time its leave arrives, so widget_by_surface misses it. */
        if (bodylen >= 8) {
            Widget *lw = widget_by_surface(*(uint32_t *)(body + 4));
            /* Not a dismissal if the pointer sits on the menu itself: mango's
             * focuslayer bounces kbd focus (leave+enter) on every press over an
             * on_demand layer, and closing here would eat the click. */
            /* Only dropdowns dismiss on focus loss: full menus have their own
             * closers (pick/esc/closed), and pointer-follows-focus compositors
             * bounce kbd focus on mere hover, which would kill them. */
            if (lw && lw->kind == W_MENU && lw->s.menu.anchored
                && lw->surface != ptr_focus)
                menu_reply_and_close(lw, -1);
        }
#endif
        kbd_focus = 0; key_rep_cancel(); break;
    }
    case 3: {
        if (bodylen < 16) return;
        uint32_t key   = *(uint32_t *)(body + 8);
        uint32_t state = *(uint32_t *)(body + 12);
        Widget *w = widget_by_surface(kbd_focus);
        if (!w) return;
        (void)key; (void)state;
#ifdef WISP_HAS_MENU
        if (w->kind == W_MENU) {
            menu_on_key(w, key, state);
            if (state == 1) key_rep_arm(key);
            else if (key == key_rep_key) key_rep_cancel();
            break;
        }
#endif
#ifdef WISP_HAS_LOCK
        if (w->kind == W_LOCK) {
            lock_on_key(w, key, state, 0);
            if (state == 1) key_rep_arm(key);
            else if (key == key_rep_key) key_rep_cancel();
            break;
        }
#endif
        break;
    }
    case 4: {  /* modifiers */
        if (bodylen < 16) return;
        uint32_t dep  = *(uint32_t *)(body + 4);
        uint32_t lat  = *(uint32_t *)(body + 8);
        uint32_t lck  = *(uint32_t *)(body + 12);
        xkb_on_modifiers(dep, lat, lck);
        break;
    }
    case 5: {  /* repeat_info */
        if (bodylen < 8) return;
        int32_t rate  = *(int32_t *)body;
        int32_t delay = *(int32_t *)(body + 4);
        if (rate > 0)  key_rep_rate_ms  = 1000 / rate;
        if (delay > 0) key_rep_delay_ms = delay;
        break;
    }
    default: break;
    }
}

/* Per-kind repaint. Shared by configure and the runtime output-scale change,
 * which has to redraw without a configure ever arriving. */
void widget_repaint(Widget *w, int first_configure) {
    (void)first_configure;
#ifdef WISP_HAS_BAR
    if (w->kind == W_BAR) bar_render(w);
#endif
#ifdef WISP_HAS_WALL
    if (w->kind == W_WALL) wall_render(w);
#endif
#ifdef WISP_HAS_MENU
    if (w->kind == W_MENU) menu_render(w);
#endif
#ifdef WISP_HAS_OSD
    if (w->kind == W_OSD) {
        if (first_configure) osd_on_first_configure(w);
        osd_render(w);
    }
#endif
#ifdef WISP_HAS_HUD
    if (w->kind == W_HUD) {
        /* Ensure BEFORE taking a slot: a configure can resize the surface (e.g.
         * a reload-adopted widget whose pool is still the old preset's size),
         * and memsetting a stale-size slot writes past its mapping. */
        widget_ensure_pool(w, 2);
        BufSlot *s = widget_free_slot(w);
        if (s) {
            memset(s->px, 0, (size_t)widget_pw(w) * widget_ph(w) * 4);
            widget_attach(w, s, 1);
            w->want_pool_free = 1;
        }
    }
#endif
}

/* wl_output.scale changed after startup: restamp every widget on that output,
 * drop pools sized for the old scale, repaint. Rare (a monitorrule reload). */
void widget_rescale_output(Output *o) {
    if (compositor_ver < 3) return;
    for (int i = 0; i < MAX_WIDGETS; i++) {
        Widget *w = &widgets[i];
        if (w->kind == W_NONE || w->output != o) continue;
        widget_set_scale(w, o->scale120);
    }
}

/* One widget's scale changed (output scale, or a per-surface preferred_scale
 * under WISP_FRACTIONAL). Drop pools sized for the old scale, repaint. */
void widget_set_scale(Widget *w, int s120) {
    if (s120 < 120) s120 = 120;
    if (s120 > 480) s120 = 480;   /* same clamp as the integer path */
    if (w->kind == W_NONE || w->scale120 == s120) return;
    w->scale120 = s120;
    widget_free_pool(w);
#ifdef WISP_HAS_WALL
    if (w->kind == W_WALL) w->s.wall.painted_w = 0;   /* force re-decode */
#endif
    if (w->configured) widget_repaint(w, 0);
}

void on_ls_event(Widget *w, uint16_t op, uint8_t *body, uint32_t bodylen) {
    if (op == LS_EV_CONFIGURE) {
        if (bodylen < 12) return;
        uint32_t serial = *(uint32_t *)body;
        uint32_t nw = *(uint32_t *)(body + 4);
        uint32_t nh = *(uint32_t *)(body + 8);
        /* Clamp compositor-supplied dims: widget_ensure_pool sizes the memfd
         * with int math (w*h*4*slots); an absurd configure would overflow to a
         * tiny mapping that render then writes past. No real output exceeds 16k. */
        uint32_t sc = (uint32_t)(w->scale120 > 0 ? (w->scale120 + 119) / 120 : 1);
        if (nw * sc > 16384 || nh * sc > 16384)
            die("layer_surface configure %ux%u (scale %u) too large", nw, nh, sc);
        if (nw) w->w = nw;
        if (nh) w->h = nh;
        wl_req(w->layer_surface, LS_REQ_ACK_CONFIGURE, &serial, 1, -1);
        int first_configure = !w->configured;
        w->configured = 1;
        widget_repaint(w, first_configure);
        return;
    }
    if (op == LS_EV_CLOSED) {
#ifdef WISP_HAS_MENU
        if (w->kind == W_MENU) { menu_reply_and_close(w, -1); return; }
#endif
        widget_destroy(w);
        return;
    }
}

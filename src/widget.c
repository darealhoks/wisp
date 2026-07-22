/* Widget abstraction: surface + layer-surface + SHM pool + ping-pong buffers. */
#include "wisp.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>

Widget widgets[MAX_WIDGETS];

Widget *widget_alloc(WidgetKind k) {
    for (int i = 0; i < MAX_WIDGETS; i++)
        if (widgets[i].kind == W_NONE) {
            memset(&widgets[i], 0, sizeof widgets[i]);
            widgets[i].kind = k;
            widgets[i].pool_fd = -1;
            widgets[i].client_fd = -1;
            return &widgets[i];
        }
    return NULL;
}

Widget *widget_by_surface(uint32_t sid) {
    if (!sid) return NULL;
    for (int i = 0; i < MAX_WIDGETS; i++)
        if (widgets[i].kind != W_NONE && widgets[i].surface == sid)
            return &widgets[i];
    return NULL;
}
Widget *widget_by_ls(uint32_t lsid) {
    if (!lsid) return NULL;
    for (int i = 0; i < MAX_WIDGETS; i++)
        if (widgets[i].kind != W_NONE && widgets[i].layer_surface == lsid)
            return &widgets[i];
    return NULL;
}
Widget *widget_by_slock_surf(uint32_t id) {
    if (!id) return NULL;
    for (int i = 0; i < MAX_WIDGETS; i++)
        if (widgets[i].kind == W_LOCK && widgets[i].s.lock.slock_surf_id == id)
            return &widgets[i];
    return NULL;
}
Widget *widget_first(WidgetKind k) {
    for (int i = 0; i < MAX_WIDGETS; i++)
        if (widgets[i].kind == k) return &widgets[i];
    return NULL;
}

void widget_free_pool(Widget *w) {
    for (int i = 0; i < w->n_slots; i++) {
        if (w->slots[i].id) {
            wl_req(w->slots[i].id, BUFFER_REQ_DESTROY, NULL, 0, -1);
            w->slots[i].id = 0;
        }
    }
    if (w->id_pool) { wl_req(w->id_pool, POOL_REQ_DESTROY, NULL, 0, -1); w->id_pool = 0; }
    if (w->shm_base) { munmap(w->shm_base, w->pool_size); w->shm_base = NULL; }
    if (w->pool_fd >= 0) { close(w->pool_fd); w->pool_fd = -1; }
    w->pool_size = 0;
    w->n_slots = 0;
}

static void region_destroy(uint32_t rid) {
    if (rid) wl_req(rid, REGION_REQ_DESTROY, NULL, 0, -1);
}

void widget_destroy(Widget *w) {
    if (w->kind == W_NONE) return;
    /* Deregister from any codegen-emitted `__<surface>_widgets[]` registry
     * before teardown, so a subsequent redraw/tag fanout can't dereference this
     * now-freed widget (the monitor-disconnect crash). Weak: presets without
     * generated surfaces link cleanly with this resolving to NULL. */
    extern void wispgen_widget_destroyed(Widget *w) __attribute__((weak));
    if (wispgen_widget_destroyed) wispgen_widget_destroyed(w);
    /* Cancel any owner-keyed tweens (item slide/reveal) so they don't keep
     * firing renders on this slot after it's freed/reused. Weak: lock build
     * links no anim.o. */
    extern void anim_cancel_owner(Widget *w) __attribute__((weak));
    if (anim_cancel_owner) anim_cancel_owner(w);
#ifdef WISP_HAS_WALL
    if (w->kind == W_WALL) wall_fade_cancel(w);   /* free heap fade frames */
#endif
    widget_free_pool(w);
    region_destroy(w->input_region_id);
    w->input_region_id = 0;
    if (w->kind == W_HUD) {
        region_destroy(w->s.hud.region_trigger);
        region_destroy(w->s.hud.region_full);
        w->s.hud.region_trigger = w->s.hud.region_full = 0;
    }
    if (w->kind == W_LOCK && w->s.lock.slock_surf_id) {
        wl_req(w->s.lock.slock_surf_id, SLOCK_SURF_REQ_DESTROY, NULL, 0, -1);
        w->s.lock.slock_surf_id = 0;
    }
    if (w->kind == W_MENU) {
        free(w->s.menu.items);    w->s.menu.items = NULL;
        free(w->s.menu.filtered); w->s.menu.filtered = NULL;
    }
    if (w->layer_surface) {
        wl_req(w->layer_surface, LS_REQ_DESTROY, NULL, 0, -1);
        w->layer_surface = 0;
    }
#ifdef WISP_FRACTIONAL
    if (w->frac_scale) { wl_req(w->frac_scale, FRAC_REQ_DESTROY, NULL, 0, -1); w->frac_scale = 0; }
    if (w->viewport)   { wl_req(w->viewport, VIEWPORT_REQ_DESTROY, NULL, 0, -1); w->viewport = 0; }
    w->sent_dw = w->sent_dh = 0;
#endif
    if (w->surface) {
        wl_req(w->surface, SURFACE_REQ_DESTROY, NULL, 0, -1);
        w->surface = 0;
    }
    if (w->client_fd >= 0) { close(w->client_fd); w->client_fd = -1; }
    /* Clear back-pointer from Output if this widget owned a slot there. */
    if (w->output) {
        Output *o = w->output;
        if (o->bar  == w) o->bar  = NULL;
        if (o->wall == w) o->wall = NULL;
        if (o->hud  == w) o->hud  = NULL;
        if (o->lock == w) o->lock = NULL;
        w->output = NULL;
    }
    /* Make this slot inert for any stale event (frame callback, buffer release,
     * dwl-ipc tag fanout) that reaches it before the slot is reused: every
     * generated render_<surface>() early-returns on !configured || w<=0. */
    w->configured = 0;
    w->w = w->h = 0;
    w->kind = W_NONE;
}

#ifdef WISP_FRACTIONAL
/* Both halves are required: without a viewport a non-integer buffer size has
 * no legal logical size, so we take neither or both. */
void widget_frac_attach(Widget *w) {
    if (!id_viewporter || !id_frac_mgr || !w->surface || w->viewport) return;
    w->viewport = wl_new_id();
    { uint32_t a[2] = { w->viewport, w->surface };
      wl_req(id_viewporter, VIEWPORTER_REQ_GET_VIEWPORT, a, 2, -1); }
    w->frac_scale = wl_new_id();
    { uint32_t a[2] = { w->frac_scale, w->surface };
      wl_req(id_frac_mgr, FRAC_MGR_REQ_GET_FRACTIONAL_SCALE, a, 2, -1); }
}

Widget *widget_by_frac(uint32_t obj) {
    if (!obj) return NULL;
    for (int i = 0; i < MAX_WIDGETS; i++)
        if (widgets[i].kind != W_NONE && widgets[i].frac_scale == obj) return &widgets[i];
    return NULL;
}
#endif

void widget_setup_surface(Widget *w, uint32_t layer, const char *ns, Output *o) {
    w->output        = o;
    /* Output-agnostic surfaces (menu/osd) follow the focused output. */
    w->scale120      = o ? o->scale120 : (focused_output ? focused_output->scale120 : 120);
    if (w->scale120 < 120) w->scale120 = 120;
    if (compositor_ver < 3) w->scale120 = 120;   /* set_buffer_scale is wl_surface v3 */
    /* Reload path: reuse the pre-exec process's mapped surface instead of
     * creating one — no unmap/remap, so nothing for the compositor to animate. */
    if (o && wl_take_adopted(w, o)) return;
    w->surface       = wl_new_id();
    w->layer_surface = wl_new_id();
    uint32_t sa = w->surface;
    wl_req(id_compositor, COMPOSITOR_REQ_CREATE_SURFACE, &sa, 1, -1);
#ifdef WISP_FRACTIONAL
    widget_frac_attach(w);
#endif
    uint32_t b[64];
    /* layer-shell.get_layer_surface(new_id, surface, output, layer, ns) — wire
       order: id, surface, output, layer, ns. Manual build because mid-position
       string + uint32 layer. Output is mandatory on dwl's layer-shell v3. */
    b[0] = id_layer_shell;
    b[2] = w->layer_surface;
    b[3] = w->surface;
    b[4] = o ? o->wl_output : 0;
    b[5] = layer;
    int sl = (int)strlen(ns) + 1, pl = pad4(sl);
    b[6] = sl;
    memset((char *)&b[7], 0, pl);
    memcpy((char *)&b[7], ns, sl);
    int p = 7 + pl / 4;
    uint32_t size = p * 4;
    b[1] = (size << 16) | LAYER_SHELL_REQ_GET_LAYER_SURFACE;
    wl_send(b, size, -1);
}

void widget_set_size(Widget *w, int width, int height) {
    uint32_t a[2] = { (uint32_t)width, (uint32_t)height };
    wl_req(w->layer_surface, LS_REQ_SET_SIZE, a, 2, -1);
}
void widget_set_anchor(Widget *w, uint32_t bits) {
    wl_req(w->layer_surface, LS_REQ_SET_ANCHOR, &bits, 1, -1);
}
void widget_set_exclusive_zone(Widget *w, int zone) {
    uint32_t a = (uint32_t)zone; wl_req(w->layer_surface, LS_REQ_SET_EXCLUSIVE_ZONE, &a, 1, -1);
}
void widget_set_margin(Widget *w, int top, int right, int bot, int left) {
    uint32_t a[4] = { (uint32_t)top, (uint32_t)right, (uint32_t)bot, (uint32_t)left };
    wl_req(w->layer_surface, LS_REQ_SET_MARGIN, a, 4, -1);
}
void widget_set_kbd_interactive(Widget *w, int on) {
    uint32_t a = on; wl_req(w->layer_surface, LS_REQ_SET_KEYBOARD_INTERACTIVITY, &a, 1, -1);
}
void widget_set_input_region(Widget *w, uint32_t region_id) {
    uint32_t a = region_id;
    wl_req(w->surface, /*SET_INPUT_REGION=*/5, &a, 1, -1);
}

/* compositor.create_region + region.add(x,y,w,h). Returns new region id.
 * Caller is responsible for destroying the region (either explicitly or via
 * widget_set_input_region_rect, which manages a single live region per widget). */
uint32_t widget_make_region(int x, int y, int w, int h) {
    uint32_t rid = wl_new_id();
    uint32_t a = rid;
    wl_req(id_compositor, COMPOSITOR_REQ_CREATE_REGION, &a, 1, -1);
    uint32_t args[4] = { (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h };
    wl_req(rid, REGION_REQ_ADD, args, 4, -1);
    return rid;
}

void widget_set_input_region_rect(Widget *w, int x, int y, int ww, int hh) {
    uint32_t rid = widget_make_region(x, y, ww, hh);
    widget_set_input_region(w, rid);
    region_destroy(w->input_region_id);
    w->input_region_id = rid;
}

/* Multi-rect input region: one wl_region with N rects added. The previous
 * auto-managed input region (if any) is destroyed. */
void widget_set_input_region_multi(Widget *w, const Rect *rects, int n) {
    uint32_t rid = wl_new_id();
    uint32_t a = rid;
    wl_req(id_compositor, COMPOSITOR_REQ_CREATE_REGION, &a, 1, -1);
    for (int i = 0; i < n; i++) {
        uint32_t args[4] = { (uint32_t)rects[i].x, (uint32_t)rects[i].y,
                             (uint32_t)rects[i].w, (uint32_t)rects[i].h };
        wl_req(rid, REGION_REQ_ADD, args, 4, -1);
    }
    widget_set_input_region(w, rid);
    region_destroy(w->input_region_id);
    w->input_region_id = rid;
}

void widget_ensure_pool(Widget *w, int n_slots) {
    if (w->w <= 0 || w->h <= 0 || n_slots <= 0) return;
    if (w->scale120 < 120) w->scale120 = 120;
    int pw = widget_pw(w), ph = widget_ph(w);
    int stride = pw * 4;
    int one = stride * ph;
    int total = one * n_slots;
    if (w->pool_size == total && w->n_slots == n_slots) return;
    widget_free_pool(w);

    w->pool_fd = syscall(SYS_memfd_create, "wisp-w", 1u);
    if (w->pool_fd < 0) die("memfd_create: %s", strerror(errno));
    if (ftruncate(w->pool_fd, total) < 0) die("ftruncate: %s", strerror(errno));
    w->shm_base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, w->pool_fd, 0);
    if (w->shm_base == MAP_FAILED) die("mmap: %s", strerror(errno));
    w->pool_size = total;

    w->id_pool = wl_new_id();
    uint32_t pa[2] = { w->id_pool, (uint32_t)total };
    wl_req(id_shm, SHM_REQ_CREATE_POOL, pa, 2, w->pool_fd);
    w->n_slots = n_slots;
    for (int i = 0; i < n_slots; i++) {
        w->slots[i].id = wl_new_id();
        w->slots[i].px = (uint32_t *)(w->shm_base + one * i);
        w->slots[i].busy = 0;
        w->slots[i].off = one * i;
        uint32_t ba[6] = { w->slots[i].id, (uint32_t)w->slots[i].off,
                           (uint32_t)pw, (uint32_t)ph,
                           (uint32_t)stride, WL_SHM_FORMAT_ARGB8888 };
        wl_req(w->id_pool, POOL_REQ_CREATE_BUFFER, ba, 6, -1);
    }
}

BufSlot *widget_free_slot(Widget *w) {
    /* Every render path acquires its buffer here, so this is the one choke
     * point where the primitives' logical->physical scale can be armed
     * without touching each (partly generated) render entry point. */
    render_set_scale(w->scale120);
    for (int i = 0; i < w->n_slots; i++)
        if (!w->slots[i].busy) return &w->slots[i];
    return NULL;
}

void widget_attach(Widget *w, BufSlot *s, int request_frame) {
#ifdef WISP_FRACTIONAL
    /* buffer_scale stays 1 here; the viewport maps the physical buffer back
     * onto the logical size, which is what makes a non-integer scale legal. */
    if (w->viewport && (w->sent_dw != w->w || w->sent_dh != w->h)) {
        uint32_t d[2] = { (uint32_t)w->w, (uint32_t)w->h };
        wl_req(w->viewport, VIEWPORT_REQ_SET_DESTINATION, d, 2, -1);
        w->sent_dw = w->w; w->sent_dh = w->h;
    }
    if (!w->viewport)
#endif
    /* Must accompany the first buffer of a new scale, or the compositor reads
     * the physical buffer as logical and shows a surface scale× too big. */
    if (w->sent_scale120 != w->scale120 && compositor_ver >= 3) {
        uint32_t sc = (uint32_t)(w->scale120 / 120);
        wl_req(w->surface, SURFACE_REQ_SET_BUFFER_SCALE, &sc, 1, -1);
        w->sent_scale120 = w->scale120;
    }
    uint32_t at[3] = { s->id, 0, 0 };
    wl_req(w->surface, SURFACE_REQ_ATTACH, at, 3, -1);
    uint32_t dm[4] = { 0, 0, (uint32_t)w->w, (uint32_t)w->h };
    wl_req(w->surface, SURFACE_REQ_DAMAGE, dm, 4, -1);
    if (request_frame && !w->frame_cb) {
        w->frame_cb = wl_new_id();
        uint32_t a = w->frame_cb;
        wl_req(w->surface, SURFACE_REQ_FRAME, &a, 1, -1);
    }
    wl_req(w->surface, SURFACE_REQ_COMMIT, NULL, 0, -1);
    s->busy = 1;
    /* Reset any pending "free pool when idle" flag — the surface is no longer
     * idle. Callers that want a one-shot free re-set the flag *after* this
     * returns. Doing it here (rather than in widget_ensure_pool) avoids
     * stranding the flag when a render path early-returns without attaching. */
    w->want_pool_free = 0;
}

/* Frame callback dispatch. Generic widget plumbing: any kind that requested
 * a frame via widget_attach(_, _, 1) lands here on .done. HUD piggybacks for
 * its slide animation; other widgets only use it to drive want_pool_free. */
void on_frame_done(Widget *w, uint32_t cb_id) {
    if (w->frame_cb == cb_id) w->frame_cb = 0;
#ifdef WISP_HAS_HUD
    if (w->kind == W_HUD && w->s.hud.animating) {
        hud_tick(w, now_ms());
        return;
    }
#endif
    extern void osd_tick(Widget *w) __attribute__((weak));
    if (w->kind == W_OSD && osd_tick) osd_tick(w);
    /* Fall through (don't return) so the frame callback can drive the one-shot
     * pool free the comment in osd.c promises, instead of relying solely on
     * buffer-release. If osd_tick re-attached, widget_attach cleared the flag.
     * Only free once every slot is released — freeing a busy slot would yank
     * the pool out from under the compositor (see on_buffer_release). */
    if (w->want_pool_free) {
        int all_free = 1;
        for (int k = 0; k < w->n_slots; k++)
            if (w->slots[k].busy) { all_free = 0; break; }
        if (all_free) {
            w->want_pool_free = 0;
            widget_free_pool(w);
        }
    }
#ifdef WISP_HAS_WALL
    /* A deferred reload waits here, not on the fade's anim-done: that callback
     * only *attaches* the final frame. Exec'ing before the compositor has
     * presented it destroys the surface holding it, putting back the flash the
     * fade exists to hide. */
    if (reload_pending && w->kind == W_WALL && !wall_fade_active())
        ctl_reload_exec();
#endif
}

/* Buffer release: clear busy flag for matching slot. If the widget has been
 * marked for pool teardown (e.g. OSD after the last slab dismissed), free the
 * pool once every slot has been returned by the compositor. */
void on_buffer_release(uint32_t buf_id) {
    for (int i = 0; i < MAX_WIDGETS; i++) {
        Widget *w = &widgets[i];
        if (w->kind == W_NONE) continue;
        for (int j = 0; j < w->n_slots; j++)
            if (w->slots[j].id == buf_id) {
                w->slots[j].busy = 0;
                if (w->want_pool_free) {
                    int all_free = 1;
                    for (int k = 0; k < w->n_slots; k++)
                        if (w->slots[k].busy) { all_free = 0; break; }
                    if (all_free) {
                        w->want_pool_free = 0;
                        widget_free_pool(w);
                    }
                }
                return;
            }
    }
}

/* Surface cutout registry. Lets one surface (e.g. an OSD slab) punch a
 * transparent rect through another surface's painted body (e.g. the bar
 * strip directly underneath it), so a translucent surface stacked over
 * another translucent surface doesn't double-blend.
 *
 * Source surfaces call cutout_set/cutout_clear with the target's DSL
 * surface name; target surfaces invoke cutout_apply(self_name, ...) after
 * rendering. Codegen emits the apply call automatically; hand-written
 * runtime modules can call it too. */
#define CUTOUT_MAX 4
#define CUTOUT_NAME_MAX 24
#define CUTOUT_X_CENTER 0x7fffffff  /* sentinel: center horizontally in target */
static struct {
    char    target[CUTOUT_NAME_MAX];
    Output *scope;          /* NULL = applies on every output */
    int     active, x, y, w, h;
} cuts[CUTOUT_MAX];

static int cutout_find(const char *target, Output *scope) {
    for (int i = 0; i < CUTOUT_MAX; i++)
        if (cuts[i].active
            && cuts[i].scope == scope
            && strncmp(cuts[i].target, target, CUTOUT_NAME_MAX) == 0)
            return i;
    return -1;
}

void cutout_set(const char *target, Output *scope, int x, int y, int w, int h) {
    int i = cutout_find(target, scope);
    if (i < 0) for (i = 0; i < CUTOUT_MAX; i++) if (!cuts[i].active) break;
    if (i >= CUTOUT_MAX) return;
    snprintf(cuts[i].target, CUTOUT_NAME_MAX, "%s", target);
    cuts[i].active = 1;
    cuts[i].scope = scope;
    cuts[i].x = x; cuts[i].y = y; cuts[i].w = w; cuts[i].h = h;
}

void cutout_clear(const char *target, Output *scope) {
    int i = cutout_find(target, scope);
    if (i >= 0) cuts[i].active = 0;
}

/* Drop every cutout scoped to an output that is going away. Without this the
 * stale Output* in cuts[].scope would alias a different monitor once the slot
 * in outputs[] is reused by a hotplug, punching a hole in the wrong surface. */
void cutout_drop_output(Output *o) {
    if (!o) return;
    for (int i = 0; i < CUTOUT_MAX; i++)
        if (cuts[i].active && cuts[i].scope == o) cuts[i].active = 0;
}

void cutout_apply(const char *self, Output *self_out, uint32_t *px, int sw, int sh) {
    if (!self || !px) return;
    for (int i = 0; i < CUTOUT_MAX; i++) {
        if (!cuts[i].active) continue;
        if (strncmp(cuts[i].target, self, CUTOUT_NAME_MAX) != 0) continue;
        if (cuts[i].scope && cuts[i].scope != self_out) continue;
        int cw = cuts[i].w, ch = cuts[i].h;
        int cx = cuts[i].x == CUTOUT_X_CENTER ? (sw - cw) / 2 : cuts[i].x;
        int cy = cuts[i].y;
        if (cx < 0) { cw += cx; cx = 0; }
        if (cy < 0) { ch += cy; cy = 0; }
        if (cx + cw > sw) cw = sw - cx;
        if (cy + ch > sh) ch = sh - cy;
        if (cw <= 0 || ch <= 0) continue;
        for (int j = 0; j < ch; j++) {
            uint32_t *row = px + (cy + j) * sw + cx;
            for (int k = 0; k < cw; k++) row[k] = 0;
        }
    }
}

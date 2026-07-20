/* Wallpaper. Background-layer surface filling the bound output; decodes
 * WALL_PATH at configure time, bilinear cover-fit (max scale, center-crop),
 * blits once. Falls back to WALL_BG solid fill if the file is missing.
 *
 * Source pixels are freed after the blit — peak RSS during startup, steady-
 * state cost is just the SHM (out_w*out_h*4) same as swaybg. */

#include "wisp.h"
#include "image.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void wall_render(Widget *w) {
    if (!w->configured || w->w <= 0 || w->h <= 0) return;
    /* The compositor re-sends a configure to every layer surface on any layout
     * change (e.g. an OSD overlay appearing after idle). Re-decoding the PNG +
     * cover-scaling is ~150ms and blocks the whole event loop — which stalls
     * OSD slide-in animations. The wallpaper only changes with the output size,
     * so skip the repaint when it hasn't (the GPU keeps our one-shot texture). */
    if (w->s.wall.painted_w == w->w && w->s.wall.painted_h == w->h) return;
    widget_ensure_pool(w, 1);
    BufSlot *s = widget_free_slot(w);
    if (!s) return;

    /* Fast path: the disk cache already holds this exact output size, scaled
     * from the current file. A 4K source decodes in ~150ms; a read() of the
     * finished w*h buffer is a few ms, and it is what we'd have produced. */
    int pw = widget_pw(w), ph = widget_ph(w);
    /* The disk cache keys on dst size, so a scaled output just caches the
     * physical variant — no separate key needed. */
    uint32_t *cached = image_bgcache_load(WALL_PATH, pw, ph);
    if (cached) {
        memcpy(s->px, cached, (size_t)pw * ph * 4);
        free(cached);
        w->s.wall.painted_w = w->w;
        w->s.wall.painted_h = w->h;
        widget_attach(w, s, 1);
        w->want_pool_free = 1;
        return;
    }

    int sw = 0, sh = 0;
    uint8_t *src = image_load(WALL_PATH, &sw, &sh);

    if (src && sw > 1 && sh > 1) {
        image_blit_cover(s->px, pw, ph, src, sw, sh);
        image_free(src);
        /* Seed wisp-lock's background cache with the buffer we just built —
         * the lock shows the same unmodified wallpaper, so its first load
         * becomes a plain read() instead of a decode + scale. No-op when the
         * cache is already current. */
        image_bgcache_store(WALL_PATH, pw, ph, s->px);
        w->s.wall.painted_w = w->w;
        w->s.wall.painted_h = w->h;
    } else {
        clear_buf(s->px, w->w, w->h, WALL_BG);   /* clear_buf takes logical dims */
        msg("wisp: wallpaper decode failed (%s)", WALL_PATH);
    }
    /* Wallpaper attaches exactly once. The compositor will not fire
     * wl_buffer.release until a different buffer is attached — and we never
     * attach again — so we instead piggyback on wl_surface.frame: once
     * frame.done arrives we know the compositor has displayed (and uploaded)
     * this buffer at least once, so destroying the buffer + pool and
     * munmapping our SHM view is safe. The compositor keeps a GPU texture
     * reference, so the wallpaper stays on screen. on_frame_done (hud.c)
     * does the actual free. Saves ~8 MB on a 1080p output. */
    widget_attach(w, s, 1);
    w->want_pool_free = 1;

    /* Force glibc to return freed PNG-decode pages to the kernel — stb_image's
     * intermediate buffers can be tens of MB for high-res sources. */
    malloc_trim(0);
}

void wall_create_on(Output *o) {
    if (!o || o->wall) return;
    Widget *w = widget_alloc(W_WALL);
    if (!w) { msg("wisp: no widget slot for wall"); return; }
    o->wall = w;
    widget_setup_surface(w, LAYER_BACKGROUND, "wisp-wall", o);
    widget_set_anchor(w, LS_ANCHOR_TOP | LS_ANCHOR_BOTTOM
                      | LS_ANCHOR_LEFT | LS_ANCHOR_RIGHT);
    widget_set_size(w, 0, 0);
    widget_set_exclusive_zone(w, -1);  /* fill output regardless of bar */
    widget_set_kbd_interactive(w, 0);
    widget_set_input_region_rect(w, 0, 0, 0, 0);  /* click-through */
    wl_req(w->surface, SURFACE_REQ_COMMIT, NULL, 0, -1);
}

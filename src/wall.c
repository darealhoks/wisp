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

/* Runtime override from `wispctl wall <path>`; empty = the .wisp WALL_PATH. */
static char wall_path_rt[512];
static const char *wall_path(void) {
    return wall_path_rt[0] ? wall_path_rt : WALL_PATH;
}

void wall_render(Widget *w) {
    if (!w->configured || w->w <= 0 || w->h <= 0) return;
    /* The compositor re-sends a configure to every layer surface on any layout
     * change (e.g. an OSD overlay appearing after idle). Re-decoding the PNG +
     * cover-scaling is ~150ms and blocks the whole event loop — which stalls
     * OSD slide-in animations. The wallpaper only changes with the output size,
     * so skip the repaint when it hasn't (the GPU keeps our one-shot texture). */
    if (w->s.wall.painted_w == w->w && w->s.wall.painted_h == w->h) return;
    wall_fade_cancel(w);   /* size changed mid-fade: the full repaint wins */
    widget_ensure_pool(w, 1);
    BufSlot *s = widget_free_slot(w);
    if (!s) return;

    /* Fast path: the disk cache already holds this exact output size, scaled
     * from the current file. A 4K source decodes in ~150ms; a read() of the
     * finished w*h buffer is a few ms, and it is what we'd have produced. */
    int pw = widget_pw(w), ph = widget_ph(w);
    /* The disk cache keys on dst size, so a scaled output just caches the
     * physical variant — no separate key needed. */
    uint32_t *cached = image_bgcache_load(wall_path(), pw, ph);
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
    uint8_t *src = image_load(wall_path(), &sw, &sh);

    if (src && sw > 1 && sh > 1) {
        image_blit_cover(s->px, pw, ph, src, sw, sh);
        image_free(src);
        /* Seed wisp-lock's background cache with the buffer we just built —
         * the lock shows the same unmodified wallpaper, so its first load
         * becomes a plain read() instead of a decode + scale. No-op when the
         * cache is already current. */
        image_bgcache_store(wall_path(), pw, ph, s->px);
        w->s.wall.painted_w = w->w;
        w->s.wall.painted_h = w->h;
    } else {
        clear_buf(s->px, w->w, w->h, WALL_BG);   /* clear_buf takes logical dims */
        msg("wisp: wallpaper decode failed (%s)", wall_path());
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

/* --- runtime wallpaper switch with crossfade -------------------------------
 * Two finished cover-fit frames (old + new) live on the heap for the fade's
 * ~300 ms; each anim tick blends them into a free SHM slot. On done both
 * frames and the pool are freed, restoring the texture-only steady state. */

static void wall_blend(uint32_t *dst, const uint32_t *a, const uint32_t *b,
                       size_t n, int u) {   /* u: 0..255 */
    uint32_t iu = 255 - (uint32_t)u;
    for (size_t i = 0; i < n; i++) {
        uint32_t pa = a[i], pb = b[i];
        uint32_t rb = (((pa & 0x00ff00ffu) * iu + (pb & 0x00ff00ffu) * (uint32_t)u) >> 8) & 0x00ff00ffu;
        uint32_t g  = (((pa & 0x0000ff00u) * iu + (pb & 0x0000ff00u) * (uint32_t)u) >> 8) & 0x0000ff00u;
        dst[i] = 0xff000000u | rb | g;
    }
}

/* Block dissolve: each n×n block gets a fixed pseudo-random threshold and
 * flips to the new frame when t passes it. Cheaper than the blend (row
 * memcpy, no per-pixel math) and reads as a different transition, not a
 * dimmer fade. */
static void wall_dither(uint32_t *dst, const uint32_t *a, const uint32_t *b,
                        int w, int h, int u) {
    int bs = WALL_DITHER_PX < 1 ? 1 : WALL_DITHER_PX;
    for (int by = 0; by < h; by += bs) {
        int bh = by + bs > h ? h - by : bs;
        for (int bx = 0; bx < w; bx += bs) {
            int bw = bx + bs > w ? w - bx : bs;
            /* xorshift-ish mix of the block coords → threshold 0..255 */
            uint32_t k = (uint32_t)(bx / bs) * 73856093u ^ (uint32_t)(by / bs) * 19349663u;
            k ^= k >> 13; k *= 0x5bd1e995u; k ^= k >> 15;
            const uint32_t *src = (int)(k & 0xff) < u ? b : a;
            for (int y = by; y < by + bh; y++)
                memcpy(dst + (size_t)y * w + bx, src + (size_t)y * w + bx,
                       (size_t)bw * 4);
        }
    }
}

/* New-frame coverage at coordinate p for an edge at `edge`, soft px wide. */
static int wipe_cov(int p, int edge, int soft) {
    int d = edge - p;
    if (d <= 0) return 0;
    if (d >= soft) return 255;
    return d * 255 / soft;
}

/* Wipe: an edge sweeps along WALL_WIPE_DIR with a soft lerp band at its front.
 * Coverage is a function of x, y or x+y, so the band is always one contiguous
 * run per row — only those pixels do any math, the rest of the row is a memcpy
 * from one frame or the other. */
static void wall_wipe(uint32_t *dst, const uint32_t *a, const uint32_t *b,
                      int w, int h, int u) {
    int soft = WALL_WIPE_SOFT < 1 ? 1 : WALL_WIPE_SOFT;
    int dir = WALL_WIPE_DIR;
    int usex = dir != WALL_WIPE_DIR_DOWN && dir != WALL_WIPE_DIR_UP;
    int usey = dir != WALL_WIPE_DIR_RIGHT && dir != WALL_WIPE_DIR_LEFT;
    int fx = dir == WALL_WIPE_DIR_LEFT || dir == WALL_WIPE_DIR_DOWN_LEFT
             || dir == WALL_WIPE_DIR_UP_LEFT;
    int fy = dir == WALL_WIPE_DIR_UP || dir == WALL_WIPE_DIR_UP_RIGHT
             || dir == WALL_WIPE_DIR_UP_LEFT;
    int span = (usex ? w : 0) + (usey ? h : 0);
    /* Runs past the far edge by `soft` so the band is fully off-screen at u=255. */
    int edge = (int)(((long)(span + soft) * u) / 255);
    /* The retarget freeze composes in place (dst == a), so copies from `a` are
     * no-ops there and memcpy would be aliasing UB. */
    int alias = dst == a;

    for (int y = 0; y < h; y++) {
        uint32_t *d = dst + (size_t)y * w;
        const uint32_t *pa = a + (size_t)y * w, *pb = b + (size_t)y * w;
        int base = usey ? (fy ? h - 1 - y : y) : 0;
        if (!usex) {   /* row-constant coverage */
            int cov = wipe_cov(base, edge, soft);
            if      (cov == 0)   { if (!alias) memcpy(d, pa, (size_t)w * 4); }
            else if (cov == 255) memcpy(d, pb, (size_t)w * 4);
            else                 wall_blend(d, pa, pb, (size_t)w, cov);
            continue;
        }
        /* Band in unmirrored x: cov is 255 before `lo`, 0 from `hi` on. */
        int lo = edge - soft + 1 - base, hi = edge - base;
        if (lo < 0) lo = 0;
        if (lo > w) lo = w;
        if (hi < lo) hi = lo;
        if (hi > w) hi = w;
        if (!fx) {
            memcpy(d, pb, (size_t)lo * 4);
            for (int x = lo; x < hi; x++)
                wall_blend(d + x, pa + x, pb + x, 1, wipe_cov(base + x, edge, soft));
            if (!alias) memcpy(d + hi, pa + hi, (size_t)(w - hi) * 4);
        } else {
            if (!alias) memcpy(d, pa, (size_t)(w - hi) * 4);
            for (int x = w - hi; x < w - lo; x++)
                wall_blend(d + x, pa + x, pb + x, 1,
                           wipe_cov(base + w - 1 - x, edge, soft));
            memcpy(d + (w - lo), pb + (w - lo), (size_t)lo * 4);
        }
    }
}

/* The one f(from,to,t) seam — every transition type goes through here, so
 * the mid-transition retarget freeze in wall_start_fade generalizes for free. */
static void wall_compose(uint32_t *dst, const uint32_t *a, const uint32_t *b,
                         int w, int h, int u) {
    if (WALL_TRANSITION == WALL_TRANSITION_DITHER)    wall_dither(dst, a, b, w, h, u);
    else if (WALL_TRANSITION == WALL_TRANSITION_WIPE) wall_wipe(dst, a, b, w, h, u);
    else wall_blend(dst, a, b, (size_t)w * h, u);
}

void wall_fade_cancel(Widget *w) {
    if (!w->s.wall.fade_from && !w->s.wall.fade_to) return;
    anim_cancel_for(&w->s.wall.fade);
    free(w->s.wall.fade_from); free(w->s.wall.fade_to);
    w->s.wall.fade_from = w->s.wall.fade_to = NULL;
}

int wall_fade_active(void) {
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (outputs[i].active && outputs[i].wall && outputs[i].wall->s.wall.fade_to)
            return 1;
    return 0;
}

static int fade_u(double f) {
    int u = (int)(f * 255.0 + 0.5);
    return u < 0 ? 0 : u > 255 ? 255 : u;
}

/* Per-tick blend; called from anim_tick via the owner hook. */
void wall_fade_frame(Widget *w) {
    if (!w->s.wall.fade_from || !w->s.wall.fade_to) return;
    if (widget_pw(w) != w->s.wall.fade_w || widget_ph(w) != w->s.wall.fade_h) return;
    int u = fade_u(w->s.wall.fade);
    if (u == w->s.wall.fade_u_last) return;   /* quantized step unchanged: skip the full-buffer compose */
    BufSlot *s = widget_free_slot(w);
    if (!s) return;   /* both slots pending release; next tick */
    wall_compose(s->px, w->s.wall.fade_from, w->s.wall.fade_to,
                 w->s.wall.fade_w, w->s.wall.fade_h, u);
    w->s.wall.fade_u_last = u;
    widget_attach(w, s, 1);
}

static void wall_fade_done(void *user) {
    Widget *w = user;
    /* The tick blend tops out at 255/256 of the target; attach the exact
     * final frame before dropping everything. */
    BufSlot *s = widget_free_slot(w);
    if (s && w->s.wall.fade_to
        && widget_pw(w) == w->s.wall.fade_w && widget_ph(w) == w->s.wall.fade_h) {
        memcpy(s->px, w->s.wall.fade_to,
               (size_t)w->s.wall.fade_w * w->s.wall.fade_h * 4);
        widget_attach(w, s, 1);
    }
    free(w->s.wall.fade_from); free(w->s.wall.fade_to);
    w->s.wall.fade_from = w->s.wall.fade_to = NULL;
    w->s.wall.painted_w = w->w;
    w->s.wall.painted_h = w->h;
    w->want_pool_free = 1;   /* frame.done from the last attach frees the pool */
    malloc_trim(0);
}

/* Build a finished pw*ph frame of `path`: disk cache first, else decode +
 * cover-fit (and seed the cache so the lock/restart path stays cheap). */
static uint32_t *wall_frame_of(const char *path, int pw, int ph, int store) {
    uint32_t *px = image_bgcache_load(path, pw, ph);
    if (px) return px;
    int sw = 0, sh = 0;
    uint8_t *src = image_load(path, &sw, &sh);
    if (src && sw > 1 && sh > 1) {
        px = malloc((size_t)pw * ph * 4);
        if (px) {
            image_blit_cover(px, pw, ph, src, sw, sh);
            if (store) image_bgcache_store(path, pw, ph, px);
        }
    }
    image_free(src);
    return px;
}

/* Pass 1 of a switch: build both fade frames (the expensive decodes) without
 * starting the clock. Arming is a separate pass so a slow decode for one
 * output can't eat another output's already-running fade window. */
static void wall_fade_prepare(Widget *w, const char *oldpath) {
    if (!w->configured || w->w <= 0 || w->h <= 0) { w->s.wall.painted_w = 0; return; }
    int pw = widget_pw(w), ph = widget_ph(w);

    uint32_t *from;
    if (w->s.wall.fade_to) {
        /* Retarget mid-fade: freeze the current blend as the new start. */
        wall_compose(w->s.wall.fade_from, w->s.wall.fade_from, w->s.wall.fade_to,
                     pw, ph, fade_u(w->s.wall.fade));
        from = w->s.wall.fade_from;
        free(w->s.wall.fade_to); w->s.wall.fade_to = NULL;
    } else {
        /* Load `from` BEFORE the new frame is built: the bg disk cache is a
         * single file, so building the new frame evicts the old one. */
        from = wall_frame_of(oldpath, pw, ph, 0);
    }

    uint32_t *to = wall_frame_of(wall_path(), pw, ph, 1);
    malloc_trim(0);
    if (!to) {   /* validated in wall_set, so only a truly corrupt file */
        msg("wisp: wallpaper decode failed (%s)", wall_path());
        free(from); w->s.wall.fade_from = NULL;
        anim_cancel_for(&w->s.wall.fade);
        return;
    }
    if (!from) {   /* nothing to fade from: hard cut via the normal path */
        free(to);
        w->s.wall.painted_w = 0;
        wall_render(w);
        return;
    }
    w->s.wall.fade_from = from;
    w->s.wall.fade_to = to;
    w->s.wall.fade_w = pw;
    w->s.wall.fade_h = ph;
}

/* Pass 2: start the clock. No decode may happen between two arms — every
 * output's fade must share the same wall-time window. */
static void wall_fade_arm(Widget *w) {
    w->s.wall.fade = 0;
    w->s.wall.fade_u_last = -1;
    widget_ensure_pool(w, 2);
    w->want_pool_free = 0;
    anim_start_num(&w->s.wall.fade, ANIM_T_FLOAT, 0, 1, WALL_FADE_MS,
                   EASE_IN_OUT, NULL, w, wall_fade_done, w);
}

int wall_set(const char *path) {
    if (!image_is_png(path)) return -1;   /* missing or not a PNG: keep current */
    char old[sizeof wall_path_rt];
    snprintf(old, sizeof old, "%s", wall_path());
    snprintf(wall_path_rt, sizeof wall_path_rt, "%s", path);
    if (!strcmp(old, wall_path_rt)) return 0;
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (outputs[i].active && outputs[i].wall)
            wall_fade_prepare(outputs[i].wall, old);
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (outputs[i].active && outputs[i].wall && outputs[i].wall->s.wall.fade_to)
            wall_fade_arm(outputs[i].wall);
    return 0;
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

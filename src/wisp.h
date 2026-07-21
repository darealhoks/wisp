/* wisp — single header. All shared types + cross-module decls. */
#ifndef WISP_H
#define WISP_H

#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#include "proto.h"
#include "font.h"
#include "bake.h"
#include "config.h"

/* ============================================================ */
/* Geometry / config                                             */
/* ============================================================ */

#define MAX_OUTPUTS 8
/* MAX_WIDGETS scales with MAX_OUTPUTS: per-output bar/wall/hud/lock = 4,
 * plus 1 OSD + a couple of slots for transient menus = 4*MAX_OUTPUTS + 8. */
#define MAX_WIDGETS (4 * MAX_OUTPUTS + 8)
#define MAX_CLIENTS 16
#define MAX_TAGS    9
#define MAX_TEXT    512
#define MAX_BUTTONS 16
#define MAX_ITEMS   256
#define ITEM_MAX    160
#ifndef MAX_OSDS
#define MAX_OSDS    8
#endif
#define OSD_SUM_MAX 160
#ifndef OSD_BODY_MAX
#define OSD_BODY_MAX 256
#endif
#ifndef OSD_MAX_BODY_LINES
#define OSD_MAX_BODY_LINES 4
#endif

/* Forward decls (Output and Widget reference each other). */
typedef struct Output Output;
typedef struct Widget Widget;

/* ============================================================ */
/* Wayland I/O (wl.c)                                            */
/* ============================================================ */

extern int      wl_fd;
extern uint32_t wl_next_id;
extern uint8_t  wl_rbuf[8192];
extern int      wl_rlen;

uint32_t wl_new_id(void);
int  pad4(int x);

void wl_send(const void *buf, unsigned len, int fd);
void wl_req(uint32_t obj, uint16_t op, const uint32_t *args, int n, int fd);
void wl_req_str(uint32_t obj, uint16_t op, const uint32_t *pre, int npre,
                const char *s, const uint32_t *post, int npost);
void wl_registry_bind(uint32_t name, const char *iface, uint32_t version,
                      uint32_t new_oid);

int  wl_recv(int block);   /* 0 = got data, 1 = would block, -1 = EOF/error */
int  wl_take_pending_fd(void);
void wl_close_pending_fds(void);

void wl_connect(void);
void wl_adopt(int fd);
void wl_dispatch(void);

/* Bound globals (set by registry handling). Per-output object ids
 * (wl_output, zwlr_gamma_control_v1, lock_surface)
 * live on the Output struct below — these are session-wide singletons. */
extern uint32_t id_compositor, id_shm, id_seat;
extern uint32_t compositor_ver;      /* bound wl_compositor version (set_buffer_scale needs 3) */
extern uint32_t id_layer_shell, id_wm_base;
extern uint32_t id_pointer, id_keyboard;
extern uint32_t id_gamma_mgr;
extern uint32_t id_slock_mgr, id_slock;
extern uint32_t id_extws_mgr;    /* ext_workspace_manager_v1; 0 = unsupported */
#ifdef WISP_FRACTIONAL
extern uint32_t id_viewporter, id_frac_mgr;   /* 0 = compositor lacks them */
void     widget_frac_attach(Widget *w);       /* per-surface viewport + scale listener */
Widget  *widget_by_frac(uint32_t obj);
#endif

/* Input routing state (set by pointer/keyboard events). */
extern uint32_t ptr_focus, kbd_focus;
extern int      ptr_x, ptr_y;
extern uint32_t enter_serial;

/* Key-repeat (wisp.c). */
extern int      key_rep_tfd;
extern uint32_t key_rep_key;
extern int      key_rep_delay_ms, key_rep_rate_ms;
void key_rep_cancel(void);

/* ============================================================ */
/* Widget abstraction (widget.c)                                 */
/* ============================================================ */

typedef enum { W_NONE = 0, W_BAR, W_HUD, W_MENU, W_OSD, W_WALL, W_LOCK } WidgetKind;

typedef struct { int x, y, w, h; } Rect;

/* ============================================================ */
/* Per-output state (one per connected monitor)                  */
/* ============================================================ */

struct Output {
    int      active;                 /* slot in use */
    uint32_t registry_name;          /* wl_registry global name (for hotplug) */
    uint32_t wl_output;              /* bound wl_output object id */
    char     name[64];               /* wl_output.name (v4); mango ipc key */
    uint32_t gamma_ctrl;             /* zwlr_gamma_control_v1; 0 if not held */
    uint32_t gamma_size;             /* ramp size (set by gamma_size event) */
    int      gamma_failed;           /* sticky once compositor sent FAILED */
    int      last_applied_k;         /* last Kelvin written to this output */
    int      widgets_created;        /* bar/wall/hud spawned for this output */
    int      mode_w, mode_h;         /* current mode pixel size (wl_output.mode) */
    int      scale120;               /* scale in 120ths (wl_output.scale * 120), >= 120 */
    struct Widget *bar, *wall, *hud, *lock;
};

extern Output  outputs[MAX_OUTPUTS];
extern Output *focused_output;       /* currently focused (kbd) output; NULL = none */

Output *output_alloc(uint32_t registry_name);
Output *output_by_wl(uint32_t wl_output);
Output *output_by_gamma(uint32_t gamma_ctrl);
Output *output_by_registry_name(uint32_t name);
Output *output_by_name(const char *name);

/* Tag/workspace seam (workspace.c) — tag state in, tag switches out. Two
 * backends: ext_workspace_v1 when the compositor advertises it (sway, niri,
 * hyprland, labwc, cosmic, kwin, patched dwl), else mango's unix socket.
 * tags_fd is >= 0 only for a backend with its own fd (mango); ext-workspace
 * rides the wl_display connection. */
extern int tags_fd;
void    tags_init(void);
void    tags_dispatch(void);
/* Switch to view tag `idx` (1-based, matching the DSL `tag.index`). */
void    tags_view(Output *o, int idx);
/* Returns 1 if the event was consumed by the ext-workspace backend. */
int     extws_handle_event(uint32_t obj, uint16_t op, uint8_t *body, uint32_t bodylen);

/* mango unix-socket IPC backend (mango.c); driven by workspace.c. */
extern int mango_fd;
void    mango_init(void);
void    mango_dispatch(void);
void    mango_view_tag(Output *o, int idx);
void    output_destroy(Output *o);
void    output_init_widgets(Output *o);   /* spawn bar/wall/hud + ipc + gamma */
int     output_count(void);

typedef struct {
    int      active;
    uint32_t replace_id;         /* synchronous-id / dbus notification id */
    char     summary[OSD_SUM_MAX];
    char     body[OSD_BODY_MAX];
    uint32_t icon_cp;            /* nerd-font codepoint, 0 = none */
    int      progress;           /* 0..100, -1 = no bar */
    int      urgency;            /* 0=low 1=normal 2=critical */
    int      muted;              /* category=="muted" → red styling */
    int64_t  expires_at_ms;      /* 0 = sticky */
    int64_t  spawn_ms;           /* monotonic ms when slab first posted (slide-in anim) */
    int64_t  closing_at_ms;      /* 0 = not closing; otherwise slide-out anim start */
    uint32_t close_reason;       /* dbus-spec close reason, deferred until slide-out done */
    int      h;                  /* rendered slab height; filled by osd_render */
} Osd;

typedef struct {
    uint32_t id;
    uint32_t *px;
    int      busy;
    int      off;          /* byte offset in pool */
} BufSlot;

struct Widget {
    WidgetKind kind;
    /* Output this widget is bound to (NULL for menu/osd which are output-
     * agnostic at creation time; osd then re-anchors per focused_output). */
    Output    *output;
    uint32_t   surface;
    uint32_t   layer_surface;
    int        configured;
    /* Logical size. Buffers are w*scale by h*scale physical pixels; every
     * coordinate outside the pixel layer (layout, input, cutouts) stays
     * logical, which is what keeps the scale==1 path byte-identical. */
    int        w, h;
    int        scale120;             /* copy of the output's, in 120ths, >= 120 */
    int        sent_scale120;        /* last set_buffer_scale sent (120ths); 0 = none */
#ifdef WISP_FRACTIONAL
    uint32_t   viewport;             /* wp_viewport; destination = logical size */
    uint32_t   frac_scale;           /* wp_fractional_scale_v1 */
    int        sent_dw, sent_dh;     /* last viewport destination sent */
#endif
    int        client_fd;            /* deferred-reply fd, -1 if none */

    /* SHM pool: one pool, up to 2 slots ping-pong. */
    int        pool_fd;
    int        pool_size;
    uint8_t   *shm_base;
    uint32_t   id_pool;
    int        n_slots;
    BufSlot    slots[2];

    /* Frame callback for animation (NULL when idle). */
    uint32_t   frame_cb;

    /* Auto-managed input region (destroyed+recreated by widget_set_input_region_rect).
       Widgets that pre-create regions (HUD's trigger/full) leave this 0 and swap by id. */
    uint32_t   input_region_id;

    /* When set, on_buffer_release frees the SHM pool once every slot is idle.
       Used by OSD after committing a transparent buffer following last dismissal. */
    int        want_pool_free;

    /* Widget-specific state (one big union avoids per-widget allocation). */
    union {
        struct {
            uint32_t tag_mask;        /* bit i set => tag i is occupied */
            uint32_t active_mask;     /* bit i set => tag i is active   */
            uint32_t urgent_mask;     /* bit i set => tag i is urgent   */
            char     title[MAX_TEXT];
            int      have_tags;
            uint32_t render_hash;     /* FNV-1a over displayed state; 0 = uninit */
        } bar;
        struct {
            /* Slide-axis margin: cur_off interpolates toward target_off.
             * 0 = fully revealed, -slide_extent = fully hidden off-screen.
             * Sign convention: applied to the slide_edge (top/bottom/left/right)
             * as a negative margin to push the surface off-screen. */
            double   cur_off, target_off;
            int64_t  anim_last_ms;
            int64_t  hide_at_ms;
            int      animating;
            int      visible;
            int      ptr_inside;
            uint32_t region_trigger;
            uint32_t region_full;
            int      anchor;        /* full anchor bitmask supplied by codegen */
            int      slide_edge;    /* single LS_ANCHOR_* bit picked from anchor */
            int      full_w, full_h;
            int      trigger_size;
            /* Step 6.2: when reveal_ms > 0, slide uses a fixed duration +
             * easing curve instead of the exponential decay (HUD_ANIM_TAU_MS).
             * from_off = cur_off snapshot taken when show()/hide() arms the
             * tween; anim_start_ms = arming time. */
            int      reveal_ms;
            int      reveal_easing;  /* Easing enum value */
            double   from_off;
            int64_t  anim_start_ms;
        } hud;
        struct {
            /* Heap-allocated by menu_create, freed in widget_destroy. NULL
             * for non-menu widgets — keeps the Widget union ~40 KB smaller
             * (this struct used to baseline MAX_ITEMS*ITEM_MAX even for
             * widgets that never become menus). */
            char     (*items)[ITEM_MAX];
            int      n_items;
            int     *filtered;
            int      n_filtered;
            int      sel;
            char     query[128];
            int      query_len;
            const char *prompt;      /* prompt override (static lit); NULL → MENU_PROMPT */
            int      mods;            /* xkb-free: tracked from key events */
            const int *rank;          /* optional per-item sort weight (apps usage counts); not owned */
            int      view_top;        /* vertical mode: first visible filtered row */
            /* optional per-item icons (original item order), premultiplied
             * ARGB icon_px×icon_px buffers; NULL entries ok; not owned */
            uint32_t *const *icons;
            int      icon_px;
        } menu;
        struct {
            Osd      items[MAX_OSDS];
            uint32_t next_id;
            int      has_pixels;    /* widget currently has non-empty content */
            int      is_pill;       /* pct-pill widget (items[0] only), not the stack */
            /* Fillet anim is decoupled from any individual slab: it tweens 0→1
             * when the stack transitions from empty → populated and 1→0 on the
             * reverse. Holds steady at 1 during slab hand-offs so the arcs
             * don't sag at slab-close/spawn crossovers. */
            int64_t  fillet_anim_start;
            double   fillet_from_p;
            int      fillet_target;        /* 0 or 1 */
        } osd;
        struct {
            uint32_t slock_surf_id;  /* this lock surface's object id */
        } lock;
        struct {
            int painted_w, painted_h;  /* last decoded+blitted size; 0 = never */
            /* crossfade (wispctl wall): heap frames blended per anim tick */
            double    fade;                 /* 0..1, anim target */
            uint32_t *fade_from, *fade_to;  /* pw*ph ARGB; NULL = no fade */
            int       fade_w, fade_h;       /* physical dims at fade start */
        } wall;
    } s;
};

extern Widget widgets[MAX_WIDGETS];

Widget *widget_alloc(WidgetKind k);
Widget *widget_by_surface(uint32_t sid);
Widget *widget_by_ls(uint32_t lsid);
Widget *widget_by_slock_surf(uint32_t id);
Widget *widget_first(WidgetKind k);
void    widget_destroy(Widget *w);
/* Pass NULL for `o` to let the compositor pick the output (dwl ships layer-
 * shell v3 which requires a non-null output, so callers must supply one in
 * practice — but we still send the call to keep upstream-future-compat). */
/* Physical (buffer) dimensions — only the pixel layer may use these. */
/* Physical pixels. Rounded, not truncated: at a fractional scale a truncated
 * buffer is a pixel short of what the compositor maps the surface onto. */
static inline int widget_pw(const Widget *w) { return (w->w * w->scale120 + 60) / 120; }
static inline int widget_ph(const Widget *w) { return (w->h * w->scale120 + 60) / 120; }
void    widget_setup_surface(Widget *w, uint32_t layer, const char *ns, Output *o);
void    widget_repaint(Widget *w, int first_configure);
void    widget_rescale_output(Output *o);
void    widget_set_scale(Widget *w, int s120);   /* restamp + repool + repaint */
void    widget_ensure_pool(Widget *w, int n_slots);
/* Release the SHM pool (destroys buffers, destroys pool, munmaps, closes fd).
 * Safe to call after a frame.done has confirmed the compositor consumed the
 * last attached buffer — the compositor keeps its texture reference, so any
 * displayed content stays on screen until another buffer is attached. */
void    widget_free_pool(Widget *w);
BufSlot *widget_free_slot(Widget *w);
void    widget_attach(Widget *w, BufSlot *s, int request_frame);
/* Cutout registry: one surface can punch a transparent rect through another
 * surface's pixels (e.g. OSD body cuts the bar strip underneath so the
 * translucent stack doesn't double-blend). Target keyed by DSL surface name.
 * Pass CUTOUT_X_CENTER as x to center the rect horizontally in the target
 * at apply time (use when the source doesn't know the target's width).
 *
 * `scope` constrains a cutout to a single Output — the source surface lives on
 * exactly one output (e.g. the OSD on focused_output), and the bar on every
 * other monitor must NOT punch a hole. Pass NULL on set/clear for a global
 * cutout that hits every output. cutout_apply matches when the stored scope is
 * NULL or equals self_out. */
#define CUTOUT_X_CENTER 0x7fffffff
void    cutout_set  (const char *target, Output *scope, int x, int y, int w, int h);
void    cutout_clear(const char *target, Output *scope);
void    cutout_drop_output(Output *o);   /* clear all cutouts scoped to a removed output */
void    cutout_apply(const char *self,   Output *self_out, uint32_t *px, int sw, int sh);
void    widget_set_anchor(Widget *w, uint32_t anchor_bits);
void    widget_set_size(Widget *w, int width, int height);
void    widget_set_margin(Widget *w, int top, int right, int bot, int left);
void    widget_set_exclusive_zone(Widget *w, int zone);
void    widget_set_kbd_interactive(Widget *w, int on);
void    widget_set_input_region(Widget *w, uint32_t region_id);
uint32_t widget_make_region(int x, int y, int w, int h);
/* Atomic replace: destroy the prior auto-managed input region (if any),
   create+set a new one. Use this for any region that changes over time. */
void    widget_set_input_region_rect(Widget *w, int x, int y, int ww, int hh);
void    widget_set_input_region_multi(Widget *w, const Rect *rects, int n);

/* ============================================================ */
/* Rendering (render.c)                                          */
/* ============================================================ */

/* All colors are 0xAARRGGBB premultiplied at composite time. Every primitive
 * takes LOGICAL geometry (sw/sh included) and multiplies by the scale set
 * here; the buffer is widget_pw x widget_ph physical pixels. Call this once
 * per surface render, before the first primitive. */
void render_set_scale(int scale120);   /* 120ths: 120 = 1x, 180 = 1.5x */

void clear_buf(uint32_t *px, int w, int h, uint32_t c);
void fill_rect(uint32_t *px, int sw, int sh, int x, int y, int w, int h, uint32_t c);
void fill_rect_rounded(uint32_t *px, int sw, int sh,
                       int x, int y, int w, int h,
                       int r_tl, int r_tr, int r_br, int r_bl, uint32_t c);
/* Anti-aliased rounded-rectangle border (annular outline) of thickness `bw`,
 * following the outer rounded shape. Each side may be suppressed by passing
 * side_t/r/b/l = 0 (matches the 4-side fill_rect_clipped border the codegen
 * emits for non-rounded surfaces). Pixels with y < clip_top are skipped. */
void fill_rect_rounded_border(uint32_t *px, int sw, int sh,
                              int x, int y, int w, int h,
                              int r_tl, int r_tr, int r_br, int r_bl,
                              int bw, int side_t, int side_r, int side_b, int side_l,
                              int clip_top, uint32_t c);
/* Concave outward-curving corner fillet — extends the surface body at the
 * specified corner into the adjacent armpit with a quarter-arc of bg color,
 * so the surface visually merges into a neighbouring surface that meets it
 * at that corner. (x_corner, y_corner) = the corner point of the content
 * rect. corner_id: 0=tl, 1=tr, 2=br, 3=bl. The armpit lies outside the
 * surface horizontally and inside vertically (or vice versa per corner);
 * bg fills the inner wedge of the armpit, leaving the outer wedge
 * transparent so the curve reads as concave from outside the union. */
void fill_corner_fillet(uint32_t *px, int sw, int sh,
                        int x_corner, int y_corner, int r, int corner_id,
                        uint32_t bg);
/* Outline for the above: the annulus r..r+bw along the fillet's arc, which is
 * the wedge's only exposed edge. Composites over the already-filled wedge. */
void fill_corner_fillet_border(uint32_t *px, int sw, int sh,
                               int x_corner, int y_corner, int r, int corner_id,
                               int bw, uint32_t c);
/* Punch a quarter-disc transparent hole at (cx,cy), radius r, into the
 * given corner (0=tl, 1=tr, 2=br, 3=bl of the wedge bounding box). Used by
 * compound surfaces to soften the concave inner-corner where two regions
 * meet. 2x supersampled at the rim. */
void punch_inner_corner(uint32_t *px, int sw, int sh,
                        int cx, int cy, int r, int corner_id);
void fill_inner_fillet(uint32_t *px, int sw, int sh,
                       int cx, int cy, int r, int corner_id, uint32_t color);
/* Anti-aliased filled disc (knobs). Center may be fractional. */
void fill_circle(uint32_t *px, int sw, int sh, double cx, double cy, double r, uint32_t c);
/* Single-pass soft drop-shadow of a rounded rect (already offset+spread by the
 * caller). `blur` is the softness band in px; full alpha inside, smoothstep to
 * 0 over `blur` outside. Composites src-over — draw it behind the widget slab. */
void fill_rounded_shadow(uint32_t *px, int sw, int sh,
                         int x, int y, int w, int h, int r, double blur, uint32_t c);
/* Anti-aliased rounded-rect fill masked by a second (clip) rounded rect — the
 * pixel is drawn only where it lies inside both. Used for slider thumbs that
 * span the full track width: a flat-topped bar reaching a rounded track end has
 * its corners cut to follow the track outline instead of poking past it. */
void fill_rounded_clipped(uint32_t *px, int sw, int sh,
                          int x, int y, int w, int h,
                          int r_tl, int r_tr, int r_br, int r_bl,
                          int cx, int cy, int cw, int ch,
                          int cr_tl, int cr_tr, int cr_br, int cr_bl, uint32_t c);

/* Slider visual style passed to draw_slider. All fields zero → a plain bar
 * knob in track_fg with square track ends (the pre-styling default). */
typedef struct {
    int      thumb_size;     /* knob main-axis thickness (bar/pill) or diameter (circle); 0 = no knob */
    int      thumb_shape;    /* 0 bar, 1 pill, 2 circle, 3 none */
    int      thumb_radius;   /* pill corner radius; 0 = auto (half short axis) */
    uint32_t thumb_color;    /* knob fill; 0 → track_fg */
    uint32_t thumb_border;   /* knob ring color; alpha 0 → no ring */
    int      thumb_border_w; /* ring thickness */
    int      track_radius;   /* round track ends + filled portion; <0 → auto pill */
    uint32_t shadow_color;   /* knob drop-shadow; alpha 0 → none */
    int      shadow_x, shadow_y;
    double   shadow_blur;    /* softness band; 0 → 8px default */
} SliderStyle;
void draw_slider(uint32_t *px, int sw, int sh, int x, int y, int w, int h,
                 int vertical, double value,
                 uint32_t track_bg, uint32_t track_fg, const SliderStyle *st);

int  utf8_decode(const char *s, uint32_t *cp);
const Glyph *font_find(const Font *f, uint32_t cp);
int  text_width(const Font *f, const char *s);
void draw_text(uint32_t *px, int sw, int sh, int x, int y,
               const Font *f, const char *s, uint32_t fg);
void draw_glyph(uint32_t *px, int sw, int sh, int x, int y,
                const Font *f, const Glyph *g, uint32_t fg);

/* ============================================================ */
/* Status sampling (status.c)                                    */
/* ============================================================ */

typedef struct {
    int cpu_t10;       /* CPU% × 10 */
    int cpu_temp;      /* °C or -1  */
    int disk_pct;
    int mem_used_kb;   /* RAM used, kB; -1 if /proc/meminfo missing */
    int bat_pct;       /* -1 if no battery */
    int bat_charging;
    int vpn_state;     /* 0=off 1=on 2=stale */
    int wifi_level;    /* -1=off, 0..3 */
} Status;

extern Status status;

void status_init(void);
/* Per-kind probe override from a source arg: kind = "temp"/"bat"/"wifi"/"disk",
 * val = zone / battery name / iface / mount path. Call before first sample. */
void status_set_arg(const char *kind, const char *val);
void status_tick(int tick_n);   /* called once per second */
void status_sample_all(void);

/* ============================================================ */
/* Bar (bar.c)                                                   */
/* ============================================================ */

void bar_create_on(Output *o);
void bar_render(Widget *w);
void bar_redraw_all(void);
void bar_input_click(Widget *w, int wx, int wy, int btn);
/* bar_input_* is the generic per-surface input dispatcher; the `bar_` prefix
 * is historical (Step 1 reused it). press fires on button-down, release on
 * button-up (and synthesizes on_click when the release rect matches the
 * pressed rect), motion fires while pointer moves (drag handlers gated on
 * active press inside the generated handler). */
void bar_input_press(Widget *w, int wx, int wy, int btn);
void bar_input_release(Widget *w, int wx, int wy, int btn);
void bar_input_motion(Widget *w, int wx, int wy);
/* Broadcast helpers: external `wispctl bar tags`/`bar title` callers don't
 * specify an output, so we apply to every connected bar. */
void bar_set_tags(uint32_t mask, uint32_t active, uint32_t urgent);
void bar_set_title(const char *s);
/* Per-output: mango ipc tag updates push directly via these. */
void bar_set_tags_on(Output *o, uint32_t mask, uint32_t active, uint32_t urgent);
void bar_set_title_on(Output *o, const char *s);

/* ============================================================ */
/* HUD button panel (hud.c)                                      */
/* ============================================================ */

/* HUD is a generic shell now: per-surface render/click come from codegen.
 * `hud_register` wires up trigger region + slide animation against the
 * widget's anchor. Render dispatch goes through bar_render(w). */
void hud_register(Widget *w, int anchor, int trigger_size, int full_w, int full_h);
void hud_set_reveal_anim(Widget *w, int ms, int easing);
void hud_render(Widget *w);
void hud_tick(Widget *w, int64_t now);
void hud_on_pointer_enter(Widget *w, int x, int y);
void hud_on_pointer_leave(Widget *w);
void hud_on_pointer_motion(Widget *w, int x, int y);
void hud_on_pointer_button(Widget *w, uint32_t button, uint32_t state);
int  hud_check_deferred(int64_t now);   /* returns timeout-ms or -1 */

/* ============================================================ */
/* Menu (dmenu replacement) (menu.c)                             */
/* ============================================================ */

Widget *menu_create(const char *title, char items[][ITEM_MAX], int n,
                    int client_fd);
Widget *menu_create_action(const char *title,
                           char items[][ITEM_MAX], char actions[][ITEM_MAX],
                           int n);
void    menu_render(Widget *w);
void    menu_on_key(Widget *w, uint32_t key, uint32_t state);
void    menu_on_click(Widget *w, int x, int y);
void    menu_on_scroll(Widget *w, int dir);
void    menu_reply_and_close(Widget *w, int picked);
void    menu_cancel_all(void);
void    menu_set_ranks(Widget *w, const int *rank);
void    menu_set_icons(Widget *w, uint32_t *const *icons, int icon_px);
int     menu_icon_px(void);   /* icon size that fits a vertical row */
/* Hook fired once per menu lifetime with the picked ORIGINAL item index
 * (-1 on cancel), then cleared. Used by apps.c to bump usage + free state. */
void    menu_set_pick_hook(void (*fn)(int idx));
/* Replace a live menu's item list in place (query/selection kept). */
void    menu_update_items(Widget *w, char items[][ITEM_MAX], int n);
void    spawn_detached(const char *shell_cmd);

/* Apps launcher (apps.c) — cached XDG desktop-entry index over the menu. */
Widget *apps_open(void);

/* ============================================================ */
/* OSD / notifications (osd.c)                                   */
/* ============================================================ */

extern int dnd_on;

/* `wispctl hide` — surfaces gate themselves via the DSL `ui_hidden()` source
 * (lives in ctl.c, unconditional: any preset may reference it). */
extern int ui_hidden;

/* OSD widget is created on demand (and re-anchored if focus moves to a
 * different output) — no startup constructor. */
void     osd_render(Widget *w);
uint32_t osd_post(uint32_t replace_id, const char *summary, const char *body,
                  uint32_t icon_cp, int progress, int urgency, int muted,
                  int timeout_ms);
void     osd_close(uint32_t id);
void     osd_close_all(void);
int      osd_check_expiry(int64_t now);  /* returns ms-until-next or -1 */
void     osd_tick(Widget *w);            /* compositor frame callback hook */
void     osd_on_first_configure(Widget *w); /* restart spawn tweens pending on initial configure */
void     osd_on_click(Widget *w, int x, int y);

/* ============================================================ */
/* Wallpaper (wall.c)                                            */
/* ============================================================ */

void wall_create_on(Output *o);
void wall_render(Widget *w);
int  wall_set(const char *path);   /* runtime switch + crossfade; -1 = no file */
void wall_fade_frame(Widget *w);   /* anim-tick blend (anim.c owner hook) */
void wall_fade_cancel(Widget *w);

/* ============================================================ */
/* Gamma / night mode (gamma.c)                                  */
/* ============================================================ */

typedef enum {
    GM_AUTO = 0,   /* follow schedule */
    GM_DAY,        /* force day temperature */
    GM_NIGHT,      /* force night temperature */
    GM_FLAT,       /* force flat warm (HUD override, warmer than night) */
    GM_OFF,        /* no gamma applied (passthrough) */
} GammaMode;

void     gamma_init(void);                       /* one-shot: bind for all current outputs */
void     gamma_bind_output(Output *o);           /* request gamma_control for a new output */
void     gamma_on_size(Output *o, uint32_t size);
void     gamma_on_failed(Output *o);
void     gamma_set_mode(GammaMode m);
void     gamma_tick(int tick_n);
int      gamma_is_warm(void);            /* 1 if currently warming the screen */
const char *gamma_mode_str(void);        /* one of "auto-day", "auto-night", "day", "night", "flat", "off" */

/* ============================================================ */
/* Session lock (lock.c)                                         */
/* ============================================================ */

void lock_engage(void);                  /* request session lock */
void lock_on_locked(void);               /* compositor confirmed lock */
void lock_on_finished(void);             /* lock rejected / forcibly ended */
void lock_on_surf_configure(Widget *w, uint32_t serial, int width, int height);
void lock_on_key(Widget *w, uint32_t key, uint32_t state, uint32_t mods);
void lock_on_helper_event(void);         /* helper pipe became readable */
int  lock_helper_fd(void);               /* -1 when no helper running */
int  lock_active(void);
/* Hotplug: spawn a lock surface for an output that arrives mid-lock,
 * and quietly drop one when its output goes away. */
void lock_on_output_added(Output *o);
void lock_on_output_removed(Output *o);
void lock_render_all(void);              /* re-render every lock widget */
void wl_roundtrip(void);                 /* block until one display.sync completes */

/* ============================================================ */
/* Media controls (media.c)                                      */
/* ============================================================ */

void media_volume(const char *arg);    /* "up" | "down" | "mute" */
void media_mic(const char *arg);       /* "mute" */
void media_backlight(const char *arg); /* "up" | "down" */

/* ============================================================ */
/* D-Bus notification server (dbus.c) — optional                 */
/* ============================================================ */

int      dbus_connect(void);                              /* fd or -1 */
extern int dbus_fd;
void     dbus_dispatch(void);
void     dbus_emit_closed(uint32_t id, uint32_t reason);  /* signal NotificationClosed */

/* Reconnect plumbing — timerfd fires on a backoff while dbus_fd is closed.
 * Generated main loop adds dbus_reconnect_fd to epoll and routes events here. */
extern int dbus_reconnect_fd;
void     dbus_reconnect_init(void);
void     dbus_reconnect_handle(void);

/* Generic signal subscription (used by codegen-emitted dbus_signal sources). */
typedef void (*dbus_sig_cb)(const uint8_t *body, int body_len, const char *sig);
void     dbus_subscribe(const char *iface, const char *member, dbus_sig_cb cb);
int      dbus_signal_first_str(const uint8_t *body, int body_len, const char *sig,
                               char *out, int outcap);

/* Decoded subset of org.freedesktop.Notifications/Notify body (susssasa{sv}i).
 * Populated by dbus_signal_decode_notify; used by codegen-emitted ring buffers
 * for notification-center-style surfaces. */
typedef struct {
    char    summary[128];
    char    body[256];
    char    url[256];
    uint8_t urgent;
} DbusNotifyFields;
int      dbus_signal_decode_notify(const uint8_t *body, int body_len,
                                   const char *sig, DbusNotifyFields *out);

/* ============================================================ */
/* Control socket (ctl.c)                                        */
/* ============================================================ */

typedef struct {
    int  fd;
    char buf[2048];
    int  len;
} Client;

extern Client    clients[MAX_CLIENTS];
extern int       ctl_fd;
extern char      ctl_path[128];

void  ctl_open(void);
void  ctl_adopt(int fd);
void  ctl_close(void);
void  ctl_accept(void);
void  ctl_read(Client *c);

/* ============================================================ */
/* xkb keymap (xkb.c)                                            */
/* ============================================================ */

typedef struct {
    uint32_t lo, hi;     /* level-1 and level-2 keysyms (codepoints) */
    uint8_t  alpha;      /* 1 if caps-lock should swap lo↔hi */
} XkbKey;

extern XkbKey xkb_keys[256];
extern int    xkb_loaded;
extern int    xkb_caps_on;
extern int    xkb_shift_on;

void     xkb_load(int fd, size_t size);
uint32_t xkb_xlat(uint32_t evdev, int shift);
void     xkb_on_modifiers(uint32_t depressed, uint32_t latched, uint32_t locked);
void     lock_on_caps_changed(void);  /* implemented in lock.c; called from xkb */
int      utf8_encode(uint32_t cp, char *out);
int      utf8_back(const char *s, int len);

/* ============================================================ */
/* Animation scheduler (anim.c) — optional                       */
/* ============================================================ */

typedef enum {
    EASE_LINEAR = 0,
    EASE_IN,
    EASE_OUT,
    EASE_IN_OUT,
    EASE_CUBIC_BEZIER,
} Easing;

typedef enum { ANIM_T_INT = 0, ANIM_T_FLOAT, ANIM_T_COLOR } AnimType;

typedef void (*AnimDone)(void *user);

#define ANIM_MAX 32

typedef struct Anim {
    int        active;
    void      *target;
    AnimType   type;
    double     from, to;
    uint32_t   from_c, to_c;
    int64_t    start_ms;
    int        duration_ms;
    Easing     easing;
    double     bez[4];
    Widget    *owner;
    AnimDone   on_done;
    void      *user;
} Anim;

extern Anim anims[ANIM_MAX];
extern int  anim_active_count;

/* Declarative transition slot — generated codegen per (widget-item, prop) that
 * carries a `transition_<prop> = <ms>` declaration. On each render the slot's
 * `last` target is compared to the freshly-lowered target; on change, an
 * anim_start_color tween is kicked off from `cur` to the new target. Render
 * reads `cur` instead of the raw target, so the displayed colour interpolates. */
typedef struct { int has; uint32_t last; uint32_t cur; } TransSlot;

/* Step 6.3: per-item slot for `enter_anim` / `exit_anim` on `visible = <expr>`.
 * Holds the previous lowered vis value plus a 0..1 reveal factor that the
 * scheduler tweens on edges. Render scales bg/fg/border alpha by `rev` and
 * keeps drawing while `rev > 0` so fade-outs complete after the underlying
 * expression flips to false. */
typedef struct { int has; int prev; double rev; } VisSlot;

uint32_t anim_start_num(void *target, AnimType type, double from, double to,
                        int duration_ms, Easing e, const double bez[4],
                        Widget *owner, AnimDone on_done, void *user);
uint32_t anim_start_color(uint32_t *target, uint32_t from, uint32_t to,
                          int duration_ms, Easing e, const double bez[4],
                          Widget *owner, AnimDone on_done, void *user);
void     anim_cancel_for(void *target);
void     anim_tick(int64_t now);
int      anim_any_active(void);
int      anim_fd(void);
void     anim_on_tfd(void);
double   anim_ease(int easing, double t);  /* exposed for hud reveal tween */

/* ============================================================ */
/* Utilities                                                     */
/* ============================================================ */

void msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
__attribute__((noreturn)) void die(const char *fmt, ...);

int64_t now_ms(void);

/* epoll plumbing (wisp.c) — ctl.c calls these on accept/close so the main loop
   doesn't have to rebuild client fd registrations every wakeup. */
void epoll_add_fd(int fd);
void epoll_del_fd(int fd);

#endif

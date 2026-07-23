/* Menu — dmenu-wl clone. Type-to-filter, arrows navigate, Enter picks, Esc
 * cancels. Reply (over the original control fd): "<idx>\t<text>\n" or
 * "-1\t\n" on cancel. What it looks like is declared in the .wisp and drawn
 * by the generated renderer; this file owns state, keys, sizing and scroll. */

#include "wisp.h"

#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Action-menu state: when set, pick fires action_cmds[i] instead of writing
 * to client_fd. Only one menu is ever live at a time (menu_cancel_all in
 * menu_create), so a single static table is sufficient. */
static char action_cmds[MAX_ITEMS][ITEM_MAX];
static int  action_set;

/* Linux input keycodes (evdev). */
#define KEY_ESC    1
#define KEY_BS    14
#define KEY_TAB   15
#define KEY_ENTER 28
#define KEY_LSHIFT 42
#define KEY_RSHIFT 54
#define KEY_LEFT  105
#define KEY_RIGHT 106
#define KEY_UP    103
#define KEY_DOWN  108
#define KEY_HOME  102
#define KEY_END   107

/* Per-key translation comes from xkb.c (xkb_xlat), driven by the wl_keyboard
 * keymap + modifier events. Shift state is read from xkb_shift_on directly. */

/* Scored case-insensitive match: 3 = prefix, 2 = word start (after space,
 * '-', '_', '.'), 1 = substring, 0 = none. Higher scores sort first, so
 * "fi" ranks Firefox above Profile-cleaner. */
static int match_score(const char *item, const char *q) {
    if (!q[0]) return 1;
    int qn = strlen(q), best = 0;
    for (const char *p = item; *p; p++) {
        int k;
        for (k = 0; k < qn && p[k]; k++) {
            char a = p[k], b = q[k];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (k == qn) {
            int s = p == item ? 3
                  : (p[-1] == ' ' || p[-1] == '-' || p[-1] == '_' || p[-1] == '.') ? 2 : 1;
            if (s > best) best = s;
            if (best == 3) break;
        }
    }
    return best;
}

static void rebuild_filtered(Widget *w) {
    int n = 0;
    int sc[MAX_ITEMS];
    for (int i = 0; i < w->s.menu.n_items && n < MAX_ITEMS; i++) {
        int s = match_score(w->s.menu.items[i], w->s.menu.query);
        if (!s) continue;
        sc[n] = s;
        w->s.menu.filtered[n++] = i;
    }
    /* Stable insertion sort by (match score desc, rank desc); items arrive
     * pre-sorted by the caller, so ties keep that order. n ≤ 256. */
    const int *rk = w->s.menu.rank;
    for (int i = 1; i < n; i++) {
        int fi = w->s.menu.filtered[i], si = sc[i], j = i;
        while (j > 0 && (sc[j-1] < si ||
               (sc[j-1] == si && rk && rk[w->s.menu.filtered[j-1]] < rk[fi]))) {
            w->s.menu.filtered[j] = w->s.menu.filtered[j-1];
            sc[j] = sc[j-1]; j--;
        }
        w->s.menu.filtered[j] = fi; sc[j] = si;
    }
    w->s.menu.n_filtered = n;
    if (w->s.menu.sel >= n) w->s.menu.sel = n - 1;
    if (w->s.menu.sel < 0)  w->s.menu.sel = 0;
}

static int menu_row_h(const Widget *w, const Font *f) {
    int rh = w->s.menu.geom.row_h;
    return rh ? rh : (MENU_ROW_H ? MENU_ROW_H : f->line_h + 10);
}

/* Set by ctl.c from the WispMenu entry just before menu_create; one menu is
 * live at a time, so a single pending slot is enough. */
static WispMenuGeom pend_geom;
void menu_set_geom(const WispMenuGeom *g) { pend_geom = *g; }

int menu_icon_px(void) {
    int rh = MENU_ROW_H ? MENU_ROW_H : font_small.line_h + 10;
    return rh - 12;
}

/* Layout lives in the .wisp: `render` is the generated renderer for this
 * menu's declared body (or the `spawned_by = menu` template's). menu.c owns
 * only what surrounds it — the surface size, and keeping the selection inside
 * the visible window. */
void menu_render(Widget *w) {
    if (!w->configured) return;
    const Font *f = &font_small;
    int rh = menu_row_h(w, f);
    const WispMenuGeom *g = &w->s.menu.geom;
    if (MENU_VERTICAL) {
        /* Height from the TOTAL item count, not the filtered count: a
         * per-keystroke resize costs a commit + configure round-trip + pool
         * realloc, which reads as lag. Empty rows just show background. */
        int slots = w->s.menu.n_items < g->max_vis ? w->s.menu.n_items : g->max_vis;
        int want_h = g->hdr_h + slots * rh + 2 * g->pad_y;
        if (w->w != g->width || w->h != want_h) {
            widget_set_size(w, g->width, want_h);
            wl_req(w->surface, SURFACE_REQ_COMMIT, NULL, 0, -1);
            return;
        }
        int vis = w->s.menu.n_filtered < slots ? w->s.menu.n_filtered : slots;
        int *top = &w->s.menu.view_top;
        if (w->s.menu.sel < *top) *top = w->s.menu.sel;
        if (w->s.menu.sel >= *top + vis) *top = w->s.menu.sel - vis + 1;
        if (*top > w->s.menu.n_filtered - vis) *top = w->s.menu.n_filtered - vis;
        if (*top < 0) *top = 0;
    } else {
        /* Horizontal: scroll by whole items so the selection stays inside the
         * strip. Widths are re-measured here against an approximate reserve
         * for the prompt/query cells — the declared renderer owns the real
         * layout, so a page may flip one item early, never late.
         * ponytail: ~16px/item + 48px reserve mirror the .wisp's cell pads;
         * feed real hit rects back from the renderer if presets diverge. */
        int avail = w->w - text_width(f, w->s.menu.prompt)
                  - text_width(f, w->s.menu.query) - 48;
        int *top = &w->s.menu.view_top;
        if (w->s.menu.sel < *top) *top = w->s.menu.sel;
        while (*top < w->s.menu.sel) {
            int x = 0;
            for (int i = *top; i <= w->s.menu.sel && x <= avail; i++)
                x += text_width(f, w->s.menu.items[w->s.menu.filtered[i]]) + 16;
            if (x <= avail) break;
            (*top)++;
        }
        if (*top < 0) *top = 0;
    }
    w->s.menu.render(w);
}

/* Filtered-row index under a pointer, or -1. Rows are a fixed grid, so it's
 * arithmetic. Horizontal menus have variable-width items and no hit grid of
 * their own, so only the vertical layout is hit-tested. */
static int menu_row_at(const Widget *w, int px_y) {
    if (!MENU_VERTICAL) return -1;
    int rh = menu_row_h(w, &font_small);
    int top = w->s.menu.geom.pad_y + w->s.menu.geom.hdr_h;
    int r = (px_y - top) / rh;
    if (px_y < top || r < 0) return -1;
    int i = w->s.menu.view_top + r;
    return i < w->s.menu.n_filtered ? i : -1;
}

void menu_on_click(Widget *w, int px_x, int px_y) {
    (void)px_x;
    int i = menu_row_at(w, px_y);
    if (i >= 0) menu_reply_and_close(w, w->s.menu.filtered[i]);
}

/* Menus with `hover;` (tray dropdowns): the pointer moves the same selection
 * the arrow keys do — one indicator, either input. Selection stays put on
 * leave. Gated so keyboard-driven menus never repaint on motion. */
void menu_on_hover(Widget *w, int px_x, int px_y) {
    (void)px_x;
    if (!w->s.menu.geom.wants_hover) return;
    int i = menu_row_at(w, px_y);
    if (i < 0 || i == w->s.menu.sel) return;
    w->s.menu.sel = i;
    menu_render(w);
}

void menu_on_scroll(Widget *w, int dir) {
    int s = w->s.menu.sel + dir;
    if (s < 0) s = 0;
    if (s >= w->s.menu.n_filtered) s = w->s.menu.n_filtered - 1;
    if (s == w->s.menu.sel || s < 0) return;
    w->s.menu.sel = s;
    menu_render(w);
}

void menu_set_ranks(Widget *w, const int *rank) {
    w->s.menu.rank = rank;
    rebuild_filtered(w);
}

void menu_set_icons(Widget *w, uint32_t *const *icons, int icon_px) {
    w->s.menu.icons = icons;
    w->s.menu.icon_px = icon_px;
    menu_render(w);
}

static void (*pick_hook)(int idx);
void menu_set_pick_hook(void (*fn)(int idx)) { pick_hook = fn; }

/* Fork+exec detached from the daemon, so the child survives this widget's
 * destruction (e.g. `pkill dwl` for logout, which would otherwise kill the
 * pid running it). */
void spawn_detached(const char *shell_cmd) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        signal(SIGCHLD, SIG_DFL);  /* main() ignores SIGCHLD to auto-reap; undo it so the launched program's own waitpid() works. */
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        execl("/bin/sh", "sh", "-c", shell_cmd, (char *)NULL);
        _exit(127);
    }
}

void menu_update_items(Widget *w, char items[][ITEM_MAX], int n) {
    if (n <= 0) return;
    if (n > MAX_ITEMS) n = MAX_ITEMS;
    char (*ni)[ITEM_MAX] = calloc((size_t)n, sizeof *ni);
    int *nfil = calloc((size_t)n, sizeof *nfil);
    if (!ni || !nfil) { free(ni); free(nfil); return; }
    memcpy(ni, items, (size_t)n * ITEM_MAX);
    free(w->s.menu.items);
    free(w->s.menu.filtered);
    w->s.menu.items    = ni;
    w->s.menu.filtered = nfil;
    w->s.menu.n_items  = n;
    rebuild_filtered(w);
    menu_render(w);
}

void menu_reply_and_close(Widget *w, int picked) {
    if (w->client_fd >= 0) {
        char buf[ITEM_MAX + 32];
        int n;
        if (picked >= 0 && picked < w->s.menu.n_items)
            n = snprintf(buf, sizeof buf, "%d\t%s\n", picked, w->s.menu.items[picked]);
        else
            n = snprintf(buf, sizeof buf, "-1\t\n");
        (void)!write(w->client_fd, buf, n);
        close(w->client_fd);
        w->client_fd = -1;
    } else if (action_set && picked >= 0 && picked < w->s.menu.n_items
               && action_cmds[picked][0]) {
        /* Internal action menu: fire the per-item shell command. */
        spawn_detached(action_cmds[picked]);
    }
    if (pick_hook) {
        void (*h)(int) = pick_hook;
        pick_hook = NULL;
        h(picked);
    }
    action_set = 0;
    widget_destroy(w);
}

/* Every open-a-menu entry point calls this first: hitting the same keybind
 * again toggles the menu shut instead of respawning it. */
int menu_toggle(const char *tag) {
    if (!tag[0]) return 0;   /* titleless ad-hoc menus have no toggle identity */
    for (int i = 0; i < MAX_WIDGETS; i++)
        if (widgets[i].kind == W_MENU && !strcmp(widgets[i].s.menu.tag, tag)) {
            menu_cancel_all();
            return 1;
        }
    return 0;
}

void menu_cancel_all(void) {
    for (int i = 0; i < MAX_WIDGETS; i++)
        if (widgets[i].kind == W_MENU) menu_reply_and_close(&widgets[i], -1);
}

void menu_on_key(Widget *w, uint32_t key, uint32_t state) {
    if (state == 0) return;
    if (key == KEY_LSHIFT || key == KEY_RSHIFT) return;
    if (key == KEY_ESC) { menu_reply_and_close(w, -1); return; }
    if (key == KEY_ENTER) {
        int picked = w->s.menu.n_filtered > 0
                   ? w->s.menu.filtered[w->s.menu.sel] : -1;
        menu_reply_and_close(w, picked); return;
    }
    /* horizontal nav + arrow fallbacks for muscle memory */
    if (key == KEY_RIGHT || key == KEY_DOWN || key == KEY_TAB) {
        if (w->s.menu.sel + 1 < w->s.menu.n_filtered) w->s.menu.sel++;
        menu_render(w); return;
    }
    if (key == KEY_LEFT  || key == KEY_UP) {
        if (w->s.menu.sel > 0) w->s.menu.sel--;
        menu_render(w); return;
    }
    if (key == KEY_HOME) { w->s.menu.sel = 0; menu_render(w); return; }
    if (key == KEY_END)  { w->s.menu.sel = w->s.menu.n_filtered ? w->s.menu.n_filtered - 1 : 0; menu_render(w); return; }
    if (key == KEY_BS) {
        if (w->s.menu.query_len > 0) {
            int nl = utf8_back(w->s.menu.query, w->s.menu.query_len);
            w->s.menu.query_len = nl;
            w->s.menu.query[nl] = 0;
            rebuild_filtered(w); menu_render(w);
        }
        return;
    }
    uint32_t cp = xkb_xlat(key, xkb_shift_on);
    if (!cp || cp < 0x20 || cp == 0x7f) return;
    char enc[4];
    int n = utf8_encode(cp, enc);
    if (n <= 0) return;
    if (w->s.menu.query_len + n >= (int)sizeof w->s.menu.query) return;
    memcpy(w->s.menu.query + w->s.menu.query_len, enc, n);
    w->s.menu.query_len += n;
    w->s.menu.query[w->s.menu.query_len] = 0;
    rebuild_filtered(w); menu_render(w);
}

Widget *menu_create(const char *title, char items[][ITEM_MAX], int n, int client_fd) {
    if (n <= 0) return NULL;
    if (n > MAX_ITEMS) n = MAX_ITEMS;
    menu_cancel_all();
    action_set = 0;       /* socket-reply mode; no per-item actions */
    Widget *w = widget_alloc(W_MENU);
    if (!w) return NULL;
    /* Heap-allocate the items + filtered storage on demand. Sized to N,
     * not MAX_ITEMS — typical menus are <50 items, so this also tracks
     * the real cost. widget_destroy frees both. */
    w->s.menu.items    = calloc((size_t)n, sizeof *w->s.menu.items);
    w->s.menu.filtered = calloc((size_t)n, sizeof *w->s.menu.filtered);
    if (!w->s.menu.items || !w->s.menu.filtered) {
        free(w->s.menu.items); free(w->s.menu.filtered);
        w->s.menu.items = NULL; w->s.menu.filtered = NULL;
        widget_destroy(w);
        return NULL;
    }
    w->client_fd = client_fd;
    w->s.menu.n_items = n;
    w->s.menu.sel = 0;
    if (title) snprintf(w->s.menu.prompt, sizeof w->s.menu.prompt, "%s", title);
    else       w->s.menu.prompt[0] = 0;
    for (int i = 0; i < n; i++) {
        size_t l = strnlen(items[i], ITEM_MAX - 1);
        memcpy(w->s.menu.items[i], items[i], l);
        w->s.menu.items[i][l] = 0;
        w->s.menu.filtered[i] = i;
    }
    w->s.menu.n_filtered = n;
    w->s.menu.query[0] = 0;
    w->s.menu.query_len = 0;
    w->s.menu.mods = 0;
    w->s.menu.rank = NULL;
    w->s.menu.view_top = 0;
    w->s.menu.icons = NULL;
    w->s.menu.icon_px = 0;
    w->s.menu.render = render_menu_default;

    /* Per-menu geometry, else the template's. Consumed here so a later
     * `wispctl menu` / apps launcher can't inherit a dropdown's size. */
    w->s.menu.geom = pend_geom;
    pend_geom = (WispMenuGeom){0};
    WispMenuGeom *g = &w->s.menu.geom;
    if (!g->width)   g->width   = MENU_W;
    if (!g->max_vis) g->max_vis = MENU_MAX_VIS;
    if (!g->gap)     g->gap     = MENU_GAP;
    if (!g->own_body) { g->pad_y = MENU_PAD_Y; g->hdr_h = MENU_HDR_H; }

    /* A menu opened as a result of a bar click hangs under the cell that was
     * clicked. The rect can't ride the exec() → wispctl → ctl round trip, so
     * it's picked up from the last click instead, time-boxed so an unrelated
     * `wispctl menu` later doesn't inherit it.
     * ponytail: 500 ms window; give ctl an explicit --at x,w if a second,
     * non-click caller ever needs to place a popup. */
    int anchored = click_anchor.ms && now_ms() - click_anchor.ms < 500
                   && click_anchor.out && MENU_VERTICAL;
    click_anchor.ms = 0;
    w->s.menu.anchored = anchored;

    /* Menu sits on the user's current monitor — wherever dwl says focus is. */
    Output *o = anchored ? click_anchor.out : focused_output;
    if (!o) {
        for (int i = 0; i < MAX_OUTPUTS; i++)
            if (outputs[i].active) { o = &outputs[i]; break; }
    }
    widget_setup_surface(w, LAYER_OVERLAY, "wisp-menu", o);
    if (MENU_VERTICAL) {
        const Font *f = &font_small;
        int rh = menu_row_h(w, f);
        int vis = n < g->max_vis ? n : g->max_vis;
        widget_set_size(w, g->width, g->hdr_h + vis * rh + 2 * g->pad_y);
        if (anchored) {
            /* Centered on the cell, kept fully on-screen. mode_w is physical;
             * layer-shell margins are logical. */
            int ow = o->scale120 > 0 ? o->mode_w * 120 / o->scale120 : o->mode_w;
            int mx = click_anchor.x + click_anchor.w / 2 - g->width / 2;
            if (mx > ow - g->width) mx = ow - g->width;
            if (mx < 0) mx = 0;
            widget_set_anchor(w, LS_ANCHOR_TOP | LS_ANCHOR_LEFT);
            widget_set_margin(w, click_anchor.below + g->gap, 0, 0, mx);
        } else {
            widget_set_anchor(w, LS_ANCHOR_TOP);   /* top-only anchor auto-centers */
            widget_set_margin(w, MENU_MARGIN, 0, 0, 0);
        }
    } else {
        widget_set_size(w, 0, MENU_HEIGHT);
        widget_set_anchor(w, LS_ANCHOR_TOP | LS_ANCHOR_LEFT | LS_ANCHOR_RIGHT);
    }
    /* exclusive_zone = -1 → sit at y=0 ignoring the bar's claim, so the menu
       visually replaces the bar instead of stacking under it. */
    widget_set_exclusive_zone(w, -1);
    /* Dropdowns take on_demand (v4+) so the compositor can move focus away on
     * click-off, which the keyboard-leave handler treats as dismissal. Full
     * menus take exclusive: on focus-follows-mouse compositors on_demand lets
     * mere hover steal kbd focus, leaving the menu deaf to Enter/Esc. */
    widget_set_kbd_interactive(w, anchored && layer_shell_ver >= 4 ? 2 : 1);
    wl_req(w->surface, SURFACE_REQ_COMMIT, NULL, 0, -1);
    return w;
}

/* Built-in action menu: each item carries a shell command run on pick.
 * Used by `wispctl powermenu` (and any future built-in selector). */
Widget *menu_create_action(const char *title,
                           char items[][ITEM_MAX], char actions[][ITEM_MAX],
                           int n) {
    Widget *w = menu_create(title, items, n, -1);
    if (!w) return NULL;
    for (int i = 0; i < n; i++) {
        size_t l = strnlen(actions[i], ITEM_MAX - 1);
        memcpy(action_cmds[i], actions[i], l);
        action_cmds[i][l] = 0;
    }
    action_set = 1;
    return w;
}

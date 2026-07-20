/* Menu — dmenu-wl clone. Horizontal top bar:
 *
 *   [ run: query_  | item1  item2  [selected]  item3 ... ]
 *
 * Full-width anchored layer surface, MENU_HEIGHT tall. Type-to-filter,
 * Left/Right to navigate, Enter picks, Esc cancels. Items past the right
 * edge are scrolled in as the selection moves. Reply (over the original
 * control fd): "<idx>\t<text>\n" or "-1\t\n" on cancel. */

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

/* Width of one item's slab (text + 2× padding). */
static int item_slab_w(const Font *f, const char *text) {
    return text_width(f, text) + 2 * MENU_ITEM_PAD_X;
}

static int menu_row_h(const Font *f) {
    return MENU_ROW_H ? MENU_ROW_H : f->line_h + 10;
}
#define MENU_VPAD 6   /* vertical mode: top/bottom body inset */

int menu_icon_px(void) { return menu_row_h(&font_small) - 12; }

/* Src-over blit of a premultiplied ARGB icon, clipped to the buffer. */
static void blit_icon(uint32_t *px, int W, int H, int x, int y,
                      const uint32_t *ic, int s) {
    for (int j = 0; j < s; j++) {
        if (y + j < 0 || y + j >= H) continue;
        for (int i = 0; i < s; i++) {
            if (x + i < 0 || x + i >= W) continue;
            uint32_t sp = ic[j * s + i], a = sp >> 24;
            if (!a) continue;
            uint32_t *d = &px[(y + j) * W + x + i];
            if (a == 255) { *d = sp; continue; }
            uint32_t inv = 255 - a, dv = *d;
            uint32_t da = ((dv >> 24)        * inv) / 255 + a;
            uint32_t dr = ((dv >> 16 & 0xff) * inv) / 255 + (sp >> 16 & 0xff);
            uint32_t dg = ((dv >>  8 & 0xff) * inv) / 255 + (sp >>  8 & 0xff);
            uint32_t db = ((dv       & 0xff) * inv) / 255 + (sp       & 0xff);
            *d = da << 24 | dr << 16 | dg << 8 | db;
        }
    }
}

/* Vertical launcher float: query row on top, up to MENU_MAX_VIS item rows
 * below, height tracks the filtered count. On size change we only commit the
 * new size — the configure event re-enters menu_render at the right w/h. */
static void menu_render_vert(Widget *w) {
    const Font *f = &font_small;
    int rh = menu_row_h(f);
    int nf = w->s.menu.n_filtered;
    /* Fixed height from the TOTAL item count, not the filtered count: a
     * per-keystroke resize costs a commit + configure round-trip + pool
     * realloc, which reads as lag. Empty rows just show background. */
    int slots = w->s.menu.n_items < MENU_MAX_VIS ? w->s.menu.n_items : MENU_MAX_VIS;
    int vis = nf < slots ? nf : slots;
    int want_h = rh + slots * rh + 2 * MENU_VPAD;
    if (w->w != MENU_W || w->h != want_h) {
        widget_set_size(w, MENU_W, want_h);
        wl_req(w->surface, SURFACE_REQ_COMMIT, NULL, 0, -1);
        return;
    }
    widget_ensure_pool(w, 1);
    BufSlot *s = widget_free_slot(w);
    if (!s) return;
    int W = w->w, H = w->h, R = MENU_RADIUS;
    clear_buf(s->px, W, H, R ? 0x00000000u : MENU_BG);
    if (R) fill_rect_rounded(s->px, W, H, 0, 0, W, H, R, R, R, R, MENU_BG);
    if (MENU_BORDER >> 24)
        fill_rect_rounded_border(s->px, W, H, 0, 0, W, H, R, R, R, R,
                                 MENU_BORDER_W, 1, 1, 1, 1, 0, MENU_BORDER);

    int ty = MENU_VPAD + (rh - f->line_h) / 2;
    int x = MENU_PAD_X + MENU_ITEM_PAD_X;
    const char *prompt = w->s.menu.prompt ? w->s.menu.prompt : MENU_PROMPT;
    draw_text(s->px, W, H, x, ty, f, prompt, MENU_DIM);
    char q[140];
    snprintf(q, sizeof q, "%s_", w->s.menu.query);
    draw_text(s->px, W, H, x + text_width(f, prompt) + 6, ty, f, q, MENU_FG);

    /* keep selection inside the visible window */
    int *top = &w->s.menu.view_top;
    if (w->s.menu.sel < *top) *top = w->s.menu.sel;
    if (w->s.menu.sel >= *top + vis) *top = w->s.menu.sel - vis + 1;
    if (*top > nf - vis) *top = nf - vis;
    if (*top < 0) *top = 0;

    int ipx = w->s.menu.icons ? w->s.menu.icon_px : 0;
    for (int r = 0; r < vis; r++) {
        int i = *top + r;
        int idx = w->s.menu.filtered[i];
        int y0 = MENU_VPAD + rh + r * rh;
        uint32_t fg = MENU_FG;
        if (i == w->s.menu.sel) {
            fill_rect_rounded(s->px, W, H, MENU_PAD_X, y0 + 1,
                              W - 2 * MENU_PAD_X, rh - 2, 4, 4, 4, 4, MENU_SEL_BG);
            fg = MENU_SEL_FG;
        }
        int tx = x;
        if (ipx) {
            if (w->s.menu.icons[idx])
                blit_icon(s->px, W, H, x, y0 + (rh - ipx) / 2,
                          w->s.menu.icons[idx], ipx);
            tx += ipx + 8;   /* indent all rows so text stays aligned */
        }
        draw_text(s->px, W, H, tx, y0 + (rh - f->line_h) / 2, f,
                  w->s.menu.items[idx], fg);
    }
    widget_attach(w, s, 0);
}

void menu_render(Widget *w) {
    if (!w->configured) return;
    if (MENU_VERTICAL) { menu_render_vert(w); return; }
    widget_ensure_pool(w, 1);
    BufSlot *s = widget_free_slot(w);
    if (!s) return;

    const Font *f = &font_small;
    int W = w->w, H = w->h;
    clear_buf(s->px, W, H, MENU_BG);

    int y = (H - f->line_h) / 2;
    int x = MENU_PAD_X;

    /* prompt + query — query gets a faux cursor "_" so the field looks live */
    char inp[256];
    snprintf(inp, sizeof inp, "%s %s_",
             w->s.menu.prompt ? w->s.menu.prompt : MENU_PROMPT, w->s.menu.query);
    draw_text(s->px, W, H, x, y, f, inp, MENU_FG);
    x += text_width(f, inp) + MENU_GAP;

    /* scroll: keep selection on-screen by sliding the items strip leftward */
    int items_x0 = x;
    int items_right = W - MENU_PAD_X;
    int avail = items_right - items_x0;
    if (avail <= 0) { widget_attach(w, s, 0); return; }

    /* compute selection's left/right within the un-scrolled items strip */
    int sel_left = 0, sel_w = 0;
    int total_w = 0;
    for (int i = 0; i < w->s.menu.n_filtered; i++) {
        int iw = item_slab_w(f, w->s.menu.items[w->s.menu.filtered[i]]);
        if (i == w->s.menu.sel) { sel_left = total_w; sel_w = iw; }
        total_w += iw;
    }
    int scroll = 0;
    if (sel_left + sel_w > avail) scroll = (sel_left + sel_w) - avail;
    if (scroll > sel_left) scroll = sel_left;

    /* render items left→right with horizontal scroll applied */
    int cx = items_x0 - scroll;
    for (int i = 0; i < w->s.menu.n_filtered; i++) {
        const char *text = w->s.menu.items[w->s.menu.filtered[i]];
        int iw = item_slab_w(f, text);
        if (cx + iw < items_x0) { cx += iw; continue; }  /* fully off-left */
        if (cx >= items_right) break;                     /* fully off-right */
        uint32_t fg = MENU_FG;
        if (i == w->s.menu.sel) {
            fill_rect(s->px, W, H, cx, 0, iw, H, MENU_SEL_BG);
            fg = MENU_SEL_FG;
        }
        draw_text(s->px, W, H, cx + MENU_ITEM_PAD_X, y, f, text, fg);
        cx += iw;
    }

    widget_attach(w, s, 0);
}

/* Click→pick: walk the same scrolled item layout used by menu_render. */
void menu_on_click(Widget *w, int px_x, int px_y) {
    if (MENU_VERTICAL) {
        const Font *fv = &font_small;
        int rh = menu_row_h(fv);
        int r = (px_y - MENU_VPAD - rh) / rh;
        if (px_y >= MENU_VPAD + rh && r >= 0) {
            int i = w->s.menu.view_top + r;
            if (i < w->s.menu.n_filtered)
                menu_reply_and_close(w, w->s.menu.filtered[i]);
        }
        return;
    }
    (void)px_y;
    const Font *f = &font_small;
    int W = w->w;
    char inp[256];
    snprintf(inp, sizeof inp, "%s %s_",
             w->s.menu.prompt ? w->s.menu.prompt : MENU_PROMPT, w->s.menu.query);
    int items_x0 = MENU_PAD_X + text_width(f, inp) + MENU_GAP;
    int items_right = W - MENU_PAD_X;
    int avail = items_right - items_x0;
    if (avail <= 0) return;
    int sel_left = 0, sel_w = 0, total_w = 0;
    for (int i = 0; i < w->s.menu.n_filtered; i++) {
        int iw = item_slab_w(f, w->s.menu.items[w->s.menu.filtered[i]]);
        if (i == w->s.menu.sel) { sel_left = total_w; sel_w = iw; }
        total_w += iw;
    }
    int scroll = 0;
    if (sel_left + sel_w > avail) scroll = (sel_left + sel_w) - avail;
    if (scroll > sel_left) scroll = sel_left;
    int cx = items_x0 - scroll;
    for (int i = 0; i < w->s.menu.n_filtered; i++) {
        int iw = item_slab_w(f, w->s.menu.items[w->s.menu.filtered[i]]);
        if (px_x >= cx && px_x < cx + iw && cx + iw > items_x0 && cx < items_right) {
            menu_reply_and_close(w, w->s.menu.filtered[i]);
            return;
        }
        cx += iw;
    }
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
    w->s.menu.prompt = title;   /* static lit or NULL; render falls back to MENU_PROMPT */
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

    /* Menu sits on the user's current monitor — wherever dwl says focus is. */
    Output *o = focused_output;
    if (!o) {
        for (int i = 0; i < MAX_OUTPUTS; i++)
            if (outputs[i].active) { o = &outputs[i]; break; }
    }
    widget_setup_surface(w, LAYER_OVERLAY, "wisp-menu", o);
    if (MENU_VERTICAL) {
        const Font *f = &font_small;
        int rh = menu_row_h(f);
        int vis = n < MENU_MAX_VIS ? n : MENU_MAX_VIS;
        widget_set_size(w, MENU_W, rh + vis * rh + 2 * MENU_VPAD);
        widget_set_anchor(w, LS_ANCHOR_TOP);   /* top-only anchor auto-centers */
        widget_set_margin(w, MENU_MARGIN, 0, 0, 0);
    } else {
        widget_set_size(w, 0, MENU_HEIGHT);
        widget_set_anchor(w, LS_ANCHOR_TOP | LS_ANCHOR_LEFT | LS_ANCHOR_RIGHT);
    }
    /* exclusive_zone = -1 → sit at y=0 ignoring the bar's claim, so the menu
       visually replaces the bar instead of stacking under it. */
    widget_set_exclusive_zone(w, -1);
    widget_set_kbd_interactive(w, 1);
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

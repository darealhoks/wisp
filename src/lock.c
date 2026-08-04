/* Session lock via ext_session_lock_v1. Replaces swaylock.
 *
 * Multi-output: one ext_session_lock_surface_v1 per connected wl_output, each
 * backed by its own Widget(W_LOCK). All widgets render from a single shared
 * LockState (input buffer, wrong-state flag, PAM helper) so the lock prompt
 * mirrors across every monitor and any keyboard with focus drives it.
 *
 * Sequence:
 *   1. lock_engage()           lock_mgr.lock() + for each Output: create
 *                              wl_surface + lock.get_lock_surface(out)
 *   2. surf_configure          ack + attach a buffer (per surface); ext-lock
 *                              spec requires *every* surface to commit before
 *                              `locked` arrives
 *   3. lock.locked             compositor confirms; we accept keystrokes
 *   4. on Enter                fork wisp-lock-helper (PAM) once; pipe pw → stdin
 *   5. helper "ok"             lock.unlock_and_destroy(); tear down all widgets
 *      helper "fail"           wrong-state ring, clear input, try again
 *      helper "exec"/no reply  helper unrunnable — logged, never silent
 *   6. lock.finished           compositor aborted us — drop state
 *   7. on hotplug add          lock_on_output_added() spawns a fresh surface
 *      on hotplug remove       lock_on_output_removed() drops the widget
 */

#include "wisp.h"
#include "image.h"
#include "gen_lock.h"

#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

/* Linux input keycodes (evdev) — same subset as menu.c, kept local. */
#define KEY_ESC    1
#define KEY_BS    14
#define KEY_ENTER 28
#define KEY_LSHIFT 42
#define KEY_RSHIFT 54
#define KEY_CAPSLOCK 58

/* Per-key translation comes from xkb.c (xkb_xlat); shift/caps state from
 * xkb_shift_on / xkb_caps_on, driven by wl_keyboard.modifiers. */

/* Shared lock state — drives every per-output W_LOCK widget. */
static struct {
    int     requested;
    int     locked_state;
    char    input[256];          /* UTF-8 bytes (passwords can be multi-byte) */
    int     input_len;
    int     wrong;
    int     fails;               /* wrong tries this session; drives the delay */
    int64_t wrong_until;         /* no attempt accepted before this */
    pid_t   helper_pid;
    int     helper_fd;
    int     hl_start;            /* keypress arc angle, 2048ths of a turn */
    int     hl_bs;               /* last input was a backspace, not a character */
} ls = { .helper_fd = -1 };

/* swaylock's jump: a new angle at least a quarter turn from the last, so two
 * keystrokes never look like one. Cosmetic only — it says nothing about the
 * password, which is why plain rand() is enough. */
static void hl_advance(int backspace) {
    ls.hl_start = (ls.hl_start + (rand() % 1024) + 512) % 2048;
    ls.hl_bs = backspace;
}

/* Count codepoints in `s` so the dot row mirrors typed characters, not bytes. */
static int utf8_count(const char *s, int len) {
    int n = 0;
    for (int i = 0; i < len; i++)
        if ((((unsigned char)s[i]) & 0xc0) != 0x80) n++;
    return n;
}

static int locked_out(void) {
    return LOCK_LOCKOUT_AFTER > 0 && ls.fails >= LOCK_LOCKOUT_AFTER;
}
static int throttled(void) { return now_ms() < ls.wrong_until; }

/* Backoff for the ls.fails'th wrong try. Computed in int64 and clamped, so a
 * config with a large growth can't overflow into a negative (= no) delay. */
static int64_t retry_delay(void) {
    int64_t d = LOCK_RETRY_MS;
    int growth = LOCK_RETRY_GROWTH < 1 ? 1 : LOCK_RETRY_GROWTH;
    if (locked_out()) return LOCK_RETRY_MAX_MS;
    for (int i = 1; i < ls.fails && d < LOCK_RETRY_MAX_MS; i++) d *= growth;
    if (d > LOCK_RETRY_MAX_MS) d = LOCK_RETRY_MAX_MS;
    return d < LOCK_WRONG_MS ? LOCK_WRONG_MS : d;
}

int lock_active(void)   { return ls.requested; }
int lock_helper_fd(void) { return ls.helper_fd; }

/* Pick the font matching the DSL-requested pixel size (`lock { font_size }`
 * flows into the baked size list via wispc --font-sizes), else fall back. */
static const Font *font_px(int px, const Font *fall) {
    for (int i = 0; i < wisp_n_fonts; i++)
        if (wisp_fonts[i]->px_size == px) return wisp_fonts[i];
    return fall;
}

/* Background stage: wallpaper (if declared) or flat fill, plus the `dim`
 * scrim. Takes PHYSICAL dims — it memcpy's a whole cover-fit, which the
 * logical-coordinate primitives never do. */
#if LOCK_WALLPAPER
/* The finished cover-fit of WALL_PATH, mmap'd from the disk cache the daemon
 * seeds in wall.c. Read-only MAP_SHARED, so the daemon's copy and ours are the
 * same page-cache pages — no private RSS, and no PNG decode on the critical
 * path (a 4K decode is ~0.5 s of black screen). Kept mapped for the session:
 * every keystroke repaints the whole surface. */
static const uint32_t *bg_map;
static int bg_map_w, bg_map_h;

/* Returns 0 when the wallpaper is unavailable and the caller must flat-fill. */
static int lock_draw_bg_px(uint32_t *px, int W, int H) {
    if (!bg_map || bg_map_w != W || bg_map_h != H) {
        image_bgcache_unmap(bg_map, bg_map_w, bg_map_h);
        /* ponytail: compiled WALL_PATH, so a `wispctl wall` override isn't
         * reflected — plumb the runtime path through the ctl socket if it
         * ever matters. */
        bg_map = image_bgcache_map(WALL_PATH, W, H);
        bg_map_w = W; bg_map_h = H;
        if (!bg_map) {
            /* Cold cache (lock before the daemon ever painted this size):
             * decode once and seed it, so the next lock is a map. */
            int sw = 0, sh = 0;
            uint8_t *src = image_load(WALL_PATH, &sw, &sh);
            if (src && sw > 1 && sh > 1) {
                image_blit_cover(px, W, H, src, sw, sh);
                image_bgcache_store(WALL_PATH, W, H, px);
                image_free(src);
                bg_map = image_bgcache_map(WALL_PATH, W, H);
                return 1;
            }
            image_free(src);
        }
    }
    if (!bg_map) return 0;
    memcpy(px, bg_map, (size_t)W * H * 4);
    return 1;
}
#endif

static void lock_draw_bg(uint32_t *px, int W, int H, int PW, int PH) {
#if LOCK_WALLPAPER
    if (!lock_draw_bg_px(px, PW, PH)) clear_buf(px, W, H, LOCK_BG);
#else
    (void)PW; (void)PH;
    clear_buf(px, W, H, LOCK_BG);
#endif
    /* Src-over: fill_rect overwrites, which would replace the wallpaper with
     * a flat translucent wash instead of scrimming it. */
    if (LOCK_DIM & 0xff000000u) fill_rect_over(px, W, H, 0, 0, W, H, LOCK_DIM);
}

/* Expand a declared template: every LT_* byte is replaced by the value it
 * stands for. Everything else is literal UTF-8 straight from the .wisp. */
static void lock_expand(const LockEl *e, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = e->fmt; *p && o + 1 < cap; p++) {
        char tmp[64];
        const char *v = tmp;
        switch ((unsigned char)*p) {
        case LT_DOTS: {
            int n = utf8_count(ls.input, ls.input_len);
            if (LOCK_PRIVACY) n = n > 0;
            if (n > (int)sizeof tmp - 1) n = (int)sizeof tmp - 1;
            memset(tmp, '*', (size_t)n); tmp[n] = 0;
            break;
        }
        case LT_COUNT:
            if (LOCK_PRIVACY) tmp[0] = 0;
            else snprintf(tmp, sizeof tmp, "%d", utf8_count(ls.input, ls.input_len));
            break;
        case LT_LAYOUT: v = xkb_layout_name(); break;
        case LT_PROMPT: v = LOCK_PROMPT; break;
        case LT_TIME: {
            time_t t = time(NULL);
            struct tm tm;
            localtime_r(&t, &tm);
            if (!e->time_fmt || !strftime(tmp, sizeof tmp, e->time_fmt, &tm))
                tmp[0] = 0;
            break;
        }
        default: tmp[0] = *p; tmp[1] = 0; break;
        }
        size_t n = strlen(v);
        if (n > cap - 1 - o) n = cap - 1 - o;
        memcpy(out + o, v, n);
        o += n;
    }
    out[o] = 0;
}

static int lock_show(const LockEl *e) {
    int v;
    switch (e->show & ~LSHOW_NEG) {
    case LSHOW_TYPING:     v = ls.input_len > 0; break;
    case LSHOW_WRONG:      v = ls.wrong; break;
    case LSHOW_CAPS:       v = xkb_caps_on; break;
    case LSHOW_VERIFYING:  v = ls.helper_pid > 0; break;
    case LSHOW_LAYOUT_ALT: v = xkb_group != 0; break;
    case LSHOW_EMPTY:      v = ls.input_len == 0; break;
    case LSHOW_THROTTLED:  v = throttled(); break;
    case LSHOW_LOCKED_OUT: v = locked_out(); break;
    default:               v = 1; break;
    }
    return (e->show & LSHOW_NEG) ? !v : v;
}

/* Place a w×h box against the anchored edges; an axis with no anchor bit is
 * centered and x/y then read as a nudge instead of an inset. */
static void lock_place(const LockEl *e, int W, int H, int w, int h, int *px, int *py) {
    if (e->anchor & LA_LEFT)        *px = e->x;
    else if (e->anchor & LA_RIGHT)  *px = W - e->x - w;
    else                            *px = (W - w) / 2 + e->x;
    if (e->anchor & LA_TOP)         *py = e->y;
    else if (e->anchor & LA_BOTTOM) *py = H - e->y - h;
    else                            *py = (H - h) / 2 + e->y;
}

/* Content stage: walk the declared element table. Nothing here knows what a
 * prompt or a caps indicator is — that is all in the .wisp. */
static void lock_draw_content(uint32_t *px, int W, int H) {
    for (int i = 0; i < LOCK_N_ELS; i++) {
        const LockEl *e = &lock_els[i];
        if (!lock_show(e)) continue;
        int x, y;
        if (e->kind == LEL_FRAME) {
            lock_place(e, W, H, e->w, e->h, &x, &y);
            if (e->bg & 0xff000000u)
                fill_rect_rounded(px, W, H, x, y, e->w, e->h,
                                  e->radius, e->radius, e->radius, e->radius, e->bg);
            if ((e->border & 0xff000000u) && e->border_w > 0)
                fill_rect_rounded_border(px, W, H, x, y, e->w, e->h,
                                         e->radius, e->radius, e->radius, e->radius,
                                         e->border_w, 1, 1, 1, 1, 0, e->border);
            continue;
        }
        if (e->kind == LEL_RING) {
            int th = e->w > 0 ? e->w : 1;
            int bw = (e->border & 0xff000000u) ? e->border_w : 0;
            int box = 2 * e->radius + th + 2 * bw;
            lock_place(e, W, H, box, box, &x, &y);
            double cx = x + box / 2.0, cy = y + box / 2.0;
            if (e->bg & 0xff000000u)
                fill_circle(px, W, H, cx, cy, e->radius - th / 2.0, e->bg);
            if (bw > 0) {
                /* swaylock draws the separator line on both faces of the ring. */
                fill_ring(px, W, H, cx, cy, e->radius - th / 2.0 - bw / 2.0,
                          bw, e->h, e->gap, e->border);
                fill_ring(px, W, H, cx, cy, e->radius + th / 2.0 + bw / 2.0,
                          bw, e->h, e->gap, e->border);
            }
            fill_ring(px, W, H, cx, cy, e->radius, th, e->h, e->gap, e->fg);
            if (e->hl & 0xff000000u) {
                double a0 = ls.hl_start * (360.0 / 2048.0);
                uint32_t hc = (ls.hl_bs && (e->hl_bs & 0xff000000u)) ? e->hl_bs : e->hl;
                fill_arc(px, W, H, cx, cy, e->radius, th, a0, e->hl_arc, hc);
                if (e->sep & 0xff000000u) {
                    /* Both ends get a 2px-wide radial line, expressed as the
                     * degrees that subtends at this radius. */
                    double s = 114.591559 / (e->radius > 1 ? e->radius : 1);
                    fill_arc(px, W, H, cx, cy, e->radius, th, a0 - s / 2, s, e->sep);
                    fill_arc(px, W, H, cx, cy, e->radius, th,
                             a0 + e->hl_arc - s / 2, s, e->sep);
                }
            }
            continue;
        }
        char buf[256];
        lock_expand(e, buf, sizeof buf);
        if (!buf[0]) continue;
        const Font *f = font_px(e->font_px ? e->font_px : LOCK_FONT_SIZE, &font_small);
        lock_place(e, W, H, text_width(f, buf), f->line_h, &x, &y);
        draw_text(px, W, H, x, y, f, buf, e->fg);
    }
}

/* Frame stage: buffer plumbing only — acquire, draw, attach. Keyboard only. */
static void lock_render(Widget *w) {
    if (!w->configured || w->w <= 0 || w->h <= 0) return;
    widget_ensure_pool(w, 1);
    BufSlot *s = widget_free_slot(w);
    if (!s) return;

    lock_draw_bg(s->px, w->w, w->h, widget_pw(w), widget_ph(w));
    lock_draw_content(s->px, w->w, w->h);
    widget_attach(w, s, 0);
}

void lock_on_caps_changed(void) {
    if (ls.locked_state) lock_render_all();
}

void lock_render_all(void) {
    for (int i = 0; i < MAX_WIDGETS; i++)
        if (widgets[i].kind == W_LOCK) lock_render(&widgets[i]);
}

/* Spawn a lock surface + Widget(W_LOCK) for one Output. Called from
 * lock_engage for each existing output, and from lock_on_output_added on
 * hotplug while already locked. */
static void lock_attach_surface(Output *o) {
    if (!o || !id_compositor || !id_slock || o->lock) return;
    Widget *w = widget_alloc(W_LOCK);
    if (!w) { msg("lock: no widget slot for %u", o->wl_output); return; }
    o->lock = w;
    w->output = o;
    w->scale120 = (compositor_ver >= 3 && o->scale120 > 0) ? o->scale120 : 120;
    w->surface = wl_new_id();
    { uint32_t a = w->surface; wl_req(id_compositor, COMPOSITOR_REQ_CREATE_SURFACE, &a, 1, -1); }
#ifdef WISP_FRACTIONAL
    widget_frac_attach(w);
#endif
    w->s.lock.slock_surf_id = wl_new_id();
    { uint32_t args[3] = { w->s.lock.slock_surf_id, w->surface, o->wl_output };
      wl_req(id_slock, SLOCK_REQ_GET_LOCK_SURFACE, args, 3, -1); }
    w->w = 1; w->h = 1;   /* placeholder until first configure */
}

void lock_on_output_added(Output *o) {
    if (!ls.requested) return;
    lock_attach_surface(o);
}

void lock_on_output_removed(Output *o) {
    if (!o || !o->lock) return;
    widget_destroy(o->lock);   /* clears o->lock via back-pointer */
}

void lock_on_surf_configure(Widget *w, uint32_t serial, int width, int height) {
    if (width)  w->w = width;
    if (height) w->h = height;
    wl_req(w->s.lock.slock_surf_id, SLOCK_SURF_REQ_ACK_CONFIGURE, &serial, 1, -1);
    w->configured = 1;
    lock_render(w);
}

void lock_on_locked(void) {
    ls.locked_state = 1;
    msg("lock: locked");
}

/* Full teardown: invoked on successful unlock OR on `finished` rejection.
 * Destroys every per-output lock widget and resets shared state. */
static void teardown_all(int issued_unlock) {
    if (ls.helper_fd >= 0) {
        epoll_del_fd(ls.helper_fd);
        close(ls.helper_fd);
        ls.helper_fd = -1;
    }
    if (ls.helper_pid > 0) {
        kill(ls.helper_pid, SIGKILL);
        ls.helper_pid = 0;
    }
    /* Destroy widgets first; each sends ext_session_lock_surface.destroy
     * via widget_destroy's W_LOCK branch. */
    for (int i = 0; i < MAX_WIDGETS; i++)
        if (widgets[i].kind == W_LOCK) widget_destroy(&widgets[i]);
    /* If we issued unlock_and_destroy already, id_slock is gone server-side;
     * otherwise (on `finished`) destroy from our end. */
    if (id_slock && !issued_unlock) {
        wl_req(id_slock, SLOCK_REQ_DESTROY, NULL, 0, -1);
    }
    id_slock = 0;
    explicit_bzero(ls.input, sizeof ls.input);
    ls.input_len = 0;
    ls.wrong = 0;
    ls.fails = 0;
    ls.wrong_until = 0;
    ls.requested = ls.locked_state = 0;
}

static void finish_unlock(void) {
    if (id_slock) wl_req(id_slock, SLOCK_REQ_UNLOCK_AND_DESTROY, NULL, 0, -1);
    teardown_all(1);
}

void lock_on_finished(void) {
    msg("lock: finished (compositor rejected lock)");
    teardown_all(0);
}

void lock_engage(void) {
    if (ls.requested) return;
    if (!id_slock_mgr) { msg("lock: ext_session_lock_manager_v1 unavailable"); return; }
    if (!id_compositor) { msg("lock: no compositor"); return; }
    if (output_count() == 0) { msg("lock: no outputs to lock"); return; }
    srand((unsigned)now_ms());   /* so the arc doesn't walk the same path every lock */

    /* lock_manager.lock(new_id) */
    id_slock = wl_new_id();
    { uint32_t a = id_slock; wl_req(id_slock_mgr, SLOCK_MGR_REQ_LOCK, &a, 1, -1); }
    ls.requested = 1;

    /* One lock_surface per output. Compositor sends `locked` once every
     * surface has acked configure and committed a buffer. */
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (outputs[i].active) lock_attach_surface(&outputs[i]);
}

/* The session that runs the lock is spawned by the compositor, which greetd
 * started — ~/.local/bin is off its PATH, so a bare LOCK_HELPER_BIN would
 * never resolve. Prefer the helper sitting next to the running binary; PATH
 * is only the fallback. Writes into `buf`, returns it or NULL. */
static const char *helper_path(char *buf, size_t cap) {
    if (strchr(LOCK_HELPER_BIN, '/')) return LOCK_HELPER_BIN;
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return NULL;
    exe[n] = 0;
    char *slash = strrchr(exe, '/');
    if (!slash) return NULL;
    *slash = 0;
    if ((size_t)snprintf(buf, cap, "%s/" LOCK_HELPER_BIN, exe) >= cap) return NULL;
    if (access(buf, X_OK) != 0) return NULL;
    return buf;
}

static void spawn_helper(void) {
    if (ls.helper_pid > 0) return;
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) < 0) return;
    if (pipe(out_pipe) < 0) { close(in_pipe[0]); close(in_pipe[1]); return; }

    pid_t pid = fork();
    if (pid == 0) {
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        char buf[512];
        const char *p = helper_path(buf, sizeof buf);
        if (p) execl(p, p, LOCK_PAM_SERVICE, (char *)NULL);
        execlp(LOCK_HELPER_BIN, LOCK_HELPER_BIN, LOCK_PAM_SERVICE, (char *)NULL);
        /* Distinct from "fail": a helper we cannot exec is a broken install,
         * not a wrong password, and must not masquerade as one. */
        (void)!write(1, "exec\n", 5);
        _exit(127);
    }
    close(in_pipe[0]); close(out_pipe[1]);
    if (pid < 0) { close(in_pipe[1]); close(out_pipe[0]); return; }

    (void)!write(in_pipe[1], ls.input, ls.input_len);
    (void)!write(in_pipe[1], "\n", 1);
    close(in_pipe[1]);
    explicit_bzero(ls.input, sizeof ls.input);
    ls.input_len = 0;

    int fl = fcntl(out_pipe[0], F_GETFL); fcntl(out_pipe[0], F_SETFL, fl | O_NONBLOCK);
    ls.helper_pid = pid;
    ls.helper_fd  = out_pipe[0];
    epoll_add_fd(out_pipe[0]);
}

void lock_on_helper_event(void) {
    char buf[16] = {0};
    ssize_t n = read(ls.helper_fd, buf, sizeof buf - 1);
    epoll_del_fd(ls.helper_fd);
    close(ls.helper_fd);
    ls.helper_fd = -1;
    if (ls.helper_pid > 0) {
        int st; waitpid(ls.helper_pid, &st, 0);
        ls.helper_pid = 0;
    }
    int ok = (n > 0 && buf[0] == 'o');
    /* "exec\n", or a closed pipe with no reply at all (helper died) — either
     * way PAM was never consulted, so say so instead of blaming the password. */
    if (n <= 0 || buf[0] == 'e')
        msg("lock: %s did not run — check that it is installed next to wisp-lock",
            LOCK_HELPER_BIN);
    if (ok) {
        msg("lock: unlock");
        finish_unlock();
    } else {
        ls.wrong = 1;
        ls.fails++;
        ls.wrong_until = now_ms() + retry_delay();
        lock_render_all();
    }
}

void lock_on_key(Widget *w, uint32_t key, uint32_t state, uint32_t mods) {
    (void)w; (void)mods;
    if (state == 0) return;
    if (key == KEY_LSHIFT || key == KEY_RSHIFT || key == KEY_CAPSLOCK) return;

    /* The wrong flash doubles as the retry timer: it clears when the backoff
     * has run out, not on the next keystroke. Nothing repaints in between —
     * the only observer is the next key, and this stays a 0-tick idle. */
    if (ls.wrong && !throttled()) ls.wrong = 0;

    if (key == KEY_ESC) {
        explicit_bzero(ls.input, sizeof ls.input);
        ls.input_len = 0;
        lock_render_all();
        return;
    }
    if (key == KEY_BS) {
        if (ls.input_len > 0) {
            int nl = LOCK_WIPE_ON_BACKSPACE ? 0 : utf8_back(ls.input, ls.input_len);
            explicit_bzero(ls.input + nl, ls.input_len - nl);
            ls.input_len = nl;
            ls.input[nl] = 0;
            hl_advance(1);   /* a wipe_on_backspace wipe is still a backspace */
            lock_render_all();
        }
        return;
    }
    if (key == KEY_ENTER) {
        if (ls.input_len == 0 || throttled()) return;
        spawn_helper();
        return;
    }
    uint32_t cp = xkb_xlat(key, xkb_shift_on);
    if (!cp || cp < 0x20 || cp == 0x7f) return;
    char enc[4];
    int n = utf8_encode(cp, enc);
    if (n <= 0) return;
    if (ls.input_len + n >= (int)sizeof ls.input) return;
    memcpy(ls.input + ls.input_len, enc, n);
    ls.input_len += n;
    ls.input[ls.input_len] = 0;
    hl_advance(0);
    lock_render_all();
}

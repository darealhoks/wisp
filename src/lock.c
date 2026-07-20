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
 *   6. lock.finished           compositor aborted us — drop state
 *   7. on hotplug add          lock_on_output_added() spawns a fresh surface
 *      on hotplug remove       lock_on_output_removed() drops the widget
 */

#include "wisp.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
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
    int64_t wrong_until;
    pid_t   helper_pid;
    int     helper_fd;
} ls = { .helper_fd = -1 };

/* Count codepoints in `s` so the dot row mirrors typed characters, not bytes. */
static int utf8_count(const char *s, int len) {
    int n = 0;
    for (int i = 0; i < len; i++)
        if ((((unsigned char)s[i]) & 0xc0) != 0x80) n++;
    return n;
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

/* Render one lock surface: black screen, a centered prompt that fills with
 * asterisks as you type, a wrong-password line, a CAPS line. Keyboard only. */
static void lock_render(Widget *w) {
    if (!w->configured || w->w <= 0 || w->h <= 0) return;
    widget_ensure_pool(w, 1);
    BufSlot *s = widget_free_slot(w);
    if (!s) return;
    int W = w->w, H = w->h;

    clear_buf(s->px, W, H, LOCK_BG);

    const Font *fs = font_px(LOCK_FONT_SIZE, &font_small);
    int cx = W / 2, cy = H / 2;

    int n = utf8_count(ls.input, ls.input_len);
    if (n > 0) {
        char stars[128];
        if (n > (int)sizeof stars - 1) n = (int)sizeof stars - 1;
        memset(stars, '*', n); stars[n] = 0;
        draw_text(s->px, W, H, cx - text_width(fs, stars) / 2,
                  cy - fs->line_h / 2, fs, stars, LOCK_FG);
    }

    if (ls.wrong)
        draw_text(s->px, W, H, cx - text_width(fs, "wrong password") / 2,
                  cy + fs->line_h, fs, "wrong password", LOCK_RING_WRONG);

    if (xkb_caps_on)
        draw_text(s->px, W, H, cx - text_width(fs, "CAPS") / 2,
                  cy + 2 * fs->line_h + 8, fs, "CAPS", LOCK_CAPS);

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
    w->scale = (compositor_ver >= 3 && o->scale > 0) ? o->scale : 1;
    w->surface = wl_new_id();
    { uint32_t a = w->surface; wl_req(id_compositor, COMPOSITOR_REQ_CREATE_SURFACE, &a, 1, -1); }
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
    ls.wrong_until = 0;
    ls.requested = ls.locked_state = 0;
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

    /* lock_manager.lock(new_id) */
    id_slock = wl_new_id();
    { uint32_t a = id_slock; wl_req(id_slock_mgr, SLOCK_MGR_REQ_LOCK, &a, 1, -1); }
    ls.requested = 1;

    /* One lock_surface per output. Compositor sends `locked` once every
     * surface has acked configure and committed a buffer. */
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (outputs[i].active) lock_attach_surface(&outputs[i]);
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
        execlp(LOCK_HELPER_BIN, LOCK_HELPER_BIN, LOCK_PAM_SERVICE, (char *)NULL);
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
    if (ok) {
        msg("lock: unlock");
        if (id_slock) wl_req(id_slock, SLOCK_REQ_UNLOCK_AND_DESTROY, NULL, 0, -1);
        teardown_all(1);
    } else {
        ls.wrong = 1;
        ls.wrong_until = now_ms() + LOCK_WRONG_MS;
        lock_render_all();
    }
}

void lock_on_key(Widget *w, uint32_t key, uint32_t state, uint32_t mods) {
    (void)w; (void)mods;
    if (state == 0) return;
    if (key == KEY_LSHIFT || key == KEY_RSHIFT || key == KEY_CAPSLOCK) return;

    if (ls.wrong) { ls.wrong = 0; ls.wrong_until = 0; }

    if (key == KEY_ESC) {
        explicit_bzero(ls.input, sizeof ls.input);
        ls.input_len = 0;
        lock_render_all();
        return;
    }
    if (key == KEY_BS) {
        if (ls.input_len > 0) {
            int nl = utf8_back(ls.input, ls.input_len);
            explicit_bzero(ls.input + nl, ls.input_len - nl);
            ls.input_len = nl;
            ls.input[nl] = 0;
            lock_render_all();
        }
        return;
    }
    if (key == KEY_ENTER) {
        if (ls.input_len == 0) return;
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
    lock_render_all();
}

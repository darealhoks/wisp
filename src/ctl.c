/* Control socket: SOCK_STREAM at $XDG_RUNTIME_DIR/wisp.sock. Commands are
 * tab-separated args, one per line. */
#include "wisp.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <fcntl.h>

int    ui_hidden = 0;
Client clients[MAX_CLIENTS];
int    ctl_fd = -1;
char   ctl_path[128];
int    reload_pending = 0;

/* Hot reload: hand off ctl_fd AND wl_fd across exec. Keeping the wl socket
 * open keeps every old object alive, so the arg carries enough for the new
 * process to adopt the long-lived ones IN PLACE — surfaces are never
 * unmapped/remapped (which layer_animations-style compositors would animate)
 * and gamma controls (exclusive per output) never released. The new process
 * allocates above the id high-water mark, reuses the adoptable objects, and
 * reaps only unclaimed leftovers (`old=` + unmatched adopts) after its own
 * are up. Transient widgets (menu/osd/lock) just get reaped. Other old
 * sub-objects (pools, regions, seat devices) leak until exit — one reload's
 * worth.
 *
 * We exec by PATH ("wisp"), NOT /proc/self/exe — `install -m 755 …` unlinks
 * the destination before creating the new inode, so /proc/self/exe of the
 * running daemon resolves to the OLD (deleted) inode and re-execing it would
 * re-run the stale binary. PATH lookup picks the freshly installed
 * ~/.local/bin/wisp. */
void ctl_reload_exec(void) {
    extern char **environ;
    int fl;
    if (ctl_fd >= 0 && (fl = fcntl(ctl_fd, F_GETFD)) >= 0)
        fcntl(ctl_fd, F_SETFD, fl & ~FD_CLOEXEC);
    if (wl_fd >= 0 && (fl = fcntl(wl_fd, F_GETFD)) >= 0)
        fcntl(wl_fd, F_SETFD, fl & ~FD_CLOEXEC);
    char arg[1024];
    int n = snprintf(arg, sizeof arg, "wl=%d,ctl=%d,hi=%u,adopt=",
                     wl_fd, ctl_fd, wl_next_id);
    /* WidgetKind values are the adoption contract between old and new binary;
     * both come from this repo's wisp.h, so they only skew mid-rebase. */
    for (int i = 0; i < MAX_WIDGETS && n < (int)sizeof arg - 96; i++) {
        Widget *w = &widgets[i];
        if (w->kind != W_BAR && w->kind != W_WALL && w->kind != W_HUD) continue;
        if (!w->surface || !w->configured || !w->output || !w->output->name[0])
            continue;
        uint32_t vp = 0, fs = 0;
#ifdef WISP_FRACTIONAL
        vp = w->viewport; fs = w->frac_scale;
#endif
        n += snprintf(arg + n, sizeof arg - n, "%d:%s:%u:%u:%u:%u:%d:%d+",
                      (int)w->kind, w->output->name, w->surface,
                      w->layer_surface, vp, fs, w->w, w->h);
        w->kind = W_NONE;   /* claimed — keep it out of the old= list below */
    }
    n += snprintf(arg + n, sizeof arg - n, ",gamma=");
    for (int i = 0; i < MAX_OUTPUTS && n < (int)sizeof arg - 64; i++)
        if (outputs[i].active && outputs[i].gamma_ctrl && outputs[i].name[0])
            n += snprintf(arg + n, sizeof arg - n, "%s:%u:%u+",
                          outputs[i].name, outputs[i].gamma_ctrl,
                          outputs[i].gamma_size);
    n += snprintf(arg + n, sizeof arg - n, ",old=");
    for (int i = 0; i < MAX_WIDGETS && n < (int)sizeof arg - 24; i++) {
        Widget *w = &widgets[i];
        if (w->kind == W_NONE || !w->surface) continue;
        n += snprintf(arg + n, sizeof arg - n, "%u/%u+",
                      w->layer_surface, w->surface);
    }
    char *argv[] = { (char*)"wisp", (char*)"--reload-fds", arg, NULL };
    execvpe("wisp", argv, environ);
    msg("wisp: reload exec failed: %s", strerror(errno));
    reload_pending = 0;
}

static Client *client_add(int fd) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd < 0) {
            clients[i].fd = fd; clients[i].len = 0;
            epoll_add_fd(fd);
            return &clients[i];
        }
    return NULL;
}
static void client_close(Client *c) {
    if (c->fd >= 0) { epoll_del_fd(c->fd); close(c->fd); }
    c->fd = -1; c->len = 0;
}

void ctl_open(void) {
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir) die("XDG_RUNTIME_DIR not set");
    int n = snprintf(ctl_path, sizeof ctl_path, "%s/%s", dir, CTL_SOCK_NAME);
    if (n <= 0 || n >= (int)sizeof ctl_path) die("ctl path too long");
    /* ponytail: singleton per user — a second instance (e.g. on another tty)
     * steals this socket and the first loses all IPC; suffix CTL_SOCK_NAME
     * with $WAYLAND_DISPLAY here and in wispctl.c if multi-session matters. */
    unlink(ctl_path);
    ctl_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (ctl_fd < 0) die("ctl socket: %s", strerror(errno));
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strncpy(a.sun_path, ctl_path, sizeof a.sun_path - 1);
    if (bind(ctl_fd, (struct sockaddr *)&a, sizeof a) < 0)
        die("ctl bind: %s", strerror(errno));
    if (listen(ctl_fd, 8) < 0) die("ctl listen: %s", strerror(errno));
    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;
}

void ctl_close(void) {
    /* Race-safe unlink: only remove the socket file if it still points at OUR
     * bound inode. Otherwise a fresh daemon that bound after us would have its
     * socket yanked when the outgoing one runs its shutdown handler. */
    if (ctl_path[0] && ctl_fd >= 0) {
        struct stat st_fd, st_path;
        if (fstat(ctl_fd, &st_fd) == 0
            && stat(ctl_path, &st_path) == 0
            && st_fd.st_ino == st_path.st_ino
            && st_fd.st_dev == st_path.st_dev) {
            unlink(ctl_path);
        }
    }
    if (ctl_fd >= 0) close(ctl_fd);
}

/* Adopt an inherited listen socket across exec (--reload-fds). The socket is
 * already bound + listening; we just record the fd and the existing path so
 * ctl_close() can unlink it on shutdown. */
void ctl_adopt(int fd) {
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (dir) snprintf(ctl_path, sizeof ctl_path, "%s/%s", dir, CTL_SOCK_NAME);
    ctl_fd = fd;
    int fl = fcntl(ctl_fd, F_GETFL); if (fl >= 0) fcntl(ctl_fd, F_SETFL, fl | O_NONBLOCK);
    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;
}

static int split_tab(char *s, char **argv, int maxv) {
    int n = 0;
    while (n < maxv) {
        argv[n++] = s;
        char *p = strchr(s, '\t');
        if (!p) break;
        *p = 0; s = p + 1;
    }
    return n;
}

static int parse_hex(const char *s, unsigned *out) {
    unsigned v = 0; int any = 0;
    while (*s == ' ') s++;
    if (s[0] == '#') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        char c = *s++; int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = 10 + c - 'a';
        else if (c >= 'A' && c <= 'F') d = 10 + c - 'A';
        else break;
        v = (v << 4) | d; any = 1;
    }
    if (!any) return -1;
    *out = v; return 0;
}

/* Reply "err: <why>\n". wispctl prints the reply line verbatim and exits
 * nonzero on anything but ok — so the message can say what actually went
 * wrong without breaking any script that checks the exit code. */
static int fail(Client *c, const char *fmt, ...) {
    char b[256];
    int n = snprintf(b, sizeof b, "err: ");
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(b + n, sizeof b - n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
    b[n++] = '\n';
    (void)!write(c->fd, b, n);
    return 0;
}

#ifdef WISP_HAS_MENU
#include "gen_menus.h"   /* -iquote, not -I: GENDIR holds a features.h that would shadow glibc's */

/* Open a menu declared in the .wisp. Entries are pre-rendered by wispc
 * (icon already UTF-8); the emoji preset expands the baked gemoji table. */
static int open_declared_menu(Client *c, const char *name) {
    for (const WispMenu *m = wisp_menus; m->name; m++) {
        if (strcmp(m->name, name)) continue;
        char items[MAX_ITEMS][ITEM_MAX], cmds[MAX_ITEMS][ITEM_MAX];
        int n = 0;
#ifdef WISP_MENU_EMOJI
        if (m->emoji) {
            struct { const char *glyph; const char *name; } e[] = EMOJI_INIT;
            n = (int)(sizeof e / sizeof *e);
            if (n > MAX_ITEMS) n = MAX_ITEMS;
            for (int i = 0; i < n; i++) {
                snprintf(items[i], ITEM_MAX, "%s  %s", e[i].glyph, e[i].name);
                snprintf(cmds[i],  ITEM_MAX, "printf %%s '%s' | wl-copy", e[i].glyph);
            }
        }
#endif
        for (int i = 0; i < m->n && n < MAX_ITEMS; i++, n++) {
            snprintf(items[n], ITEM_MAX, "%s", m->e[i].item);
            snprintf(cmds[n],  ITEM_MAX, "%s", m->e[i].cmd);
        }
        char title[64];
        snprintf(title, sizeof title, "%s:", name);
        Widget *mw = menu_create_action(title, items, cmds, n);
        if (!mw)
            return fail(c, "menu: no free widget slot or out of memory");
        if (m->render) mw->s.menu.render = m->render;
        (void)!write(c->fd, "ok\n", 3);
        return 0;
    }
    return fail(c, "no menu '%s' declared in the config", name);
}
#endif


/* Returns 1 if client should be kept open (deferred reply by widget), 0 close.
 * argv[] is sized for the menu command (which carries up to MAX_ITEMS items);
 * non-menu commands never use beyond the first few slots. */
static int dispatch(Client *c, char *cmd) {
    char *argv[MAX_ITEMS + 4];
    int argc = split_tab(cmd, argv, sizeof argv / sizeof *argv);
    if (!argc) return 0;
    const char *op = argv[0];

    if (!strcmp(op, "ping")) {
        (void)!write(c->fd, "pong\n", 5); return 0;
    }
    if (!strcmp(op, "quit")) {
        (void)!write(c->fd, "ok\n", 3);
        ctl_close();
        exit(0);
    }
    if (!strcmp(op, "reload")) {
        (void)!write(c->fd, "ok\n", 3);
#ifdef WISP_HAS_WALL
        /* `wispctl rebuild` sends `wall <newpath>` first, so the wallpaper is
         * mid-crossfade right now. Exec'ing here would tear the surface down
         * halfway through it; wait for the fade's last frame instead. */
        if (wall_fade_active()) { reload_pending = 1; return 0; }
#endif
        ctl_reload_exec();
        return 0;
    }
    /* tag <n> [output-slot] — view tag n (1-based,
     * matching the DSL `tag.index`). Driven by the bar's workspace on_click,
     * which passes `tag.output` so the clicked monitor switches, not the
     * kbd-focused one. Without the slot arg, falls back to focused output. */
    if (!strcmp(op, "tag")) {
        if (argc < 2) return fail(c, "usage: tag <n> [output-slot]");
        Output *o = focused_output;
        if (argc >= 3) {
            int oi = atoi(argv[2]);
            if (oi >= 0 && oi < MAX_OUTPUTS && outputs[oi].active) o = &outputs[oi];
        }
        tags_view(o, atoi(argv[1]));
        (void)!write(c->fd, "ok\n", 3); return 0;
    }
#ifdef WISP_HAS_BAR
    if (!strcmp(op, "bar")) {
        if (argc < 2) return fail(c, "usage: bar title|tags|refresh ...");
        const char *sub = argv[1];
        if (!strcmp(sub, "title")) {
            bar_set_title(argc >= 3 ? argv[2] : "");
            (void)!write(c->fd, "ok\n", 3); return 0;
        }
        if (!strcmp(sub, "tags") && argc >= 5) {
            unsigned occ, act, urg;
            if (parse_hex(argv[2], &occ) || parse_hex(argv[3], &act) || parse_hex(argv[4], &urg))
                return fail(c, "bar tags: masks must be hex");
            bar_set_tags(occ, act, urg);
            (void)!write(c->fd, "ok\n", 3); return 0;
        }
        if (!strcmp(sub, "refresh")) {
            bar_redraw_all();
            (void)!write(c->fd, "ok\n", 3); return 0;
        }
        return fail(c, "unknown bar subcommand: %s", sub);
    }
#endif
#ifdef WISP_HAS_MENU
    if (!strcmp(op, "menu") || !strcmp(op, "hud-cancel") || !strcmp(op, "menu-cancel")) {
        if (!strcmp(op, "hud-cancel") || !strcmp(op, "menu-cancel")) {
            menu_cancel_all();
            (void)!write(c->fd, "ok\n", 3); return 0;
        }
        /* `menu <name>` opens a menu declared in the .wisp; two or more args
         * is still the ad-hoc "pick one of these strings" form. */
        if (argc == 2) return open_declared_menu(c, argv[1]);
        if (argc < 3) return fail(c, "usage: menu <title> <item>... | menu <name>");
        const char *title = argv[1];
        int n = argc - 2;
        if (n > MAX_ITEMS) n = MAX_ITEMS;
        /* 40 KB scratch — declared inside the branch so non-menu commands
         * (bar tags, osd, notify, ...) don't reserve it in the stack frame. */
        char items[MAX_ITEMS][ITEM_MAX];
        for (int i = 0; i < n; i++) {
            size_t l = strnlen(argv[2 + i], ITEM_MAX - 1);
            memcpy(items[i], argv[2 + i], l); items[i][l] = 0;
        }
        Widget *w = menu_create(title[0] ? title : NULL, items, n, c->fd);
        if (!w) return fail(c, "menu: no free widget slot or out of memory");
        return 1;  /* fd handed off; reply on pick/cancel */
    }
    /* App launcher: daemon owns items + exec; reply immediately. */
    if (!strcmp(op, "apps")) {
        if (apps_open()) (void)!write(c->fd, "ok\n", 3);
        else             return fail(c, "apps: no free widget slot or out of memory");
        return 0;
    }
#endif
#ifdef WISP_HAS_HUD
    if (!strcmp(op, "hud")) {
        /* `hud probe` was a no-op shim for the old hardcoded probe table.
         * Probes now live in the DSL as exec_line sources; reply ok for
         * back-compat callers (e.g. older wispctl scripts). */
        (void)!write(c->fd, "ok\n", 3); return 0;
    }
#endif
#ifdef WISP_HAS_OSD
    /* osd <slot> <summary> [progress] [icon-cp] [muted0/1]
     *   slot: small uint, used as replace_id so successive presses replace
     *   progress: -1 omits the bar
     *   icon-cp: hex nerd-font codepoint (e.g. f028 for volume)
     *   muted: 1 → red styling */
    if (!strcmp(op, "osd")) {
        if (argc < 3) return fail(c, "usage: osd <slot> <summary> [progress] [icon-cp] [muted]");
        unsigned slot = 0; parse_hex(argv[1], &slot);
        const char *summary = argv[2];
        int progress = argc >= 4 ? atoi(argv[3]) : -1;
        unsigned icon = 0;
        if (argc >= 5) parse_hex(argv[4], &icon);
        int muted = argc >= 6 ? atoi(argv[5]) : 0;
        osd_post(slot ? slot : 0, summary, "", icon, progress, 0, muted, OSD_TIMEOUT_OSD);
        (void)!write(c->fd, "ok\n", 3); return 0;
    }
    /* notify <urgency:0|1|2> <summary> [body] [icon-cp] [timeout-ms]
     *   timeout: -1 → urgency default; 0 → sticky. */
    if (!strcmp(op, "notify")) {
        if (argc < 3) return fail(c, "usage: notify <urgency> <summary> [body] [icon-cp] [timeout-ms]");
        int urgency = atoi(argv[1]);
        const char *summary = argv[2];
        const char *body = argc >= 4 ? argv[3] : "";
        unsigned icon = 0;
        if (argc >= 5) parse_hex(argv[4], &icon);
        int timeout = argc >= 6 ? atoi(argv[5]) : -1;
        osd_post(0, summary, body, icon, -1, urgency, 0, timeout);
        (void)!write(c->fd, "ok\n", 3); return 0;
    }
    if (!strcmp(op, "osd-clear")) {
        osd_close_all();
        (void)!write(c->fd, "ok\n", 3); return 0;
    }
#endif
#ifdef WISP_HAS_MENU
#endif
#ifdef WISP_HAS_MEDIA
    /* media controls — absorbed dwl-osd. */
    if (!strcmp(op, "volume")) {
        media_volume(argc >= 2 ? argv[1] : "");
        (void)!write(c->fd, "ok\n", 3); return 0;
    }
    if (!strcmp(op, "mic")) {
        media_mic(argc >= 2 ? argv[1] : "");
        (void)!write(c->fd, "ok\n", 3); return 0;
    }
    if (!strcmp(op, "backlight")) {
        media_backlight(argc >= 2 ? argv[1] : "");
        (void)!write(c->fd, "ok\n", 3); return 0;
    }
#endif
#ifdef WISP_HAS_GAMMA
    /* gamma auto|day|night|flat|off|state|is-warm */
    if (!strcmp(op, "gamma")) {
        if (argc < 2) return fail(c, "usage: gamma auto|day|night|flat|off|state|is-warm");
        const char *sub = argv[1];
        if (!strcmp(sub, "auto"))       gamma_set_mode(GM_AUTO);
        else if (!strcmp(sub, "day"))   gamma_set_mode(GM_DAY);
        else if (!strcmp(sub, "night")) gamma_set_mode(GM_NIGHT);
        else if (!strcmp(sub, "flat"))  gamma_set_mode(GM_FLAT);
        else if (!strcmp(sub, "off"))   gamma_set_mode(GM_OFF);
        else if (!strcmp(sub, "state")) {
            const char *s = gamma_mode_str();
            (void)!write(c->fd, s, strlen(s));
            (void)!write(c->fd, "\n", 1);
            return 0;
        }
        else if (!strcmp(sub, "is-warm")) {
            /* exit 0 if warm, 1 otherwise — HUD state_cmd convention */
            const char *s = gamma_is_warm() ? "1\n" : "0\n";
            (void)!write(c->fd, s, 2);
            return 0;
        }
        else return fail(c, "unknown gamma mode: %s", sub);
        (void)!write(c->fd, "ok\n", 3);
        return 0;
    }
#endif
#ifdef WISP_HAS_LOCK
    /* lock — engage session lock. Returns "ok" once requested; the compositor
     * confirms via locked event. Idempotent if already locked. */
    if (!strcmp(op, "lock")) {
        lock_engage();
        (void)!write(c->fd, "ok\n", 3);
        return 0;
    }
#endif
#ifdef WISP_HAS_WALL
    /* wall <path> — switch the wallpaper at runtime with a crossfade. Path is
     * resolved by the daemon ("~/" ok); a missing file keeps the current one. */
    if (!strcmp(op, "wall")) {
        if (argc < 2 || !argv[1][0]) return fail(c, "usage: wall <path.png>");
        if (wall_set(argv[1]) < 0)
            return fail(c, "unreadable or not a PNG: %s", argv[1]);
        (void)!write(c->fd, "ok\n", 3);
        return 0;
    }
#endif
    /* hide on|off|toggle|status — surfaces whose .wisp gates
     * `visible = !ui_hidden()` are destroyed/recreated (exclusive zones
     * release, so windows reclaim the space). ponytail: global, not
     * per-output — apply_visibility has no focused-output filter yet. */
    if (!strcmp(op, "hide")) {
        if (argc < 2) return fail(c, "usage: hide on|off|toggle|status");
        const char *sub = argv[1];
        if (!strcmp(sub, "on"))          ui_hidden = 1;
        else if (!strcmp(sub, "off"))    ui_hidden = 0;
        else if (!strcmp(sub, "toggle")) ui_hidden = !ui_hidden;
        else if (!strcmp(sub, "status")) {
            (void)!write(c->fd, ui_hidden ? "on\n" : "off\n", ui_hidden ? 3 : 4);
            return 0;
        }
        else return fail(c, "unknown hide subcommand: %s", sub);
        {
            extern void wispgen_wisp_state_changed(void) __attribute__((weak));
            if (wispgen_wisp_state_changed) wispgen_wisp_state_changed();
        }
        (void)!write(c->fd, "ok\n", 3);
        return 0;
    }
#ifdef WISP_HAS_OSD
    /* dnd on|off|toggle|status — when on, dbus app notifications (urgency<2)
     * are swallowed. Critical urgency=2 always passes through. */
    if (!strcmp(op, "dnd")) {
        if (argc < 2) return fail(c, "usage: dnd on|off|toggle|status");
        const char *sub = argv[1];
        if (!strcmp(sub, "on"))      dnd_on = 1;
        else if (!strcmp(sub, "off")) dnd_on = 0;
        else if (!strcmp(sub, "toggle")) dnd_on = !dnd_on;
        else if (!strcmp(sub, "status")) {
            (void)!write(c->fd, dnd_on ? "on\n" : "off\n", dnd_on ? 3 : 4);
            return 0;
        }
        else return fail(c, "unknown dnd subcommand: %s", sub);
        {
            extern void wispgen_wisp_state_changed(void) __attribute__((weak));
            if (wispgen_wisp_state_changed) wispgen_wisp_state_changed();
        }
        (void)!write(c->fd, "ok\n", 3);
        return 0;
    }
#endif
    /* Feature-gated ops fall through here when their #ifdef is off — say so,
     * a bare "err" once cost a debugging session against a stale daemon. */
    return fail(c, "unknown command: %s (not in this build/preset?)", op);
    return 0;
}

void ctl_read(Client *c) {
    for (;;) {
        ssize_t n = recv(c->fd, c->buf + c->len, sizeof c->buf - 1 - c->len, 0);
        if (n < 0) {
            if (errno == EAGAIN) return;
            client_close(c); return;
        }
        if (n == 0) { client_close(c); return; }
        c->len += n;
        c->buf[c->len] = 0;
        char *nl = memchr(c->buf, '\n', c->len);
        if (!nl) {
            if (c->len >= (int)sizeof c->buf - 1) client_close(c);
            return;
        }
        *nl = 0;
        if (dispatch(c, c->buf) == 0) client_close(c);
        else {
            /* fd handed off to a widget (menu); it owns lifecycle now. The
             * widget replies synchronously and doesn't read further events,
             * so detach from epoll to avoid spurious wakeups on peer EOF. */
            epoll_del_fd(c->fd);
            c->fd = -1; c->len = 0;
        }
        return;
    }
}

void ctl_accept(void) {
    for (;;) {
        int fd = accept4(ctl_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) {
            if (errno == EAGAIN) return;
            msg("ctl accept: %s", strerror(errno));
            return;
        }
        if (!client_add(fd)) close(fd);
    }
}

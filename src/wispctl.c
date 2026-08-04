/* wispctl — client for the wisp control socket. Every argv is joined with tabs
 * and shipped to the daemon, which parses it in ctl.c dispatch(); `help` below
 * is the only command handled client-side. Keep the two in sync. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <time.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>

/* Overridden by the Makefile with the real $(PREFIX)/share/wisp. */
#ifndef WISP_DATADIR
#define WISP_DATADIR "/usr/local/share/wisp"
#endif

static int connect_daemon(void) {
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir) { fprintf(stderr, "wispctl: XDG_RUNTIME_DIR not set\n"); return -1; }
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (snprintf(a.sun_path, sizeof a.sun_path, "%s/wisp.sock", dir)
        >= (int)sizeof a.sun_path) return -1;
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) { perror("socket"); return -1; }
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
        fprintf(stderr, "wispctl: connect %s: %s\n", a.sun_path, strerror(errno));
        close(s); return -1;
    }
    return s;
}

/* The wallpaper the just-built binary will use, read back out of wispc's
 * emitted overrides — the running daemon can't know it, the path is baked into
 * generated C. Absent when the config declares no `wallpaper { path }`; the
 * caller then just skips the fade rather than duplicating config.h's default.
 * Returns 1 on a hit. */
static int new_wall_path(const char *src, char *out, size_t outsz) {
    char p[PATH_MAX], cfg[NAME_MAX] = "";
    /* Per-config build caches: the gen dir is build/<name>/gen-tw. make just
     * wrote the resolved selection to build/.selected — take the WISP field's
     * basename (FRACTIONAL= delimits it; only FONT paths may contain spaces). */
    snprintf(p, sizeof p, "%s/build/.selected", src);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    char line[PATH_MAX + 64];
    if (fgets(line, sizeof line, f)) {
        char *w = strstr(line, "WISP="), *end = w ? strstr(w, " FRACTIONAL=") : NULL;
        if (w && end) {
            *end = 0;
            char *base = strrchr(w + 5, '/');
            base = base ? base + 1 : w + 5;
            char *dot = strrchr(base, '.');
            if (dot) *dot = 0;
            snprintf(cfg, sizeof cfg, "%s", base);
        }
    }
    fclose(f);
    if (!cfg[0]) return 0;
    snprintf(p, sizeof p, "%s/build/%s/gen-tw/gen_overrides.h", src, cfg);
    f = fopen(p, "r");
    if (!f) return 0;
    int got = 0;
    while (fgets(line, sizeof line, f)) {
        char *q = strstr(line, "#define WALL_PATH \"");
        if (!q) continue;
        q += sizeof "#define WALL_PATH \"" - 1;
        char *end = strchr(q, '"');
        if (!end) continue;
        *end = 0;
        got = snprintf(out, outsz, "%s", q) < (int)outsz;
    }
    fclose(f);
    return got;
}

/* Config discovery walk. nftw() has no user-data hook, so the walk state is
 * file-static — wispctl is a one-shot process, one walk per run. */
#define WALK_TIED 8
static const char *walk_name;
static size_t walk_namelen;
static char walk_hit[WALK_TIED][PATH_MAX];
static int walk_nhit, walk_depth;

static int walk_cb(const char *p, const struct stat *sb, int t, struct FTW *f) {
    const char *base = p + f->base;
    /* Dot-dirs are never config trees; 8 levels is deeper than any real one. */
    if (t == FTW_D)
        return (f->level >= 8 || (f->level && base[0] == '.'))
               ? FTW_SKIP_SUBTREE : FTW_CONTINUE;
    if (t != FTW_F || base[0] == '.' || f->level > walk_depth) return FTW_CONTINUE;
    if (strncmp(base, walk_name, walk_namelen)
        || (base[walk_namelen] && strcmp(base + walk_namelen, ".wisp")))
        return FTW_CONTINUE;
    if (f->level < walk_depth) { walk_depth = f->level; walk_nhit = 0; }
    if (walk_nhit < WALK_TIED) snprintf(walk_hit[walk_nhit], PATH_MAX, "%s", p);
    walk_nhit++;
    return FTW_CONTINUE;
}

/* A literal path wins outright; otherwise <conf> is walked recursively for
 * `<name>.wisp` or a plain file `<name>`, shallowest match winning. A tie is a
 * hard error — silently picking one of two same-named configs costs an hour of
 * confusion. FTW_PHYS keeps symlinked dirs (and their loops) out of the walk.
 * Prints its own diagnostic; returns 1 on a hit. */
static int resolve_config(const char *name, const char *conf, const char *src,
                          char *out) {
    if (access(name, R_OK) == 0 && realpath(name, out)) return 1;

    walk_name = name; walk_namelen = strlen(name);
    walk_nhit = 0; walk_depth = 1 << 20;
    nftw(conf, walk_cb, 16, FTW_PHYS | FTW_ACTIONRETVAL);
    if (walk_nhit > 1) {
        fprintf(stderr, "wispctl: config '%s' is ambiguous, %d matches:\n",
                name, walk_nhit);
        for (int i = 0; i < walk_nhit && i < WALK_TIED; i++)
            fprintf(stderr, "  %s\n", walk_hit[i]);
        return 0;
    }
    if (walk_nhit == 1 && realpath(walk_hit[0], out)) return 1;

    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/configs/%s.wisp", src, name);
    if (access(p, R_OK) == 0 && realpath(p, out)) return 1;

    fprintf(stderr, "wispctl: config '%s' not found "
            "(searched %s recursively and %s/configs)\n", name, conf, src);
    return 0;
}

/* rebuild [name] — recompile the installed daemon from a .wisp, then reload.
 * Runtime sources come from $WISP_SRC (a checkout) or the installed share dir;
 * the chosen config is remembered in <confdir>/current so a bare `rebuild`
 * repeats it. With no name and no memory, make's own sticky selection rules. */
static int cmd_rebuild(const char *name) {
    char conf[PATH_MAX];
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(conf, sizeof conf, "%s/wisp", xdg);
    else if (home)   snprintf(conf, sizeof conf, "%s/.config/wisp", home);
    else { fprintf(stderr, "wispctl: HOME not set\n"); return 1; }

    const char *src = getenv("WISP_SRC");
    if (!src || !*src) src = WISP_DATADIR;
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/Makefile", src);
    if (access(path, R_OK) != 0) {
        fprintf(stderr, "wispctl: no runtime sources at %s "
                "(run the installer, or point $WISP_SRC at a wisp checkout)\n", src);
        return 1;
    }

    char cur[PATH_MAX], wisp[PATH_MAX] = "";
    snprintf(cur, sizeof cur, "%s/current", conf);
    if (name) {
        if (!resolve_config(name, conf, src, wisp)) return 1;
        /* Remember even if the build then fails: a bare `rebuild` retrying
         * the config you're fixing is the behavior you want. */
        mkdir(conf, 0755);
        FILE *f = fopen(cur, "w");
        if (f) { fprintf(f, "%s\n", wisp); fclose(f); }
    } else {
        FILE *f = fopen(cur, "r");
        if (f) {
            if (fgets(wisp, sizeof wisp, f)) wisp[strcspn(wisp, "\n")] = 0;
            fclose(f);
        }
    }

    /* Friendly TTY build: capture make's output and show a spinner instead of
     * a silent stall; on failure replay the captured log so compiler errors
     * are intact. Non-tty (scripts, syml.sh) keeps the raw passthrough. */
    char cfgname[NAME_MAX] = "config";
    if (wisp[0]) {
        char *b = strrchr(wisp, '/');
        snprintf(cfgname, sizeof cfgname, "%s", b ? b + 1 : wisp);
        char *dot = strrchr(cfgname, '.');
        if (dot) *dot = 0;
    }
    int tty = isatty(2);
    int logfd = -1;
    if (tty) {
        char tmpl[] = "/tmp/wispctl-build-XXXXXX";
        logfd = mkstemp(tmpl);
        if (logfd >= 0) unlink(tmpl); else tty = 0;
    }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        if (tty) { dup2(logfd, 1); dup2(logfd, 2); }
        char wisparg[PATH_MAX + 8];
        const char *mkargv[8] = { "make", "-s", "-C", src, "install",
                                  "WISP_NOWARM=1" };
        int n = 6;
        if (wisp[0]) {
            snprintf(wisparg, sizeof wisparg, "WISP=%s", wisp);
            mkargv[n++] = wisparg;
        }
        mkargv[n] = NULL;
        execvp("make", (char *const *)mkargv);
        perror("wispctl: exec make");
        _exit(127);
    }
    static const char *frames[] = { "⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏" };
    int st, fi = 0;
    for (;;) {
        pid_t r = waitpid(pid, &st, tty ? WNOHANG : 0);
        if (r < 0) { if (errno == EINTR) continue; perror("waitpid"); return 1; }
        if (r == pid) break;
        fprintf(stderr, "\r\033[32m%s\033[0m compiling %s ", frames[fi++ % 10], cfgname);
        nanosleep(&(struct timespec){ 0, 100 * 1000 * 1000 }, NULL);
    }
    if (tty) fputs("\r\033[K", stderr);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        if (tty) {
            /* Replay the log with the severity word tinted, so wispc/gcc
             * errors read as styled as the success path. Plain text otherwise. */
            lseek(logfd, 0, SEEK_SET);
            FILE *lf = fdopen(logfd, "r");
            char line[4096];
            while (lf && fgets(line, sizeof line, lf)) {
                char *p;
                if ((p = strstr(line, "error:"))) {
                    fprintf(stderr, "%.*s\033[1;31merror:\033[0m%s",
                            (int)(p - line), line, p + 6);
                } else if ((p = strstr(line, "warning:"))) {
                    fprintf(stderr, "%.*s\033[1;33mwarning:\033[0m%s",
                            (int)(p - line), line, p + 8);
                } else if (!strncmp(line, "  help: ", 8)) {
                    line[strcspn(line, "\n")] = 0;
                    fprintf(stderr, "  \033[32m%s\033[0m\n", line + 2);
                } else if ((p = strstr(line, "note:"))) {
                    fprintf(stderr, "%.*s\033[36mnote:\033[0m%s",
                            (int)(p - line), line, p + 5);
                } else {
                    fputs(line, stderr);
                }
            }
            if (lf) fclose(lf); else close(logfd);
            fprintf(stderr, "\033[1;31m✗\033[0m build failed\n");
        } else {
            fprintf(stderr, "wispctl: build failed\n");
            if (logfd >= 0) close(logfd);
        }
        return 1;
    }
    if (logfd >= 0) close(logfd);
    if (tty) fprintf(stderr, "\033[32m✓\033[0m compiled %s\n", cfgname);

    int s = connect_daemon();
    if (s < 0) {
        fprintf(stderr, "wispctl: installed; wisp not running — start it with `wisp`\n");
        return 0;
    }

    /* Crossfade to the new wallpaper in the OLD process, then reload: the
     * daemon holds the exec until the fade's last frame is on screen, so the
     * switch is a transition instead of a blank frame. The old process also
     * eats the ~150 ms decode and seeds the bg cache, so the new binary's
     * first paint is a read(). A no-op when the path is unchanged (wall_set)
     * or unreadable (err reply) — neither may block the reload. */
    char wall[PATH_MAX];
    if (new_wall_path(src, wall, sizeof wall)) {
        char req[PATH_MAX + 8];
        int n = snprintf(req, sizeof req, "wall\t%s\n", wall);
        if (n > 0 && n < (int)sizeof req
            && send(s, req, (size_t)n, MSG_NOSIGNAL) > 0) {
            char ack[64];
            (void)!recv(s, ack, sizeof ack, 0);
        }
        close(s);
        s = connect_daemon();
        if (s < 0) return 1;
    }

    if (send(s, "reload\n", 7, MSG_NOSIGNAL) < 0) { perror("send"); close(s); return 1; }
    char rep[64];
    ssize_t k = recv(s, rep, sizeof rep - 1, 0);
    close(s);
    if (k > 0) {
        rep[k] = 0;
        if (tty && !strncmp(rep, "ok", 2))
            fprintf(stderr, "\033[32m✓\033[0m reloaded\n");
        else
            fputs(rep, stdout);
    }

    /* Warm the other configs' caches AFTER the reload, detached: the switch
     * shouldn't wait on rebuilding configs it isn't switching to. Double-fork
     * so no zombie; output discarded — warm-cache itself only warns. */
    pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            int nul = open("/dev/null", O_RDWR);
            if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
            execvp("make", (char *const[]){ "make", "-s", "-C",
                                            (char *)src, "warm-cache", NULL });
        }
        _exit(0);
    }
    if (pid > 0) while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return 0;
}

/* update — re-run the curl-pipable installer (it always fetches latest), then
 * re-exec the freshly installed wispctl to rebuild + reload. Exec, not a call:
 * this process is the OLD binary; rebuild must run in the new one. */
static int cmd_update(void) {
    const char *url =
        "https://raw.githubusercontent.com/darealhoks/wisp/main/install.sh";
    char cmd[512];
    /* ponytail: system() + curl-or-wget; PREFIX passes through the env. */
    snprintf(cmd, sizeof cmd,
             "{ curl -fsSL %s 2>/dev/null || wget -qO- %s; } | sh", url, url);
    int st = system(cmd);
    if (st != 0) { fprintf(stderr, "wispctl: update failed\n"); return 1; }
    execvp("wispctl", (char *const[]){ "wispctl", "rebuild", NULL });
    perror("wispctl: exec wispctl");
    return 1;
}

/* Commands below a module heading only exist if that module is declared in the
 * .wisp the running daemon was built from; the daemon replies "err" otherwise. */
static const char USAGE[] =
"usage: wispctl <command> [args...]\n"
"\n"
"daemon\n"
"  ping                      check the daemon is up (replies \"pong\")\n"
"  reload                    re-exec the installed wisp binary in place.\n"
"                            does NOT rebuild: use `rebuild` for that\n"
"  rebuild [config]          recompile from a .wisp, install, reload.\n"
"                            config = a name found anywhere under\n"
"                            ~/.config/wisp (searched recursively), or a path;\n"
"                            omitted = the last one used\n"
"  update                    fetch + install the latest wisp from github,\n"
"                            then rebuild + reload\n"
"  quit                      stop the daemon\n"
"  hide on|off|toggle|status hide surfaces that gate on ui_hidden()\n"
"  wall <path>               switch the wallpaper (png only, crossfade); lasts until\n"
"                            reload — put it in the .wisp to persist\n"
"  tag <n> [output]          switch to tag n (1-based)\n"
"\n"
"bar\n"
"  bar refresh               redraw now\n"
"  bar tags <occ> <act> <urg>  set workspace bitmasks (hex)\n"
"\n"
"menu\n"
"  menu <title> <item>...    pick one; prints \"<index>\\t<text>\", exit 1 if cancelled\n"
"  menu-cancel               close any open menu\n"
"  apps                      open the application launcher\n"
"  menu <name>               open a menu declared in the .wisp\n"
"  menu --at x,w[,below] ... hang the menu under an explicit rect (logical px)\n"
"\n"
"osd / notifications\n"
"  osd <slot> <summary> [progress] [icon-cp] [muted]\n"
"                            progress -1 omits the bar; icon-cp is hex; same\n"
"                            slot replaces the previous slab\n"
"  tooltip <x> <width> <below> <text> | tooltip hide\n"
"  notify [-t] <urgency> <summary> [body] [icon-cp] [timeout-ms]\n"
"                            urgency 0|1|2; timeout -1 default, 0 sticky;\n"
"                            -t = transient, skipped by the notification center\n"
"  osd-clear                 dismiss everything on screen\n"
"  dnd on|off|toggle|status  do not disturb\n"
"  notif open|close|toggle|status    show/hide the notification center panel\n"
"  notif dismiss <id> | notif clear  drop one history row (note.id) or all\n"
"\n"
"media\n"
"  volume up|down|mute       also shows the OSD\n"
"  mic mute\n"
"  backlight up|down\n"
"\n"
"media players / tray (present only if the config declares the source)\n"
"  mpris play-pause|next|prev\n"
"  tray activate|secondary|menu <index>  index is the tray item's loop index\n"
"\n"
"gamma\n"
"  gamma auto|day|night|flat|off\n"
"  gamma state               print the current mode\n"
"  gamma is-warm             exit 0 if the screen is warmed\n"
"\n"
"lock\n"
"  lock                      execs wisp-lock directly, so it works even if\n"
"                            the daemon is down\n"
"\n"
"status commands (hide, dnd, gamma is-warm) exit 0 when active, 1 when not,\n"
"so they can be used as HUD state probes.\n";

int main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "help")
        || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        FILE *f = argc < 2 ? stderr : stdout;
        fputs(USAGE, f);
        return argc < 2 ? 2 : 0;
    }
    /* `wispctl lock` is intentionally NOT a socket command — we exec wisp-lock
     * directly so the session can still be locked when the wisp daemon is
     * down or crashed. The lock binary owns its own Wayland connection and
     * outlives wisp. */
    if (!strcmp(argv[1], "update"))
        return cmd_update();
    if (!strcmp(argv[1], "rebuild"))
        return cmd_rebuild(argc > 2 ? argv[2] : NULL);
    if (!strcmp(argv[1], "lock")) {
        execvp("wisp-lock", (char *const[]){ "wisp-lock", NULL });
        perror("wispctl: exec wisp-lock");
        return 1;
    }
    /* The daemon resolves paths against ITS cwd; absolutize a relative one.
     * (static: gcc flags a stack buffer stored into argv as dangling.) */
    static char rp[PATH_MAX];
    if (argc >= 3 && !strcmp(argv[1], "wall")
        && argv[2][0] != '/' && argv[2][0] != '~' && realpath(argv[2], rp))
        argv[2] = rp;

    char msg[16384];
    int n = 0;
    for (int i = 1; i < argc; i++) {
        int r = snprintf(msg + n, sizeof msg - n,
                         i == 1 ? "%s" : "\t%s", argv[i]);
        if (r < 0 || r >= (int)(sizeof msg - n)) {
            fprintf(stderr, "wispctl: command too long\n"); return 1;
        }
        n += r;
    }
    if (n + 1 >= (int)sizeof msg) return 1;
    msg[n++] = '\n';

    int s = connect_daemon();
    if (s < 0) return 1;
    if (send(s, msg, n, MSG_NOSIGNAL) < 0) { perror("send"); close(s); return 1; }

    char rep[256];
    int r = 0;
    for (;;) {
        ssize_t k = recv(s, rep + r, sizeof rep - 1 - r, 0);
        if (k < 0) { if (errno == EINTR) continue; perror("recv"); close(s); return 1; }
        if (k == 0) break;
        r += k;
        if (memchr(rep, '\n', r)) break;
        if (r >= (int)sizeof rep - 1) break;
    }
    close(s);
    if (r == 0) return 1;
    rep[r] = 0;
    char *nl = strchr(rep, '\n'); if (nl) *nl = 0;
    fputs(rep, stdout); fputc('\n', stdout);

    if (!strcmp(argv[1], "menu") || !strcmp(argv[1], "menu-cancel")) {
        int idx = atoi(rep);
        return idx < 0 ? 1 : 0;
    }
    /* `dnd status` → exit 0 if DnD active (mirrors HUD probe contract). */
    if (argc >= 3 && !strcmp(argv[1], "dnd") && !strcmp(argv[2], "status"))
        return strcmp(rep, "on") == 0 ? 0 : 1;
    /* `notif status` → exit 0 if the center is open (same contract as dnd). */
    if (argc >= 3 && !strcmp(argv[1], "notif") && !strcmp(argv[2], "status"))
        return strcmp(rep, "on") == 0 ? 0 : 1;
    /* `hide status` → exit 0 if surfaces hidden (same contract as dnd). */
    if (argc >= 3 && !strcmp(argv[1], "hide") && !strcmp(argv[2], "status"))
        return strcmp(rep, "on") == 0 ? 0 : 1;
    /* `gamma is-warm` → exit 0 if currently warming the screen (HUD probe). */
    if (argc >= 3 && !strcmp(argv[1], "gamma") && !strcmp(argv[2], "is-warm"))
        return strcmp(rep, "1") == 0 ? 0 : 1;
    return strcmp(rep, "ok") == 0 || strcmp(rep, "pong") == 0 ? 0 : 1;
}

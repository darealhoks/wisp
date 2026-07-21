/* wispctl — client for the wisp control socket. Every argv is joined with tabs
 * and shipped to the daemon, which parses it in ctl.c dispatch(); `help` below
 * is the only command handled client-side. Keep the two in sync. */
#define _GNU_SOURCE
#include <errno.h>
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
        char cand[4][PATH_MAX];
        snprintf(cand[0], PATH_MAX, "%s", name);
        snprintf(cand[1], PATH_MAX, "%s/%s.wisp", conf, name);
        snprintf(cand[2], PATH_MAX, "%s/%s", conf, name);
        snprintf(cand[3], PATH_MAX, "%s/configs/%s.wisp", src, name);
        int found = 0;
        for (int i = 0; i < 4 && !found; i++)
            if (access(cand[i], R_OK) == 0 && realpath(cand[i], wisp)) found = 1;
        if (!found) {
            fprintf(stderr, "wispctl: config '%s' not found "
                    "(looked in %s and %s/configs)\n", name, conf, src);
            return 1;
        }
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

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        char wisparg[PATH_MAX + 8];
        const char *mkargv[7] = { "make", "-s", "-C", src, "install" };
        int n = 5;
        if (wisp[0]) {
            snprintf(wisparg, sizeof wisparg, "WISP=%s", wisp);
            mkargv[n++] = wisparg;
        }
        mkargv[n] = NULL;
        execvp("make", (char *const *)mkargv);
        perror("wispctl: exec make");
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fprintf(stderr, "wispctl: build failed\n");
        return 1;
    }

    int s = connect_daemon();
    if (s < 0) {
        fprintf(stderr, "wispctl: installed; wisp not running — start it with `wisp`\n");
        return 0;
    }
    if (send(s, "reload\n", 7, MSG_NOSIGNAL) < 0) { perror("send"); close(s); return 1; }
    char rep[64];
    ssize_t k = recv(s, rep, sizeof rep - 1, 0);
    close(s);
    if (k > 0) { rep[k] = 0; fputs(rep, stdout); }
    return 0;
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
"                            config = a name in ~/.config/wisp (or a path);\n"
"                            omitted = the last one used\n"
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
"  powermenu                 open the power menu\n"
"  emoji                     pick an emoji, copies it with wl-copy\n"
"\n"
"osd / notifications\n"
"  osd <slot> <summary> [progress] [icon-cp] [muted]\n"
"                            progress -1 omits the bar; icon-cp is hex; same\n"
"                            slot replaces the previous slab\n"
"  notify <urgency> <summary> [body] [icon-cp] [timeout-ms]\n"
"                            urgency 0|1|2; timeout -1 default, 0 sticky\n"
"  osd-clear                 dismiss everything on screen\n"
"  dnd on|off|toggle|status  do not disturb\n"
"\n"
"media\n"
"  volume up|down|mute       also shows the OSD\n"
"  mic mute\n"
"  backlight up|down\n"
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
    /* `hide status` → exit 0 if surfaces hidden (same contract as dnd). */
    if (argc >= 3 && !strcmp(argv[1], "hide") && !strcmp(argv[2], "status"))
        return strcmp(rep, "on") == 0 ? 0 : 1;
    /* `gamma is-warm` → exit 0 if currently warming the screen (HUD probe). */
    if (argc >= 3 && !strcmp(argv[1], "gamma") && !strcmp(argv[2], "is-warm"))
        return strcmp(rep, "1") == 0 ? 0 : 1;
    return strcmp(rep, "ok") == 0 || strcmp(rep, "pong") == 0 ? 0 : 1;
}

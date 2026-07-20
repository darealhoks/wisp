/* wispctl — client for the wisp control socket. Every argv is joined with tabs
 * and shipped to the daemon, which parses it in ctl.c dispatch(); `help` below
 * is the only command handled client-side. Keep the two in sync. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

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

/* Commands below a module heading only exist if that module is declared in the
 * .wisp the running daemon was built from; the daemon replies "err" otherwise. */
static const char USAGE[] =
"usage: wispctl <command> [args...]\n"
"\n"
"daemon\n"
"  ping                      check the daemon is up (replies \"pong\")\n"
"  reload                    re-exec the installed wisp binary in place.\n"
"                            does NOT rebuild: run `make install` first\n"
"  quit                      stop the daemon\n"
"  hide on|off|toggle|status hide surfaces that gate on ui_hidden()\n"
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
    if (!strcmp(argv[1], "lock")) {
        execvp("wisp-lock", (char *const[]){ "wisp-lock", NULL });
        perror("wispctl: exec wisp-lock");
        return 1;
    }
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

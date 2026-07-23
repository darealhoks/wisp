/* wispc codegen — source drivers and bindings (split from codegen.c). */
#include "codegen_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================ */
/* Source driver table                                           */
/* ============================================================ */

/* DrvKind / SrcDrv are declared in codegen_internal.h. */
static const SrcDrv DRVS[] = {
    { "clock",   DRV_CLOCK,   {{0}} },
    { "cpu",     DRV_STATUS,  {{"pct", "(status.cpu_t10 / 10)", 0},
                               {"load1", "0", 0}} },  /* load1 stub: status.c has no loadavg yet */
    { "mem",     DRV_STATUS,  {{"pct", "wispgen_mem_pct()", 0},
                               {"used_mb", "(status.mem_used_kb / 1024)", 0}} },
    { "temp",    DRV_STATUS,  {{"c", "status.cpu_temp", 0}} },
    { "bat",     DRV_STATUS,  {{"pct", "status.bat_pct", 0},
                               {"charging", "status.bat_charging", 0}} },
    { "disk",    DRV_STATUS,  {{"pct", "status.disk_pct", 0}} },
    { "vpn",     DRV_STATUS,  {{"state", "wispgen_vpn_state_s()", 1},
                               {"ok", "(status.vpn_state == 1)", 0}} },
    { "wifi",    DRV_STATUS,  {{"ssid", "wispgen_wifi_ssid()", 1},
                               {"signal", "status.wifi_level", 0}} },
    { "tags",DRV_TAGS,{{"title", "(($W)->s.bar.title)", 1}} },
    /* DRV_WISP: value is read straight from daemon state, so polling it via
     * `exec_line("wispctl …")` (a fork + socket round-trip back into ourselves
     * every tick) is pure waste — these are free and update instantly. */
    { "gamma_warm",DRV_WISP,  {{"value", "(gamma_is_warm() ? \"1\" : \"0\")", 1}} },
    { "dnd",     DRV_WISP,    {{"value", "(dnd_on ? \"on\" : \"off\")", 1}} },
    { "ui_hidden",DRV_WISP,   {{"value", "(ui_hidden ? \"1\" : \"0\")", 1}} },
    /* mpris rides DRV_WISP: no fd of its own, repainted via
     * wispgen_wisp_state_changed() which mpris.c calls on every bus event. */
    { "mpris",   DRV_WISP,    {{"title",  "mpris_title()",  1},
                               {"artist", "mpris_artist()", 1},
                               {"status", "mpris_status()", 1},
                               {"player", "mpris_player()", 1}} },
    /* tray likewise; `items` is for-only, lowered in collect_bar_items. */
    { "tray",    DRV_WISP,    {{"count", "tray_count()", 0}} },
    { "exec_line",DRV_EXEC,   {{"value", "", 1}} },  /* lowering: see lower_member */
    { "dbus_signal",DRV_DBUS, {{"value", "", 1}} },  /* lowering: see lower_member */
};
#define NDRVS (int)(sizeof DRVS / sizeof DRVS[0])

const SrcDrv *find_drv(const char *name, size_t n) {
    for (int i = 0; i < NDRVS; i++)
        if (strlen(DRVS[i].name) == n && memcmp(DRVS[i].name, name, n) == 0)
            return &DRVS[i];
    return NULL;
}
const char *drv_field_expr(const SrcDrv *d, const char *f, size_t n, int *is_str) {
    for (int i = 0; i < 8 && d->fields[i].field; i++) {
        if (strlen(d->fields[i].field) == n && memcmp(d->fields[i].field, f, n) == 0) {
            if (is_str) *is_str = d->fields[i].is_string;
            return d->fields[i].c_expr;
        }
    }
    return NULL;
}

/* SrcInst is declared in codegen_internal.h. */

int collect_srcs(Unit *u, SrcInst *out, int max) {
    int n = 0;
    for (int i = 0; i < u->n; i++) {
        Decl *d = u->decls[i];
        if (d->kind != D_SOURCE) continue;
        if (n >= max) { diag_error(d->loc, "codegen: too many sources"); return -1; }
        Expr *c = d->source.call;
        if (!c || c->kind != EX_CALL) {
            diag_error(d->loc, "codegen: source RHS must be a call"); return -1;
        }
        const SrcDrv *drv = find_drv(c->call.name, c->call.nlen);
        if (!drv) {
            diag_error(d->loc, "codegen: source type '%.*s' not supported in this slice",
                       (int)c->call.nlen, c->call.name);
            return -1;
        }
        out[n].decl = d; out[n].drv = drv; out[n].fmt = NULL; out[n].flen = 0;
        out[n].interval_ms = 1000;
        out[n].refresh_ms = 120;
        out[n].arg2 = NULL; out[n].a2len = 0;
        if (drv->drv == DRV_CLOCK) {
            if (c->call.nargs != 1 || c->call.args[0]->kind != EX_STRING) {
                diag_error(d->loc, "codegen: clock() needs one string arg"); return -1;
            }
            out[n].fmt = c->call.args[0]->str.s;
            out[n].flen = c->call.args[0]->str.n;
        } else if (drv->drv == DRV_EXEC) {
            if (c->call.nargs < 1 || c->call.args[0]->kind != EX_STRING) {
                diag_error(d->loc, "codegen: exec_line() needs a string command as the first arg"); return -1;
            }
            out[n].fmt = c->call.args[0]->str.s;
            out[n].flen = c->call.args[0]->str.n;
            for (int k = 1; k < c->call.nargs; k++) {
                const char *kn = c->call.argnames ? c->call.argnames[k] : NULL;
                size_t kl = c->call.anlen ? c->call.anlen[k] : 0;
                if (kn && kl == 5 && memcmp(kn, "every", 5) == 0 &&
                    c->call.args[k]->kind == EX_STRING) {
                    /* parse "<int>(s|ms)" */
                    const char *s = c->call.args[k]->str.s; size_t L = c->call.args[k]->str.n;
                    long v = 0; size_t i = 0;
                    while (i < L && s[i] >= '0' && s[i] <= '9') { v = v*10 + (s[i]-'0'); i++; }
                    if (i+1 < L && s[i]=='m' && s[i+1]=='s') out[n].interval_ms = (int)v;
                    else if (i < L && s[i] == 's')          out[n].interval_ms = (int)v * 1000;
                    else                                     out[n].interval_ms = (int)v;
                }
                /* refresh_ms="<n>(s|ms)" — delay before re-poll after on_click.
                 * `refresh_ms="0"` (or `refresh="instant"`) skips the timerfd
                 * and calls the kick synchronously, so the probe runs in the
                 * same iteration of the event loop. */
                if (kn && ((kl == 10 && memcmp(kn, "refresh_ms", 10) == 0)
                       ||  (kl ==  7 && memcmp(kn, "refresh",    7) == 0)) &&
                    c->call.args[k]->kind == EX_STRING) {
                    const char *s = c->call.args[k]->str.s; size_t L = c->call.args[k]->str.n;
                    if ((L == 7 && memcmp(s, "instant", 7) == 0)
                     || (L == 5 && memcmp(s, "sync", 4) == 0)) {
                        out[n].refresh_ms = 0;
                    } else {
                        long v = 0; size_t i = 0;
                        while (i < L && s[i] >= '0' && s[i] <= '9') { v = v*10 + (s[i]-'0'); i++; }
                        if (i+1 < L && s[i]=='m' && s[i+1]=='s') out[n].refresh_ms = (int)v;
                        else if (i < L && s[i] == 's')          out[n].refresh_ms = (int)v * 1000;
                        else                                    out[n].refresh_ms = (int)v;
                    }
                }
            }
            if (out[n].interval_ms < 50) out[n].interval_ms = 50;
            if (out[n].refresh_ms < 0)  out[n].refresh_ms = 0;
        } else if (drv->drv == DRV_DBUS) {
            if (c->call.nargs < 2 ||
                c->call.args[0]->kind != EX_STRING ||
                c->call.args[1]->kind != EX_STRING) {
                diag_error(d->loc, "codegen: dbus_signal() needs two string args (iface, member)");
                return -1;
            }
            out[n].fmt  = c->call.args[0]->str.s; out[n].flen  = c->call.args[0]->str.n;
            out[n].arg2 = c->call.args[1]->str.s; out[n].a2len = c->call.args[1]->str.n;
        } else if (drv->drv == DRV_TAGS) {
            /* tags(labels="term www chat", pinned="1 2 3") — both optional.
             * labels: space-separated, overrides TAG_LABELS positionally.
             * pinned: tag numbers whose pills always show (tag.pinned field). */
            for (int k = 0; k < c->call.nargs; k++) {
                const char *kn = c->call.argnames ? c->call.argnames[k] : NULL;
                size_t kl = c->call.anlen ? c->call.anlen[k] : 0;
                if (!kn || c->call.args[k]->kind != EX_STRING) {
                    diag_error(d->loc, "codegen: tags() args must be labels=\"...\" / pinned=\"...\"");
                    return -1;
                }
                if (kl == 6 && memcmp(kn, "labels", 6) == 0) {
                    out[n].fmt  = c->call.args[k]->str.s;
                    out[n].flen = c->call.args[k]->str.n;
                } else if (kl == 6 && memcmp(kn, "pinned", 6) == 0) {
                    out[n].arg2  = c->call.args[k]->str.s;
                    out[n].a2len = c->call.args[k]->str.n;
                } else {
                    diag_error(d->loc, "codegen: tags() has no arg '%.*s'", (int)kl, kn);
                    return -1;
                }
            }
        } else if (drv->drv == DRV_STATUS && c->call.nargs >= 1 &&
                   c->call.args[0]->kind == EX_STRING) {
            /* per-kind probe arg (temp(zone=), bat(name=), wifi(iface=),
             * disk(path=)) — value only, the keyword is implied by the kind */
            out[n].fmt  = c->call.args[0]->str.s;
            out[n].flen = c->call.args[0]->str.n;
        }
        n++;
    }
    return n;
}

SrcInst *find_inst(SrcInst *s, int n, const char *name, size_t L) {
    for (int i = 0; i < n; i++)
        if (s[i].decl->nlen == L && memcmp(s[i].decl->name, name, L) == 0) return &s[i];
    return NULL;
}

/* ============================================================ */
/* gen_sources.c                                                 */
/* ============================================================ */

int has_status_src(SrcInst *s, int n) {
    for (int i = 0; i < n; i++) if (s[i].drv->drv == DRV_STATUS) return 1;
    return 0;
}
int has_tags(SrcInst *s, int n) {
    for (int i = 0; i < n; i++) if (s[i].drv->drv == DRV_TAGS) return 1;
    return 0;
}
int has_dbus_src(SrcInst *s, int n) {
    for (int i = 0; i < n; i++) if (s[i].drv->drv == DRV_DBUS) return 1;
    return 0;
}

void emit_sources(FILE *o, SrcInst *srcs, int nsrc) {
    fputs("/* Generated by wispc. Do not edit. */\n", o);
    fputs("#include \"wisp.h\"\n", o);
    fputs("#include <stdio.h>\n#include <string.h>\n#include <time.h>\n#include <errno.h>\n", o);
    fputs("#include <unistd.h>\n#include <fcntl.h>\n#include <signal.h>\n#include <sys/timerfd.h>\n#include <sys/wait.h>\n\n", o);

    /* Shared helpers (only emitted if their producing source is in use, so
     * the linker doesn't drag in unused state). */
    int need_mem = 0, need_vpn = 0, need_wifi = 0;
    for (int i = 0; i < nsrc; i++) {
        if (srcs[i].drv->drv != DRV_STATUS) continue;
        const char *nm = srcs[i].drv->name;
        if (!strcmp(nm, "mem"))  need_mem = 1;
        if (!strcmp(nm, "vpn"))  need_vpn = 1;
        if (!strcmp(nm, "wifi")) need_wifi = 1;
    }
    if (need_mem) {
        fputs("int wispgen_mem_pct(void) {\n"
              "    static long total_kb = -1;\n"
              "    if (total_kb < 0) {\n"
              "        FILE *f = fopen(\"/proc/meminfo\", \"r\");\n"
              "        if (f) {\n"
              "            char ln[128];\n"
              "            while (fgets(ln, sizeof ln, f))\n"
              "                if (!strncmp(ln, \"MemTotal:\", 9)) { sscanf(ln+9, \"%ld\", &total_kb); break; }\n"
              "            fclose(f);\n"
              "        }\n"
              "    }\n"
              "    if (total_kb <= 0 || status.mem_used_kb < 0) return 0;\n"
              "    return (int)((long)status.mem_used_kb * 100 / total_kb);\n"
              "}\n\n", o);
    }
    if (need_vpn) {
        fputs("const char *wispgen_vpn_state_s(void) {\n"
              "    switch (status.vpn_state) { case 1: return \"on\"; case 2: return \"stale\"; default: return \"off\"; }\n"
              "}\n\n", o);
    }
    if (need_wifi) {
        fputs("const char *wispgen_wifi_ssid(void) {\n"
              "    static char ssid[128] = \"\";\n"
              "    if (status.wifi_level < 0) { ssid[0] = 0; return ssid; }\n"
              "    if (ssid[0]) return ssid;\n"
              "    FILE *f = fopen(\"/proc/net/wireless\", \"r\");\n"
              "    if (f) {\n"
              "        char ln[128]; int n = 0;\n"
              "        while (fgets(ln, sizeof ln, f)) {\n"
              "            if (++n < 3) continue;\n"
              "            char *colon = strchr(ln, ':'); if (!colon) break;\n"
              "            *colon = 0; char *p = ln; while (*p == ' ') p++;\n"
              "            snprintf(ssid, sizeof ssid, \"%s\", p); break;\n"
              "        }\n"
              "        fclose(f);\n"
              "    }\n"
              "    if (!ssid[0]) snprintf(ssid, sizeof ssid, \"wifi\");\n"
              "    return ssid;\n"
              "}\n\n", o);
    }

    /* Per-source state — clock owns its own timerfd; status sources share
     * the global tick fd; tags has no fd (driven by wl_dispatch). */
    for (int i = 0; i < nsrc; i++) {
        SrcInst *s = &srcs[i];
        const char *nm = sname(s->decl->name, s->decl->nlen);
        fprintf(o, "void on_%s_change(void);\n", nm);
        if (s->drv->drv == DRV_CLOCK) {
            char *fmt = strndup0(s->fmt, s->flen);
            /* Formats without a seconds field only need one wakeup per minute.
             * strftime seconds conversions: %S %T %s %r %X %c %+ (locale ones
             * conservatively assumed to include seconds). */
            int has_sec = 0;
            for (const char *p = fmt; *p; p++)
                if (p[0] == '%' && p[1] && strchr("STsrXc+", p[1])) { has_sec = 1; break; }
            fprintf(o, "int  src_%s_fd = -1;\n", nm);
            fprintf(o, "char src_%s_value[64];\n", nm);
            fprintf(o, "static void src_%s_format(void) {\n", nm);
            fputs  ("    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);\n", o);
            fprintf(o, "    strftime(src_%s_value, sizeof src_%s_value, \"%s\", &tm);\n", nm, nm, fmt);
            fputs  ("}\n", o);
            if (has_sec) {
                fprintf(o, "void src_%s_init(void) {\n", nm);
                fprintf(o, "    src_%s_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);\n", nm);
                fputs  ("    struct itimerspec ts = { .it_value = { .tv_sec = 1 }, .it_interval = { .tv_sec = 1 } };\n", o);
                fprintf(o, "    timerfd_settime(src_%s_fd, 0, &ts, NULL);\n", nm);
                fprintf(o, "    src_%s_format();\n", nm);
                fputs  ("}\n", o);
                fprintf(o, "void src_%s_handle(void) {\n", nm);
                fprintf(o, "    uint64_t exp; (void)!read(src_%s_fd, &exp, sizeof exp);\n", nm);
                fprintf(o, "    src_%s_format(); on_%s_change();\n", nm, nm);
                fputs  ("}\n\n", o);
            } else {
                /* Absolute REALTIME timer on the minute boundary; CANCEL_ON_SET
                 * makes read() fail with ECANCELED on a wall-clock step (NTP,
                 * suspend/resume settime) so we re-align instead of drifting. */
                fprintf(o, "static void src_%s_arm(void) {\n", nm);
                fputs  ("    struct itimerspec ts = { .it_value = { .tv_sec = time(NULL) / 60 * 60 + 60 },\n"
                        "                             .it_interval = { .tv_sec = 60 } };\n", o);
                fprintf(o, "    timerfd_settime(src_%s_fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &ts, NULL);\n", nm);
                fputs  ("}\n", o);
                fprintf(o, "void src_%s_init(void) {\n", nm);
                fprintf(o, "    src_%s_fd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC | TFD_NONBLOCK);\n", nm);
                fprintf(o, "    src_%s_arm();\n", nm);
                fprintf(o, "    src_%s_format();\n", nm);
                fputs  ("}\n", o);
                fprintf(o, "void src_%s_handle(void) {\n", nm);
                fprintf(o, "    uint64_t exp;\n");
                fprintf(o, "    if (read(src_%s_fd, &exp, sizeof exp) < 0) src_%s_arm();\n", nm, nm);
                fprintf(o, "    src_%s_format(); on_%s_change();\n", nm, nm);
                fputs  ("}\n\n", o);
            }
            free(fmt);
        }
        /* DRV_STATUS and DRV_TAGS need only on_<n>_change(); the actual
         * dirty-flag setting is in gen_bindings.c. They're triggered by the
         * shared status tick / wl frame, not by a per-source fd. */
        if (s->drv->drv == DRV_EXEC) {
            /* Async exec: timer kicks off a fork; the read pipe is registered
             * with epoll. src_<n>_handle_pipe drains and finalizes when the
             * child closes stdout. Avoids deadlock when the child talks back
             * into wisp (e.g. `wispctl …`). One in-flight child max; subsequent
             * timer ticks are dropped if previous run hasn't finished. */
            char *cmd = strndup0(s->fmt, s->flen);
            fprintf(o, "int  src_%s_fd = -1;\n", nm);
            fprintf(o, "int  src_%s_pipe = -1;\n", nm);
            fprintf(o, "static pid_t src_%s_pid = 0;\n", nm);
            fprintf(o, "static int   src_%s_blen = 0;\n", nm);
            fprintf(o, "char src_%s_line[256];\n", nm);
            fprintf(o, "static char src_%s_buf[256];\n", nm);
            fputs  ("void epoll_add_fd(int); void epoll_del_fd(int);\n", o);
            fprintf(o, "static void src_%s_kick(void) {\n", nm);
            fprintf(o, "    if (src_%s_pipe >= 0) return; /* previous run still in flight */\n", nm);
            fprintf(o, "    if (src_%s_pid > 0) {\n", nm);
            fprintf(o, "        int st; pid_t w = waitpid(src_%s_pid, &st, WNOHANG);\n", nm);
            fprintf(o, "        if (w == 0) return; /* zombie wait — try again next tick */\n");
            fprintf(o, "        src_%s_pid = 0;\n", nm);
            fputs  ("    }\n", o);
            fputs  ("    int p[2]; if (pipe(p) != 0) return;\n", o);
            fputs  ("    int fl0 = fcntl(p[0], F_GETFL, 0); if (fl0 >= 0) fcntl(p[0], F_SETFL, fl0 | O_NONBLOCK);\n", o);
            fputs  ("    fcntl(p[0], F_SETFD, FD_CLOEXEC);\n", o);
            fputs  ("    pid_t pid = fork();\n", o);
            fputs  ("    if (pid < 0) { close(p[0]); close(p[1]); return; }\n", o);
            fputs  ("    if (pid == 0) {\n"
                    "        dup2(p[1], 1); close(p[0]); close(p[1]);\n"
                    "        int n = open(\"/dev/null\", 2);\n"
                    "        if (n >= 0) { dup2(n, 2); close(n); }\n"
                    "        setsid(); signal(SIGCHLD, SIG_DFL);\n", o);
            fprintf(o, "        execl(\"/bin/sh\", \"sh\", \"-c\", \"%s\", (char*)0); _exit(127);\n", cmd);
            fputs  ("    }\n", o);
            fputs  ("    close(p[1]);\n", o);
            fprintf(o, "    src_%s_pid = pid; src_%s_pipe = p[0]; src_%s_blen = 0;\n", nm, nm, nm);
            fprintf(o, "    epoll_add_fd(src_%s_pipe);\n", nm);
            fputs  ("}\n", o);
            fprintf(o, "void src_%s_handle_pipe(void) {\n", nm);
            fprintf(o, "    if (src_%s_pipe < 0) return;\n", nm);
            fputs  ("    for (;;) {\n", o);
            fprintf(o, "        int cap = (int)sizeof(src_%s_buf) - 1 - src_%s_blen;\n", nm, nm);
            fputs  ("        if (cap <= 0) { /* drain rest, keep first 255 bytes */\n", o);
            fputs  ("            char junk[256]; ssize_t rr = read(", o);
            fprintf(o, "src_%s_pipe", nm);
            fputs  (", junk, sizeof junk);\n", o);
            fputs  ("            if (rr > 0) continue;\n"
                    "            if (rr < 0 && errno == EAGAIN) return;\n", o);
            fputs  ("            break;\n", o);
            fputs  ("        }\n", o);
            fprintf(o, "        ssize_t r = read(src_%s_pipe, src_%s_buf + src_%s_blen, (size_t)cap);\n", nm, nm, nm);
            fputs  ("        if (r > 0) {\n", o);
            fprintf(o, "            src_%s_blen += (int)r; continue;\n", nm);
            fputs  ("        }\n", o);
            fputs  ("        if (r < 0 && errno == EAGAIN) return;\n", o);
            fputs  ("        break; /* EOF or hard error */\n", o);
            fputs  ("    }\n", o);
            fprintf(o, "    epoll_del_fd(src_%s_pipe); close(src_%s_pipe); src_%s_pipe = -1;\n", nm, nm, nm);
            fprintf(o, "    if (src_%s_pid > 0) {\n", nm);
            fprintf(o, "        int st; waitpid(src_%s_pid, &st, WNOHANG);\n", nm);
            fprintf(o, "        src_%s_pid = 0;\n", nm);
            fputs  ("    }\n", o);
            fprintf(o, "    src_%s_buf[src_%s_blen] = 0;\n", nm, nm);
            fprintf(o, "    char *nl = (char*)memchr(src_%s_buf, '\\n', (size_t)src_%s_blen);\n", nm, nm);
            fputs  ("    if (nl) *nl = 0;\n", o);
            fprintf(o, "    memcpy(src_%s_line, src_%s_buf, sizeof src_%s_line);\n", nm, nm, nm);
            fprintf(o, "    src_%s_line[sizeof src_%s_line - 1] = 0;\n", nm, nm);
            fprintf(o, "    on_%s_change();\n", nm);
            fputs  ("}\n", o);
            fprintf(o, "void src_%s_init(void) {\n", nm);
            fprintf(o, "    src_%s_line[0] = 0;\n", nm);
            fprintf(o, "    src_%s_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);\n", nm);
            fprintf(o, "    struct itimerspec ts = { .it_value = { .tv_sec = 0, .tv_nsec = 1 },\n");
            fprintf(o, "                             .it_interval = { .tv_sec = %d, .tv_nsec = %d } };\n",
                    s->interval_ms / 1000, (s->interval_ms % 1000) * 1000000);
            fprintf(o, "    timerfd_settime(src_%s_fd, 0, &ts, NULL);\n", nm);
            fputs  ("}\n", o);
            fprintf(o, "void src_%s_handle(void) {\n", nm);
            fprintf(o, "    uint64_t exp; (void)!read(src_%s_fd, &exp, sizeof exp);\n", nm);
            fprintf(o, "    src_%s_kick();\n", nm);
            fputs  ("}\n", o);
            /* refresh(): after a state-mutating on_click, re-poll the source.
             * Delay is set per-source via `refresh_ms=...` on exec_line(); the
             * default (120ms) lets the user's exec() land before the probe runs.
             * `refresh_ms="0"` (or `="instant"`) makes the kick synchronous —
             * use it when the click body doesn't actually need to wait. */
            fprintf(o, "void src_%s_refresh(void) {\n", nm);
            if (s->refresh_ms == 0) {
                fprintf(o, "    src_%s_kick();\n", nm);
            } else {
                fprintf(o, "    struct itimerspec __ts = { .it_value = { .tv_sec = %d, .tv_nsec = %d },\n",
                        s->refresh_ms / 1000, (s->refresh_ms % 1000) * 1000000);
                fprintf(o, "                                .it_interval = { .tv_sec = %d, .tv_nsec = %d } };\n",
                        s->interval_ms / 1000, (s->interval_ms % 1000) * 1000000);
                fprintf(o, "    timerfd_settime(src_%s_fd, 0, &__ts, NULL);\n", nm);
            }
            fputs  ("}\n\n", o);
            free(cmd);
        }
        if (s->drv->drv == DRV_DBUS) {
            char *iface  = strndup0(s->fmt,  s->flen);
            char *member = strndup0(s->arg2, s->a2len);
            /* Uppercase variant of nm for the cap macro. */
            char NM[64]; size_t nL = strlen(nm); if (nL >= sizeof NM) nL = sizeof NM - 1;
            for (size_t i = 0; i < nL; i++) NM[i] = (nm[i] >= 'a' && nm[i] <= 'z') ? nm[i] - 32 : nm[i];
            NM[nL] = 0;
            fprintf(o, "char src_%s_value[256];\n", nm);
            fprintf(o, "#define SRC_%s_HIST_CAP 8\n", NM);
            fprintf(o, "typedef struct { char summary[128]; char body[256]; char url[256]; uint8_t urgent; } src_%s_hist_t;\n", nm);
            fprintf(o, "src_%s_hist_t src_%s_hist[SRC_%s_HIST_CAP];\n", nm, nm, NM);
            fprintf(o, "int src_%s_hist_n = 0;\n", nm);
            fprintf(o, "int src_%s_hist_head = 0;\n", nm);
            fprintf(o, "static void src_%s_on_signal(const char *sender, const char *path,\n"
                       "                             const uint8_t *body, int body_len, const char *sig) {\n", nm);
            fputs  ("    (void)sender; (void)path;\n", o);
            fprintf(o, "    dbus_signal_first_str(body, body_len, sig, src_%s_value, sizeof src_%s_value);\n", nm, nm);
            fprintf(o, "    DbusNotifyFields __nf;\n");
            fprintf(o, "    if (dbus_signal_decode_notify(body, body_len, sig, &__nf) == 0) {\n");
            fprintf(o, "        int slot = src_%s_hist_head;\n", nm);
            fprintf(o, "        src_%s_hist[slot].urgent = __nf.urgent;\n", nm);
            fprintf(o, "        memcpy(src_%s_hist[slot].summary, __nf.summary, sizeof __nf.summary);\n", nm);
            fprintf(o, "        memcpy(src_%s_hist[slot].body,    __nf.body,    sizeof __nf.body);\n", nm);
            fprintf(o, "        memcpy(src_%s_hist[slot].url,     __nf.url,     sizeof __nf.url);\n", nm);
            fprintf(o, "        src_%s_hist_head = (slot + 1) %% SRC_%s_HIST_CAP;\n", nm, NM);
            fprintf(o, "        if (src_%s_hist_n < SRC_%s_HIST_CAP) src_%s_hist_n++;\n", nm, NM, nm);
            fputs  ("    }\n", o);
            fprintf(o, "    on_%s_change();\n", nm);
            fputs  ("}\n", o);
            fprintf(o, "void src_%s_init(void) {\n", nm);
            fprintf(o, "    src_%s_value[0] = 0;\n", nm);
            fprintf(o, "    dbus_subscribe(\"%s\", \"%s\", src_%s_on_signal);\n", iface, member, nm);
            fputs  ("}\n\n", o);
            free(iface); free(member);
        }
    }
    /* Shared status tick: fires every second, runs status_sample_all, then
     * calls every status source's on_change. tags sources are pinged
     * from wl.c whenever bar_set_tags_on/title_on writes. */
    if (has_status_src(srcs, nsrc)) {
        fputs("int wispgen_status_tfd = -1;\n", o);
        fputs("void wispgen_status_init(void) {\n", o);
        fputs("    wispgen_status_tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);\n", o);
        fputs("    struct itimerspec ts = { .it_value = { .tv_sec = 1 }, .it_interval = { .tv_sec = 1 } };\n", o);
        fputs("    timerfd_settime(wispgen_status_tfd, 0, &ts, NULL);\n", o);
        for (int i = 0; i < nsrc; i++)
            if (srcs[i].drv->drv == DRV_STATUS && srcs[i].fmt)
                fprintf(o, "    status_set_arg(\"%s\", \"%.*s\");\n",
                        srcs[i].drv->name, (int)srcs[i].flen, srcs[i].fmt);
        fputs("    status_init(); status_sample_all();\n", o);
        fputs("}\n", o);
        fputs("static unsigned wispgen_tick_n = 0;\n", o);
        fputs("void wispgen_status_handle(void) {\n", o);
        fputs("    uint64_t exp; (void)!read(wispgen_status_tfd, &exp, sizeof exp);\n", o);
        fputs("    status_tick(++wispgen_tick_n);\n", o);
        for (int i = 0; i < nsrc; i++) {
            if (srcs[i].drv->drv != DRV_STATUS) continue;
            fprintf(o, "    on_%s_change();\n", sname(srcs[i].decl->name, srcs[i].decl->nlen));
        }
        fputs("}\n\n", o);
    }
    /* Always emit wispgen_tags_changed() — empty stub if no tags
     * source. The generated bar setters call it unconditionally. */
    fputs("void wispgen_tags_changed(void) {\n", o);
    for (int i = 0; i < nsrc; i++) {
        if (srcs[i].drv->drv != DRV_TAGS) continue;
        fprintf(o, "    on_%s_change();\n", sname(srcs[i].decl->name, srcs[i].decl->nlen));
    }
    fputs("}\n\n", o);
    /* wispgen_wisp_state_changed() — gamma.c/ctl.c call it (weak ref, like
     * wispgen_widget_destroyed) whenever daemon state a DRV_WISP source
     * exposes mutates. Only emitted when such a source exists. */
    int has_wisp = 0;
    for (int i = 0; i < nsrc; i++) if (srcs[i].drv->drv == DRV_WISP) has_wisp = 1;
    if (has_wisp) {
        fputs("void wispgen_wisp_state_changed(void) {\n", o);
        for (int i = 0; i < nsrc; i++) {
            if (srcs[i].drv->drv != DRV_WISP) continue;
            fprintf(o, "    on_%s_change();\n", sname(srcs[i].decl->name, srcs[i].decl->nlen));
        }
        fputs("}\n\n", o);
    }
    (void)has_tags;
}

/* ============================================================ */
/* gen_bindings.c                                                */
/* ============================================================ */

void emit_bindings(FILE *o, SrcInst *srcs, int nsrc, SemaResult *r,
                          CGCtx *ctx) {
    fputs("/* Generated by wispc. Do not edit. */\n", o);
    fputs("#include \"wisp.h\"\n#include <stdio.h>\n#include <string.h>\n\n", o);
    /* Status-field helpers live in gen_sources.c; harmless if unused. */
    fputs("int wispgen_mem_pct(void);\n"
          "const char *wispgen_vpn_state_s(void);\n"
          "const char *wispgen_wifi_ssid(void);\n\n", o);
    for (int i = 0; i < r->nsurfaces; i++)
        fprintf(o, "int dirty_%s = 1;\n", r->surface_names[i]);
    fputc('\n', o);

    /* Emit `static <T> mut_<name> = <init>;` for every D_MUT declaration.
     * Type is inferred from the init's lowered CE.type. Strings get a static
     * char buffer initialised by memcpy at boot, since C doesn't allow
     * non-constant array initializers cleanly with the snprintf path. */
    for (int i = 0; i < ctx->nkonst; i++) {
        Decl *k = ctx->konst[i];
        if (k->kind != D_MUT) continue;
        const char *nm = sname(k->name, k->nlen);
        CE init = lower(ctx, k->konst.val);
        /* mut initializers must be compile-time constants. Discard any
         * snprintf prelude — if init isn't a literal, the resulting C will
         * fail to compile, which is the right signal for v0. */
        if (ctx->prelude) {
            fclose(ctx->prelude); ctx->prelude = NULL;
            free(ctx->prelude_buf); ctx->prelude_buf = NULL; ctx->prelude_sz = 0;
        }
        if (ctx->failed) return;
        const char *cty = "int";
        switch (init.type) {
        case T_FLOAT: cty = "double"; break;
        case T_COLOR: cty = "uint32_t"; break;
        case T_BOOL:  cty = "int"; break;
        case T_STR:   cty = NULL; break;
        default:      cty = "int"; break;
        }
        cgctx_flush_prelude(ctx, o, "");
        if (init.type == T_STR) {
            fprintf(o, "char mut_%s[128] = ", nm);
            /* If init is a literal string, use the literal; otherwise leave 0 */
            if (k->konst.val && k->konst.val->kind == EX_STRING) {
                fprintf(o, "%s;\n", init.text);
            } else {
                fputs("{0};\n", o);
            }
        } else {
            fprintf(o, "%s mut_%s = (%s)(%s);\n", cty, nm, cty, init.text);
        }
    }
    fputc('\n', o);
    for (int i = 0; i < nsrc; i++) {
        const char *nm = sname(srcs[i].decl->name, srcs[i].decl->nlen);
        fprintf(o, "void on_%s_change(void) {\n", nm);
        /* Status sources fire from the shared 1 Hz tick whether or not their
         * value moved; without this guard the bar repaints every second at
         * idle. Compare every driver field (any could be bound) and bail if
         * none changed. Other driver kinds only fire on a real event. */
        if (srcs[i].drv->drv == DRV_STATUS) {
            fputs("    int __chg = 0;\n", o);
            for (int f = 0; f < 8 && srcs[i].drv->fields[f].field; f++) {
                const char *ex = srcs[i].drv->fields[f].c_expr;
                if (srcs[i].drv->fields[f].is_string) {
                    fprintf(o, "    { const char *__v = %s; static char __last%d[128];\n"
                               "      if (strcmp(__v, __last%d)) { snprintf(__last%d, sizeof __last%d, \"%%s\", __v); __chg = 1; } }\n",
                            ex, f, f, f, f);
                } else {
                    fprintf(o, "    { int __v = (int)(%s); static int __last%d = -2147483647;\n"
                               "      if (__v != __last%d) { __last%d = __v; __chg = 1; } }\n",
                            ex, f, f, f);
                }
            }
            fputs("    if (!__chg) return;\n", o);
        }
        for (int j = 0; j < r->nsurfaces; j++) {
            const char **deps = r->surface_deps[j];
            for (int k = 0; deps && deps[k]; k++) {
                if (strcmp(deps[k], nm) == 0) {
                    fprintf(o, "    dirty_%s = 1;\n", r->surface_names[j]);
                    break;
                }
            }
        }
        fputs("}\n\n", o);
    }
}


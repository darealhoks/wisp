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
    /* net: up/ssid/signal are rtnetlink-event-driven; rx/tx ride the shared
     * tick, and only when a config reads them (see status_kind_polled). */
    { "net",     DRV_STATUS,  {{"up", "status.net_up", 0},
                               {"ssid", "wispgen_net_ssid()", 1},
                               {"signal", "status.wifi_level", 0},
                               {"rx_kbps", "status.net_rx_kbps", 0},
                               {"tx_kbps", "status.net_tx_kbps", 0}} },
    { "backlight",DRV_STATUS, {{"pct", "status.backlight_pct", 0}} },
    /* power_profile rides DRV_WISP: power.c owns pp_fd, repaints via
     * wispgen_wisp_state_changed() on every bus event. */
    { "power_profile",DRV_WISP,{{"profile", "pp_profile()", 1}} },
    /* bluez rides DRV_WISP: bluez.c owns bz_fd, repaints via
     * wispgen_wisp_state_changed() on every bus event. */
    { "bluez",   DRV_WISP,    {{"powered",   "bz_powered()",   0},
                               {"connected", "bz_connected()", 0},
                               {"device",    "bz_device()",    1},
                               {"battery",   "bz_battery()",   0}} },
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
    /* pipewire rides DRV_WISP: no fd registered by gen code (pipewire.c owns
     * pw_fd/pw_reconnect_fd), repainted via wispgen_wisp_state_changed(). */
    { "pipewire",DRV_WISP,    {{"vol",      "pw_vol_pct()",   0},
                               {"mute",     "pw_vol_muted()", 0},
                               {"mic_vol",  "pw_mic_vol_pct()", 0},
                               {"mic_mute", "pw_mic_muted()", 0},
                               {"ok",       "pw_ok()",        0}} },
    /* toplevel rides DRV_WISP (repainted via wispgen_wisp_state_changed(), which
     * wl_toplevel.c calls on every published change). Fields lower to
     * tl_exists/tl_count/tl_title(<idx>) where <idx> is the declaration's slot in
     * the emitted tl_match_app_ids[] table — done in lower_member, since the
     * static c_expr here can't carry the per-declaration index. */
    { "toplevel",DRV_WISP,    {{"exists", "", 0}, {"count", "", 0}, {"title", "", 1}} },
    { "exec_line",DRV_EXEC,   {{"value", "", 1}} },  /* lowering: see lower_member */
    { "inotify", DRV_INOTIFY, {{"value", "", 1}} },  /* lowering: see lower_member */
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

/* "<int>(s|ms)"; a bare number is ms. */
static int parse_dur_ms(const char *s, size_t L) {
    long v = 0;
    size_t i = 0;
    while (i < L && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; }
    if (i + 1 < L && s[i] == 'm' && s[i + 1] == 's') return (int)v;
    if (i < L && s[i] == 's') return (int)v * 1000;
    return (int)v;
}

/* every= is only meaningful on kinds the shared status tick samples;
 * vpn/bat/disk/backlight run on their own event/slow-timer cadence, and net
 * joins the tick only when the config reads its rx/tx rates. */
int cg_net_rates_used;
static int status_kind_polled(const SrcDrv *d) {
    if (!strcmp(d->name, "net")) return cg_net_rates_used;
    return strcmp(d->name, "vpn") && strcmp(d->name, "bat") &&
           strcmp(d->name, "disk") && strcmp(d->name, "backlight");
}

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
                    out[n].interval_ms = parse_dur_ms(c->call.args[k]->str.s,
                                                      c->call.args[k]->str.n);
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
                        out[n].refresh_ms = parse_dur_ms(s, L);
                    }
                }
            }
            /* every="0" = one-shot: probe at startup + on refresh() only. */
            if (out[n].interval_ms && out[n].interval_ms < 50) out[n].interval_ms = 50;
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
        } else if (drv->drv == DRV_INOTIFY) {
            /* inotify(path="/abs/file") — watch is on the parent dir so atomic
             * rename writes (editors, status files) keep working; path must be
             * absolute so the dir/base split is meaningful. */
            for (int k = 0; k < c->call.nargs; k++) {
                const char *kn = c->call.argnames ? c->call.argnames[k] : NULL;
                size_t kl = c->call.anlen ? c->call.anlen[k] : 0;
                if (kn && kl == 4 && memcmp(kn, "path", 4) == 0 &&
                    c->call.args[k]->kind == EX_STRING) {
                    out[n].fmt  = c->call.args[k]->str.s;
                    out[n].flen = c->call.args[k]->str.n;
                } else {
                    diag_error(d->loc, "codegen: inotify() takes only path=\"…\"");
                    return -1;
                }
            }
            if (!out[n].fmt || out[n].fmt[0] != '/' || out[n].fmt[out[n].flen - 1] == '/') {
                diag_error(d->loc, "codegen: inotify() requires an absolute file path=\"/…\"");
                return -1;
            }
        } else if (drv->drv == DRV_WISP && !strcmp(drv->name, "toplevel")) {
            /* toplevel(app_id="…") — app_id is required; stored in fmt for the
             * match-table emission and used as the source's match key. */
            for (int k = 0; k < c->call.nargs; k++) {
                const char *kn = c->call.argnames ? c->call.argnames[k] : NULL;
                size_t kl = c->call.anlen ? c->call.anlen[k] : 0;
                if (kn && kl == 6 && memcmp(kn, "app_id", 6) == 0 &&
                    c->call.args[k]->kind == EX_STRING) {
                    out[n].fmt  = c->call.args[k]->str.s;
                    out[n].flen = c->call.args[k]->str.n;
                } else {
                    diag_error(d->loc, "codegen: toplevel() takes only app_id=\"…\"");
                    return -1;
                }
            }
            if (!out[n].fmt) {
                diag_error(d->loc, "codegen: toplevel() requires app_id=\"…\"");
                return -1;
            }
        } else if (drv->drv == DRV_STATUS) {
            for (int k = 0; k < c->call.nargs; k++) {
                const char *kn = c->call.argnames ? c->call.argnames[k] : NULL;
                size_t kl = c->call.anlen ? c->call.anlen[k] : 0;
                if (c->call.args[k]->kind != EX_STRING) {
                    diag_error(d->loc, "codegen: %s() args must be strings", drv->name);
                    return -1;
                }
                if (kn && kl == 5 && memcmp(kn, "every", 5) == 0) {
                    if (!status_kind_polled(drv)) {
                        diag_error(d->loc, "codegen: every= only applies to polled kinds (cpu/mem/temp); %s() is event-driven", drv->name);
                        return -1;
                    }
                    out[n].interval_ms = parse_dur_ms(c->call.args[k]->str.s,
                                                      c->call.args[k]->str.n);
                    if (out[n].interval_ms < 250) {
                        diag_error(d->loc, "codegen: %s(every=) must be >= 250ms", drv->name);
                        return -1;
                    }
                } else {
                    /* per-kind probe arg (temp(zone=), bat(name=), net(iface=),
                     * disk(path=)) — value only, the keyword is implied by the kind */
                    out[n].fmt  = c->call.args[k]->str.s;
                    out[n].flen = c->call.args[k]->str.n;
                }
            }
        }
        n++;
    }
    return n;
}

int src_bit(SrcInst *srcs, int nsrc, const char *name, size_t nlen) {
    for (int i = 0; i < nsrc; i++)
        if (srcs[i].decl->nlen == nlen && memcmp(srcs[i].decl->name, name, nlen) == 0)
            return i;
    return -1;
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
static int is_status_named(const SrcInst *s, const char *nm) {
    return s->drv->drv == DRV_STATUS && !strcmp(s->drv->name, nm);
}
int has_vpn_src(SrcInst *s, int n) {
    for (int i = 0; i < n; i++) if (is_status_named(&s[i], "vpn")) return 1;
    return 0;
}
int has_net_src(SrcInst *s, int n) {
    for (int i = 0; i < n; i++) if (is_status_named(&s[i], "net")) return 1;
    return 0;
}
int has_bat_src(SrcInst *s, int n) {
    for (int i = 0; i < n; i++) if (is_status_named(&s[i], "bat")) return 1;
    return 0;
}
int has_disk_src(SrcInst *s, int n) {
    for (int i = 0; i < n; i++) if (is_status_named(&s[i], "disk")) return 1;
    return 0;
}
int has_backlight_src(SrcInst *s, int n) {
    for (int i = 0; i < n; i++) if (is_status_named(&s[i], "backlight")) return 1;
    return 0;
}
/* The 1 Hz shared tick is needed only for polled status kinds; the event/slow-
 * timer kinds don't arm it, so a config using only those runs no status timer. */
int has_polled_status_src(SrcInst *s, int n) {
    for (int i = 0; i < n; i++)
        if (s[i].drv->drv == DRV_STATUS && status_kind_polled(s[i].drv))
            return 1;
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
static int is_toplevel(const SrcInst *s) {
    return s->drv->drv == DRV_WISP && !strcmp(s->drv->name, "toplevel");
}
/* Match-table slot of a toplevel source = its ordinal among toplevel sources in
 * declaration order. wl_toplevel.c reads tl_match_app_ids[] in this same order,
 * so the index a field lowers to and the runtime's slot agree. */
int tl_match_index(SrcInst *s, int n, const SrcInst *target) {
    int idx = 0;
    for (int i = 0; i < n; i++) {
        if (&s[i] == target) return idx;
        if (is_toplevel(&s[i])) idx++;
    }
    return -1;
}

void emit_sources(FILE *o, SrcInst *srcs, int nsrc) {
    fputs("/* Generated by wispc. Do not edit. */\n", o);
    fputs("#include \"wisp.h\"\n", o);
    fputs("#include <stdio.h>\n#include <string.h>\n#include <time.h>\n#include <errno.h>\n", o);
    fputs("#include <unistd.h>\n#include <fcntl.h>\n#include <signal.h>\n#include <sys/timerfd.h>\n#include <sys/wait.h>\n#include <sys/inotify.h>\n\n", o);

    /* Shared helpers (only emitted if their producing source is in use, so
     * the linker doesn't drag in unused state). */
    int need_mem = 0, need_vpn = 0, need_net = 0;
    for (int i = 0; i < nsrc; i++) {
        if (srcs[i].drv->drv != DRV_STATUS) continue;
        const char *nm = srcs[i].drv->name;
        if (!strcmp(nm, "mem")) need_mem = 1;
        if (!strcmp(nm, "vpn")) need_vpn = 1;
        if (!strcmp(nm, "net")) need_net = 1;
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
    if (need_net) {
        fputs("const char *wispgen_net_ssid(void) {\n"
              "    static char ssid[128] = \"\";\n"
              "    if (status.wifi_level < 0) { ssid[0] = 0; return ssid; }\n"
              /* No cross-call cache: on a link change the iface may differ, and
               * callers only reach here on a wifi event or a render — cheap. */
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
        if (s->drv->drv == DRV_INOTIFY) {
            /* Poll-free file source: one inotify fd per source, watch on the
             * parent dir (IN_CLOSE_WRITE|IN_MOVED_TO|IN_CREATE|IN_DELETE) so
             * both in-place writes and atomic renames re-read the file. */
            char *path = strndup0(s->fmt, s->flen);
            char *base = strrchr(path, '/') + 1;  /* collect_srcs guarantees '/' */
            char *dir  = strndup0(path, base == path + 1 ? 1 : (size_t)(base - path - 1));
            fprintf(o, "int  src_%s_fd = -1;\n", nm);
            fprintf(o, "char src_%s_value[256];\n", nm);
            fprintf(o, "static void src_%s_read(void) {\n", nm);
            fprintf(o, "    src_%s_value[0] = 0;\n", nm);
            fprintf(o, "    FILE *f = fopen(\"%s\", \"r\");\n", path);
            fputs  ("    if (!f) return;\n", o);
            fprintf(o, "    if (fgets(src_%s_value, sizeof src_%s_value, f)) {\n", nm, nm);
            fprintf(o, "        char *nl = strchr(src_%s_value, '\\n');\n", nm);
            fputs  ("        if (nl) *nl = 0;\n", o);
            fputs  ("    } else {\n", o);
            fprintf(o, "        src_%s_value[0] = 0;\n", nm);
            fputs  ("    }\n", o);
            fputs  ("    fclose(f);\n", o);
            fputs  ("}\n", o);
            fprintf(o, "void src_%s_init(void) {\n", nm);
            fprintf(o, "    src_%s_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);\n", nm);
            fprintf(o, "    if (src_%s_fd < 0 ||\n", nm);
            fprintf(o, "        inotify_add_watch(src_%s_fd, \"%s\",\n", nm, dir);
            fputs  ("                          IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE) < 0)\n", o);
            fprintf(o, "        die(\"inotify: %s: %%s\", strerror(errno));\n", dir);
            fprintf(o, "    src_%s_read();\n", nm);
            fputs  ("}\n", o);
            fprintf(o, "void src_%s_handle(void) {\n", nm);
            fputs  ("    char buf[1024] __attribute__((aligned(__alignof__(struct inotify_event))));\n", o);
            fputs  ("    int hit = 0;\n", o);
            fputs  ("    ssize_t n;\n", o);
            fprintf(o, "    while ((n = read(src_%s_fd, buf, sizeof buf)) > 0) {\n", nm);
            fputs  ("        for (char *p = buf; p < buf + n; ) {\n", o);
            fputs  ("            struct inotify_event *e = (struct inotify_event *)p;\n", o);
            fprintf(o, "            if (e->len && strcmp(e->name, \"%s\") == 0) hit = 1;\n", base);
            fputs  ("            p += sizeof *e + e->len;\n", o);
            fputs  ("        }\n", o);
            fputs  ("    }\n", o);
            fputs  ("    if (!hit) return;\n", o);
            fprintf(o, "    src_%s_read();\n", nm);
            fprintf(o, "    on_%s_change();\n", nm);
            fputs  ("}\n\n", o);
            free(dir); free(path);
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
    /* One-shot status setup: per-kind probe args + initial full sample. Needed
     * whenever any status source exists — including a vpn/net-only config that
     * runs no shared tick. */
    if (has_status_src(srcs, nsrc)) {
        fputs("void wispgen_status_setup(void) {\n", o);
        for (int i = 0; i < nsrc; i++)
            if (srcs[i].drv->drv == DRV_STATUS && srcs[i].fmt)
                fprintf(o, "    status_set_arg(\"%s\", \"%.*s\");\n",
                        srcs[i].drv->name, (int)srcs[i].flen, srcs[i].fmt);
        fputs("    status_init(); status_sample_all();\n", o);
        fputs("}\n", o);
    }
    /* Shared status tick — polled status kinds only (cpu/mem/temp).
     * vpn/net/bat/backlight are event-driven (rtnetlink / uevent + slow timers), so they
     * neither arm this timer nor fire from it. */
    if (has_polled_status_src(srcs, nsrc)) {
        /* One timer for all polled kinds: it runs at the fastest every= among
         * them (default 1 s), and each tick samples every polled kind — a
         * slower per-source cadence isn't worth a second timer. */
        int tick_ms = 0;
        for (int i = 0; i < nsrc; i++) {
            if (srcs[i].drv->drv != DRV_STATUS || !status_kind_polled(srcs[i].drv)) continue;
            if (!tick_ms || srcs[i].interval_ms < tick_ms) tick_ms = srcs[i].interval_ms;
        }
        fputs("int wispgen_status_tfd = -1;\n", o);
        fputs("void wispgen_status_init(void) {\n", o);
        fputs("    wispgen_status_tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);\n", o);
        fprintf(o, "    struct itimerspec ts = { .it_value = { .tv_sec = %d, .tv_nsec = %d }, .it_interval = { .tv_sec = %d, .tv_nsec = %d } };\n",
                tick_ms / 1000, (tick_ms % 1000) * 1000000,
                tick_ms / 1000, (tick_ms % 1000) * 1000000);
        fputs("    timerfd_settime(wispgen_status_tfd, 0, &ts, NULL);\n", o);
        fputs("}\n", o);
        fputs("static unsigned wispgen_tick_n = 0;\n", o);
        fputs("void wispgen_status_handle(void) {\n", o);
        fputs("    uint64_t exp; (void)!read(wispgen_status_tfd, &exp, sizeof exp);\n", o);
        fputs("    status_tick(++wispgen_tick_n);\n", o);
        for (int i = 0; i < nsrc; i++) {
            if (srcs[i].drv->drv != DRV_STATUS || !status_kind_polled(srcs[i].drv)) continue;
            fprintf(o, "    on_%s_change();\n", sname(srcs[i].decl->name, srcs[i].decl->nlen));
        }
        fputs("}\n\n", o);
    }
    /* Wifi signal strength changes constantly, so it stays polled — but slowly
     * (WIFI_SIGNAL_S) and only while the link is up; armed/disarmed as the
     * netlink handler observes the link come and go. */
    int have_net = has_net_src(srcs, nsrc);
    SrcInst *net_s = NULL, *vpn_s = NULL;
    for (int i = 0; i < nsrc; i++) {
        if (is_status_named(&srcs[i], "net")) net_s = &srcs[i];
        if (is_status_named(&srcs[i], "vpn")) vpn_s = &srcs[i];
    }
    if (have_net) {
        const char *wn = sname(net_s->decl->name, net_s->decl->nlen);
        fputs("int wispgen_net_sig_tfd = -1;\n", o);
        fputs("static void wispgen_net_sig_arm(int up) {\n", o);
        fputs("    struct itimerspec ts = {0};\n", o);
        fputs("    if (up) { ts.it_value.tv_sec = WIFI_SIGNAL_S; ts.it_interval.tv_sec = WIFI_SIGNAL_S; }\n", o);
        fputs("    timerfd_settime(wispgen_net_sig_tfd, 0, &ts, NULL);\n", o);
        fputs("}\n", o);
        fputs("void wispgen_net_sig_init(void) {\n", o);
        fputs("    wispgen_net_sig_tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);\n", o);
        fputs("    wispgen_net_sig_arm(status.wifi_level >= 0);\n", o);
        fputs("}\n", o);
        fputs("void wispgen_net_sig_handle(void) {\n", o);
        fputs("    uint64_t exp; (void)!read(wispgen_net_sig_tfd, &exp, sizeof exp);\n", o);
        fputs("    status_sample_net();\n", o);
        fputs("    wispgen_net_sig_arm(status.wifi_level >= 0);\n", o);
        fprintf(o, "    on_%s_change();\n", wn);
        fputs("}\n\n", o);
    }
    /* bat: kernel uevents give instant AC/charging flips; a slow fallback timer
     * still re-reads capacity % (drivers often only uevent on threshold crossings).
     * Both paths re-sample and rely on on_*_change to suppress no-op repaints. */
    int have_bat = has_bat_src(srcs, nsrc);
    SrcInst *bat_s = NULL;
    for (int i = 0; i < nsrc; i++) if (is_status_named(&srcs[i], "bat")) bat_s = &srcs[i];
    if (have_bat) {
        const char *bn = sname(bat_s->decl->name, bat_s->decl->nlen);
        fputs("int wispgen_bat_tfd = -1;\n", o);
        fputs("void wispgen_bat_init(void) {\n", o);
        fputs("    wispgen_bat_tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);\n", o);
        fputs("    struct itimerspec ts = { .it_value = { .tv_sec = BAT_FALLBACK_S },\n"
              "                             .it_interval = { .tv_sec = BAT_FALLBACK_S } };\n", o);
        fputs("    timerfd_settime(wispgen_bat_tfd, 0, &ts, NULL);\n", o);
        fputs("}\n", o);
        fputs("void wispgen_bat_handle(void) {\n", o);
        fputs("    uint64_t exp; (void)!read(wispgen_bat_tfd, &exp, sizeof exp);\n", o);
        fputs("    status_sample_bat();\n", o);
        fprintf(o, "    on_%s_change();\n", bn);
        fputs("}\n", o);
        /* Weak-called by netlink.c on any power_supply uevent. */
        fputs("void wispgen_uevent_power(void) {\n", o);
        fputs("    status_sample_bat();\n", o);
        fprintf(o, "    on_%s_change();\n", bn);
        fputs("}\n\n", o);
    }
    /* backlight: kernel uevents only — the backlight class emits a change
     * event on every brightness write, so no fallback timer. */
    SrcInst *bl_s = NULL;
    for (int i = 0; i < nsrc; i++) if (is_status_named(&srcs[i], "backlight")) bl_s = &srcs[i];
    if (bl_s) {
        fputs("void wispgen_uevent_backlight(void) {\n", o);
        fputs("    status_sample_backlight();\n", o);
        fprintf(o, "    on_%s_change();\n", sname(bl_s->decl->name, bl_s->decl->nlen));
        fputs("}\n\n", o);
    }
    /* disk: free space moves glacially, so it rides a dedicated slow timer (DISK_S)
     * instead of the 1 Hz tick. on_*_change suppresses no-op repaints. */
    int have_disk = has_disk_src(srcs, nsrc);
    SrcInst *disk_s = NULL;
    for (int i = 0; i < nsrc; i++) if (is_status_named(&srcs[i], "disk")) disk_s = &srcs[i];
    if (have_disk) {
        const char *dn = sname(disk_s->decl->name, disk_s->decl->nlen);
        fputs("int wispgen_disk_tfd = -1;\n", o);
        fputs("void wispgen_disk_init(void) {\n", o);
        fputs("    wispgen_disk_tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);\n", o);
        fputs("    struct itimerspec ts = { .it_value = { .tv_sec = DISK_S },\n"
              "                             .it_interval = { .tv_sec = DISK_S } };\n", o);
        fputs("    timerfd_settime(wispgen_disk_tfd, 0, &ts, NULL);\n", o);
        fputs("}\n", o);
        fputs("void wispgen_disk_handle(void) {\n", o);
        fputs("    uint64_t exp; (void)!read(wispgen_disk_tfd, &exp, sizeof exp);\n", o);
        fputs("    status_sample_disk();\n", o);
        fprintf(o, "    on_%s_change();\n", dn);
        fputs("}\n\n", o);
    }
    /* rtnetlink handler (weak-called by netlink.c): re-sample vpn/net and
     * repaint on real change. The on_*_change guards suppress no-op events. */
    if (vpn_s || have_net) {
        fputs("void wispgen_netlink_changed(void) {\n", o);
        if (vpn_s) {
            fputs("    status_sample_vpn();\n", o);
            fprintf(o, "    on_%s_change();\n", sname(vpn_s->decl->name, vpn_s->decl->nlen));
        }
        if (have_net) {
            fputs("    status_sample_net();\n", o);
            fputs("    wispgen_net_sig_arm(status.wifi_level >= 0);\n", o);
            fprintf(o, "    on_%s_change();\n", sname(net_s->decl->name, net_s->decl->nlen));
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

    /* toplevel match table consumed by wl_toplevel.c (extern tl_match_app_ids /
     * tl_n_matches). One entry per toplevel() source, in declaration order. */
    int n_tl = 0;
    for (int i = 0; i < nsrc; i++) if (is_toplevel(&srcs[i])) n_tl++;
    if (n_tl > 16)   /* TL_MATCH_MAX in wl_toplevel.c */
        diag_error(srcs[0].decl->loc, "codegen: too many toplevel() sources (max 16)");
    /* Emitted even with zero toplevel() sources: wl_toplevel.o is also linked
     * for OSD/menu focus tracking (sema forces has_toplevel) and externs these. */
    fputs("const char *const tl_match_app_ids[] = {\n", o);
    for (int i = 0; i < nsrc; i++) {
        if (!is_toplevel(&srcs[i])) continue;
        char *aid = strndup0(srcs[i].fmt, srcs[i].flen);
        fprintf(o, "    \"%s\",\n", aid);
        free(aid);
    }
    if (!n_tl) fputs("    0,\n", o);
    fputs("};\n", o);
    fprintf(o, "const int tl_n_matches = %d;\n\n", n_tl);
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
          "const char *wispgen_net_ssid(void);\n\n", o);
    for (int i = 0; i < r->nsurfaces; i++)
        fprintf(o, "int dirty_%s = 1;\n", r->surface_names[i]);
    /* Per-surface source-dirty mask: bit i = source srcs[i] changed since the
     * last render. The bar render snapshots + resets it each frame and repaints
     * only the items whose dep_mask intersects it (partial path in
     * codegen_surface.c). Init all-set so the first render is a full paint.
     * Disabled past 64 sources (no config approaches that) — mask type is u64. */
    int masks_ok = nsrc <= 64;
    if (masks_ok)
        for (int i = 0; i < r->nsurfaces; i++)
            fprintf(o, "uint64_t bar_dirty_srcs_%s = ~0ull;\n", r->surface_names[i]);
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
                    if (masks_ok)
                        fprintf(o, "    bar_dirty_srcs_%s |= (1ull << %d);\n",
                                r->surface_names[j], i);
                    break;
                }
            }
        }
        fputs("}\n\n", o);
    }
}


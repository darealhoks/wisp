/* hyprland.c — Hyprland workspace source. Two unix sockets under
 * $XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/ (v0.40.0+, no /tmp
 * fallback): .socket2.sock is a persistent event stream (registered as
 * tags_fd, idle stays at 0 ticks — we only act on pushed events) and
 * .socket.sock is a ONE-SHOT request channel for seed queries and tag
 * switches. The request socket is served synchronously by the compositor, so
 * every request connects, writes, reads to EOF, and closes immediately — a
 * held-open connection freezes Hyprland.
 *
 * Occupancy can't be tracked purely from events (closewindow carries no
 * workspace), so — like Waybar — any window/workspace/monitor event triggers a
 * re-seed (batched j/monitors;j/workspaces) that rebuilds occ+active from
 * scratch. Queries fire only on real events, so idle-0 holds. */
#include "wisp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int hyprland_fd = -1;   /* .socket2.sock event stream; workspace.c sets tags_fd to it */

/* Per-monitor tag state, keyed by Hyprland monitor `name` — the same string as
 * the wl_output name, so it maps straight onto wisp's outputs. `urg` persists
 * across re-seeds (re-seeds only rewrite occ/act); occ/act are rebuilt each
 * seed. */
typedef struct {
    char     name[64];      /* empty = free slot */
    Output  *out;
    uint32_t occ, act, urg;
    int      active_id;     /* active workspace id on this monitor */
} HlMon;
static HlMon hmons[MAX_OUTPUTS];

/* Event line reassembly: Hyprland truncates a data field at 1024 chars and can
 * pack several `event>>data\n` lines per read (or split one across reads). */
static char   hl_buf[4096];
static size_t hl_len;

/* ---- socket path + one-shot request ---- */

/* Build $XDG_RUNTIME_DIR/hypr/$HIS/<sock> into dst. Returns 0 on failure
 * (missing env or a path that would overflow the unix sun_path). HIS is opaque
 * — copied verbatim, never parsed. */
static int hl_sock_path(char *dst, size_t dstsz, const char *sock) {
    const char *rt  = getenv("XDG_RUNTIME_DIR");
    const char *his = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!rt || !*rt || !his || !*his) return 0;
    int n = snprintf(dst, dstsz, "%s/hypr/%s/%s", rt, his, sock);
    return n > 0 && (size_t)n < dstsz;
}

static int hl_connect(const char *sock) {
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if (!hl_sock_path(sa.sun_path, sizeof sa.sun_path, sock)) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return -1; }
    return fd;
}

/* One-shot request on .socket.sock: write cmd, read the whole (unframed, up to
 * tens of KB) reply into a malloc'd NUL-terminated buffer, close. Caller frees.
 * Transient allocation on purpose — queries are event-driven and rare, so this
 * keeps nothing resident. Returns NULL on any failure. */
static char *hl_request(const char *cmd) {
    int fd = hl_connect(".socket.sock");
    if (fd < 0) return NULL;
    size_t clen = strlen(cmd);
    if (write(fd, cmd, clen) != (ssize_t)clen) { close(fd); return NULL; }

    char  *buf = NULL;
    size_t cap = 0, len = 0;
    for (;;) {
        if (len + 4096 + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 8192;
            char  *nb = realloc(buf, ncap);
            if (!nb) { free(buf); close(fd); return NULL; }
            buf = nb; cap = ncap;
        }
        ssize_t n = read(fd, buf + len, cap - len - 1);
        if (n < 0) { if (errno == EINTR) continue; free(buf); close(fd); return NULL; }
        if (n == 0) break;
        len += (size_t)n;
    }
    close(fd);
    if (buf) buf[len] = 0;
    return buf;
}

/* ---- minimal JSON field scanner (Hyprland pretty-prints `"key": value`) ---- */

/* Find the next `"key":` at or after p; return a pointer to its value (past the
 * colon), else NULL. Matching the full `"key":` token disambiguates lookalikes
 * ("id" vs "monitorID", "monitor" vs "monitorID"). */
static const char *hl_field(const char *p, const char *end, const char *key) {
    size_t kl = strlen(key);
    for (; p + kl + 3 <= end; p++) {
        if (p[0] != '"' || memcmp(p + 1, key, kl) != 0) continue;
        if (p[1 + kl] != '"') continue;
        const char *q = p + 2 + kl;
        if (q < end && *q == ':') return q + 1;
    }
    return NULL;
}

static const char *skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}

/* Copy a JSON string value ("...") at p into dst. */
static void hl_copystr(const char *p, const char *end, char *dst, size_t dstsz) {
    p = skip_ws(p, end);
    size_t i = 0;
    if (p >= end || *p != '"') { dst[0] = 0; return; }
    for (p++; p < end && *p != '"' && i + 1 < dstsz; p++) dst[i++] = *p;
    dst[i] = 0;
}

/* ---- per-monitor state ---- */

static HlMon *hl_mon(const char *name) {
    HlMon *free_slot = NULL;
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        if (hmons[i].name[0]) {
            if (!strcmp(hmons[i].name, name)) return &hmons[i];
        } else if (!free_slot) {
            free_slot = &hmons[i];
        }
    }
    if (free_slot) {
        snprintf(free_slot->name, sizeof free_slot->name, "%s", name);
        free_slot->out = NULL;
        free_slot->occ = free_slot->act = free_slot->urg = 0;
        free_slot->active_id = 0;
    }
    return free_slot;
}

static void hl_publish(HlMon *m) {
#ifdef WISP_HAS_BAR
    if (m->out) bar_set_tags_on(m->out, m->occ, m->act, m->urg);
#else
    (void)m;
#endif
}

/* ---- seed: rebuild occ + active for every monitor from a batched query ---- */

/* Parse the j/monitors array: name (== wl_output name), activeWorkspace.id,
 * focused (unused for masks — active is per-monitor regardless of focus). */
static void hl_parse_monitors(const char *p, const char *end) {
    for (;;) {
        const char *name_at = hl_field(p, end, "name");
        if (!name_at) return;
        char name[64];
        hl_copystr(name_at, end, name, sizeof name);
        const char *aw = hl_field(name_at, end, "activeWorkspace");
        const char *id_at = aw ? hl_field(aw, end, "id") : NULL;
        const char *foc = id_at ? hl_field(id_at, end, "focused") : NULL;
        if (!id_at) return;
        HlMon *m = hl_mon(name);
        if (m) {
            m->active_id = atoi(skip_ws(id_at, end));
            m->out = output_by_name(name);
        }
        /* advance past this record; focused is the last key we read, and the
         * next monitor's top-level name follows it. */
        p = foc ? foc : id_at;
    }
}

/* Parse the j/workspaces array: id (int64), monitor (name), windows (count).
 * Only ids 1..32 map to tag bits (bit = id-1); special/named workspaces have
 * id < 1 and huge numeric workspaces id > 32 — both never touch tag bits. */
static void hl_parse_workspaces(const char *p, const char *end) {
    for (;;) {
        const char *id_at = hl_field(p, end, "id");
        if (!id_at) return;
        long long id = atoll(skip_ws(id_at, end));
        const char *mon_at = hl_field(id_at, end, "monitor");
        const char *win_at = mon_at ? hl_field(mon_at, end, "windows") : NULL;
        if (!win_at) return;
        if (id >= 1 && id <= 32 && atoi(skip_ws(win_at, end)) > 0) {
            char mon[64];
            hl_copystr(mon_at, end, mon, sizeof mon);
            HlMon *m = mon[0] ? hl_mon(mon) : NULL;
            if (m) m->occ |= 1u << (id - 1);
        }
        p = win_at;
    }
}

static void hl_seed(void) {
    char *r = hl_request("[[BATCH]]j/monitors;j/workspaces");
    if (!r) return;
    char *end = r + strlen(r);

    /* replies joined by three newlines; without the separator treat it all as
     * monitors (a partial reply just yields empty occ, self-heals next event). */
    char *sep = strstr(r, "\n\n\n");
    char *ws_start = sep ? sep + 3 : end;

    for (int i = 0; i < MAX_OUTPUTS; i++) if (hmons[i].name[0]) hmons[i].occ = 0;
    hl_parse_monitors(r, sep ? sep : end);
    hl_parse_workspaces(ws_start, end);

    for (int i = 0; i < MAX_OUTPUTS; i++) {
        HlMon *m = &hmons[i];
        if (!m->name[0]) continue;
        m->act = (m->active_id >= 1 && m->active_id <= 32) ? 1u << (m->active_id - 1) : 0;
        m->urg &= ~m->act;   /* the active workspace can't stay urgent */
        hl_publish(m);
    }
    free(r);
}

/* ---- urgent: resolve window address -> workspace via j/clients ---- */

/* addr comes from the event without a 0x prefix; j/clients addresses have it. */
static void hl_urgent(const char *addr) {
    char *r = hl_request("j/clients");
    if (!r) return;
    char *end = r + strlen(r);
    char want[40];
    snprintf(want, sizeof want, "0x%s", addr);

    for (const char *p = r;;) {
        const char *addr_at = hl_field(p, end, "address");
        if (!addr_at) break;
        char got[40];
        hl_copystr(addr_at, end, got, sizeof got);
        const char *ws = hl_field(addr_at, end, "workspace");
        const char *id_at = ws ? hl_field(ws, end, "id") : NULL;
        if (!id_at) break;
        if (!strcmp(got, want)) {
            long long id = atoll(skip_ws(id_at, end));
            if (id >= 1 && id <= 32) {
                uint32_t bit = 1u << (id - 1);
                /* the workspace's monitor is whichever one already shows it
                 * occupied — the urgent window makes windows>0 there. */
                for (int i = 0; i < MAX_OUTPUTS; i++) {
                    HlMon *m = &hmons[i];
                    if (m->name[0] && (m->occ & bit) && !(m->act & bit)) {
                        m->urg |= bit;
                        hl_publish(m);
                    }
                }
            }
            break;
        }
        p = id_at;
    }
    free(r);
}

/* ---- event dispatch ---- */

/* v2 events only (v1 duplicates ignored). Occupancy and active both fall out of
 * a re-seed, so these all just re-seed; only urgent needs the address lookup.
 * ponytail: a re-seed per event is fine for interactive rates; if event storms
 * (rapid window churn) ever matter, coalesce with a short debounce timerfd. */
static const char *const reseed_events[] = {
    "workspacev2", "focusedmonv2", "moveworkspacev2",
    "createworkspacev2", "destroyworkspacev2",
    "openwindow", "closewindow", "movewindowv2",
    "monitoraddedv2", "monitorremoved", "monitorremovedv2",
};

static void hl_event(const char *line, const char *sep) {
    size_t namelen = (size_t)(sep - line);
    const char *data = sep + 2;
    if (namelen == 6 && !memcmp(line, "urgent", 6)) { hl_urgent(data); return; }
    for (size_t i = 0; i < sizeof reseed_events / sizeof *reseed_events; i++) {
        const char *e = reseed_events[i];
        if (strlen(e) == namelen && !memcmp(line, e, namelen)) { hl_seed(); return; }
    }
}

void hyprland_dispatch(void) {
    for (;;) {
        ssize_t n = read(hyprland_fd, hl_buf + hl_len, sizeof hl_buf - hl_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            n = 0;
        }
        if (n == 0) {   /* Hyprland gone; the session is over anyway */
            epoll_del_fd(hyprland_fd); close(hyprland_fd); hyprland_fd = -1;
            msg("wisp: hyprland ipc closed");
            return;
        }
        hl_len += (size_t)n;
        char *nl;
        while ((nl = memchr(hl_buf, '\n', hl_len))) {
            *nl = 0;   /* terminate the line so event data is a valid C string */
            char *sep = memchr(hl_buf, '>', (size_t)(nl - hl_buf));
            if (sep && sep + 1 < nl && sep[1] == '>') hl_event(hl_buf, sep);
            hl_len -= (size_t)(nl + 1 - hl_buf);
            memmove(hl_buf, nl + 1, hl_len);
        }
        if (hl_len == sizeof hl_buf) hl_len = 0;   /* oversized line: drop */
    }
}

void hyprland_init(void) {
    hyprland_fd = hl_connect(".socket2.sock");
    if (hyprland_fd < 0) return;   /* not hyprland; workspace.c falls back */
    fcntl(hyprland_fd, F_SETFL, O_NONBLOCK);
    hl_seed();
}

/* ponytail: `dispatch workspace` acts on the focused monitor, so `o` is
 * advisory-only — single-laptop. Prefix `dispatch focusmonitor <name>` keyed
 * off o->name if a second monitor ever needs targeted switching. */
void hyprland_view_tag(Output *o, int idx) {
    (void)o;
    if (idx < 1 || idx > 32) return;
    char cmd[48];
    snprintf(cmd, sizeof cmd, "dispatch workspace %d", idx);
    char *r = hl_request(cmd);   /* reply is "ok" or an error; we don't act on it */
    free(r);
}

/* niri.c — niri workspace source. One unix socket at $NIRI_SOCKET: writing
 * "EventStream"\n turns the connection into a persistent line-delimited JSON
 * event stream (registered as tags_fd, idle stays 0 ticks — we only act on
 * pushed events). Actions use a fresh one-shot connection.
 *
 * niri reports windows with their workspace_id, so occupancy is a real window
 * count — the reason to prefer this over ext-workspace, which niri also
 * advertises but which can't say whether a workspace holds anything. */
#include "wisp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int niri_fd = -1;   /* event stream; workspace.c sets tags_fd to it */

#define NIRI_MAX_WS  64
#define NIRI_MAX_WIN 256

typedef struct {
    int  id;            /* 0 = free slot */
    int  idx;           /* 1-based per-output position; tag bit = idx-1 */
    int  active, urgent;
    char out[32];
} NiriWs;

typedef struct { int id, ws; } NiriWin;   /* id 0 = free slot */

static int npub;   /* a drained batch changed state; publish once at its end */

static NiriWs  nws[NIRI_MAX_WS];
static NiriWin nwin[NIRI_MAX_WIN];

/* Growable line buffer: a WindowsChanged line carries every window's full
 * record and easily passes 64 KB, but that size must not stay resident. */
static char  *nbuf;
static size_t nlen, ncap;
#define NIRI_BUF_KEEP (16u * 1024)
#define NIRI_BUF_MAX  (1024u * 1024)

/* ---- minimal JSON scanning ---- */

static const char *nj_field(const char *p, const char *end, const char *key) {
    size_t kl = strlen(key);
    for (; p + kl + 3 <= end; p++) {
        if (p[0] != '"' || memcmp(p + 1, key, kl) != 0) continue;
        if (p[1 + kl] != '"') continue;
        const char *q = p + 2 + kl;
        if (q < end && *q == ':') return q + 1;
    }
    return NULL;
}

static void nj_copystr(const char *p, const char *end, char *dst, size_t dstsz) {
    size_t i = 0;
    dst[0] = 0;
    if (p >= end || *p != '"') return;
    for (p++; p < end && *p != '"' && i + 1 < dstsz; p++) dst[i++] = *p;
    dst[i] = 0;
}

static int nj_bool(const char *p, const char *end) { return p < end && *p == 't'; }

/* ---- tables ---- */

static NiriWs *ws_slot(int id) {
    NiriWs *free_slot = NULL;
    for (int i = 0; i < NIRI_MAX_WS; i++) {
        if (nws[i].id == id) return &nws[i];
        if (!nws[i].id && !free_slot) free_slot = &nws[i];
    }
    return free_slot;
}

static void win_set(int id, int ws) {
    NiriWin *free_slot = NULL;
    for (int i = 0; i < NIRI_MAX_WIN; i++) {
        if (nwin[i].id == id) { nwin[i].ws = ws; return; }
        if (!nwin[i].id && !free_slot) free_slot = &nwin[i];
    }
    if (free_slot) { free_slot->id = id; free_slot->ws = ws; }
}

static int ws_has_windows(int ws) {
    for (int i = 0; i < NIRI_MAX_WIN; i++) if (nwin[i].id && nwin[i].ws == ws) return 1;
    return 0;
}

/* ponytail: O(outputs x ws x windows) rebuild per event — n is a handful of
 * workspaces; index the tables if a compositor ever gets hundreds. */
static void niri_publish(void) {
    for (int i = 0; i < NIRI_MAX_WS; i++) {
        if (!nws[i].id || !nws[i].out[0]) continue;
        int first = 1;
        for (int j = 0; j < i; j++)
            if (nws[j].id && !strcmp(nws[j].out, nws[i].out)) { first = 0; break; }
        if (!first) continue;

        uint32_t occ = 0, act = 0, urg = 0, ex = 0;
        for (int j = 0; j < NIRI_MAX_WS; j++) {
            NiriWs *w = &nws[j];
            if (!w->id || strcmp(w->out, nws[i].out)) continue;
            if (w->idx < 1 || w->idx > 32) continue;
            uint32_t bit = 1u << (w->idx - 1);
            ex |= bit;
            if (ws_has_windows(w->id)) occ |= bit;
            if (w->active) act |= bit;
            if (w->urgent) urg |= bit;
        }
#ifdef WISP_HAS_BAR
        Output *o = output_by_name(nws[i].out);
        if (o) bar_set_tags_on(o, occ, act, urg, ex);
#else
        (void)occ; (void)act; (void)urg; (void)ex;
#endif
    }
}

/* ---- events ---- */

/* record order is serde's: id, idx, name, output, is_urgent, is_active, … */
static void niri_workspaces(const char *p, const char *end) {
    memset(nws, 0, sizeof nws);
    for (;;) {
        const char *id_at = nj_field(p, end, "id");
        if (!id_at) break;
        const char *idx_at = nj_field(id_at, end, "idx");
        const char *out_at = idx_at ? nj_field(idx_at, end, "output") : NULL;
        const char *urg_at = out_at ? nj_field(out_at, end, "is_urgent") : NULL;
        const char *act_at = urg_at ? nj_field(urg_at, end, "is_active") : NULL;
        if (!act_at) break;
        NiriWs *w = ws_slot(atoi(id_at));
        if (w) {
            w->id     = atoi(id_at);
            w->idx    = atoi(idx_at);
            w->urgent = nj_bool(urg_at, end);
            w->active = nj_bool(act_at, end);
            nj_copystr(out_at, end, w->out, sizeof w->out);
        }
        p = act_at;
    }
}

static void niri_window(const char *p, const char *end) {
    const char *id_at = nj_field(p, end, "id");
    const char *ws_at = id_at ? nj_field(id_at, end, "workspace_id") : NULL;
    if (ws_at) win_set(atoi(id_at), atoi(ws_at));
}

static void niri_windows(const char *p, const char *end) {
    memset(nwin, 0, sizeof nwin);
    for (;;) {
        const char *id_at = nj_field(p, end, "id");
        const char *ws_at = id_at ? nj_field(id_at, end, "workspace_id") : NULL;
        if (!ws_at) break;
        win_set(atoi(id_at), atoi(ws_at));
        p = ws_at;
    }
}

static void niri_line(char *line, size_t len) {
    const char *end = line + len;
    if (!memcmp(line, "{\"WorkspacesChanged\"", 20)) {
        niri_workspaces(line, end);
    } else if (!memcmp(line, "{\"WindowsChanged\"", 17)) {
        niri_windows(line, end);
    } else if (!memcmp(line, "{\"WindowOpenedOrChanged\"", 24)) {
        niri_window(line, end);
    } else if (!memcmp(line, "{\"WindowClosed\"", 15)) {
        const char *id_at = nj_field(line, end, "id");
        if (!id_at) return;
        int id = atoi(id_at);
        for (int i = 0; i < NIRI_MAX_WIN; i++) if (nwin[i].id == id) nwin[i].id = 0;
    } else if (!memcmp(line, "{\"WorkspaceActivated\"", 21)) {
        const char *id_at = nj_field(line, end, "id");
        NiriWs *w = id_at ? ws_slot(atoi(id_at)) : NULL;
        if (!w || !w->id) return;
        for (int i = 0; i < NIRI_MAX_WS; i++)
            if (nws[i].id && !strcmp(nws[i].out, w->out)) nws[i].active = 0;
        w->active = 1;
        w->urgent = 0;
    } else if (!memcmp(line, "{\"WorkspaceUrgencyChanged\"", 26)) {
        const char *id_at = nj_field(line, end, "id");
        const char *u_at  = id_at ? nj_field(id_at, end, "urgent") : NULL;
        NiriWs *w = id_at ? ws_slot(atoi(id_at)) : NULL;
        if (!u_at || !w || !w->id) return;
        w->urgent = nj_bool(u_at, end);
    } else {
        return;
    }
    npub = 1;
}

/* ---- socket ---- */

static int niri_connect(void) {
    const char *path = getenv("NIRI_SOCKET");
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if (!path || !*path || strlen(path) >= sizeof sa.sun_path) return -1;
    memcpy(sa.sun_path, path, strlen(path));
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return -1; }
    return fd;
}

void niri_dispatch(void) {
    for (;;) {
        if (nlen + 4096 + 1 > ncap) {
            size_t ncap2 = ncap ? ncap * 2 : NIRI_BUF_KEEP;
            char  *nb = ncap2 > NIRI_BUF_MAX ? NULL : realloc(nbuf, ncap2);
            if (!nb) { nlen = 0; break; }   /* oversized line: drop, self-heals */
            nbuf = nb; ncap = ncap2;
        }
        ssize_t n = read(niri_fd, nbuf + nlen, ncap - nlen - 1);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            n = 0;
        }
        if (n == 0) {
            epoll_del_fd(niri_fd); close(niri_fd); niri_fd = -1;
            msg("wisp: niri ipc closed");
            break;
        }
        nlen += (size_t)n;
        char *nl;
        while ((nl = memchr(nbuf, '\n', nlen))) {
            *nl = 0;
            if (nl - nbuf > 2) niri_line(nbuf, (size_t)(nl - nbuf));
            nlen -= (size_t)(nl + 1 - nbuf);
            memmove(nbuf, nl + 1, nlen);
        }
        if (ncap > NIRI_BUF_KEEP && nlen < NIRI_BUF_KEEP) {
            char *nb = realloc(nbuf, NIRI_BUF_KEEP);
            if (nb) { nbuf = nb; ncap = NIRI_BUF_KEEP; }
        }
    }
    if (npub) { npub = 0; niri_publish(); }
}

void niri_init(void) {
    niri_fd = niri_connect();
    if (niri_fd < 0) return;   /* not niri; workspace.c falls back */
    if (write(niri_fd, "\"EventStream\"\n", 14) != 14) {
        close(niri_fd); niri_fd = -1; return;
    }
    fcntl(niri_fd, F_SETFL, O_NONBLOCK);
}

/* ponytail: Index is resolved against the focused output, so `o` is
 * advisory-only — single-laptop. Add a FocusMonitor action before it if
 * targeted multi-head switching ever matters. */
void niri_view_tag(Output *o, int idx) {
    (void)o;
    if (idx < 1 || idx > 32) return;
    int fd = niri_connect();
    if (fd < 0) return;
    char cmd[80];
    int  n = snprintf(cmd, sizeof cmd,
                      "{\"Action\":{\"FocusWorkspace\":{\"reference\":{\"Index\":%d}}}}\n", idx);
    if (n > 0) { ssize_t w = write(fd, cmd, (size_t)n); (void)w; }
    close(fd);
}

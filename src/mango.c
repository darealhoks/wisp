/* mango.c — mango compositor IPC (>= 0.15, the dwl-ipc Wayland protocol is
 * gone). Line-delimited commands + JSON replies over the unix socket in
 * $MANGO_INSTANCE_SIGNATURE. We hold one persistent `watch all-tags`
 * subscription (mango pushes a snapshot on subscribe, then on every change —
 * zero polling, idle stays at 0 ticks) and fire one-shot `dispatch view`
 * connections for tag switches. */
#include "wisp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int mango_fd = -1;

/* Carry-over for a JSON line split across reads. An all-tags message is
 * ~1 KB/monitor; 8 KB covers any sane setup, oversized lines are dropped. */
static char   mg_buf[8192];
static size_t mg_len;

static int mango_connect(void) {
    const char *path = getenv("MANGO_INSTANCE_SIGNATURE");
    if (!path || !*path) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return -1; }
    return fd;
}

void mango_init(void) {
    mango_fd = mango_connect();
    if (mango_fd < 0) return;   /* not mango; workspace.c falls back and reports */
    static const char sub[] = "watch all-tags\n";
    if (write(mango_fd, sub, sizeof sub - 1) != (ssize_t)(sizeof sub - 1)) {
        close(mango_fd); mango_fd = -1; return;
    }
    fcntl(mango_fd, F_SETFL, O_NONBLOCK);
}

/* Scan forward to `key` within [p, end); return pointer past the key or NULL.
 * ponytail: not a JSON parser — leans on mango emitting fixed key order
 * (monitor, then per tag: index, is_active, is_urgent, layout, client_count).
 * Upgrade path: real tokenizer if mango ever reorders keys. */
static const char *mg_find(const char *p, const char *end, const char *key) {
    size_t kl = strlen(key);
    for (; p + kl <= end; p++)
        if (*p == '"' && !memcmp(p, key, kl)) return p + kl;
    return NULL;
}

static void mg_flush(Output *o, uint32_t occ, uint32_t act, uint32_t urg) {
    (void)o; (void)occ; (void)act; (void)urg;
#ifdef WISP_HAS_BAR
    if (o) bar_set_tags_on(o, occ, act, urg);
#endif
}

/* One all-tags line: {"all_tags":[{"monitor":"eDP-1","tags":[{"index":1,
 * "is_active":true,"is_urgent":false,...,"client_count":2},...]},...]} */
static void mg_parse_line(const char *p, const char *end) {
    Output *o = NULL;
    uint32_t occ = 0, act = 0, urg = 0;
    for (;;) {
        const char *mon = mg_find(p, end, "\"monitor\":\"");
        const char *idx = mg_find(p, end, "\"index\":");
        if (idx && (!mon || idx < mon)) {
            uint32_t bit, n = (uint32_t)atoi(idx);
            const char *e = mg_find(idx, end, "\"client_count\":");
            if (!e || n < 1 || n > 32) break;
            bit = 1u << (n - 1);
            const char *q = mg_find(idx, end, "\"is_active\":");
            if (q && q < e && *q == 't') act |= bit;
            q = mg_find(idx, end, "\"is_urgent\":");
            if (q && q < e && *q == 't') urg |= bit;
            if (atoi(e) > 0) occ |= bit;
            p = e;
        } else if (mon) {
            mg_flush(o, occ, act, urg);
            occ = act = urg = 0;
            char name[64]; size_t i = 0;
            for (p = mon; p < end && *p != '"' && i + 1 < sizeof name; p++) name[i++] = *p;
            name[i] = 0;
            o = output_by_name(name);
        } else {
            mg_flush(o, occ, act, urg);
            return;
        }
    }
}

void mango_dispatch(void) {
    for (;;) {
        ssize_t n = read(mango_fd, mg_buf + mg_len, sizeof mg_buf - mg_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            n = 0;
        }
        if (n == 0) {   /* mango died; the session is over anyway */
            epoll_del_fd(mango_fd); close(mango_fd); mango_fd = -1;
            msg("wisp: mango ipc closed");
            return;
        }
        mg_len += (size_t)n;
        char *nl;
        while ((nl = memchr(mg_buf, '\n', mg_len))) {
            mg_parse_line(mg_buf, nl);
            mg_len -= (size_t)(nl + 1 - mg_buf);
            memmove(mg_buf, nl + 1, mg_len);
        }
        if (mg_len == sizeof mg_buf) mg_len = 0;   /* oversized line: drop */
    }
}

/* ponytail: mango's `view` dispatch always acts on the focused monitor, so
 * `o` is advisory-only. Single-laptop setup; if a second monitor ever shows
 * up, prefix with a focusmon dispatch keyed off o->name. */
void mango_view_tag(Output *o, int idx) {
    (void)o;
    if (idx < 1 || idx > 32) return;
    int fd = mango_connect();
    if (fd < 0) return;
    char cmd[48];
    int len = snprintf(cmd, sizeof cmd, "dispatch view,%d\n", idx);
    (void)!write(fd, cmd, (size_t)len);
    /* Must wait for the reply: closing right away can deliver EPOLLHUP with
     * the data and mango's handler drops hung-up clients before reading. */
    char ack[64];
    (void)!read(fd, ack, sizeof ack);
    close(fd);
}

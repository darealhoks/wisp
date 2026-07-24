/* D-Bus session-bus transport: socket + SASL, marshalling, dispatch,
 * reconnect. Interface logic lives in the consumers (notify.c serves
 * org.freedesktop.Notifications); shared wire guts are in dbus.h.
 *
 * No libdbus / sd-bus dependency — we speak the wire protocol directly.
 *
 *   ┌─ session bus
 *   │
 *   │  > byte 0x00              (kernel SCM_CRED placeholder)
 *   │  > AUTH EXTERNAL <hex-uid>\r\n
 *   │  < OK <guid>\r\n
 *   │  > BEGIN\r\n
 *   │  > Hello()                (assign unique name)
 *   │  < method_return
 *   │  > RequestName("org.freedesktop.Notifications", 4)  // REPLACE_EXISTING
 *   │  < method_return
 *   │  ↺ recv loop: dispatch method_call's destination=us, member=Notify/...
 *   ↓
 *
 * Endian-ness fixed to little-endian throughout (the 'l' header byte). */

#define _GNU_SOURCE
#include "wisp.h"
#include "dbus.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <unistd.h>

int dbus_fd = -1;
int dbus_reconnect_fd = -1;

static uint32_t dbus_serial = 1;
/* Receive buffer is dynamic: starts at RBUF_BASE (covers ~all realistic
 * notifications) and grows up to RBUF_MAX when a single message is bigger
 * (browsers/Discord routinely embed image-data hints of 256+ KB). Anything
 * past the cap is drained off the socket and discarded so we never desync.
 * After handling a giant message we shrink back to the base so idle RSS
 * stays in budget. */
#define RBUF_BASE 4096
#define RBUF_MAX  (1 << 20)
static uint8_t *rbuf = NULL;
static int      rcap = 0;
static int      rlen = 0;
/* When > 0, that many bytes of the next inbound message must be drained
 * straight off the socket (the message was bigger than RBUF_MAX). */
static int      rskip = 0;

static void arm_reconnect(int delay_ms);
static void dbus_drop(int delay_ms);

/* ================================================================== */
/* Outbound send (wire marshalling lives in dbus_wire.c)               */
/* ================================================================== */

uint32_t send_msg(const Msg *m) {
    uint32_t ser = m->serial ? m->serial : dbus_serial++;
    if (!m->serial && dbus_serial == 0) dbus_serial = 1;
    W h = {0};
    dbus_msg_build(&h, m, ser);
    int rc = (int)send(dbus_fd, h.b, h.pos, MSG_NOSIGNAL);
    free(h.b);
    if (rc != h.pos) { dbus_drop(2000); return 0; }
    return ser;
}

/* ================================================================== */
/* Outbound: Hello, RequestName, signals                                */
/* ================================================================== */

static int wait_method_return(uint32_t serial);

static int call_hello(void) {
    Msg m = { .type = DBUS_TYPE_METHOD_CALL,
              .path = "/org/freedesktop/DBus",
              .interface = "org.freedesktop.DBus",
              .member = "Hello",
              .destination = "org.freedesktop.DBus" };
    uint32_t s = send_msg(&m);
    return wait_method_return(s);
}
/* Reply body of last RequestName, parsed out by wait_method_return for the
 * sake of telling primary-owner from "in-queue" from "exists". */
static uint32_t last_request_name_result;

/* Extra well-known names consumers asked for (tray.c: the Watcher + Host
 * pair). Stored by pointer — callers pass string literals or static buffers. */
#define DBUS_OWN_CAP 4
static const char *own_names[DBUS_OWN_CAP];
static int         own_name_n = 0;

void dbus_own_name(const char *name) {
    if (own_name_n >= DBUS_OWN_CAP) return;
    own_names[own_name_n++] = name;
    /* Names registered after bring-up are the caller's problem to re-request;
     * every consumer today registers from its init(), before dbus_connect(). */
}

static int call_request_name(const char *name) {
    W b = {0};
    wstr(&b, name);
    /* ALLOW_REPLACEMENT (1) | REPLACE_EXISTING (2) | DO_NOT_QUEUE (4).
     * mako 1.x grabs the name without ALLOW_REPLACEMENT, so REPLACE_EXISTING
     * alone won't dislodge it — the user's session must launch wisp before
     * mako, or mako's d-bus service file must be removed. */
    wu32(&b, 1 | 2 | 4);
    Msg m = { .type = DBUS_TYPE_METHOD_CALL,
              .path = "/org/freedesktop/DBus",
              .interface = "org.freedesktop.DBus",
              .member = "RequestName",
              .destination = "org.freedesktop.DBus",
              .signature = "su",
              .body = b.b, .body_len = b.pos };
    uint32_t s = send_msg(&m);
    int rc = wait_method_return(s);
    free(b.b);
    if (rc < 0) return rc;
    /* 1=primary, 2=in_queue, 3=exists (someone else owns), 4=already_owner */
    if (last_request_name_result != 1 && last_request_name_result != 4) {
        msg("wisp: RequestName(%s) returned %u — not the primary owner",
            name, last_request_name_result);
        return -1;
    }
    return 0;
}

/* Generic signal subscriptions registered by codegen-driven sources.
 * Populated before dbus_connect() runs; AddMatch is replayed once we're up. */
#define DBUS_SUB_CAP 32
typedef struct {
    const char *iface;
    const char *member;
    dbus_sig_cb cb;
} DbusSub;
static DbusSub dbus_subs[DBUS_SUB_CAP];
static int     dbus_sub_n = 0;

static int call_add_match(const char *iface, const char *member) {
    char rule[256];
    snprintf(rule, sizeof rule,
             "type='signal',interface='%s',member='%s'", iface, member);
    W b = {0};
    wstr(&b, rule);
    Msg m = { .type = DBUS_TYPE_METHOD_CALL,
              .flags = 1,                       /* no reply expected */
              .path = "/org/freedesktop/DBus",
              .interface = "org.freedesktop.DBus",
              .member = "AddMatch",
              .destination = "org.freedesktop.DBus",
              .signature = "s",
              .body = b.b, .body_len = b.pos };
    send_msg(&m);
    free(b.b);
    return 0;
}

void dbus_subscribe(const char *iface, const char *member, dbus_sig_cb cb) {
    if (dbus_sub_n >= DBUS_SUB_CAP) return;
    dbus_subs[dbus_sub_n].iface = iface;
    dbus_subs[dbus_sub_n].member = member;
    dbus_subs[dbus_sub_n].cb = cb;
    dbus_sub_n++;
    if (dbus_fd >= 0) call_add_match(iface, member);
}

/* Extract the first string argument from a signal body. Returns 0 on success.
 * `sig` is the body signature (header field 8). Only handles a leading 's'/'o'. */
int dbus_signal_first_str(const uint8_t *body, int body_len, const char *sig,
                          char *out, int outcap) {
    if (!sig || (sig[0] != 's' && sig[0] != 'o')) { if (outcap) out[0] = 0; return -1; }
    R r = { .b = body, .len = body_len, .pos = 0, .ok = 1 };
    const char *s = rstr(&r);
    if (!r.ok) { if (outcap) out[0] = 0; return -1; }
    snprintf(out, outcap, "%s", s);
    return 0;
}

/* ================================================================== */
/* Async method calls                                                   */
/* ================================================================== */

/* Pending calls awaiting a reply. Fixed and lossy on purpose: a peer that
 * never answers must not pin a slot forever, and there is no timer here to
 * expire one (idle = 0 CPU). Slots are freed on reply and on disconnect. */
#define DBUS_PENDING_CAP 32
typedef struct {
    uint32_t      serial;      /* 0 = free */
    dbus_reply_cb cb;
    void         *ud;
} DbusPending;
static DbusPending pending[DBUS_PENDING_CAP];

uint32_t dbus_call(const Msg *m, dbus_reply_cb cb, void *ud) {
    int slot = -1;
    for (int i = 0; i < DBUS_PENDING_CAP; i++)
        if (!pending[i].serial) { slot = i; break; }
    /* ponytail: table full → drop the call; raise the cap if a real workload
     * (many tray items registering at once) ever hits it. */
    if (slot < 0) return 0;
    uint32_t s = send_msg(m);
    if (!s) return 0;
    pending[slot] = (DbusPending){ .serial = s, .cb = cb, .ud = ud };
    return s;
}

/* ================================================================== */
/* Dispatch loop                                                        */
/* ================================================================== */

void dbus_reply_empty(uint32_t serial, const char *sender) {
    Msg m = { .type = DBUS_TYPE_METHOD_RETURN,
              .reply_serial = serial,
              .destination = sender };
    send_msg(&m);
}

void dbus_reply_error(uint32_t serial, const char *sender,
                      const char *name, const char *msg) {
    W b = {0};
    wstr(&b, msg);
    Msg m = { .type = DBUS_TYPE_ERROR,
              .reply_serial = serial,
              .destination = sender,
              .error_name = name,
              .signature = "s",
              .body = b.b, .body_len = b.pos };
    send_msg(&m);
    free(b.b);
}

static void dispatch_one(const uint8_t *msg, int msg_len) {
    R r = { .b = msg, .len = msg_len, .pos = 0, .ok = 1 };
    if (msg_len < 16) return;
    /* fixed header */
    rbyte(&r);                            /* endian — always 'l' here */
    uint8_t type = rbyte(&r);
    uint8_t flags = rbyte(&r);
    rbyte(&r);                            /* version */
    uint32_t body_len = ru32(&r);
    uint32_t serial   = ru32(&r);

    /* header fields: array of (byte, variant) structs */
    uint32_t flen = ru32(&r);
    if (!r.ok) return;
    int fend = r.pos + (int)flen;
    if (fend > r.len) return;

    char member[64] = "", iface[96] = "", path[96] = "", sender[64] = "", dest[96] = "";
    uint32_t reply_serial = 0;
    char body_sig[16] = "";

    while (r.pos < fend) {
        ralign(&r, 8);
        if (!r.ok) return;
        uint8_t code = rbyte(&r);
        const char *sig = rsig(&r);
        if (!r.ok || !sig[0]) break;
        switch (code) {
        case HF_PATH:         snprintf(path,   sizeof path,   "%s", rstr(&r)); break;
        case HF_INTERFACE:    snprintf(iface,  sizeof iface,  "%s", rstr(&r)); break;
        case HF_MEMBER:       snprintf(member, sizeof member, "%s", rstr(&r)); break;
        case HF_REPLY_SERIAL: reply_serial = ru32(&r); break;
        case HF_DESTINATION:  snprintf(dest,   sizeof dest,   "%s", rstr(&r)); break;
        case HF_SENDER:       snprintf(sender, sizeof sender, "%s", rstr(&r)); break;
        case HF_SIGNATURE:    snprintf(body_sig, sizeof body_sig, "%s", rsig(&r)); break;
        default: { const char *s = sig; skip_val(&r, &s, 0); break; }
        }
        if (!r.ok) return;
    }
    r.pos = fend;
    ralign(&r, 8);                        /* body alignment */
    (void)body_len;
    (void)dest;

    if (type == DBUS_TYPE_METHOD_RETURN || type == DBUS_TYPE_ERROR) {
        for (int i = 0; reply_serial && i < DBUS_PENDING_CAP; i++) {
            if (pending[i].serial != reply_serial) continue;
            DbusPending p = pending[i];
            pending[i].serial = 0;   /* free before the cb, it may call again */
            p.cb(sender, &r, body_sig, type == DBUS_TYPE_ERROR, p.ud);
            break;
        }
        return;
    }
    if (type == DBUS_TYPE_SIGNAL) {
        for (int i = 0; i < dbus_sub_n; i++) {
            if (!strcmp(dbus_subs[i].iface,  iface) &&
                !strcmp(dbus_subs[i].member, member)) {
                dbus_subs[i].cb(sender, path, msg + r.pos, msg_len - r.pos, body_sig);
            }
        }
        return;
    }
    if (type != DBUS_TYPE_METHOD_CALL) return;
    int handled = 0;
    if (!strcmp(iface, "org.freedesktop.Notifications")) {
        notify_method_call(&r, member, serial, sender);
        handled = 1;
#ifdef WISP_HAS_TRAY
    } else if (!strcmp(iface, "org.kde.StatusNotifierWatcher") ||
               !strcmp(iface, "org.freedesktop.DBus.Properties") ||
               !strcmp(iface, "org.freedesktop.DBus.Introspectable")) {
        handled = tray_method_call(&r, iface, member, path, serial, sender);
#endif
    } else if (!strcmp(iface, "org.freedesktop.DBus.Peer") &&
               !strcmp(member, "Ping")) {
        dbus_reply_empty(serial, sender);
        handled = 1;
    }
    /* Never leave a call unanswered: Qt builds a proxy by introspecting first
     * and blocks on it, so a dropped reply freezes the caller's GUI for the
     * full 25 s timeout instead of failing fast. */
    if (!handled && !(flags & 1))
        dbus_reply_error(serial, sender, "org.freedesktop.DBus.Error.UnknownMethod",
                         "wisp: no such method");
}

/* Ensure rbuf has at least `need` bytes of capacity. Returns 0 on success.
 * Caps growth at RBUF_MAX — caller treats -1 as "this message is too big,
 * drain it from the socket and skip." */
static int rbuf_ensure(int need) {
    if (need > RBUF_MAX) return -1;
    if (need <= rcap) return 0;
    int nc = rcap ? rcap : RBUF_BASE;
    while (nc < need) nc *= 2;
    if (nc > RBUF_MAX) nc = RBUF_MAX;
    uint8_t *nb = realloc(rbuf, nc);
    if (!nb) return -1;
    rbuf = nb; rcap = nc;
    return 0;
}
/* After a giant message is gone, shrink back to baseline so idle RSS stays
 * near 2.8 MB. Only safe when no partial message is buffered. */
static void rbuf_maybe_shrink(void) {
    if (rlen != 0 || rskip != 0 || rcap <= RBUF_BASE) return;
    uint8_t *nb = realloc(rbuf, RBUF_BASE);
    if (nb) { rbuf = nb; rcap = RBUF_BASE; }
}

/* Block (briefly) until a method_return for the given serial arrives. Used
 * during connection bring-up only. Other inbound msgs are dispatched. */
static int wait_method_return(uint32_t serial) {
    int saved_flags = fcntl(dbus_fd, F_GETFL);
    fcntl(dbus_fd, F_SETFL, saved_flags & ~O_NONBLOCK);
    /* Bounded blocking: a bus that accepts the connect but stalls before
     * replying must not wedge the single event loop (clock/OSD/lock input). */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(dbus_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int rc = -1;
    for (;;) {
        if (rbuf_ensure(rlen + 16) < 0) break;
        ssize_t n = recv(dbus_fd, rbuf + rlen, rcap - rlen, 0);
        if (n <= 0) break;   /* EAGAIN/EWOULDBLOCK from the timeout lands here */
        rlen += n;
        for (;;) {
            if (rlen < 16) break;
            if (rbuf[0] != 'l') { rc = -1; goto done; }
            uint32_t body_len  = *(uint32_t *)(rbuf + 4);
            uint32_t farr_len  = *(uint32_t *)(rbuf + 12);
            int hdr = (16 + (int)farr_len + 7) & ~7;
            int total = hdr + (int)body_len;
            if (total < 0) { rc = -1; goto done; }
            if (rbuf_ensure(total) < 0) {
                /* Bring-up reply can't be this big — bail. */
                rc = -1; goto done;
            }
            if (rlen < total) break;
            /* Check if it's our reply. Field array starts at offset 16 (its
             * length u32 lives at offset 12 — already read into farr_len). */
            R r = { .b = rbuf, .len = total, .pos = 16, .ok = 1 };
            int fend = 16 + (int)farr_len;
            uint8_t type = rbuf[1];
            uint32_t got_reply = 0;
            while (r.pos < fend) {
                ralign(&r, 8);
                if (!r.ok) break;
                uint8_t code = rbyte(&r);
                const char *sig = rsig(&r);
                if (code == HF_REPLY_SERIAL) { got_reply = ru32(&r); }
                else { const char *s = sig; skip_val(&r, &s, 0); }
                if (!r.ok) break;
            }
            if (got_reply == serial && type == DBUS_TYPE_METHOD_RETURN) {
                /* Capture body's first u32 (only used by RequestName today). */
                int body_off = (16 + (int)farr_len + 7) & ~7;
                if (body_off + 4 <= total)
                    memcpy(&last_request_name_result, rbuf + body_off, 4);
                rc = 0;
            } else {
                dispatch_one(rbuf, total);
            }
            memmove(rbuf, rbuf + total, rlen - total);
            rlen -= total;
            if (rc == 0) goto done;
        }
    }
done:
    tv.tv_sec = 0;
    setsockopt(dbus_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    fcntl(dbus_fd, F_SETFL, saved_flags);
    return rc;
}

/* Schedule a reconnect attempt `delay_ms` from now. Cheap no-op if the
 * timerfd isn't created yet (e.g. dbus_connect ran before reconnect_init). */
static void arm_reconnect(int delay_ms) {
    if (dbus_reconnect_fd < 0) return;
    struct itimerspec ts = {
        .it_value    = { .tv_sec = delay_ms / 1000,
                         .tv_nsec = (long)(delay_ms % 1000) * 1000000L },
        .it_interval = {0}
    };
    timerfd_settime(dbus_reconnect_fd, 0, &ts, NULL);
}
/* Tear down the current dbus socket and queue a reconnect. Used by every
 * error path that detected a broken stream or peer disconnect. */
static void dbus_drop(int delay_ms) {
    if (dbus_fd >= 0) { close(dbus_fd); dbus_fd = -1; }
    rlen = 0; rskip = 0;
    /* Serials restart per connection — replies can never arrive now. */
    memset(pending, 0, sizeof pending);
    rbuf_maybe_shrink();
    arm_reconnect(delay_ms);
}

void dbus_dispatch(void) {
    if (dbus_fd < 0) return;
    /* Drain the tail of a message that was bigger than RBUF_MAX. */
    while (rskip > 0) {
        uint8_t scratch[4096];
        int want = rskip > (int)sizeof scratch ? (int)sizeof scratch : rskip;
        ssize_t n = recv(dbus_fd, scratch, want, MSG_DONTWAIT);
        if (n < 0) { if (errno == EAGAIN) return; dbus_drop(2000); return; }
        if (n == 0) { dbus_drop(2000); return; }
        rskip -= (int)n;
    }
    for (;;) {
        if (rbuf_ensure(rlen + 1) < 0) { dbus_drop(2000); return; }
        if (rcap == rlen) break;                     /* nothing more to grow into */
        ssize_t n = recv(dbus_fd, rbuf + rlen, rcap - rlen, MSG_DONTWAIT);
        if (n < 0) { if (errno == EAGAIN) break; dbus_drop(2000); return; }
        if (n == 0) { dbus_drop(2000); return; }
        rlen += (int)n;
    }
    for (;;) {
        if (rlen < 16) break;
        /* We read lengths as little-endian; a big-endian ('B') message would
         * desync the stream. No LE desktop bus sends BE — drop and reconnect. */
        if (rbuf[0] != 'l') { dbus_drop(2000); return; }
        uint32_t body_len  = *(uint32_t *)(rbuf + 4);
        uint32_t farr_len  = *(uint32_t *)(rbuf + 12);
        int hdr = (16 + (int)farr_len + 7) & ~7;
        int total = hdr + (int)body_len;
        if (total < 0) { dbus_drop(2000); return; }
        if (total > RBUF_MAX) {
            /* Too big to ever buffer — drain whatever we have, then keep
             * draining off the socket via the rskip path. */
            msg("wisp: dbus message %d B > %d cap, dropping", total, RBUF_MAX);
            rskip = total - rlen;
            rlen = 0;
            return;
        }
        if (rbuf_ensure(total) < 0) { dbus_drop(2000); return; }
        if (rlen < total) {
            /* Grow done; try to pull the rest of this message now so we
             * don't stall on a single epoll wake-up. */
            while (rlen < total) {
                ssize_t n = recv(dbus_fd, rbuf + rlen, total - rlen, MSG_DONTWAIT);
                if (n < 0) { if (errno == EAGAIN) return; dbus_drop(2000); return; }
                if (n == 0) { dbus_drop(2000); return; }
                rlen += (int)n;
            }
        }
        dispatch_one(rbuf, total);
        memmove(rbuf, rbuf + total, rlen - total);
        rlen -= total;
    }
    rbuf_maybe_shrink();
}

void dbus_reconnect_init(void) {
    if (dbus_reconnect_fd >= 0) return;
    dbus_reconnect_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
}

void dbus_reconnect_handle(void) {
    if (dbus_reconnect_fd < 0) return;
    uint64_t e;
    (void)!read(dbus_reconnect_fd, &e, sizeof e);
    if (dbus_fd >= 0) return;                         /* already up */
    int fd = dbus_connect();
    if (fd < 0) { arm_reconnect(5000); return; }
    epoll_add_fd(fd);
}

int dbus_connect(void) {
    /* Ensure the rx buffer exists before any recv. */
    if (rbuf_ensure(RBUF_BASE) < 0) { arm_reconnect(5000); return -1; }
    rlen = 0; rskip = 0;
    const char *addr = getenv("DBUS_SESSION_BUS_ADDRESS");
    char fallback[256] = "";
    if (!addr) {
        const char *rt = getenv("XDG_RUNTIME_DIR");
        if (!rt) { msg("wisp: no DBUS_SESSION_BUS_ADDRESS / XDG_RUNTIME_DIR"); arm_reconnect(5000); return -1; }
        snprintf(fallback, sizeof fallback, "unix:path=%s/bus", rt);
        addr = fallback;
    }
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)] = "";
    int abs_len = dbus_parse_bus_addr(addr, path, sizeof path);
    /* parse_bus_addr leaves path untouched on failure; abstract success never
     * returns <0, so abs_len<0 alone is the failure signal. */
    if (abs_len < 0) { msg("wisp: bad DBUS addr %s", addr); arm_reconnect(5000); return -1; }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { msg("wisp: dbus socket: %s", strerror(errno)); arm_reconnect(5000); return -1; }
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    int slen;
    if (abs_len > 0) {              /* abstract namespace */
        memcpy(a.sun_path, path, abs_len);
        slen = (int)offsetof(struct sockaddr_un, sun_path) + abs_len;
    } else {
        snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
        slen = sizeof a;
    }
    if (connect(fd, (struct sockaddr *)&a, slen) < 0) {
        msg("wisp: dbus connect %s: %s", path, strerror(errno));
        close(fd); arm_reconnect(5000); return -1;
    }
    dbus_fd = fd;
    if (dbus_sasl_auth(fd) < 0) {
        msg("wisp: dbus auth failed");
        close(fd); dbus_fd = -1; arm_reconnect(5000); return -1;
    }
    if (call_hello() < 0) {
        msg("wisp: dbus Hello failed");
        close(fd); dbus_fd = -1; arm_reconnect(5000); return -1;
    }
    if (call_request_name("org.freedesktop.Notifications") < 0) {
        msg("wisp: dbus RequestName failed (mako still running?)");
        close(fd); dbus_fd = -1; arm_reconnect(5000); return -1;
    }
    /* Best-effort: another tray host already owning the Watcher just means we
     * never receive items — not a reason to tear down the notification server. */
    for (int i = 0; i < own_name_n; i++) call_request_name(own_names[i]);
    int fl = fcntl(fd, F_GETFL); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    for (int i = 0; i < dbus_sub_n; i++)
        call_add_match(dbus_subs[i].iface, dbus_subs[i].member);
    /* Consumers that must issue a call the moment the bus is up (mpris.c
     * enumerates players). Weak: absent unless that object is linked in. */
    extern void mpris_on_bus_up(void) __attribute__((weak));
    if (mpris_on_bus_up) mpris_on_bus_up();
#ifdef WISP_HAS_TRAY
    tray_on_bus_up();
#endif
    msg("wisp: dbus connected, owning org.freedesktop.Notifications");
    return fd;
}

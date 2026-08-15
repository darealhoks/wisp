/* idle { timeout … before_sleep = … } — swayidle + wlopm, in-process.
 *
 * Three event sources, no timer anywhere: the compositor owns the countdowns
 * (ext-idle-notify-v1, one notification object per declared timeout), logind
 * owns the suspend signal (PrepareForSleep on the system bus, held off by a
 * `delay` inhibitor fd), and `wispctl dpms` is a request on
 * zwlr_output_power_v1. Nothing here polls; idle stays 0 ticks/sec. The one
 * timer is the dpms retry one-shot, armed only while a set_mode sits FAILED.
 *
 * We also own org.freedesktop.ScreenSaver (dbus.c routes it here): nothing else
 * on a wlroots box does, so a browser playing video would be blanked. An active
 * Inhibit cookie destroys the notification objects; the last release recreates
 * them, which restarts the compositor's countdowns.
 *
 * The logind connection is a third tiny system-bus client, same shape as
 * power.c / bluez.c (SASL + Hello + AddMatch), with one addition: the Inhibit
 * reply carries a unix fd, so the read loop is recvmsg + SCM_RIGHTS.
 *
 * before_sleep releases the inhibitor when the action *is up*, not when it was
 * started (swayidle -w): the child is spawned holding the write end of a pipe
 * on fd 3, and we release on the first byte (wisp-lock says "locked") or on
 * EOF (a plain command exited). */

#define _GNU_SOURCE
#include "wisp.h"
#include "dbus.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <unistd.h>

#include "gen_idle.h"

#define LOGIND_DEST  "org.freedesktop.login1"
#define LOGIND_PATH  "/org/freedesktop/login1"
#define LOGIND_IFACE "org.freedesktop.login1.Manager"

int idle_bus_fd = -1;

static uint32_t notif_ids[IDLE_N_TIMEOUTS ? IDLE_N_TIMEOUTS : 1];
static int      notifs_live;

static void run_sh(const char *cmd) {
    if (!cmd || !*cmd) return;
    pid_t p = fork();
    if (p == 0) {
        setsid();
        signal(SIGCHLD, SIG_DFL);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

/* ============================================================ */
/* dpms — zwlr_output_power_v1                                   */
/* ============================================================ */

uint32_t id_output_power_mgr;

static int      dpms_want = 1;      /* last requested state; outputs appearing later match it */
static uint32_t dpms_pending;       /* per-output bit: set_mode FAILED, retry on the timer */
static int      dpms_retry_tfd = -1;
static int      dpms_retry_s = 2;
static int      dpms_tries;         /* retries spent on the current dpms_set() */

/* On mango every set_mode is an output disable + DRM commit, so an output that
 * never succeeds must not be re-modeset every 60s forever. */
#define DPMS_MAX_TRIES 5

static void dpms_apply(Output *o, int on);

/* One-shot, armed only while a set_mode sits FAILED (a panel that can't light
 * yet — lid still closed — or a control held elsewhere); success leaves it
 * disarmed, so idle stays timer-free. Backoff doubles to 60s. */
static void dpms_retry_arm(void) {
    if (dpms_retry_tfd < 0) {
        dpms_retry_tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (dpms_retry_tfd < 0) return;
        epoll_add_fd(dpms_retry_tfd);
    }
    struct itimerspec its = { .it_value.tv_sec = dpms_retry_s };
    timerfd_settime(dpms_retry_tfd, 0, &its, NULL);
    if (dpms_retry_s < 60) dpms_retry_s *= 2;
}

static void dpms_retry_fire(void) {
    uint64_t junk;
    if (read(dpms_retry_tfd, &junk, sizeof junk) < 0) { /* spurious wake */ }
    uint32_t pend = dpms_pending;
    dpms_pending = 0;
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if ((pend & (1u << i)) && outputs[i].active)
            dpms_apply(&outputs[i], dpms_want);
}

static void dpms_apply(Output *o, int on) {
    if (!o->power_ctrl) {
        o->power_ctrl = wl_new_id();
        uint32_t a[2] = { o->power_ctrl, o->wl_output };
        wl_req(id_output_power_mgr, OUTPUT_POWER_MGR_REQ_GET_OUTPUT_POWER, a, 2, -1);
    }
    uint32_t mode = on ? OUTPUT_POWER_MODE_ON : OUTPUT_POWER_MODE_OFF;
    wl_req(o->power_ctrl, OUTPUT_POWER_REQ_SET_MODE, &mode, 1, -1);
}

void dpms_set(int on) {
    if (!id_output_power_mgr) {
        msg("dpms: wlr-output-power-management-unstable-v1 not advertised — ignoring");
        return;
    }
    dpms_want = on;
    dpms_pending = 0;
    dpms_retry_s = 2;
    dpms_tries = 0;
    if (dpms_retry_tfd >= 0) {
        struct itimerspec zero = {0};
        timerfd_settime(dpms_retry_tfd, 0, &zero, NULL);
    }
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (outputs[i].active) dpms_apply(&outputs[i], on);
}

/* An output hotplugged back while blanked would otherwise come up lit. */
void idle_on_output_added(Output *o) {
    if (!o) return;
    dpms_retry_s = 2;
    dpms_tries = 0;
    if (!dpms_want && id_output_power_mgr) dpms_apply(o, 0);
}

/* dpms_pending is indexed by slot, and slots are reused — a bit left set by a
 * departing output would re-fire against whoever takes its place. */
void idle_on_output_removed(Output *o) {
    if (!o) return;
    ptrdiff_t i = o - outputs;
    if (i >= 0 && i < MAX_OUTPUTS) dpms_pending &= ~(1u << i);
}

/* ============================================================ */
/* ext-idle-notify-v1                                            */
/* ============================================================ */

uint32_t id_idle_notifier;

int idle_wl_event(uint32_t obj, uint16_t op) {
    for (int i = 0; i < IDLE_N_TIMEOUTS; i++) {
        if (!notif_ids[i] || notif_ids[i] != obj) continue;
        if (op == IDLE_NOTIF_EV_IDLED)        run_sh(idle_timeouts[i].run);
        else if (op == IDLE_NOTIF_EV_RESUMED) run_sh(idle_timeouts[i].resume);
        return 1;
    }
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        Output *o = &outputs[i];
        if (!o->active || !o->power_ctrl || o->power_ctrl != obj) continue;
        if (op == OUTPUT_POWER_EV_FAILED) {
            wl_req(o->power_ctrl, OUTPUT_POWER_REQ_DESTROY, NULL, 0, -1);
            o->power_ctrl = 0;
            if (dpms_tries >= DPMS_MAX_TRIES) {
                dpms_pending &= ~(1u << i);
                msg("dpms: output power set still failing on %s after %d attempts — giving up until the next dpms request",
                    o->name, DPMS_MAX_TRIES);
                return 1;
            }
            dpms_tries++;
            dpms_pending |= 1u << i;
            msg("dpms: output power set failed on %s (panel not ready, or control held elsewhere) — retrying in %ds",
                o->name, dpms_retry_s);
            dpms_retry_arm();
        }
        return 1;
    }
    return 0;
}

static int ss_inhibited(void);

static void idle_notify_init(void) {
    if (notifs_live || IDLE_N_TIMEOUTS == 0) return;
    if (ss_inhibited()) return;     /* an Inhibit landed before init (dbus_connect runs first) */
    if (!id_idle_notifier || !id_seat) {
        static int warned;
        if (!warned) {
            warned = 1;
            msg("idle: ext-idle-notify-v1 unavailable — %d declared timeout(s) will never fire",
                IDLE_N_TIMEOUTS);
        }
        return;
    }
    for (int i = 0; i < IDLE_N_TIMEOUTS; i++) {
        notif_ids[i] = wl_new_id();
        uint32_t a[3] = { notif_ids[i], (uint32_t)idle_timeouts[i].after_ms, id_seat };
        wl_req(id_idle_notifier, IDLE_NOTIFIER_REQ_GET_IDLE_NOTIFICATION, a, 3, -1);
    }
    notifs_live = 1;
}

/* ============================================================ */
/* org.freedesktop.ScreenSaver — inhibitors from Firefox/mpv/… */
/* ============================================================ */

/* ponytail: 12 slots; overflow hands out an untracked cookie (we are already
 * inhibited by whoever filled the table) — raise the cap if it ever logs */
#define SS_MAX 12
static struct { uint32_t cookie; char owner[64]; } ss_cookies[SS_MAX];
static uint32_t ss_next_cookie = 1;

static int ss_inhibited(void) {
    for (int i = 0; i < SS_MAX; i++) if (ss_cookies[i].cookie) return 1;
    return 0;
}

static void ss_apply(void) {
    if (!ss_inhibited()) { idle_notify_init(); return; }
    if (!notifs_live) return;
    for (int i = 0; i < IDLE_N_TIMEOUTS; i++) {
        if (!notif_ids[i]) continue;
        wl_req(notif_ids[i], IDLE_NOTIF_REQ_DESTROY, NULL, 0, -1);
        notif_ids[i] = 0;
    }
    notifs_live = 0;
}

/* Cookie holders are tracked by their bus name; a session-bus drop makes their
 * NameOwnerChanged unreachable, so a stale cookie would inhibit forever. */
void idle_ss_reset(void) {
    memset(ss_cookies, 0, sizeof ss_cookies);
    ss_apply();
}

static void ss_drop_owner(const char *name) {
    int hit = 0;
    for (int i = 0; i < SS_MAX; i++)
        if (ss_cookies[i].cookie && !strcmp(ss_cookies[i].owner, name)) {
            ss_cookies[i].cookie = 0;
            hit = 1;
        }
    if (hit) ss_apply();
}

static void ss_name_owner_changed(const char *sender, const char *path,
                                  const uint8_t *body, int len, const char *sig) {
    (void)sender; (void)path;
    if (!sig || strncmp(sig, "sss", 3)) return;
    R r = { .b = body, .len = len, .pos = 0, .ok = 1 };
    char name[64];
    snprintf(name, sizeof name, "%s", rstr(&r));
    rstr(&r);
    const char *new_owner = rstr(&r);
    if (r.ok && !new_owner[0]) ss_drop_owner(name);
}

int screensaver_method_call(R *r, const char *member, const char *path,
                            uint32_t serial, const char *sender) {
    if (strcmp(path, "/ScreenSaver") && strcmp(path, "/org/freedesktop/ScreenSaver"))
        return 0;
    if (!strcmp(member, "Inhibit")) {
        static int subscribed;
        if (!subscribed) {
            subscribed = 1;
            dbus_subscribe("org.freedesktop.DBus", "NameOwnerChanged", ss_name_owner_changed);
        }
        uint32_t cookie = ss_next_cookie++;
        if (!ss_next_cookie) ss_next_cookie = 1;
        int slot = -1;
        for (int i = 0; i < SS_MAX; i++) if (!ss_cookies[i].cookie) { slot = i; break; }
        if (slot < 0) {
            msg("idle: %d ScreenSaver inhibitors held, %s's is untracked", SS_MAX, sender);
        } else {
            ss_cookies[slot].cookie = cookie;
            snprintf(ss_cookies[slot].owner, sizeof ss_cookies[slot].owner, "%s", sender);
            ss_apply();
        }
        W b = {0};
        wu32(&b, cookie);
        Msg m = { .type = DBUS_TYPE_METHOD_RETURN, .reply_serial = serial,
                  .destination = sender, .signature = "u",
                  .body = b.b, .body_len = b.pos };
        send_msg(&m);
        free(b.b);
        return 1;
    }
    if (!strcmp(member, "UnInhibit")) {
        uint32_t cookie = ru32(r);
        if (r->ok && cookie)
            for (int i = 0; i < SS_MAX; i++)
                if (ss_cookies[i].cookie == cookie) { ss_cookies[i].cookie = 0; break; }
        ss_apply();
        dbus_reply_empty(serial, sender);
        return 1;
    }
    return 0;
}

/* ============================================================ */
/* before_sleep — logind delay inhibitor on the system bus       */
/* ============================================================ */

#if defined(IDLE_SLEEP_LOCK) || defined(IDLE_SLEEP_CMD)

static uint32_t bus_serial = 1;
static uint32_t inhibit_serial;
static int      inhibit_fd = -1;
static int      child_fd   = -1;   /* before_sleep action's readiness pipe */

static uint32_t bus_send(const Msg *m) {
    uint32_t ser = bus_serial++;
    if (!bus_serial) bus_serial = 1;
    W h = {0};
    dbus_msg_build(&h, m, ser);
    int rc = (int)send(idle_bus_fd, h.b, h.pos, MSG_NOSIGNAL);
    free(h.b);
    return rc == h.pos ? ser : 0;
}

static void take_inhibitor(void) {
    if (inhibit_fd >= 0 || idle_bus_fd < 0) return;
    W b = {0};
    wstr(&b, "sleep");
    wstr(&b, "wisp");
    wstr(&b, "lock the session before sleep");
    wstr(&b, "delay");
    Msg m = { .type = DBUS_TYPE_METHOD_CALL,
              .path = LOGIND_PATH, .interface = LOGIND_IFACE, .member = "Inhibit",
              .destination = LOGIND_DEST,
              .signature = "ssss", .body = b.b, .body_len = b.pos };
    inhibit_serial = bus_send(&m);
    free(b.b);
}

static void release_inhibitor(void) {
    if (inhibit_fd >= 0) close(inhibit_fd);
    inhibit_fd = -1;
}

static void child_done(void) {
    if (child_fd < 0) return;
    epoll_del_fd(child_fd);
    close(child_fd);
    child_fd = -1;
    release_inhibitor();
}

/* Spawn the before_sleep action holding the pipe's write end on fd 3. The
 * inhibitor stays held until that fd reports ready (wisp-lock writes a byte
 * once the compositor confirms `locked`) or closes (the command exited). */
static void run_before_sleep(void) {
    if (child_fd >= 0) return;              /* already running from an earlier signal */
    int pf[2];
    if (pipe(pf) < 0) { release_inhibitor(); return; }
    pid_t p = fork();
    if (p == 0) {
        close(pf[0]);
        setsid();
        signal(SIGCHLD, SIG_DFL);
        if (pf[1] != 3) { dup2(pf[1], 3); close(pf[1]); }
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
#ifdef IDLE_SLEEP_LOCK
        execlp("wisp-lock", "wisp-lock", "--ready-fd", "3", (char *)NULL);
#else
        execl("/bin/sh", "sh", "-c", IDLE_SLEEP_CMD, (char *)NULL);
#endif
        _exit(127);
    }
    close(pf[1]);
    if (p < 0) { close(pf[0]); release_inhibitor(); return; }
    child_fd = pf[0];
    epoll_add_fd(child_fd);
}

static void on_prepare_for_sleep(int going) {
    if (going) run_before_sleep();
    else       { child_done(); take_inhibitor(); }
}

static void bus_down(void) {
    if (idle_bus_fd >= 0) { epoll_del_fd(idle_bus_fd); close(idle_bus_fd); }
    idle_bus_fd = -1;
    release_inhibitor();
}

static void bus_dispatch_one(const uint8_t *b, int len, int fd_in) {
    R r = { .b = b, .len = len, .pos = 0, .ok = 1 };
    rbyte(&r); uint8_t type = rbyte(&r); rbyte(&r); rbyte(&r);
    ru32(&r);                              /* body length */
    ru32(&r);                              /* serial */
    uint32_t flen = ru32(&r);
    if (!r.ok) return;
    int fend = r.pos + (int)flen;
    if (fend > r.len) return;
    char member[64] = "", iface[96] = "";
    uint32_t reply_serial = 0;
    while (r.pos < fend) {
        ralign(&r, 8);
        if (!r.ok) return;
        uint8_t code = rbyte(&r);
        const char *sig = rsig(&r);
        if (!r.ok || !sig[0]) break;
        switch (code) {
        case HF_INTERFACE:    snprintf(iface,  sizeof iface,  "%s", rstr(&r)); break;
        case HF_MEMBER:       snprintf(member, sizeof member, "%s", rstr(&r)); break;
        case HF_REPLY_SERIAL: reply_serial = ru32(&r); break;
        default: { const char *s = sig; skip_val(&r, &s, 0); break; }
        }
        if (!r.ok) return;
    }
    r.pos = fend;
    ralign(&r, 8);

    if (type == DBUS_TYPE_METHOD_RETURN && reply_serial == inhibit_serial) {
        if (fd_in >= 0) inhibit_fd = fd_in;
        return;
    }
    if (type == DBUS_TYPE_ERROR && reply_serial == inhibit_serial) {
        msg("idle: logind refused the sleep inhibitor — before_sleep may race suspend");
        return;
    }
    if (type != DBUS_TYPE_SIGNAL) return;
    if (!strcmp(member, "PrepareForSleep") && !strcmp(iface, LOGIND_IFACE)) {
        uint32_t going = ru32(&r);
        if (r.ok) on_prepare_for_sleep(going != 0);
    }
}

/* recvmsg, not recv: the Inhibit reply carries its fd out of band. */
static void bus_dispatch(void) {
    static uint8_t buf[2048];
    static int have;
    for (;;) {
        int got_fd = -1;
        union { struct cmsghdr a; char b[CMSG_SPACE(sizeof(int))]; } cbuf;
        struct iovec iov = { .iov_base = buf + have, .iov_len = sizeof buf - have };
        struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1,
                             .msg_control = cbuf.b, .msg_controllen = sizeof cbuf.b };
        ssize_t n = recvmsg(idle_bus_fd, &mh, MSG_CMSG_CLOEXEC);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            bus_down(); return;
        }
        if (n == 0) { bus_down(); return; }
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            int nfd = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (int i = 0; i < nfd; i++) {
                int f; memcpy(&f, CMSG_DATA(c) + i * sizeof(int), sizeof f);
                if (got_fd < 0) got_fd = f; else close(f);
            }
        }
        have += (int)n;
        for (;;) {
            if (have < 16) break;
            uint32_t body_len, fields_len;
            memcpy(&body_len, buf + 4, 4);
            memcpy(&fields_len, buf + 12, 4);
            uint32_t hdr = (16 + fields_len + 7u) & ~7u;
            uint64_t total = (uint64_t)hdr + body_len;
            if (total > sizeof buf) { if (got_fd >= 0) close(got_fd); bus_down(); return; }
            if ((uint64_t)have < total) break;
            int before = inhibit_fd;
            bus_dispatch_one(buf, (int)total, got_fd);
            if (inhibit_fd != before) got_fd = -1;   /* consumed */
            memmove(buf, buf + total, have - (int)total);
            have -= (int)total;
        }
        if (got_fd >= 0) close(got_fd);
        if (have == (int)sizeof buf) { bus_down(); return; }
    }
}

static void bus_add_match(const char *rule) {
    W b = {0};
    wstr(&b, rule);
    Msg m = { .type = DBUS_TYPE_METHOD_CALL, .flags = 1,
              .path = "/org/freedesktop/DBus", .interface = "org.freedesktop.DBus",
              .member = "AddMatch", .destination = "org.freedesktop.DBus",
              .signature = "s", .body = b.b, .body_len = b.pos };
    bus_send(&m);
    free(b.b);
}

static void sleep_init(void) {
    const char *addr = getenv("DBUS_SYSTEM_BUS_ADDRESS");
    if (!addr) addr = "unix:path=/run/dbus/system_bus_socket";
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)] = "";
    int abs_len = dbus_parse_bus_addr(addr, path, sizeof path);
    if (abs_len < 0) { msg("idle: bad system bus addr %s", addr); return; }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    int slen;
    if (abs_len > 0) {
        memcpy(a.sun_path, path, abs_len);
        slen = (int)offsetof(struct sockaddr_un, sun_path) + abs_len;
    } else {
        snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
        slen = sizeof a;
    }
    if (connect(fd, (struct sockaddr *)&a, slen) < 0 || dbus_sasl_auth(fd) < 0) {
        msg("idle: system bus unavailable — before_sleep disabled");
        close(fd); return;
    }
    idle_bus_fd = fd;
    Msg hello = { .type = DBUS_TYPE_METHOD_CALL,
                  .path = "/org/freedesktop/DBus", .interface = "org.freedesktop.DBus",
                  .member = "Hello", .destination = "org.freedesktop.DBus" };
    bus_send(&hello);
    bus_add_match("type='signal',interface='" LOGIND_IFACE "',member='PrepareForSleep'");
    take_inhibitor();
    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int idle_owns_fd(int fd) {
    return fd >= 0 && (fd == idle_bus_fd || fd == child_fd || fd == dpms_retry_tfd);
}

void idle_dispatch(int fd) {
    if (fd == idle_bus_fd) { bus_dispatch(); return; }
    if (fd == dpms_retry_tfd) { dpms_retry_fire(); return; }
    if (fd != child_fd) return;
    char c;
    ssize_t n = read(child_fd, &c, 1);
    if (n > 0 || n == 0) child_done();       /* ready byte, or the action exited */
    else if (errno != EAGAIN && errno != EINTR) child_done();
}

#else   /* no before_sleep declared */

static void sleep_init(void) {}
int  idle_owns_fd(int fd) { return fd >= 0 && (fd == idle_bus_fd || fd == dpms_retry_tfd); }
void idle_dispatch(int fd) { if (fd == dpms_retry_tfd) dpms_retry_fire(); }

#endif

void idle_init(void) {
    idle_notify_init();
    sleep_init();
}

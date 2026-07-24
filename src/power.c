/* power-profiles-daemon client (net.hadess.PowerProfiles, SYSTEM bus).
 *
 * The session transport (dbus.c) is a singleton around dbus_fd, so this is a
 * second, deliberately tiny connection: SASL + Hello + two AddMatch + one
 * property Get, then a pure event stream. Wire guts come from dbus_wire.c.
 * pp_profile() returns "" while ppd (or the system bus) is unavailable — an
 * optional external daemon being absent is normal, not a die() case. On any
 * stream error we close and stay dark; a system bus that dies is a reboot.
 * ponytail: no reconnect, no SetProfile — add when someone actually asks. */

#define _GNU_SOURCE
#include "wisp.h"
#include "dbus.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define PPD_NAME  "net.hadess.PowerProfiles"
#define PPD_PATH  "/net/hadess/PowerProfiles"

int pp_fd = -1;

static char     pp_prof[32];
static uint32_t pp_serial = 1;
static uint32_t pp_get_serial;

/* Same shape as gamma/ctl: repaint whichever surfaces read a DRV_WISP source. */
void wispgen_wisp_state_changed(void) __attribute__((weak));

const char *pp_profile(void) { return pp_prof; }

static uint32_t pp_send(const Msg *m) {
    uint32_t ser = pp_serial++;
    if (!pp_serial) pp_serial = 1;
    W h = {0};
    dbus_msg_build(&h, m, ser);
    int rc = (int)send(pp_fd, h.b, h.pos, MSG_NOSIGNAL);
    free(h.b);
    return rc == h.pos ? ser : 0;
}

static void pp_add_match(const char *rule) {
    W b = {0};
    wstr(&b, rule);
    Msg m = { .type = DBUS_TYPE_METHOD_CALL, .flags = 1,
              .path = "/org/freedesktop/DBus",
              .interface = "org.freedesktop.DBus",
              .member = "AddMatch",
              .destination = "org.freedesktop.DBus",
              .signature = "s", .body = b.b, .body_len = b.pos };
    pp_send(&m);
    free(b.b);
}

/* Properties.Get(PPD_NAME, "ActiveProfile") — async; reply matched by serial. */
static void pp_get_active(void) {
    W b = {0};
    wstr(&b, PPD_NAME);
    wstr(&b, "ActiveProfile");
    Msg m = { .type = DBUS_TYPE_METHOD_CALL,
              .path = PPD_PATH,
              .interface = "org.freedesktop.DBus.Properties",
              .member = "Get",
              .destination = PPD_NAME,
              .signature = "ss", .body = b.b, .body_len = b.pos };
    pp_get_serial = pp_send(&m);
    free(b.b);
}

static void pp_set_prof(const char *v) {
    if (!strcmp(pp_prof, v)) return;
    snprintf(pp_prof, sizeof pp_prof, "%s", v);
    if (wispgen_wisp_state_changed) wispgen_wisp_state_changed();
}

static void pp_down(void) {
    if (pp_fd >= 0) close(pp_fd);
    pp_fd = -1;
    pp_set_prof("");
}

void pp_connect(void) {
    const char *addr = getenv("DBUS_SYSTEM_BUS_ADDRESS");
    if (!addr) addr = "unix:path=/run/dbus/system_bus_socket";
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)] = "";
    int abs_len = dbus_parse_bus_addr(addr, path, sizeof path);
    if (abs_len < 0) { msg("wisp: bad system bus addr %s", addr); return; }
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
        msg("wisp: system bus unavailable, power_profile() stays empty");
        close(fd); return;
    }
    pp_fd = fd;
    Msg hello = { .type = DBUS_TYPE_METHOD_CALL,
                  .path = "/org/freedesktop/DBus",
                  .interface = "org.freedesktop.DBus",
                  .member = "Hello",
                  .destination = "org.freedesktop.DBus" };
    pp_send(&hello);
    pp_add_match("type='signal',interface='org.freedesktop.DBus.Properties',"
                 "member='PropertiesChanged',path='" PPD_PATH "'");
    /* ppd starting after us: NameOwnerChanged for its name → re-Get. */
    pp_add_match("type='signal',interface='org.freedesktop.DBus',"
                 "member='NameOwnerChanged',arg0='" PPD_NAME "'");
    pp_get_active();
    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* a{sv}: find "ActiveProfile" → variant string. `r` sits at the array. */
static void pp_scan_props(R *r) {
    uint32_t alen = ru32(r);
    if (!r->ok) return;
    ralign(r, 8);
    int end = r->pos + (int)alen;
    if (end > r->len) return;
    while (r->pos < end && r->ok) {
        ralign(r, 8);
        const char *k = rstr(r);
        const char *vs = rsig(r);
        if (!r->ok) return;
        if (!strcmp(k, "ActiveProfile") && vs[0] == 's') {
            const char *v = rstr(r);
            if (r->ok) pp_set_prof(v);
        } else {
            skip_val(r, &vs, 0);
        }
    }
}

static void pp_dispatch_one(const uint8_t *b, int len) {
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
    char body_sig[16] = "";
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
        case HF_SIGNATURE:    snprintf(body_sig, sizeof body_sig, "%s", rsig(&r)); break;
        default: { const char *s = sig; skip_val(&r, &s, 0); break; }
        }
        if (!r.ok) return;
    }
    r.pos = fend;
    ralign(&r, 8);

    if (type == DBUS_TYPE_METHOD_RETURN && reply_serial == pp_get_serial) {
        /* Get reply: variant holding the profile string. */
        if (body_sig[0] == 'v') {
            const char *vs = rsig(&r);
            if (r.ok && vs[0] == 's') {
                const char *v = rstr(&r);
                if (r.ok) pp_set_prof(v);
            }
        }
        return;
    }
    if (type != DBUS_TYPE_SIGNAL) return;
    if (!strcmp(member, "PropertiesChanged") && !strncmp(body_sig, "sa{sv}", 6)) {
        const char *pi = rstr(&r);
        if (r.ok && !strcmp(pi, PPD_NAME)) pp_scan_props(&r);
    } else if (!strcmp(member, "NameOwnerChanged") &&
               !strcmp(iface, "org.freedesktop.DBus")) {
        const char *name = rstr(&r);
        rstr(&r);                          /* old owner */
        const char *neww = rstr(&r);
        if (!r.ok || strcmp(name, PPD_NAME)) return;
        if (neww[0]) pp_get_active();
        else pp_set_prof("");
    }
}

void pp_dispatch(void) {
    /* ppd messages are tiny; anything that doesn't fit the buffer is a peer
     * we don't understand — drop the connection rather than resync. */
    static uint8_t buf[2048];
    static int have;
    for (;;) {
        ssize_t n = recv(pp_fd, buf + have, sizeof buf - have, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            pp_down(); return;
        }
        if (n == 0) { pp_down(); return; }
        have += (int)n;
        for (;;) {
            if (have < 16) break;
            uint32_t body_len, fields_len;
            memcpy(&body_len, buf + 4, 4);
            memcpy(&fields_len, buf + 12, 4);
            uint32_t hdr = (16 + fields_len + 7u) & ~7u;
            uint64_t total = (uint64_t)hdr + body_len;
            if (total > sizeof buf) { pp_down(); return; }
            if ((uint64_t)have < total) break;
            pp_dispatch_one(buf, (int)total);
            memmove(buf, buf + total, have - (int)total);
            have -= (int)total;
        }
        if (have == (int)sizeof buf) { pp_down(); return; }
    }
}

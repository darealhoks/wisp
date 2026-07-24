/* BlueZ client (org.bluez, SYSTEM bus) — read-only bluetooth state source.
 *
 * Structural clone of power.c: a second tiny system-bus connection (SASL +
 * Hello + AddMatch + one GetManagedObjects), then a pure ObjectManager event
 * stream. Wire guts come from dbus_wire.c. Everything stays empty/off while
 * bluetoothd (or the system bus) is unavailable — an optional daemon being
 * absent is normal, not a die() case. Any malformed frame drops the
 * connection (like power.c); a system bus that dies is a reboot.
 * ponytail: no reconnect, no pairing/connect controls — read-only for the bar. */

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

int bz_fd = -1;

/* ponytail: first adapter only — a laptop has one radio; a second adapter's
 * Powered is ignored. Track its path so PropertiesChanged routes to it. */
static char adapter_path[64];
static int  adapter_powered;

static struct Dev {
    char path[64];
    char alias[48];
    int  connected;
    int  battery;   /* -1 = no Battery1 */
    int  present;
} devs[8];

static uint32_t bz_serial = 1;
static uint32_t bz_gmo_serial;

/* Same shape as power.c/gamma: repaint whichever surfaces read the source. */
void wispgen_wisp_state_changed(void) __attribute__((weak));

/* ---- accessors ---------------------------------------------------- */

static struct Dev *active_dev(void) {
    for (int i = 0; i < 8; i++)
        if (devs[i].present && devs[i].connected) return &devs[i];
    return NULL;
}

int bz_powered(void)   { return adapter_powered; }
int bz_connected(void) { return active_dev() ? 1 : 0; }
const char *bz_device(void) { struct Dev *d = active_dev(); return d ? d->alias : ""; }
int bz_battery(void)   { struct Dev *d = active_dev(); return d ? d->battery : -1; }

/* ---- device table ------------------------------------------------- */

static struct Dev *dev_find(const char *path) {
    for (int i = 0; i < 8; i++)
        if (devs[i].present && !strcmp(devs[i].path, path)) return &devs[i];
    return NULL;
}
static struct Dev *dev_get(const char *path) {
    struct Dev *d = dev_find(path);
    if (d) return d;
    for (int i = 0; i < 8; i++) {
        if (devs[i].present) continue;
        memset(&devs[i], 0, sizeof devs[i]);
        snprintf(devs[i].path, sizeof devs[i].path, "%s", path);
        devs[i].battery = -1;
        devs[i].present = 1;
        return &devs[i];
    }
    return NULL;  /* table full: 9th device ignored until one leaves */
}
static void dev_remove(const char *path) {
    struct Dev *d = dev_find(path);
    if (d) d->present = 0;
}

/* ---- state fingerprint (diff → one hook call) --------------------- */

struct Snap { int powered, connected, battery; char dev[48]; };
static void snap(struct Snap *s) {
    s->powered   = bz_powered();
    s->connected = bz_connected();
    s->battery   = bz_battery();
    snprintf(s->dev, sizeof s->dev, "%s", bz_device());
}
static void maybe_changed(const struct Snap *before) {
    struct Snap now; snap(&now);
    if (now.powered == before->powered && now.connected == before->connected &&
        now.battery == before->battery && !strcmp(now.dev, before->dev))
        return;
    if (wispgen_wisp_state_changed) wispgen_wisp_state_changed();
}

/* ---- parsing ------------------------------------------------------ */

/* a{sv} of one interface's properties, routed by iface name + object path. */
static void scan_props(R *r, const char *iface, const char *path) {
    uint32_t alen = ru32(r);
    if (!r->ok) return;
    ralign(r, 8);
    int end = r->pos + (int)alen;
    if (end > r->len) { r->ok = 0; return; }
    int is_adapter = !strcmp(iface, "org.bluez.Adapter1");
    int is_device  = !strcmp(iface, "org.bluez.Device1");
    int is_battery = !strcmp(iface, "org.bluez.Battery1");
    struct Dev *d = (is_device || is_battery) ? dev_get(path) : NULL;
    while (r->pos < end && r->ok) {
        ralign(r, 8);
        const char *k  = rstr(r);
        const char *vs = rsig(r);
        if (!r->ok) return;
        if (is_adapter && !strcmp(k, "Powered") && vs[0] == 'b') {
            uint32_t v = ru32(r);
            if (r->ok && (!adapter_path[0] || !strcmp(adapter_path, path))) {
                if (!adapter_path[0])
                    snprintf(adapter_path, sizeof adapter_path, "%s", path);
                adapter_powered = v ? 1 : 0;
            }
        } else if (is_device && d && !strcmp(k, "Connected") && vs[0] == 'b') {
            uint32_t v = ru32(r); if (r->ok) d->connected = v ? 1 : 0;
        } else if (is_device && d && !strcmp(k, "Alias") && vs[0] == 's') {
            const char *v = rstr(r);
            if (r->ok) snprintf(d->alias, sizeof d->alias, "%s", v);
        } else if (is_battery && d && !strcmp(k, "Percentage") && vs[0] == 'y') {
            uint8_t v = rbyte(r); if (r->ok) d->battery = v;
        } else {
            skip_val(r, &vs, 0);
        }
    }
}

/* a{sa{sv}}: the interfaces dict of one managed object. */
static void scan_ifaces(R *r, const char *path) {
    uint32_t alen = ru32(r);
    if (!r->ok) return;
    ralign(r, 8);
    int end = r->pos + (int)alen;
    if (end > r->len) { r->ok = 0; return; }
    while (r->pos < end && r->ok) {
        ralign(r, 8);
        const char *iface = rstr(r);
        if (!r->ok) return;
        scan_props(r, iface, path);
    }
}

/* a{oa{sa{sv}}}: full GetManagedObjects reply. */
static void scan_objects(R *r) {
    uint32_t alen = ru32(r);
    if (!r->ok) return;
    ralign(r, 8);
    int end = r->pos + (int)alen;
    if (end > r->len) { r->ok = 0; return; }
    while (r->pos < end && r->ok) {
        ralign(r, 8);
        const char *path = rstr(r);
        if (!r->ok) return;
        scan_ifaces(r, path);
    }
}

/* InterfacesRemoved body oas: drop the device/adapter the interfaces belong to. */
static void scan_removed(R *r) {
    const char *path = rstr(r);
    if (!r->ok) return;
    char p[64]; snprintf(p, sizeof p, "%s", path);
    uint32_t alen = ru32(r);
    if (!r->ok) return;
    ralign(r, 4);
    int end = r->pos + (int)alen;
    if (end > r->len) { r->ok = 0; return; }
    while (r->pos < end && r->ok) {
        const char *iface = rstr(r);
        if (!r->ok) return;
        if (!strcmp(iface, "org.bluez.Device1")) dev_remove(p);
        else if (!strcmp(iface, "org.bluez.Adapter1") &&
                 !strcmp(adapter_path, p)) { adapter_path[0] = 0; adapter_powered = 0; }
    }
}

/* ---- connection --------------------------------------------------- */

static uint32_t bz_send(const Msg *m) {
    uint32_t ser = bz_serial++;
    if (!bz_serial) bz_serial = 1;
    W h = {0};
    dbus_msg_build(&h, m, ser);
    int rc = (int)send(bz_fd, h.b, h.pos, MSG_NOSIGNAL);
    free(h.b);
    return rc == h.pos ? ser : 0;
}

static void bz_add_match(const char *rule) {
    W b = {0};
    wstr(&b, rule);
    Msg m = { .type = DBUS_TYPE_METHOD_CALL, .flags = 1,
              .path = "/org/freedesktop/DBus",
              .interface = "org.freedesktop.DBus",
              .member = "AddMatch",
              .destination = "org.freedesktop.DBus",
              .signature = "s", .body = b.b, .body_len = b.pos };
    bz_send(&m);
    free(b.b);
}

static void bz_get_managed(void) {
    Msg m = { .type = DBUS_TYPE_METHOD_CALL,
              .path = "/",
              .interface = "org.freedesktop.DBus.ObjectManager",
              .member = "GetManagedObjects",
              .destination = "org.bluez" };
    bz_gmo_serial = bz_send(&m);
}

static void bz_down(void) {
    if (bz_fd >= 0) close(bz_fd);
    bz_fd = -1;
    struct Snap before; snap(&before);
    memset(devs, 0, sizeof devs);
    adapter_path[0] = 0;
    adapter_powered = 0;
    maybe_changed(&before);
}

void bz_connect(void) {
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
        msg("wisp: system bus unavailable, bluetooth() stays empty");
        close(fd); return;
    }
    bz_fd = fd;
    Msg hello = { .type = DBUS_TYPE_METHOD_CALL,
                  .path = "/org/freedesktop/DBus",
                  .interface = "org.freedesktop.DBus",
                  .member = "Hello",
                  .destination = "org.freedesktop.DBus" };
    bz_send(&hello);
    bz_add_match("type='signal',interface='org.freedesktop.DBus.ObjectManager',"
                 "member='InterfacesAdded',sender='org.bluez'");
    bz_add_match("type='signal',interface='org.freedesktop.DBus.ObjectManager',"
                 "member='InterfacesRemoved',sender='org.bluez'");
    bz_add_match("type='signal',interface='org.freedesktop.DBus.Properties',"
                 "member='PropertiesChanged',sender='org.bluez'");
    bz_get_managed();
    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void bz_dispatch_one(const uint8_t *b, int len) {
    struct Snap before; snap(&before);
    R r = { .b = b, .len = len, .pos = 0, .ok = 1 };
    rbyte(&r); uint8_t type = rbyte(&r); rbyte(&r); rbyte(&r);
    ru32(&r);                              /* body length */
    ru32(&r);                              /* serial */
    uint32_t flen = ru32(&r);
    if (!r.ok) return;
    int fend = r.pos + (int)flen;
    if (fend > r.len) return;
    char member[64] = "", iface[96] = "", hpath[64] = "";
    uint32_t reply_serial = 0;
    char body_sig[16] = "";
    while (r.pos < fend) {
        ralign(&r, 8);
        if (!r.ok) return;
        uint8_t code = rbyte(&r);
        const char *sig = rsig(&r);
        if (!r.ok || !sig[0]) break;
        switch (code) {
        case HF_PATH:         snprintf(hpath,  sizeof hpath,  "%s", rstr(&r)); break;
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

    if (type == DBUS_TYPE_METHOD_RETURN && reply_serial == bz_gmo_serial) {
        if (!strcmp(body_sig, "a{oa{sa{sv}}}")) scan_objects(&r);
        if (!r.ok) { bz_down(); return; }
        maybe_changed(&before);
        return;
    }
    if (type != DBUS_TYPE_SIGNAL) return;
    if (!strcmp(member, "InterfacesAdded") && !strcmp(body_sig, "oa{sa{sv}}")) {
        const char *op = rstr(&r);
        if (!r.ok) { bz_down(); return; }
        char p[64]; snprintf(p, sizeof p, "%s", op);
        scan_ifaces(&r, p);
    } else if (!strcmp(member, "InterfacesRemoved") && !strcmp(body_sig, "oas")) {
        scan_removed(&r);
    } else if (!strcmp(member, "PropertiesChanged") &&
               !strncmp(body_sig, "sa{sv}", 6)) {
        const char *pi = rstr(&r);
        if (r.ok && hpath[0]) scan_props(&r, pi, hpath);
    } else {
        return;
    }
    if (!r.ok) { bz_down(); return; }
    maybe_changed(&before);
}

void bz_dispatch(void) {
    /* BlueZ messages are small; a frame that overruns the buffer is a peer we
     * don't understand — drop the connection rather than resync. */
    static uint8_t buf[4096];
    static int have;
    for (;;) {
        ssize_t n = recv(bz_fd, buf + have, sizeof buf - have, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            bz_down(); return;
        }
        if (n == 0) { bz_down(); return; }
        have += (int)n;
        for (;;) {
            if (have < 16) break;
            uint32_t body_len, fields_len;
            memcpy(&body_len, buf + 4, 4);
            memcpy(&fields_len, buf + 12, 4);
            uint32_t hdr = (16 + fields_len + 7u) & ~7u;
            uint64_t total = (uint64_t)hdr + body_len;
            if (total > sizeof buf) { bz_down(); return; }
            if ((uint64_t)have < total) break;
            bz_dispatch_one(buf, (int)total);
            if (bz_fd < 0) return;         /* dispatch dropped the connection */
            memmove(buf, buf + total, have - (int)total);
            have -= (int)total;
        }
        if (have == (int)sizeof buf) { bz_down(); return; }
    }
}

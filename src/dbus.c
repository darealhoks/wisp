/* Minimal D-Bus client implementing org.freedesktop.Notifications.
 *
 * Spec: https://specifications.freedesktop.org/notification-spec/latest/
 *
 * No libdbus / sd-bus dependency — we speak the wire protocol directly.
 * Scope is intentionally tight: just enough to receive Notify /
 * CloseNotification / GetCapabilities / GetServerInformation, post into the
 * OSD widget, and emit NotificationClosed signals.
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
/* Wire connection / SASL auth                                         */
/* ================================================================== */

static int parse_bus_addr(const char *spec, char *out, size_t cap) {
    /* Accept "unix:path=...", "unix:abstract=...", semicolon-separated lists.
       Pick the first unix:path/abstract entry. */
    const char *p = spec;
    while (*p) {
        const char *end = strchr(p, ';'); if (!end) end = p + strlen(p);
        if (!strncmp(p, "unix:", 5)) {
            const char *kv = p + 5;
            const char *eq;
            if ((eq = strstr(kv, "path=")) && eq < end) {
                eq += 5;
                const char *valend = eq;
                while (valend < end && *valend != ',') valend++;
                size_t n = valend - eq;
                if (n >= cap) return -1;
                memcpy(out, eq, n); out[n] = 0; return 0;
            }
            if ((eq = strstr(kv, "abstract=")) && eq < end) {
                eq += 9;
                const char *valend = eq;
                while (valend < end && *valend != ',') valend++;
                size_t n = valend - eq;
                if (n + 1 >= cap) return -1;
                out[0] = '\0';  /* leading nul → abstract namespace */
                memcpy(out + 1, eq, n); out[n + 1] = 0;
                return (int)(n + 1);
            }
        }
        if (!*end) break;
        p = end + 1;
    }
    return -1;
}

static int sasl_auth(int fd) {
    char buf[256]; int n;
    /* Required first byte for AF_UNIX D-Bus. */
    if (send(fd, "\0", 1, MSG_NOSIGNAL) != 1) return -1;

    /* AUTH EXTERNAL <hex uid> */
    uint32_t uid = (uint32_t)geteuid();
    char ascii[16]; snprintf(ascii, sizeof ascii, "%u", uid);
    char hex[64]; int hp = 0;
    for (int i = 0; ascii[i]; i++) {
        static const char H[] = "0123456789abcdef";
        hex[hp++] = H[(ascii[i] >> 4) & 0xf];
        hex[hp++] = H[ascii[i] & 0xf];
    }
    hex[hp] = 0;
    n = snprintf(buf, sizeof buf, "AUTH EXTERNAL %s\r\n", hex);
    if (send(fd, buf, n, MSG_NOSIGNAL) != n) return -1;

    /* Read "OK <guid>\r\n" (or REJECTED). */
    int got = 0;
    for (;;) {
        ssize_t k = recv(fd, buf + got, sizeof buf - 1 - got, 0);
        if (k <= 0) return -1;
        got += k; buf[got] = 0;
        if (memmem(buf, got, "\r\n", 2)) break;
        if (got >= (int)sizeof buf - 1) return -1;
    }
    if (strncmp(buf, "OK ", 3) != 0) return -1;

    n = snprintf(buf, sizeof buf, "BEGIN\r\n");
    if (send(fd, buf, n, MSG_NOSIGNAL) != n) return -1;
    return 0;
}

/* ================================================================== */
/* Marshalling helpers                                                  */
/* ================================================================== */

typedef struct {
    uint8_t *b;
    int      cap;
    int      pos;
} W;

static void wensure(W *w, int n) {
    if (w->pos + n <= w->cap) return;
    int nc = w->cap; while (nc < w->pos + n) nc = nc ? nc * 2 : 256;
    w->b = realloc(w->b, nc);
    w->cap = nc;
}
static void walign(W *w, int a) {
    int p = (w->pos + a - 1) & ~(a - 1);
    wensure(w, p - w->pos);
    while (w->pos < p) w->b[w->pos++] = 0;
}
static void wbyte(W *w, uint8_t v)  { wensure(w, 1); w->b[w->pos++] = v; }
static void wu32 (W *w, uint32_t v) { walign(w, 4); wensure(w, 4); memcpy(w->b + w->pos, &v, 4); w->pos += 4; }
static void wstr (W *w, const char *s) {
    if (!s) s = "";
    uint32_t l = (uint32_t)strlen(s);
    wu32(w, l);
    wensure(w, l + 1);
    memcpy(w->b + w->pos, s, l + 1);
    w->pos += l + 1;
}
static void wsig (W *w, const char *s) {
    uint8_t l = (uint8_t)strlen(s);
    wbyte(w, l);
    wensure(w, l + 1);
    memcpy(w->b + w->pos, s, l + 1);
    w->pos += l + 1;
}

/* Read view (read-only). */
typedef struct {
    const uint8_t *b;
    int            len;
    int            pos;
    int            ok;
} R;

static void ralign(R *r, int a) {
    int p = (r->pos + a - 1) & ~(a - 1);
    if (p > r->len) { r->ok = 0; return; }
    r->pos = p;
}
static uint8_t  rbyte(R *r)  { if (r->pos + 1 > r->len) { r->ok = 0; return 0; } return r->b[r->pos++]; }
static uint16_t ru16 (R *r)  { ralign(r, 2); if (!r->ok || r->pos + 2 > r->len) { r->ok = 0; return 0; }
                                uint16_t v; memcpy(&v, r->b + r->pos, 2); r->pos += 2; return v; }
static uint32_t ru32 (R *r)  { ralign(r, 4); if (!r->ok || r->pos + 4 > r->len) { r->ok = 0; return 0; }
                                uint32_t v; memcpy(&v, r->b + r->pos, 4); r->pos += 4; return v; }
static int32_t  ri32 (R *r)  { return (int32_t)ru32(r); }
static const char *rstr(R *r) {
    uint32_t l = ru32(r);
    /* 64-bit compare: (int)l would go negative for l > INT_MAX and bypass the
     * bound, letting a non-terminated pointer escape to snprintf("%s") below. */
    if (!r->ok || (int64_t)r->pos + (int64_t)l + 1 > (int64_t)r->len) { r->ok = 0; return ""; }
    const char *s = (const char *)(r->b + r->pos);
    /* D-Bus mandates the NUL; a hostile sender can omit it, leaving "%s"/strcmp
     * consumers scanning past the message into the heap. */
    if (s[l] != 0) { r->ok = 0; return ""; }
    r->pos += l + 1;
    return s;
}
static const char *rsig(R *r) {
    uint8_t l = rbyte(r);
    if (!r->ok || r->pos + l + 1 > r->len) { r->ok = 0; return ""; }
    const char *s = (const char *)(r->b + r->pos);
    if (s[l] != 0) { r->ok = 0; return ""; }
    r->pos += l + 1;
    return s;
}

/* Skip a value of the given single-type signature character. Walks containers
 * by recursing on the nested signature. Used to consume header fields and
 * variant payloads we don't care about. Returns 0 on success. */
static int skip_val(R *r, const char **sigp, int depth);

static int skip_single(R *r, char c, const char **sigp, int depth) {
    /* Variants embed a fresh signature, so nesting isn't bounded by the 255-byte
     * outer signature; cap depth like the reference impl to stop stack blowup. */
    if (depth > 64) { r->ok = 0; return -1; }
    switch (c) {
    case 'y': rbyte(r); return r->ok ? 0 : -1;
    case 'b': case 'u': case 'i': ru32(r); return r->ok ? 0 : -1;
    case 'n': case 'q': ru16(r); return r->ok ? 0 : -1;
    case 'x': case 't': case 'd': ralign(r, 8); r->pos += 8; return r->pos <= r->len ? 0 : -1;
    case 'h': ru32(r); return r->ok ? 0 : -1;
    case 's': case 'o': rstr(r); return r->ok ? 0 : -1;
    case 'g': rsig(r); return r->ok ? 0 : -1;
    case 'a': {
        uint32_t len = ru32(r);
        if (!r->ok) return -1;
        char etype = **sigp;
        if (!etype) { r->ok = 0; return -1; }  /* malformed sig "a" with no element type */
        (*sigp)++;
        if (etype == '{' || etype == '(') {
            /* skip nested compound element signature in sig string */
            int depth = 1;
            while (depth && **sigp) {
                if (**sigp == '{' || **sigp == '(') depth++;
                else if (**sigp == '}' || **sigp == ')') depth--;
                (*sigp)++;
            }
        }
        ralign(r, etype == '(' || etype == '{' || etype == 'x' || etype == 't' || etype == 'd' ? 8 : 4);
        int64_t end = (int64_t)r->pos + (int64_t)len;
        if (end > r->len) { r->ok = 0; return -1; }
        r->pos = (int)end;
        return 0;
    }
    case '(': case '{': {
        ralign(r, 8);
        while (**sigp && **sigp != ')' && **sigp != '}') {
            char nc = **sigp; (*sigp)++;
            if (skip_single(r, nc, sigp, depth + 1) < 0) return -1;
        }
        if (**sigp) (*sigp)++;
        return 0;
    }
    case 'v': {
        const char *vs = rsig(r);
        if (!r->ok) return -1;
        return skip_val(r, &vs, depth + 1);
    }
    default: r->ok = 0; return -1;
    }
}
static int skip_val(R *r, const char **sigp, int depth) {
    while (**sigp) {
        char c = **sigp; (*sigp)++;
        if (skip_single(r, c, sigp, depth) < 0) return -1;
    }
    return 0;
}

/* ================================================================== */
/* Message header                                                       */
/* ================================================================== */

#define DBUS_TYPE_METHOD_CALL   1
#define DBUS_TYPE_METHOD_RETURN 2
#define DBUS_TYPE_ERROR         3
#define DBUS_TYPE_SIGNAL        4

#define HF_PATH         1
#define HF_INTERFACE    2
#define HF_MEMBER       3
#define HF_ERROR_NAME   4
#define HF_REPLY_SERIAL 5
#define HF_DESTINATION  6
#define HF_SENDER       7
#define HF_SIGNATURE    8

static void w_header_field_str(W *w, uint8_t code, char sigc, const char *val) {
    walign(w, 8);            /* struct alignment */
    wbyte(w, code);
    char sig[2] = { sigc, 0 };
    wsig(w, sig);
    if (sigc == 'g') {
        wsig(w, val);
    } else {
        wstr(w, val);
    }
}
static void w_header_field_u32(W *w, uint8_t code, uint32_t val) {
    walign(w, 8);
    wbyte(w, code);
    wsig(w, "u");
    wu32(w, val);
}

/* Build and send a complete D-Bus message.
 *   type   : DBUS_TYPE_*
 *   serial : 0 → auto-assign
 *   flags  : 0, or 1 = no-reply expected
 *   For each field, only non-NULL strings (and non-zero reply_serial) emit. */
typedef struct {
    uint8_t     type;
    uint8_t     flags;
    uint32_t    serial;        /* 0 = auto */
    const char *path;
    const char *interface;
    const char *member;
    const char *error_name;
    uint32_t    reply_serial;
    const char *destination;
    const char *signature;     /* if NULL but body_len>0, asserts */
    const uint8_t *body;
    int            body_len;
} Msg;

static uint32_t send_msg(const Msg *m) {
    W h = {0};
    wbyte(&h, 'l');                   /* little-endian */
    wbyte(&h, m->type);
    wbyte(&h, m->flags);
    wbyte(&h, 1);                     /* protocol version */
    wu32(&h, (uint32_t)m->body_len);
    uint32_t ser = m->serial ? m->serial : dbus_serial++;
    if (!m->serial && dbus_serial == 0) dbus_serial = 1;
    wu32(&h, ser);
    /* fields array — length placeholder, then entries */
    int arr_len_pos = h.pos;
    wu32(&h, 0);                      /* placeholder */
    int arr_start = h.pos;
    if (m->path)         w_header_field_str(&h, HF_PATH, 'o', m->path);
    if (m->interface)    w_header_field_str(&h, HF_INTERFACE, 's', m->interface);
    if (m->member)       w_header_field_str(&h, HF_MEMBER, 's', m->member);
    if (m->error_name)   w_header_field_str(&h, HF_ERROR_NAME, 's', m->error_name);
    if (m->reply_serial) w_header_field_u32(&h, HF_REPLY_SERIAL, m->reply_serial);
    if (m->destination)  w_header_field_str(&h, HF_DESTINATION, 's', m->destination);
    if (m->signature && m->signature[0])
        w_header_field_str(&h, HF_SIGNATURE, 'g', m->signature);
    uint32_t alen = (uint32_t)(h.pos - arr_start);
    memcpy(h.b + arr_len_pos, &alen, 4);
    walign(&h, 8);                    /* pad header to body alignment */

    /* concat body */
    if (m->body_len) {
        wensure(&h, m->body_len);
        memcpy(h.b + h.pos, m->body, m->body_len);
        h.pos += m->body_len;
    }
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

static int call_request_name(void) {
    W b = {0};
    wstr(&b, "org.freedesktop.Notifications");
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
        msg("wisp: RequestName returned %u — not the primary owner",
            last_request_name_result);
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

/* Decode the org.freedesktop.Notifications/Notify body (signature susssasa{sv}i)
 * into a fixed-shape struct for codegen-driven ring buffers.
 *   skips:  app_name (s), replaces_id (u), app_icon (s)
 *   reads:  summary (s), body (s)
 *   skips:  actions (as)
 *   reads:  hints a{sv} — picks out urgency (y) and x-url (s); skips the rest.
 *   skips:  expire (i)
 * Returns 0 on success, -1 on truncation or signature mismatch. */
int dbus_signal_decode_notify(const uint8_t *body, int body_len,
                              const char *sig, DbusNotifyFields *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!sig || strncmp(sig, "susss", 5) != 0) return -1;
    R r = { .b = body, .len = body_len, .pos = 0, .ok = 1 };
    rstr(&r);                /* app_name */
    ru32(&r);                /* replaces_id */
    rstr(&r);                /* app_icon */
    const char *sum = rstr(&r);
    const char *bod = rstr(&r);
    if (!r.ok) return -1;
    snprintf(out->summary, sizeof out->summary, "%s", sum);
    snprintf(out->body,    sizeof out->body,    "%s", bod);

    /* actions: as */
    uint32_t alen = ru32(&r);
    if (!r.ok) return -1;
    ralign(&r, 4);
    int64_t aend = (int64_t)r.pos + (int64_t)alen;
    if (aend > r.len) return -1;
    r.pos = (int)aend;

    /* hints: a{sv} */
    uint32_t hlen = ru32(&r);
    if (!r.ok) return -1;
    ralign(&r, 8);
    int64_t hend64 = (int64_t)r.pos + (int64_t)hlen;
    if (hend64 > r.len) return -1;
    int hend = (int)hend64;
    while (r.pos < hend) {
        ralign(&r, 8);
        const char *key = rstr(&r);
        if (!r.ok) break;
        const char *vsig = rsig(&r);
        if (!r.ok) break;
        char vc = vsig[0];
        if (vc == 'y' && !strcmp(key, "urgency")) {
            out->urgent = rbyte(&r);
        } else if (vc == 's' && !strcmp(key, "x-url")) {
            const char *s = rstr(&r);
            if (r.ok) snprintf(out->url, sizeof out->url, "%s", s);
        } else {
            if (skip_val(&r, &vsig, 0) < 0) break;
        }
        if (!r.ok) break;
    }
    r.pos = hend;
    ri32(&r);                /* expire */
    return r.ok ? 0 : -1;
}

void dbus_emit_closed(uint32_t id, uint32_t reason) {
    if (dbus_fd < 0) return;
    W b = {0};
    wu32(&b, id);
    wu32(&b, reason);
    Msg m = { .type = DBUS_TYPE_SIGNAL,
              .flags = 1,
              .path = "/org/freedesktop/Notifications",
              .interface = "org.freedesktop.Notifications",
              .member = "NotificationClosed",
              .signature = "uu",
              .body = b.b, .body_len = b.pos };
    send_msg(&m);
    free(b.b);
}

/* ================================================================== */
/* Inbound: parse a single Notify call's payload                        */
/* ================================================================== */

/* nf-fa codepoints used as fallback when an app passes a stock icon name. */
static uint32_t icon_from_name(const char *name) {
    if (!name || !*name) return 0;
    if (!strcmp(name, "audio-volume-high"))     return 0xf028;
    if (!strcmp(name, "audio-volume-medium"))   return 0xf028;
    if (!strcmp(name, "audio-volume-low"))      return 0xf027;
    if (!strcmp(name, "audio-volume-muted"))    return 0xf026;
    if (!strcmp(name, "microphone-sensitivity-muted")) return 0xf131;
    if (!strcmp(name, "dialog-information"))    return 0xf0eb;
    if (!strcmp(name, "dialog-warning"))        return 0xf071;
    if (!strcmp(name, "dialog-error"))          return 0xf057;
    return 0;
}

/* Pull one hint key + variant value, dispatching on key name.
 * Updates *urgency / *progress / *sync_id / *muted / *icon_cp as side effects. */
static int parse_hint(R *r, int *urgency, int *progress, char *sync_id, int sync_cap,
                      int *muted, uint32_t *icon_cp) {
    const char *key = rstr(r);
    if (!r->ok) return -1;
    const char *sig = rsig(r);
    if (!r->ok) return -1;
    char vc = sig[0];

    if (vc == 'y' && !strcmp(key, "urgency")) { *urgency = rbyte(r); return r->ok ? 0 : -1; }
    if ((vc == 'i' || vc == 'u') && !strcmp(key, "value")) {
        int v = (int)ri32(r); *progress = v; return r->ok ? 0 : -1;
    }
    if (vc == 's' && !strcmp(key, "x-canonical-private-synchronous")) {
        const char *s = rstr(r);
        snprintf(sync_id, sync_cap, "%s", s);
        return r->ok ? 0 : -1;
    }
    if (vc == 's' && !strcmp(key, "category")) {
        const char *s = rstr(r);
        if (!strcmp(s, "muted")) *muted = 1;
        return r->ok ? 0 : -1;
    }
    if (vc == 's' && !strcmp(key, "image-path")) {
        const char *s = rstr(r);
        uint32_t cp = icon_from_name(s);
        if (cp) *icon_cp = cp;
        return r->ok ? 0 : -1;
    }
    /* Unknown / unhandled — skip the variant payload. */
    return skip_val(r, &sig, 0);
}

/* djb2; used to derive a stable replace_id from a synchronous hint string. */
static uint32_t djb2(const char *s) {
    uint32_t h = 5381;
    while (*s) h = ((h << 5) + h) + (uint8_t)*s++;
    return h ? h : 1;
}

static void handle_notify(R *r, uint32_t serial, const char *sender) {
    /* signature: susssasa{sv}i */
    const char *app_name   = rstr(r); (void)app_name;
    uint32_t    replaces   = ru32(r);
    const char *app_icon   = rstr(r);
    const char *summary    = rstr(r);
    const char *body       = rstr(r);
    if (!r->ok) return;

    /* actions array — skip */
    uint32_t alen = ru32(r);
    if (!r->ok) return;
    ralign(r, 4);
    /* alen is attacker-controlled; (int)alen for alen>=0x80000000 is negative
     * and walks r->pos backwards past the bound check. 64-bit math avoids it. */
    int64_t aend = (int64_t)r->pos + (int64_t)alen;
    if (aend > r->len) { r->ok = 0; return; }
    r->pos = (int)aend;

    /* hints: a{sv} */
    int urgency = 1, progress = -1, muted = 0;
    uint32_t icon_cp = icon_from_name(app_icon);
    char sync_id[64] = "";

    uint32_t hlen = ru32(r);
    if (!r->ok) return;
    ralign(r, 8);                          /* dict_entry alignment */
    int64_t hend64 = (int64_t)r->pos + (int64_t)hlen;
    if (hend64 > r->len) { r->ok = 0; return; }
    int hend = (int)hend64;
    while (r->pos < hend) {
        ralign(r, 8);
        if (parse_hint(r, &urgency, &progress, sync_id, sizeof sync_id,
                       &muted, &icon_cp) < 0) {
            r->pos = hend; break;
        }
    }
    r->pos = hend;

    int32_t expire = ri32(r);
    if (!r->ok) return;

    uint32_t rid = replaces;
    if (!rid && sync_id[0]) rid = djb2(sync_id);

    int timeout;
    if (expire < 0)       timeout = -1;    /* server default */
    else if (expire == 0) timeout = 0;     /* spec: 0 = never expire (all urgencies) */
    else                  timeout = expire;

    uint32_t out_id;
#ifdef WISP_HAS_OSD
    if (dnd_on && urgency < 2) {
        out_id = rid;  /* swallow silently; spec allows any non-zero id */
        if (!out_id) out_id = 1;
    } else {
        out_id = osd_post(rid, summary, body, icon_cp, progress,
                          urgency, muted, timeout);
    }
#else
    /* No OSD engine linked; just acknowledge with a stable non-zero id. */
    (void)summary; (void)body; (void)icon_cp; (void)progress;
    (void)urgency; (void)muted; (void)timeout;
    out_id = rid ? rid : 1;
#endif

    /* Reply: u (notification id) */
    W rb = {0};
    wu32(&rb, out_id);
    Msg m = { .type = DBUS_TYPE_METHOD_RETURN,
              .reply_serial = serial,
              .destination = sender,
              .signature = "u",
              .body = rb.b, .body_len = rb.pos };
    send_msg(&m);
    free(rb.b);
}

static void handle_close(R *r, uint32_t serial, const char *sender) {
    uint32_t id = ru32(r);
    if (!r->ok) return;
#ifdef WISP_HAS_OSD
    osd_close(id);
#else
    (void)id;
#endif
    Msg m = { .type = DBUS_TYPE_METHOD_RETURN,
              .reply_serial = serial,
              .destination = sender };
    send_msg(&m);
}

static void handle_get_caps(uint32_t serial, const char *sender) {
    /* reply signature: as. Body: u32 array_bytes + array of strings.
     * Advertise just "body" and "icon-static" to keep it honest. */
    W b = {0};
    int len_pos = b.pos;
    wu32(&b, 0);                         /* placeholder */
    int start = b.pos;
    walign(&b, 4);
    wstr(&b, "body");
    wstr(&b, "icon-static");
    wstr(&b, "persistence");
    uint32_t alen = (uint32_t)(b.pos - start);
    memcpy(b.b + len_pos, &alen, 4);
    Msg m = { .type = DBUS_TYPE_METHOD_RETURN,
              .reply_serial = serial,
              .destination = sender,
              .signature = "as",
              .body = b.b, .body_len = b.pos };
    send_msg(&m);
    free(b.b);
}

static void handle_get_info(uint32_t serial, const char *sender) {
    W b = {0};
    wstr(&b, "wisp");
    wstr(&b, "wisp");
    wstr(&b, "0.1");
    wstr(&b, "1.2");
    Msg m = { .type = DBUS_TYPE_METHOD_RETURN,
              .reply_serial = serial,
              .destination = sender,
              .signature = "ssss",
              .body = b.b, .body_len = b.pos };
    send_msg(&m);
    free(b.b);
}

/* ================================================================== */
/* Dispatch loop                                                        */
/* ================================================================== */

static void dispatch_one(const uint8_t *msg, int msg_len) {
    R r = { .b = msg, .len = msg_len, .pos = 0, .ok = 1 };
    if (msg_len < 16) return;
    /* fixed header */
    rbyte(&r);                            /* endian — always 'l' here */
    uint8_t type = rbyte(&r);
    rbyte(&r);                            /* flags */
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
    (void)body_len; (void)reply_serial;
    (void)dest;

    if (type == DBUS_TYPE_SIGNAL) {
        for (int i = 0; i < dbus_sub_n; i++) {
            if (!strcmp(dbus_subs[i].iface,  iface) &&
                !strcmp(dbus_subs[i].member, member)) {
                dbus_subs[i].cb(msg + r.pos, msg_len - r.pos, body_sig);
            }
        }
        return;
    }
    if (type != DBUS_TYPE_METHOD_CALL) return;
    if (strcmp(iface, "org.freedesktop.Notifications") != 0) return;
    if (!strcmp(member, "Notify"))                  handle_notify(&r, serial, sender);
    else if (!strcmp(member, "CloseNotification"))  handle_close(&r, serial, sender);
    else if (!strcmp(member, "GetCapabilities"))    handle_get_caps(serial, sender);
    else if (!strcmp(member, "GetServerInformation")) handle_get_info(serial, sender);
    /* Unknown member → silently drop (most callers ignore missing reply). */
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
    int abs_len = parse_bus_addr(addr, path, sizeof path);
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
    if (sasl_auth(fd) < 0) {
        msg("wisp: dbus auth failed");
        close(fd); dbus_fd = -1; arm_reconnect(5000); return -1;
    }
    if (call_hello() < 0) {
        msg("wisp: dbus Hello failed");
        close(fd); dbus_fd = -1; arm_reconnect(5000); return -1;
    }
    if (call_request_name() < 0) {
        msg("wisp: dbus RequestName failed (mako still running?)");
        close(fd); dbus_fd = -1; arm_reconnect(5000); return -1;
    }
    int fl = fcntl(fd, F_GETFL); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    for (int i = 0; i < dbus_sub_n; i++)
        call_add_match(dbus_subs[i].iface, dbus_subs[i].member);
    msg("wisp: dbus connected, owning org.freedesktop.Notifications");
    return fd;
}

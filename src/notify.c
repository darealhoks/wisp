/* org.freedesktop.Notifications server, on top of the dbus.c transport.
 *
 * Spec: https://specifications.freedesktop.org/notification-spec/latest/
 *
 * Handles Notify / CloseNotification / GetCapabilities /
 * GetServerInformation, posts into the OSD widget, and emits
 * NotificationClosed signals. */

#define _GNU_SOURCE
#include "wisp.h"
#include "dbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

void notify_method_call(R *r, const char *member, uint32_t serial,
                        const char *sender) {
    if (!strcmp(member, "Notify"))                    handle_notify(r, serial, sender);
    else if (!strcmp(member, "CloseNotification"))    handle_close(r, serial, sender);
    else if (!strcmp(member, "GetCapabilities"))      handle_get_caps(serial, sender);
    else if (!strcmp(member, "GetServerInformation")) handle_get_info(serial, sender);
    /* Unknown member → silently drop (most callers ignore missing reply). */
}

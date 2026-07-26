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

#include "image.h"

#include <ctype.h>
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

/* Notification hints we act on. Anything else is skipped on the wire. */
typedef struct {
    int      urgency;
    int      progress;
    int      muted;
    uint32_t icon_cp;
    char     sync_id[64];
#if OSD_IMAGE_PX > 0
    char      img_path[512];   /* image-path / app_icon, resolved lazily */
    uint32_t *image;           /* from image-data, already scaled; owned */
#endif
} Hints;

#if OSD_IMAGE_PX > 0
/* "file:///a%20b" → "/a b", in place. Album art paths from MPRIS bridges are
 * URI-escaped; the icon lookup needs a real path. */
static void unescape_path(char *p) {
    if (!strncmp(p, "file://", 7)) memmove(p, p + 7, strlen(p + 7) + 1);
    char *o = p;
    for (char *i = p; *i; ) {
        if (i[0] == '%' && isxdigit((unsigned char)i[1]) && isxdigit((unsigned char)i[2])) {
            char h[3] = { i[1], i[2], 0 };
            *o++ = (char)strtol(h, NULL, 16);
            i += 3;
        } else *o++ = *i++;
    }
    *o = 0;
}

/* image-data hint: (iiibiiay) = w, h, rowstride, has_alpha, bps, channels, pixels.
 * Untrusted wire data — every dimension is checked against the byte array
 * before a single pixel is read. Returns a scaled square or NULL. */
static uint32_t *read_image_data(R *r) {
    ralign(r, 8);
    int w = ri32(r), h = ri32(r), stride = ri32(r);
    int alpha = (int)ru32(r);
    int bps = ri32(r), chan = ri32(r);
    uint32_t dlen = ru32(r);
    if (!r->ok) return NULL;
    int64_t end = (int64_t)r->pos + (int64_t)dlen;
    if (end > r->len) { r->ok = 0; return NULL; }
    const uint8_t *data = r->b + r->pos;
    r->pos = (int)end;
    (void)alpha;
    if (bps != 8 || (chan != 3 && chan != 4)) return NULL;
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) return NULL;
    if (stride < w * chan) return NULL;
    if ((int64_t)stride * (h - 1) + (int64_t)w * chan > (int64_t)dlen) return NULL;

    uint8_t *rgba = malloc((size_t)w * h * 4);
    if (!rgba) return NULL;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *sp = data + (size_t)y * stride + (size_t)x * chan;
            uint8_t *dp = rgba + 4 * ((size_t)y * w + x);
            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2];
            dp[3] = chan == 4 ? sp[3] : 255;
        }
    uint32_t *pm = image_scale_square(rgba, w, h, OSD_IMAGE_PX);
    free(rgba);
    return pm;
}

/* Decode whatever art the notification pointed at, image-data first (already
 * done), then image-path / app_icon resolved as a file or an icon name. */
static uint32_t *hints_image(Hints *hn) {
    if (hn->image) return hn->image;
    if (!hn->img_path[0]) return NULL;
    unescape_path(hn->img_path);
    char path[512];
    if (!image_find_icon(hn->img_path, NULL, path, sizeof path)) return NULL;
    if (!image_is_png(path)) return NULL;
    int w, h;
    uint8_t *px = image_load(path, &w, &h);
    if (!px) return NULL;
    uint32_t *pm = image_scale_square(px, w, h, OSD_IMAGE_PX);
    image_free(px);
    return pm;
}
#endif

/* Pull one hint key + variant value, dispatching on key name. */
static int parse_hint(R *r, Hints *hn) {
    const char *key = rstr(r);
    if (!r->ok) return -1;
    const char *sig = rsig(r);
    if (!r->ok) return -1;
    char vc = sig[0];

    if (vc == 'y' && !strcmp(key, "urgency")) { hn->urgency = rbyte(r); return r->ok ? 0 : -1; }
    if ((vc == 'i' || vc == 'u') && !strcmp(key, "value")) {
        hn->progress = (int)ri32(r); return r->ok ? 0 : -1;
    }
    if (vc == 's' && !strcmp(key, "x-canonical-private-synchronous")) {
        const char *s = rstr(r);
        snprintf(hn->sync_id, sizeof hn->sync_id, "%s", s);
        return r->ok ? 0 : -1;
    }
    if (vc == 's' && !strcmp(key, "category")) {
        const char *s = rstr(r);
        if (!strcmp(s, "muted")) hn->muted = 1;
        return r->ok ? 0 : -1;
    }
    if (vc == 's' && (!strcmp(key, "image-path") || !strcmp(key, "image_path"))) {
        const char *s = rstr(r);
        if (!r->ok) return -1;
        uint32_t cp = icon_from_name(s);
        if (cp) hn->icon_cp = cp;
#if OSD_IMAGE_PX > 0
        snprintf(hn->img_path, sizeof hn->img_path, "%s", s);
#endif
        return 0;
    }
#if OSD_IMAGE_PX > 0
    /* Spec order: image-data wins over image-path, and the deprecated
     * icon_data is the same struct under an older name. */
    if (vc == '(' && (!strcmp(key, "image-data") || !strcmp(key, "image_data") ||
                      !strcmp(key, "icon_data"))) {
        uint32_t *pm = read_image_data(r);
        if (pm) { free(hn->image); hn->image = pm; }
        return r->ok ? 0 : -1;
    }
#endif
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
    Hints hn = { .urgency = 1, .progress = -1 };
    hn.icon_cp = icon_from_name(app_icon);
#if OSD_IMAGE_PX > 0
    /* app_icon is the weakest source — an image-path hint overwrites it. */
    snprintf(hn.img_path, sizeof hn.img_path, "%s", app_icon);
#endif

    uint32_t hlen = ru32(r);
    if (!r->ok) return;
    ralign(r, 8);                          /* dict_entry alignment */
    int64_t hend64 = (int64_t)r->pos + (int64_t)hlen;
    if (hend64 > r->len) { r->ok = 0; return; }
    int hend = (int)hend64;
    while (r->pos < hend) {
        ralign(r, 8);
        if (parse_hint(r, &hn) < 0) { r->pos = hend; break; }
    }
    r->pos = hend;

    int32_t expire = ri32(r);
    if (!r->ok) {
#if OSD_IMAGE_PX > 0
        free(hn.image);
#endif
        return;
    }

    uint32_t rid = replaces;
    if (!rid && hn.sync_id[0]) rid = djb2(hn.sync_id);

    uint32_t *image = NULL;
#if OSD_IMAGE_PX > 0
    image = hints_image(&hn);
    if (image != hn.image) free(hn.image);
#endif

    int timeout;
    if (expire < 0)       timeout = -1;    /* server default */
    else if (expire == 0) timeout = 0;     /* spec: 0 = never expire (all urgencies) */
    else                  timeout = expire;

    uint32_t out_id;
#ifdef WISP_HAS_OSD
    if (dnd_on && hn.urgency < 2) {
        out_id = rid;  /* swallow silently; spec allows any non-zero id */
        if (!out_id) out_id = 1;
        free(image);
    } else {
        out_id = osd_post(rid, summary, body, hn.icon_cp, image, hn.progress,
                          hn.urgency, hn.muted, timeout);
    }
#else
    /* No OSD engine linked; just acknowledge with a stable non-zero id. */
    (void)summary; (void)body; (void)hn; (void)timeout;
    free(image);
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
     * "actions" is advertised even though clicks only dismiss: Chromium/
     * Electron (Discord) and Firefox refuse to use a server without it and
     * silently show nothing.
     * ponytail: emit ActionInvoked("default") on click if focus-on-click
     * ever matters. */
    W b = {0};
    int len_pos = b.pos;
    wu32(&b, 0);                         /* placeholder */
    int start = b.pos;
    walign(&b, 4);
    wstr(&b, "actions");
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

/* tray.c — StatusNotifierItem system tray, feeding the DSL `tray()` source.
 *
 * We are both the watcher and the only host: wisp owns
 * org.kde.StatusNotifierWatcher (+ the per-pid Host name apps sniff for),
 * serves RegisterStatusNotifierItem/Host and the watcher's three properties,
 * and pulls each item's state with an async Properties.GetAll. Items push
 * updates via NewIcon/NewTitle/NewStatus; nothing here polls.
 *
 * Right-click opens the item's com.canonical.dbusmenu in the DSL's own menu
 * surface (dbusmenu.c); items without a Menu property
 * fall back to SNI's SecondaryActivate. Left-click sends Activate, and opens
 * that same menu when the item has no Activate to send it to.
 *
 * Losing the Watcher name (another tray host got there first) is not an
 * error: no item ever registers with us and the row stays empty. */

#define _GNU_SOURCE
#include "wisp.h"
#include "dbus.h"
#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SNW_NAME  "org.kde.StatusNotifierWatcher"
#define SNW_PATH  "/StatusNotifierWatcher"
#define SNI_IFACE "org.kde.StatusNotifierItem"

typedef struct {
    char service[64];   /* bus name we send calls to; empty = free slot */
    char owner[64];     /* unique name, matched against NameOwnerChanged */
    char path[96];      /* object path of the item */
    char menu[96];      /* Menu: dbusmenu object path, empty if the item has none */
    char id[64];
    char title[64];
    char status[16];    /* "Passive" / "Active" / "NeedsAttention" */
    char icon_name[64]; /* IconName last attempted — refetches skip re-decode */
    char icon_dir[192]; /* IconThemePath: app-private icon dir, searched first */
    uint32_t icon[TRAY_ICON_PX * TRAY_ICON_PX];
    int has_icon;
    int is_menu;        /* ItemIsMenu: left-click means "open my menu" */
    int has_att_icon;   /* app supplied an AttentionIcon{Name,Pixmap} */
} Item;

static Item items[TRAY_MAX];

extern void wispgen_wisp_state_changed(void) __attribute__((weak));
static void changed(void) {
    if (wispgen_wisp_state_changed) wispgen_wisp_state_changed();
}

/* ================================================================== */
/* DSL-visible accessors                                               */
/* ================================================================== */

/* Slots are kept compacted so `for x in tray.items` can index 0..count-1
 * directly — an item leaving mid-row must not punch a hole in the loop. */
int tray_count(void) {
    int n = 0;
    while (n < TRAY_MAX && items[n].service[0]) n++;
    return n;
}
static Item *at(int i) {
    return (i >= 0 && i < TRAY_MAX && items[i].service[0]) ? &items[i] : NULL;
}
const char *tray_title(int i)  { Item *t = at(i); return t ? t->title  : ""; }
const char *tray_id(int i)     { Item *t = at(i); return t ? t->id     : ""; }
const char *tray_status(int i) { Item *t = at(i); return t ? t->status : ""; }
int tray_has_attention_icon(int i) { Item *t = at(i); return t && t->has_att_icon; }
const uint32_t *tray_icon(int i) {
    Item *t = at(i);
    return (t && t->has_icon) ? t->icon : NULL;
}

/* The seam dbusmenu.c talks to us across: it never sees an Item. */
const char *tray_service(int i)  { Item *t = at(i); return t ? t->service  : ""; }
const char *tray_menu_path(int i){ Item *t = at(i); return t ? t->menu     : ""; }
const char *tray_icon_dir(int i) { Item *t = at(i); return t ? t->icon_dir : ""; }
int tray_slot_of_service(const char *service) {
    for (int i = 0; i < TRAY_MAX; i++)
        if (!strcmp(items[i].service, service)) return i;
    return -1;
}
/* A signal's sender may be the item's unique name or its well-known one. */
int tray_slot_is_sender(int i, const char *sender) {
    Item *t = at(i);
    return t && sender && (!strcmp(t->owner, sender) || !strcmp(t->service, sender));
}

static void drop_slot(int i) {
    if (i < 0 || i >= TRAY_MAX) return;
    for (int k = i; k + 1 < TRAY_MAX; k++) items[k] = items[k + 1];
    memset(&items[TRAY_MAX - 1], 0, sizeof items[0]);
}

/* ================================================================== */
/* Icon decode                                                         */
/* ================================================================== */

/* Box-average `src` down to a `d`-px square and premultiply: blit_argb wants a
 * premultiplied square at logical size, SNI ships loose ARGB32 at whatever
 * size the app chose. Averaging before premultiplying is slightly wrong on
 * hard alpha edges — invisible at 16 px. */
static void icon_scale(uint32_t *dst, int d, const uint8_t *src, int sw, int sh) {
    /* Apps bake wildly different margins into their icons, so scaling the raw
     * bitmap makes one item look half the size of the next. Scale the alpha
     * bounding box instead, squared off around its centre so nothing distorts
     * — every item then fills the same box. */
    int bx0 = sw, by0 = sh, bx1 = -1, by1 = -1;
    for (int j = 0; j < sh; j++)
        for (int i = 0; i < sw; i++)
            if (src[((size_t)j * sw + i) * 4] > 8) {
                if (i < bx0) bx0 = i;
                if (i > bx1) bx1 = i;
                if (j < by0) by0 = j;
                if (j > by1) by1 = j;
            }
    if (bx1 < bx0) { bx0 = by0 = 0; bx1 = sw - 1; by1 = sh - 1; }   /* fully transparent */
    int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1;
    int side = bw > bh ? bw : bh;
    int ox = bx0 + (bw - side) / 2, oy = by0 + (bh - side) / 2;
    for (int y = 0; y < d; y++) {
        int y0 = oy + y * side / d, y1 = oy + (y + 1) * side / d;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < d; x++) {
            int x0 = ox + x * side / d, x1 = ox + (x + 1) * side / d;
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t a = 0, r = 0, g = 0, b = 0, n = 0;
            for (int j = y0 < 0 ? 0 : y0; j < y1 && j < sh; j++) {
                const uint8_t *row = src + (size_t)j * sw * 4;
                for (int i = x0 < 0 ? 0 : x0; i < x1 && i < sw; i++) {
                    const uint8_t *p = row + (size_t)i * 4;
                    a += p[0]; r += p[1]; g += p[2]; b += p[3]; n++;
                }
            }
            uint32_t *o = &dst[y * d + x];
            if (!n) { *o = 0; continue; }
            a /= n; r /= n; g /= n; b /= n;
            *o = (a << 24) | ((r * a / 255) << 16)
                           | ((g * a / 255) << 8) | (b * a / 255);
        }
    }
}

/* Prefer the smallest strike at or above our target, else the largest one. */
static int icon_better(int w, int bw) {
    int ok = w >= TRAY_ICON_PX, bok = bw >= TRAY_ICON_PX;
    if (ok != bok) return ok;
    return ok ? w < bw : w > bw;
}

/* Icon{,Attention,Overlay}Pixmap: a(iiay), w/h/ARGB32-in-network-order.
 * Scales the best strike into `dst` (a `d`-px square); returns 1 if it wrote. */
static int parse_pixmap(R *r, uint32_t *dst, int d) {
    uint32_t alen = ru32(r);
    if (!r->ok) return 0;
    ralign(r, 8);
    int64_t end = (int64_t)r->pos + (int64_t)alen;
    if (end > r->len) { r->ok = 0; return 0; }
    const uint8_t *best = NULL; int bw = 0, bh = 0;
    while (r->pos < end && r->ok) {
        ralign(r, 8);
        int32_t w = ri32(r), h = ri32(r);
        uint32_t blen = ru32(r);
        if (!r->ok) return 0;
        int64_t bend = (int64_t)r->pos + (int64_t)blen;
        if (bend > r->len) { r->ok = 0; return 0; }
        const uint8_t *px = r->b + r->pos;
        r->pos = (int)bend;
        /* 512 px caps the scaling work a hostile (or just KDE-ish) peer can
         * make us do; the size must match the byte count exactly. */
        if (w > 0 && h > 0 && w <= 512 && h <= 512 &&
            (int64_t)w * h * 4 == (int64_t)blen &&
            (!best || icon_better(w, bw))) { best = px; bw = w; bh = h; }
    }
    r->pos = (int)end;
    if (!best) return 0;
    icon_scale(dst, d, best, bw, bh);
    return 1;
}

/* ================================================================== */
/* Item property fetch                                                 */
/* ================================================================== */

/* IconName + IconThemePath → a themed PNG, box-scaled like a pixmap would be.
 * Most GTK/Qt items ship only a name, so without this the row is all text. */
static int load_named_icon(const char *name, const char *dir, uint32_t *dst, int d) {
    char path[512];
    if (!image_find_icon(name, dir, path, sizeof path)) return 0;
    int w = 0, h = 0;
    /* 512, the pixmap path's cap: bounds the swizzle+scale a hostile
     * IconThemePath can buy with a 16384² PNG. */
    uint8_t *px = image_load_max(path, &w, &h, 512);
    if (!px || w <= 0 || h <= 0) { image_free(px); return 0; }
    /* image_load gives RGBA8; icon_scale reads ARGB-in-network-order (a,r,g,b)
     * as SNI ships it, so swing the channels into that order in place. */
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint8_t *p = px + i * 4, t = p[3];
        p[3] = p[2]; p[2] = p[1]; p[1] = p[0]; p[0] = t;
    }
    icon_scale(dst, d, px, w, h);
    image_free(px);
    return 1;
}

/* Scratch, not per-item: an app that declares no attention/overlay icon must
 * not cost 2 more TRAY_ICON_PX² buffers × TRAY_MAX. One decode is live at a
 * time (parse_item_props runs to completion inside one reply callback), so a
 * single shared set of squares carries them from the dict pass to the
 * resolution below. */
static uint32_t px_base[TRAY_ICON_PX * TRAY_ICON_PX];
static uint32_t px_att[TRAY_ICON_PX * TRAY_ICON_PX];
static uint32_t px_ovr[TRAY_OVERLAY_PX * TRAY_OVERLAY_PX];

/* Premultiplied src-over of the badge into the bottom-right of the item's
 * square. Not blit_argb(): that one targets a screen buffer and applies the
 * output scale factor, while both sides here are logical-size squares. */
static void overlay_composite(uint32_t *icon) {
    int off = TRAY_ICON_PX - TRAY_OVERLAY_PX;
    for (int y = 0; y < TRAY_OVERLAY_PX; y++) {
        uint32_t *drow = icon + (size_t)(off + y) * TRAY_ICON_PX + off;
        const uint32_t *srow = px_ovr + (size_t)y * TRAY_OVERLAY_PX;
        for (int x = 0; x < TRAY_OVERLAY_PX; x++) {
            uint32_t sp = srow[x], a = sp >> 24;
            if (!a) continue;
            if (a == 255) { drow[x] = sp; continue; }
            uint32_t inv = 255 - a, dv = drow[x];
            drow[x] = ((dv >> 24)        * inv / 255 + a)                 << 24
                    | ((dv >> 16 & 0xff) * inv / 255 + (sp >> 16 & 0xff)) << 16
                    | ((dv >>  8 & 0xff) * inv / 255 + (sp >>  8 & 0xff)) << 8
                    | ((dv       & 0xff) * inv / 255 + (sp       & 0xff));
        }
    }
}

/* Pick the pixels the item shows now and stamp the overlay onto them.
 * NeedsAttention substituting the whole icon is SNI's own rule, not config
 * policy, so it resolves here: a `.wisp` expressing it would have to repeat
 * the same rule verbatim, and keeping both variants live would cost every
 * item a second square. `it.has_attention_icon` is what the DSL gets — the
 * config decides whether to add its own urgent styling on top. */
static void resolve_icon(Item *it, int got_base, int got_att, int got_ovr,
                         const char *name, const char *att, const char *ovr) {
    int attn = !strcmp(it->status, "NeedsAttention") && (got_att || att[0]);
    const uint32_t *pm = attn ? (got_att ? px_att : NULL)
                              : (got_base ? px_base : NULL);
    const char *nm = attn ? att : name;
    int wrote_base = 0;          /* did this call actually (re)write it->icon? */
    if (pm) {
        memcpy(it->icon, pm, sizeof it->icon);
        it->has_icon = 1;
        it->icon_name[0] = 0;    /* a later name must re-decode over us */
        wrote_base = 1;
    } else if (nm[0] && (!it->has_icon || strcmp(nm, it->icon_name))) {
        /* Record the attempt even if it fails, so a miss isn't re-stat'd on
         * every NewIcon and a stale name can never match a newly-set one. */
        snprintf(it->icon_name, sizeof it->icon_name, "%s", nm);
        if (load_named_icon(nm, it->icon_dir, it->icon, TRAY_ICON_PX))
            it->has_icon = wrote_base = 1;
    }
    if (!got_ovr && ovr[0])
        got_ovr = load_named_icon(ovr, it->icon_dir, px_ovr, TRAY_OVERLAY_PX);
    /* Composite only onto a square this call produced: the badge is burnt in,
     * so re-badging a stale base darkens it further on every NewIcon. */
    if (!got_ovr || !wrote_base) return;
    overlay_composite(it->icon);
    /* The badge is burnt into the base square, so the name dedup above must
     * not skip the next decode — it would composite a second time. */
    it->icon_name[0] = 0;
}

static void parse_item_props(R *r, Item *it) {
    char name[64] = "", att[64] = "", ovr[64] = "";
    int got_base = 0, got_att = 0, got_ovr = 0;
    uint32_t len = ru32(r);
    if (!r->ok) return;
    ralign(r, 8);
    int64_t end = (int64_t)r->pos + (int64_t)len;
    if (end > r->len) { r->ok = 0; return; }
    while (r->pos < end && r->ok) {
        ralign(r, 8);
        const char *key = rstr(r);
        const char *vs  = rsig(r);
        if (!r->ok) return;
        if (!strcmp(key, "Id") && !strcmp(vs, "s"))
            snprintf(it->id, sizeof it->id, "%s", rstr(r));
        else if (!strcmp(key, "Title") && !strcmp(vs, "s"))
            snprintf(it->title, sizeof it->title, "%s", rstr(r));
        else if (!strcmp(key, "Status") && !strcmp(vs, "s"))
            snprintf(it->status, sizeof it->status, "%s", rstr(r));
        else if (!strcmp(key, "IconName") && !strcmp(vs, "s"))
            snprintf(name, sizeof name, "%s", rstr(r));
        else if (!strcmp(key, "AttentionIconName") && !strcmp(vs, "s"))
            snprintf(att, sizeof att, "%s", rstr(r));
        else if (!strcmp(key, "OverlayIconName") && !strcmp(vs, "s"))
            snprintf(ovr, sizeof ovr, "%s", rstr(r));
        else if (!strcmp(key, "Menu") && !strcmp(vs, "o"))
            snprintf(it->menu, sizeof it->menu, "%s", rstr(r));
        else if (!strcmp(key, "ItemIsMenu") && !strcmp(vs, "b"))
            it->is_menu = ru32(r) != 0;
        else if (!strcmp(key, "IconThemePath") && !strcmp(vs, "s"))
            snprintf(it->icon_dir, sizeof it->icon_dir, "%s", rstr(r));
        else if (!strcmp(key, "IconPixmap") && !strcmp(vs, "a(iiay)"))
            got_base = parse_pixmap(r, px_base, TRAY_ICON_PX);
        else if (!strcmp(key, "AttentionIconPixmap") && !strcmp(vs, "a(iiay)"))
            got_att = parse_pixmap(r, px_att, TRAY_ICON_PX);
        else if (!strcmp(key, "OverlayIconPixmap") && !strcmp(vs, "a(iiay)"))
            got_ovr = parse_pixmap(r, px_ovr, TRAY_OVERLAY_PX);
        else { const char *s = vs; skip_val(r, &s, 0); }
    }
    /* GetAll always carries every property, so an item that dropped its
     * attention icon clears the flag on the next refetch. */
    it->has_att_icon = got_att || att[0] != 0;
    resolve_icon(it, got_base, got_att, got_ovr, name, att, ovr);
    /* Apps that only set Id leave Title empty; the row would render blank. */
    /* memcpy, not snprintf: gcc's -Wrestrict can't see that two fields of the
     * same struct don't alias. Both are char[64] and id is NUL-terminated. */
    if (!it->title[0]) memcpy(it->title, it->id, sizeof it->title);
}

/* Replies carry no item identity, so match the slot back by service name —
 * the slot index can have shifted (drop_slot compacts) while in flight. */
static void on_props_reply(const char *sender, R *r, const char *sig,
                           int is_err, void *ud) {
    (void)sender;
    char *service = ud;
    for (int i = 0; i < TRAY_MAX; i++) {
        if (strcmp(items[i].service, service)) continue;
        if (!is_err && sig && !strcmp(sig, "a{sv}")) parse_item_props(r, &items[i]);
        break;
    }
    free(service);
    changed();
}

static void fetch_props(const Item *it) {
    W b = {0};
    wstr(&b, SNI_IFACE);
    Msg m = { .type = DBUS_TYPE_METHOD_CALL,
              .path = it->path,
              .interface = "org.freedesktop.DBus.Properties",
              .member = "GetAll",
              .destination = it->service,
              .signature = "s",
              .body = b.b, .body_len = b.pos };
    char *key = strdup(it->service);
    if (!key || !dbus_call(&m, on_props_reply, key)) free(key);
    free(b.b);
}

/* ================================================================== */
/* Watcher: signals out                                                */
/* ================================================================== */

static void emit_watcher_signal(const char *member, const char *arg) {
    W b = {0};
    if (arg) wstr(&b, arg);
    Msg m = { .type = DBUS_TYPE_SIGNAL,
              .flags = 1,
              .path = SNW_PATH,
              .interface = SNW_NAME,
              .member = member,
              .signature = arg ? "s" : NULL,
              .body = b.b, .body_len = b.pos };
    send_msg(&m);
    free(b.b);
}

/* ================================================================== */
/* Watcher: registration                                               */
/* ================================================================== */

/* The spec lets an item pass either its bus name or its object path; the
 * other half is then implied (sender / the well-known item path). */
static void tray_register_item(const char *arg, const char *sender) {
    char service[64], path[96];
    if (arg[0] == '/') {
        snprintf(service, sizeof service, "%s", sender ? sender : "");
        snprintf(path, sizeof path, "%s", arg);
    } else {
        snprintf(service, sizeof service, "%s", arg);
        snprintf(path, sizeof path, "/StatusNotifierItem");
    }
    if (!service[0]) return;
    for (int i = 0; i < TRAY_MAX; i++)
        if (!strcmp(items[i].service, service) && !strcmp(items[i].path, path))
            return;                       /* re-register of a known item */
    int slot = tray_count();
    /* ponytail: 8 items. Overflow is dropped, not queued — a desktop with a
     * 9th tray icon can raise TRAY_MAX (and its wispc mirror). */
    if (slot >= TRAY_MAX) return;
    Item *it = &items[slot];
    memset(it, 0, sizeof *it);
    snprintf(it->service, sizeof it->service, "%s", service);
    snprintf(it->owner, sizeof it->owner, "%s", sender ? sender : "");
    snprintf(it->path, sizeof it->path, "%s", path);
    snprintf(it->status, sizeof it->status, "Active");
    fetch_props(it);
    emit_watcher_signal("StatusNotifierItemRegistered", service);
    changed();
}

/* ================================================================== */
/* Watcher: properties                                                 */
/* ================================================================== */

/* `as` of every registered item's bus name. */
static void w_items_array(W *b) {
    int lp = b->pos;
    wu32(b, 0);
    int start = b->pos;
    for (int i = 0; i < TRAY_MAX && items[i].service[0]; i++)
        wstr(b, items[i].service);
    uint32_t alen = (uint32_t)(b->pos - start);
    memcpy(b->b + lp, &alen, 4);
}

/* One property as a variant. Returns 0 if the name is unknown. */
static int w_watcher_prop(W *b, const char *name) {
    if (!strcmp(name, "IsStatusNotifierHostRegistered")) { wsig(b, "b"); wu32(b, 1); }
    else if (!strcmp(name, "ProtocolVersion"))           { wsig(b, "i"); wu32(b, 0); }
    else if (!strcmp(name, "RegisteredStatusNotifierItems")) {
        wsig(b, "as"); w_items_array(b);
    } else return 0;
    return 1;
}

static int handle_prop_get(R *r, uint32_t serial, const char *sender) {
    const char *iface = rstr(r);
    const char *prop  = rstr(r);
    if (!r->ok || strcmp(iface, SNW_NAME)) return 0;
    W b = {0};
    if (!w_watcher_prop(&b, prop)) { free(b.b); return 0; }
    Msg m = { .type = DBUS_TYPE_METHOD_RETURN,
              .reply_serial = serial,
              .destination = sender,
              .signature = "v",
              .body = b.b, .body_len = b.pos };
    send_msg(&m);
    free(b.b);
    return 1;
}

static int handle_prop_getall(R *r, uint32_t serial, const char *sender) {
    const char *iface = rstr(r);
    if (!r->ok || strcmp(iface, SNW_NAME)) return 0;
    static const char *NAMES[] = { "IsStatusNotifierHostRegistered",
                                   "ProtocolVersion",
                                   "RegisteredStatusNotifierItems" };
    W b = {0};
    int lp = b.pos;
    wu32(&b, 0);
    walign(&b, 8);                       /* dict_entry alignment */
    int start = b.pos;
    for (int i = 0; i < 3; i++) {
        walign(&b, 8);
        wstr(&b, NAMES[i]);
        w_watcher_prop(&b, NAMES[i]);
    }
    uint32_t alen = (uint32_t)(b.pos - start);
    memcpy(b.b + lp, &alen, 4);
    Msg m = { .type = DBUS_TYPE_METHOD_RETURN,
              .reply_serial = serial,
              .destination = sender,
              .signature = "a{sv}",
              .body = b.b, .body_len = b.pos };
    send_msg(&m);
    free(b.b);
    return 1;
}

/* Qt clients build their proxy from this before calling anything, and block
 * while they wait — so it has to describe the watcher accurately. */
static void handle_introspect(uint32_t serial, const char *sender) {
    static const char XML[] =
        "<node><interface name=\"org.freedesktop.DBus.Introspectable\">"
        "<method name=\"Introspect\"><arg type=\"s\" direction=\"out\"/></method>"
        "</interface>"
        "<interface name=\"org.freedesktop.DBus.Properties\">"
        "<method name=\"Get\"><arg type=\"s\" direction=\"in\"/>"
        "<arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"out\"/></method>"
        "<method name=\"GetAll\"><arg type=\"s\" direction=\"in\"/>"
        "<arg type=\"a{sv}\" direction=\"out\"/></method></interface>"
        "<interface name=\"" SNW_NAME "\">"
        "<method name=\"RegisterStatusNotifierItem\">"
        "<arg name=\"service\" type=\"s\" direction=\"in\"/></method>"
        "<method name=\"RegisterStatusNotifierHost\">"
        "<arg name=\"service\" type=\"s\" direction=\"in\"/></method>"
        "<property name=\"RegisteredStatusNotifierItems\" type=\"as\" access=\"read\"/>"
        "<property name=\"IsStatusNotifierHostRegistered\" type=\"b\" access=\"read\"/>"
        "<property name=\"ProtocolVersion\" type=\"i\" access=\"read\"/>"
        "<signal name=\"StatusNotifierItemRegistered\"><arg type=\"s\"/></signal>"
        "<signal name=\"StatusNotifierItemUnregistered\"><arg type=\"s\"/></signal>"
        "<signal name=\"StatusNotifierHostRegistered\"/>"
        "</interface></node>";
    W b = {0};
    wstr(&b, XML);
    Msg m = { .type = DBUS_TYPE_METHOD_RETURN,
              .reply_serial = serial,
              .destination = sender,
              .signature = "s",
              .body = b.b, .body_len = b.pos };
    send_msg(&m);
    free(b.b);
}

int tray_method_call(R *r, const char *iface, const char *member,
                     const char *path, uint32_t serial, const char *sender) {
    if (!strcmp(iface, "org.freedesktop.DBus.Introspectable")) {
        if (strcmp(member, "Introspect")) return 0;
        handle_introspect(serial, sender);
        return 1;
    }
    if (!strcmp(iface, "org.freedesktop.DBus.Properties")) {
        if (strcmp(path, SNW_PATH)) return 0;    /* not our object */
        if (!strcmp(member, "Get"))         return handle_prop_get(r, serial, sender);
        else if (!strcmp(member, "GetAll")) return handle_prop_getall(r, serial, sender);
        return 0;
    }
    if (!strcmp(member, "RegisterStatusNotifierItem")) {
        const char *arg = rstr(r);
        if (!r->ok) return 0;
        dbus_reply_empty(serial, sender);
        tray_register_item(arg, sender);
        return 1;
    } else if (!strcmp(member, "RegisterStatusNotifierHost")) {
        /* We are the host. Acknowledge so a well-behaved item registers. */
        dbus_reply_empty(serial, sender);
        emit_watcher_signal("StatusNotifierHostRegistered", NULL);
        return 1;
    }
    return 0;
}

/* ================================================================== */
/* Signals in                                                          */
/* ================================================================== */

static void on_name_owner_changed(const char *sender, const char *path,
                                  const uint8_t *body, int body_len, const char *sig) {
    (void)sender; (void)path;
    if (!sig || strcmp(sig, "sss")) return;
    R r = { .b = body, .len = body_len, .pos = 0, .ok = 1 };
    const char *name = rstr(&r);
    rstr(&r);                            /* old owner */
    const char *new_owner = rstr(&r);
    if (!r.ok || new_owner[0]) return;
    for (int i = 0; i < TRAY_MAX; i++) {
        if (!items[i].service[0]) break;
        if (strcmp(items[i].service, name) && strcmp(items[i].owner, name)) continue;
        char gone[sizeof items[0].service];
        memcpy(gone, items[i].service, sizeof gone);   /* survives drop_slot */
        drop_slot(i);
        emit_watcher_signal("StatusNotifierItemUnregistered", gone);
        changed();
        return;
    }
}

/* NewIcon / NewTitle / NewStatus / NewAttentionIcon / NewOverlayIcon carry no
 * payload worth trusting — refetch. */
static void on_item_changed(const char *sender, const char *path,
                            const uint8_t *body, int body_len, const char *sig) {
    (void)body; (void)body_len; (void)sig;
    if (!sender || !path) return;
    for (int i = 0; i < TRAY_MAX && items[i].service[0]; i++) {
        if (strcmp(items[i].owner, sender) && strcmp(items[i].service, sender)) continue;
        if (strcmp(items[i].path, path)) continue;
        fetch_props(&items[i]);
        return;
    }
}

/* ================================================================== */
/* Lifecycle + clicks                                                  */
/* ================================================================== */

void tray_init(void) {
    /* Apps scan for a bus name of this shape to decide a host exists. */
    static char host_name[64];
    snprintf(host_name, sizeof host_name,
             "org.kde.StatusNotifierHost-%d", (int)getpid());
    dbus_own_name(SNW_NAME);
    dbus_own_name(host_name);
    dbus_subscribe("org.freedesktop.DBus", "NameOwnerChanged", on_name_owner_changed);
    dbus_subscribe(SNI_IFACE, "NewIcon",   on_item_changed);
    dbus_subscribe(SNI_IFACE, "NewTitle",  on_item_changed);
    dbus_subscribe(SNI_IFACE, "NewStatus", on_item_changed);
    dbus_subscribe(SNI_IFACE, "NewAttentionIcon", on_item_changed);
    dbus_subscribe(SNI_IFACE, "NewOverlayIcon",   on_item_changed);
    dbusmenu_init();
}

void tray_on_bus_up(void) {
    /* Items are per-connection: after a reconnect every one of them has to
     * register again (they watch the Watcher name the same way we do). */
    memset(items, 0, sizeof items);
    changed();
}

typedef struct { char service[64]; ClickAnchor anchor; } ActivateUD;

/* appindicator items (Steam's included) export no Activate at all — the menu
 * *is* the left-click action. Any error, not just UnknownMethod: dbus_call
 * hands us no error name, and a failed Activate has no better answer. */
static void on_activate_reply(const char *sender, R *r, const char *sig,
                              int is_err, void *ud) {
    (void)sender; (void)r; (void)sig;
    ActivateUD *u = ud;
    if (is_err)
        for (int i = 0; i < TRAY_MAX; i++)
            if (!strcmp(items[i].service, u->service)) {
                click_anchor = u->anchor;      /* the cell that was clicked */
                tray_menu(i);
                break;
            }
    free(u);
}

void tray_click(int i, const char *member) {
    Item *t = at(i);
    if (!t) return;
    /* KDE-style items say so up front; don't bother asking. */
    if (!strcmp(member, "Activate") && t->is_menu && t->menu[0]) { tray_menu(i); return; }
    int want_reply = !strcmp(member, "Activate");
    ActivateUD *u = NULL;
    if (want_reply) {
        u = calloc(1, sizeof *u);
        if (!u) want_reply = 0;
        else { snprintf(u->service, sizeof u->service, "%s", t->service);
               u->anchor = click_anchor; }
    }
    W b = {0};
    wu32(&b, 0); wu32(&b, 0);            /* screen x, y — we have no pointer here */
    Msg m = { .type = DBUS_TYPE_METHOD_CALL,
              .flags = want_reply ? 0 : 1, /* else fire-and-forget; state comes back as New* */
              .path = t->path,
              .interface = SNI_IFACE,
              .member = member,
              .destination = t->service,
              .signature = "ii",
              .body = b.b, .body_len = b.pos };
    if (want_reply) { if (!dbus_call(&m, on_activate_reply, u)) free(u); }
    else send_msg(&m);
    free(b.b);
}

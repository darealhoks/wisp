/* tray.c — StatusNotifierItem system tray, feeding the DSL `tray()` source.
 *
 * We are both the watcher and the only host: wisp owns
 * org.kde.StatusNotifierWatcher (+ the per-pid Host name apps sniff for),
 * serves RegisterStatusNotifierItem/Host and the watcher's three properties,
 * and pulls each item's state with an async Properties.GetAll. Items push
 * updates via NewIcon/NewTitle/NewStatus; nothing here polls.
 *
 * Right-click opens the item's com.canonical.dbusmenu in the DSL's own menu
 * surface (see the dbusmenu section below); items without a Menu property
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
    char icon_name[64]; /* IconName, as last decoded — refetches skip re-decode */
    char icon_dir[192]; /* IconThemePath: app-private icon dir, searched first */
    uint32_t icon[TRAY_ICON_PX * TRAY_ICON_PX];
    int has_icon;
    int is_menu;        /* ItemIsMenu: left-click means "open my menu" */
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
const uint32_t *tray_icon(int i) {
    Item *t = at(i);
    return (t && t->has_icon) ? t->icon : NULL;
}

static void drop_slot(int i) {
    if (i < 0 || i >= TRAY_MAX) return;
    for (int k = i; k + 1 < TRAY_MAX; k++) items[k] = items[k + 1];
    memset(&items[TRAY_MAX - 1], 0, sizeof items[0]);
}

/* ================================================================== */
/* Icon decode                                                         */
/* ================================================================== */

/* Box-average `src` down to TRAY_ICON_PX and premultiply: blit_argb wants a
 * premultiplied square at logical size, SNI ships loose ARGB32 at whatever
 * size the app chose. Averaging before premultiplying is slightly wrong on
 * hard alpha edges — invisible at 16 px. */
static void icon_scale(Item *it, const uint8_t *src, int sw, int sh) {
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
    for (int y = 0; y < TRAY_ICON_PX; y++) {
        int y0 = oy + y * side / TRAY_ICON_PX, y1 = oy + (y + 1) * side / TRAY_ICON_PX;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < TRAY_ICON_PX; x++) {
            int x0 = ox + x * side / TRAY_ICON_PX, x1 = ox + (x + 1) * side / TRAY_ICON_PX;
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t a = 0, r = 0, g = 0, b = 0, n = 0;
            for (int j = y0 < 0 ? 0 : y0; j < y1 && j < sh; j++) {
                const uint8_t *row = src + (size_t)j * sw * 4;
                for (int i = x0 < 0 ? 0 : x0; i < x1 && i < sw; i++) {
                    const uint8_t *p = row + (size_t)i * 4;
                    a += p[0]; r += p[1]; g += p[2]; b += p[3]; n++;
                }
            }
            uint32_t *dst = &it->icon[y * TRAY_ICON_PX + x];
            if (!n) { *dst = 0; continue; }
            a /= n; r /= n; g /= n; b /= n;
            *dst = (a << 24) | ((r * a / 255) << 16)
                             | ((g * a / 255) << 8) | (b * a / 255);
        }
    }
    it->has_icon = 1;
}

/* Prefer the smallest strike at or above our target, else the largest one. */
static int icon_better(int w, int bw) {
    int ok = w >= TRAY_ICON_PX, bok = bw >= TRAY_ICON_PX;
    if (ok != bok) return ok;
    return ok ? w < bw : w > bw;
}

/* IconPixmap: a(iiay), width/height/ARGB32-in-network-order. */
static void parse_pixmap(R *r, Item *it) {
    uint32_t alen = ru32(r);
    if (!r->ok) return;
    ralign(r, 8);
    int64_t end = (int64_t)r->pos + (int64_t)alen;
    if (end > r->len) { r->ok = 0; return; }
    const uint8_t *best = NULL; int bw = 0, bh = 0;
    while (r->pos < end && r->ok) {
        ralign(r, 8);
        int32_t w = ri32(r), h = ri32(r);
        uint32_t blen = ru32(r);
        if (!r->ok) return;
        int64_t bend = (int64_t)r->pos + (int64_t)blen;
        if (bend > r->len) { r->ok = 0; return; }
        const uint8_t *px = r->b + r->pos;
        r->pos = (int)bend;
        /* 512 px caps the scaling work a hostile (or just KDE-ish) peer can
         * make us do; the size must match the byte count exactly. */
        if (w > 0 && h > 0 && w <= 512 && h <= 512 &&
            (int64_t)w * h * 4 == (int64_t)blen &&
            (!best || icon_better(w, bw))) { best = px; bw = w; bh = h; }
    }
    r->pos = (int)end;
    if (best) icon_scale(it, best, bw, bh);
}

/* ================================================================== */
/* Item property fetch                                                 */
/* ================================================================== */

/* IconName + IconThemePath → a themed PNG, box-scaled like a pixmap would be.
 * Most GTK/Qt items ship only a name, so without this the row is all text. */
static void load_named_icon(Item *it, const char *name) {
    char path[512];
    if (!image_find_icon(name, it->icon_dir, path, sizeof path)) return;
    int w = 0, h = 0;
    uint8_t *px = image_load(path, &w, &h);
    if (!px) return;
    /* image_load gives RGBA8; icon_scale reads ARGB-in-network-order (a,r,g,b)
     * as SNI ships it, so swing the channels into that order in place. */
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint8_t *p = px + i * 4, t = p[3];
        p[3] = p[2]; p[2] = p[1]; p[1] = p[0]; p[0] = t;
    }
    icon_scale(it, px, w, h);
    image_free(px);
    snprintf(it->icon_name, sizeof it->icon_name, "%s", name);
}

static void parse_item_props(R *r, Item *it) {
    char name[64] = "";
    int pixmap = 0;
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
        else if (!strcmp(key, "Menu") && !strcmp(vs, "o"))
            snprintf(it->menu, sizeof it->menu, "%s", rstr(r));
        else if (!strcmp(key, "ItemIsMenu") && !strcmp(vs, "b"))
            it->is_menu = ru32(r) != 0;
        else if (!strcmp(key, "IconThemePath") && !strcmp(vs, "s"))
            snprintf(it->icon_dir, sizeof it->icon_dir, "%s", rstr(r));
        else if (!strcmp(key, "IconPixmap") && !strcmp(vs, "a(iiay)")) {
            parse_pixmap(r, it);
            pixmap = it->has_icon;
        }
        else { const char *s = vs; skip_val(r, &s, 0); }
    }
    /* A pixmap always wins; otherwise decode the named icon at most once per
     * distinct name (NewIcon refetches every property, not just the icon). */
    if (!pixmap && name[0] && (!it->has_icon || strcmp(name, it->icon_name)))
        load_named_icon(it, name);
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

/* NewIcon / NewTitle / NewStatus carry no payload worth trusting — refetch. */
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

/* ================================================================== */
/* com.canonical.dbusmenu client                                       */
/* ================================================================== */

/* One level of the item's menu at a time, flattened into the DSL's menu
 * surface — a submenu row reopens the same popup at that id rather than
 * stacking a second one. */

#define DBM_IFACE "com.canonical.dbusmenu"
#define DBM_ROWS  32

typedef struct {
    int32_t id;
    int enabled, visible, sep, submenu;
    char label[ITEM_MAX];
} Row;

static Row  rows[DBM_ROWS];
static int  n_rows;
static char open_service[64], open_path[96];   /* owner of the popup on screen */
static ClickAnchor menu_anchor;                /* cell that asked for it */
/* Toggle bookkeeping: open_item is the slot whose popup is live; closed_item/
 * closed_ms remember a just-dismissed popup so the click that dismissed it
 * (click-off fires before the icon's exec reaches us) doesn't reopen it. */
static int open_item = -1, closed_item = -1;
static uint64_t closed_ms;

static void dbm_open(const char *service, const char *path, int32_t parent);

/* "_Open" is a GTK mnemonic, "__" is a literal underscore. */
static void strip_mnemonic(char *s) {
    char *d = s;
    for (const char *p = s; *p; p++) {
        if (*p == '_' && p[1] == '_') p++;
        else if (*p == '_') continue;
        *d++ = *p;
    }
    *d = 0;
}

static void parse_row_props(R *r, Row *row) {
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
        if (!strcmp(key, "label") && !strcmp(vs, "s")) {
            snprintf(row->label, sizeof row->label, "%s", rstr(r));
            strip_mnemonic(row->label);
        }
        else if (!strcmp(key, "enabled") && !strcmp(vs, "b")) row->enabled = ru32(r) != 0;
        else if (!strcmp(key, "visible") && !strcmp(vs, "b")) row->visible = ru32(r) != 0;
        else if (!strcmp(key, "type") && !strcmp(vs, "s"))
            row->sep = !strcmp(rstr(r), "separator");
        else if (!strcmp(key, "children-display") && !strcmp(vs, "s"))
            row->submenu = !strcmp(rstr(r), "submenu");
        else { const char *s = vs; skip_val(r, &s, 0); }
    }
}

/* (ia{sv}av): id, properties, children as variants of the same struct.
 * `collect` is only set for the root — we ask for depth 1, so the children's
 * own child arrays come back empty. */
static void parse_layout_item(R *r, Row *row, int collect) {
    ralign(r, 8);
    row->id = ri32(r);
    parse_row_props(r, row);
    uint32_t alen = ru32(r);
    if (!r->ok) return;
    ralign(r, 8);
    int64_t end = (int64_t)r->pos + (int64_t)alen;
    if (end > r->len) { r->ok = 0; return; }
    while (r->pos < end && r->ok) {
        const char *sg = rsig(r);
        if (!r->ok) return;
        if (collect && !strcmp(sg, "(ia{sv}av)") && n_rows < DBM_ROWS) {
            Row *ch = &rows[n_rows];
            memset(ch, 0, sizeof *ch);
            ch->visible = ch->enabled = 1;     /* both default true in the spec */
            parse_layout_item(r, ch, 0);
            /* ponytail: separators are dropped, not drawn as a rule — needs a
             * row kind in the menu model the DSL can style. */
            if (r->ok && ch->visible && !ch->sep && ch->label[0]) n_rows++;
        } else { const char *s = sg; skip_val(r, &s, 0); }
    }
    r->pos = (int)end;
}

static void dbm_event(int32_t id) {
    W b = {0};
    wu32(&b, (uint32_t)id);
    wstr(&b, "clicked");
    wsig(&b, "s"); wstr(&b, "");         /* no data for a plain click */
    wu32(&b, 0);                         /* timestamp; apps accept 0 */
    Msg m = { .type = DBUS_TYPE_METHOD_CALL,
              .flags = 1,
              .path = open_path,
              .interface = DBM_IFACE,
              .member = "Event",
              .destination = open_service,
              .signature = "isvu",
              .body = b.b, .body_len = b.pos };
    send_msg(&m);
    free(b.b);
}

static void on_menu_pick(int idx) {
    closed_item = open_item; closed_ms = now_ms(); open_item = -1;
    if (idx < 0 || idx >= n_rows) return;
    if (rows[idx].submenu) { dbm_open(open_service, open_path, rows[idx].id); return; }
    if (rows[idx].enabled) dbm_event(rows[idx].id);
}

static void on_layout(const char *sender, R *r, const char *sig,
                      int is_err, void *ud) {
    (void)sender;
    char *path = ud;
    char *service = path + 96;
    n_rows = 0;
    if (!is_err && sig && !strcmp(sig, "u(ia{sv}av)")) {
        ru32(r);                         /* layout revision */
        Row root = { .visible = 1, .enabled = 1 };
        parse_layout_item(r, &root, 1);
    }
    if (n_rows > 0) {
        static char labels[DBM_ROWS][ITEM_MAX];
        const char *title = "";
        for (int i = 0; i < n_rows; i++)
            memcpy(labels[i], rows[i].label, sizeof labels[i]);
        for (int i = 0; i < TRAY_MAX; i++)
            if (!strcmp(items[i].service, service)) { title = items[i].title; open_item = i; break; }
        /* The popup lands two round trips after the click, well past
         * menu_create's freshness window — restamp the rect that asked. */
        if (menu_anchor.out) { click_anchor = menu_anchor; click_anchor.ms = now_ms(); }
        snprintf(open_path, sizeof open_path, "%s", path);
        snprintf(open_service, sizeof open_service, "%s", service);
        /* `menu tray {}` is a look-only decl: it owns no rows, just this
         * popup's renderer and geometry. Absent → the launcher default. */
        const WispMenu *style = wisp_menu_find("tray");
        if (style) menu_set_geom(&style->geom);
        Widget *mw = menu_create(title, labels, n_rows, -1);
        if (mw) {
            if (style && style->render) mw->s.menu.render = style->render;
            menu_set_pick_hook(on_menu_pick);
        }
    }
    free(path);
}

static void dbm_open(const char *service, const char *path, int32_t parent) {
    /* ud carries path+service through the reply; the slot index can shift. */
    char *ud = calloc(1, 96 + 64);
    if (!ud) return;
    snprintf(ud, 96, "%s", path);
    snprintf(ud + 96, 64, "%s", service);

    /* AboutToShow first: apps (Steam included) fill the menu lazily. Sent
     * no-reply — its answer only says "layout changed", and the bus keeps
     * our two calls in order anyway. */
    W a = {0};
    wu32(&a, (uint32_t)parent);
    Msg s = { .type = DBUS_TYPE_METHOD_CALL, .flags = 1, .path = path,
              .interface = DBM_IFACE, .member = "AboutToShow",
              .destination = service, .signature = "i",
              .body = a.b, .body_len = a.pos };
    send_msg(&s);
    free(a.b);

    static const char *PROPS[] = { "label", "enabled", "visible",
                                   "type", "children-display" };
    W b = {0};
    wu32(&b, (uint32_t)parent);
    wu32(&b, 1);                         /* depth: this level only */
    int lp = b.pos;
    wu32(&b, 0);
    int start = b.pos;
    for (int i = 0; i < 5; i++) wstr(&b, PROPS[i]);
    uint32_t alen = (uint32_t)(b.pos - start);
    memcpy(b.b + lp, &alen, 4);
    Msg m = { .type = DBUS_TYPE_METHOD_CALL, .path = path,
              .interface = DBM_IFACE, .member = "GetLayout",
              .destination = service, .signature = "iias",
              .body = b.b, .body_len = b.pos };
    if (!dbus_call(&m, on_layout, ud)) free(ud);
    free(b.b);
}

void tray_menu(int i) {
    Item *t = at(i);
    if (!t) return;
    if (open_item == i) { menu_cancel_all(); return; }              /* toggle */
    if (closed_item == i && now_ms() - closed_ms < 400) {           /* toggle, click-off ran first */
        closed_item = -1; return;
    }
    if (!t->menu[0]) { tray_click(i, "SecondaryActivate"); return; }
    menu_anchor = click_anchor;
    dbm_open(t->service, t->menu, 0);
}

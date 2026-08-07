/* dbusmenu.c — com.canonical.dbusmenu client for the tray's popup menus.
 *
 * Split out of tray.c: that file owns the SNI item registry (identity, status,
 * icons); this one owns everything from the right-click onward — GetLayout,
 * the row table, the live popup and its refresh. It reaches the item side only
 * through tray_*() accessors, never into the item array. */

#define _GNU_SOURCE
#include "wisp.h"
#include "dbus.h"
#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Repaint hook, the same weak-symbol idiom every source uses. */
extern void wispgen_wisp_state_changed(void) __attribute__((weak));
static void changed(void) {
    if (wispgen_wisp_state_changed) wispgen_wisp_state_changed();
}

/* One level of the item's menu at a time, flattened into the DSL's menu
 * surface — a submenu row reopens the same popup at that id rather than
 * stacking a second one. */

#define DBM_IFACE "com.canonical.dbusmenu"
#define DBM_ROWS  32

/* icon-data is raw PNG from an untrusted app. 8 KB is ~4x the largest real
 * menu icon (a 16-24 px PNG); the shared 32 KB arena bounds what a whole
 * hostile layout can cost us, so RSS stays flat no matter what is claimed. */
#define DBM_ICON_MAX   8192
#define DBM_ICON_ARENA 32768

typedef struct {
    int32_t id;
    int enabled, visible, sep, submenu;
    int toggle;                 /* toggle-type present and non-empty */
    int checked;                /* toggle-state == 1; anything else is not on */
    char label[ITEM_MAX];
    char icon_name[64];         /* icon-theme name, used when icon-data is absent */
    int  icon_off, icon_len;    /* slice of icon_arena, len 0 = none */
} Row;

static Row     rows[DBM_ROWS];
static uint8_t icon_arena[DBM_ICON_ARENA];
static int     icon_used;
/* Decoded squares handed to menu_set_icons (not owned by the widget), freed on
 * the next open — same discipline as apps.c's launcher icons. */
static uint32_t *menu_icons[DBM_ROWS];
static int  n_rows;
static char open_service[64], open_path[96];   /* owner of the popup on screen */
static ClickAnchor menu_anchor;                /* cell that asked for it */
/* Toggle bookkeeping: open_item is the slot whose popup is live; closed_item/
 * closed_ms remember a just-dismissed popup so the click that dismissed it
 * (click-off fires before the icon's exec reaches us) doesn't reopen it. */
static int open_item = -1, closed_item = -1;
static uint64_t closed_ms;
/* Level the popup is showing (submenu rows reopen at their own id) and whether
 * a refresh GetLayout is already on the wire — the coalescing flag. */
static int32_t open_parent;
static int refresh_inflight;

/* path + service ride the async reply; the tray slot index can shift under it.
 * `refresh` tells on_layout to update the live popup instead of creating one. */
typedef struct {
    char path[96];
    char service[64];
    int32_t parent;
    int refresh;
} DbmUD;

static int dbm_open(const char *service, const char *path, int32_t parent, int refresh);

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

/* icon-data: raw PNG bytes in an `ay` (not base64). Copy out — the reply
 * buffer dies with the callback — or drop it if it fails the caps. */
static void parse_icon_data(R *r, Row *row) {
    uint32_t blen = ru32(r);
    if (!r->ok) return;
    int64_t end = (int64_t)r->pos + (int64_t)blen;
    if (end > r->len) { r->ok = 0; return; }
    const uint8_t *px = r->b + r->pos;
    r->pos = (int)end;
    if (blen == 0 || blen > DBM_ICON_MAX) return;
    if ((int)blen > DBM_ICON_ARENA - icon_used) return;
    memcpy(icon_arena + icon_used, px, blen);
    row->icon_off = icon_used;
    row->icon_len = (int)blen;
    icon_used += (int)blen;
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
        /* "checkmark" and "radio" both mean "this row carries a state"; the
         * DSL styles them the same, so we don't keep which. */
        else if (!strcmp(key, "toggle-type") && !strcmp(vs, "s"))
            row->toggle = rstr(r)[0] != 0;
        /* Only 1 is on. 0 is off and -1 (or any other value) is
         * indeterminate — neither may render as checked. */
        else if (!strcmp(key, "toggle-state") && !strcmp(vs, "i"))
            row->checked = ri32(r) == 1;
        else if (!strcmp(key, "icon-name") && !strcmp(vs, "s"))
            snprintf(row->icon_name, sizeof row->icon_name, "%s", rstr(r));
        else if (!strcmp(key, "icon-data") && !strcmp(vs, "ay"))
            parse_icon_data(r, row);
        else { const char *s = vs; skip_val(r, &s, 0); }
    }
    /* Resync to the dict's declared extent: a value that over- or under-reads
     * would otherwise misalign the sibling `av` and every field after it. */
    if (r->ok) r->pos = (int)end;
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
            /* Separators keep their slot (MENU_ROW_SEPARATOR, styled by the
             * .wisp); a labelless non-separator has nothing to draw. */
            if (r->ok && ch->visible && (ch->sep || ch->label[0])) n_rows++;
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

/* `tray_item.menu_open` in the DSL: lets the config keep the clicked cell
 * highlighted for the popup's whole lifetime, not just the press. */
int tray_menu_is_open(int i) {
    return open_item >= 0 && open_item == i;
}

static void on_menu_pick(int idx) {
    /* Arm the reopen-swallow only when the close came from the click-off
     * press — that same click's exec is still in flight and must not reopen
     * the popup. Esc / pick / focus-loss closes have no trailing click; arming
     * on those eats the user's next (legitimate) click on the icon. */
    if (menu_clickoff) { closed_item = open_item; closed_ms = now_ms(); }
    open_item = -1;
    /* An unanswered refresh must not poison the next popup's coalescing. */
    refresh_inflight = 0;
    changed();
    if (idx < 0 || idx >= n_rows) return;
    if (rows[idx].submenu) { dbm_open(open_service, open_path, rows[idx].id, 0); return; }
    if (rows[idx].enabled) dbm_event(rows[idx].id);
}

static void menu_icons_free(void) {
    for (int i = 0; i < DBM_ROWS; i++) { free(menu_icons[i]); menu_icons[i] = NULL; }
}

/* Decode each row's icon to a `px` premultiplied square, in row order.
 * icon-data beats icon-name, mirroring IconPixmap beating IconName on the SNI
 * side. Anything that fails to decode stays NULL — this is a PNG from an
 * arbitrary app, so a miss must cost the row its icon and nothing else.
 * Returns NULL when no row got one, so the renderer drops the icon column
 * entirely instead of indenting every label past an empty gutter. */
#define DBM_ICON_MAX_DIM 512
static uint32_t **menu_icons_load(int px, const char *icon_dir) {
    menu_icons_free();
    if (px <= 0) return NULL;
    int bake = px * image_icon_oversample();
    char path[512];
    for (int i = 0; i < n_rows && i < DBM_ROWS; i++) {
        int w = 0, h = 0;
        uint8_t *rgba = NULL;
        if (rows[i].icon_len)
            rgba = image_decode_png_max(icon_arena + rows[i].icon_off,
                                        rows[i].icon_len, &w, &h, DBM_ICON_MAX_DIM);
        else if (rows[i].icon_name[0] &&
                 image_find_icon(rows[i].icon_name, icon_dir, bake, path, sizeof path))
            rgba = image_load_max(path, &w, &h, DBM_ICON_MAX_DIM);
        if (!rgba) continue;
        if (w > 0 && h > 0) menu_icons[i] = image_scale_square(rgba, w, h, bake);
        image_free(rgba);
    }
    for (int i = 0; i < DBM_ROWS; i++) if (menu_icons[i]) return menu_icons;
    return NULL;
}

/* Lower rows[] into the menu's label + flag tables. Returns the tray slot that
 * owns `service`, or -1 (an item can vanish while its popup is up). */
static char labels[DBM_ROWS][ITEM_MAX];
static unsigned char row_flags[DBM_ROWS];
static int build_rows(const char *service) {
    for (int i = 0; i < n_rows; i++) {
        memcpy(labels[i], rows[i].label, sizeof labels[i]);
        row_flags[i] = (rows[i].enabled ? 0 : MENU_ROW_DISABLED)
                     | (rows[i].sep ? MENU_ROW_SEPARATOR : 0)
                     | (rows[i].toggle ? MENU_ROW_TOGGLE : 0)
                     | (rows[i].toggle && rows[i].checked ? MENU_ROW_CHECKED : 0);
    }
    return tray_slot_of_service(service);
}

/* menu_icon_px() sizes the launcher's rows, not this popup's. */
static int dbm_icon_px(const WispMenu *style) {
    return style && style->geom.row_h ? style->geom.row_h - 12 : menu_icon_px();
}

static void apply_open(const DbmUD *u) {
    const WispMenu *style = wisp_menu_find("tray");
    int slot = build_rows(u->service);
    /* The popup lands two round trips after the click, well past
     * menu_create's freshness window — restamp the rect that asked. */
    if (menu_anchor.out) { click_anchor = menu_anchor; click_anchor.ms = now_ms(); }
    snprintf(open_path, sizeof open_path, "%s", u->path);
    snprintf(open_service, sizeof open_service, "%s", u->service);
    /* `menu tray {}` is a look-only decl: it owns no rows, just this
     * popup's renderer and geometry. Absent → the launcher default. */
    if (style) menu_set_geom(&style->geom);
    /* menu_create cancels any live menu, which fires the old pick hook and
     * clears open_item — so claim it only after the new popup exists. */
    Widget *mw = menu_create(slot >= 0 ? tray_title(slot) : "", labels, n_rows, -1);
    if (!mw) return;
    if (style && style->render) mw->s.menu.render = style->render;
    menu_set_row_flags(mw, row_flags);
    /* After menu_create: it cancels the previous popup, which drops that
     * widget's pointer to the table we are about to free. */
    int px = dbm_icon_px(style);
    menu_set_icons(mw, menu_icons_load(px, slot >= 0 ? tray_icon_dir(slot) : NULL), px);
    menu_set_pick_hook(on_menu_pick);
    open_item = slot;
    open_parent = u->parent;
    changed();                             /* menu_open flipped for this item */
}

/* A refresh reply lands an unbounded time after the signal that asked for it:
 * the user may have picked a row, hit Esc or clicked off, the app may have
 * died, or another item's popup may have replaced ours. Touch the widget only
 * if the popup on screen is still the exact menu we refetched. */
/* `got_layout` separates "the app answered with an empty menu" (close the
 * popup) from "the call failed or came back malformed" (leave it alone) — a
 * busy app or a method timeout must not look like the menu closing itself. */
static void apply_refresh(const DbmUD *u, int got_layout) {
    if (!got_layout) return;
    if (open_item < 0 || u->parent != open_parent ||
        strcmp(open_path, u->path) || strcmp(open_service, u->service)) return;
    Widget *mw = menu_live();
    if (!mw) return;
    if (n_rows <= 0) { menu_cancel_all(); return; }   /* app emptied the menu */
    int slot = build_rows(u->service);
    int px = dbm_icon_px(wisp_menu_find("tray"));
    /* Free-then-swap with no render in between (we are single-threaded and
     * menu_icons_load never renders), so the widget can't draw through the
     * squares being replaced. */
    uint32_t **ic = menu_icons_load(px, slot >= 0 ? tray_icon_dir(slot) : NULL);
    menu_update_items(mw, labels, n_rows, row_flags, ic, px);
}

static void on_layout(const char *sender, R *r, const char *sig,
                      int is_err, void *ud) {
    (void)sender;
    DbmUD *u = ud;
    n_rows = 0;
    icon_used = 0;
    if (u->refresh) refresh_inflight = 0;
    int got_layout = 0;
    if (!is_err && sig && !strcmp(sig, "u(ia{sv}av)")) {
        ru32(r);                         /* layout revision */
        Row root = { .visible = 1, .enabled = 1 };
        parse_layout_item(r, &root, 1);
        /* A truncated reply leaves rows[] half-filled — not a real layout. */
        got_layout = r->ok;
    }
    if (u->refresh) apply_refresh(u, got_layout);
    else if (n_rows > 0) apply_open(u);
    free(u);
}

/* Returns 1 if the GetLayout went out. */
static int dbm_open(const char *service, const char *path, int32_t parent, int refresh) {
    DbmUD *u = calloc(1, sizeof *u);
    if (!u) return 0;
    snprintf(u->path, sizeof u->path, "%s", path);
    snprintf(u->service, sizeof u->service, "%s", service);
    u->parent = parent;
    u->refresh = refresh;

    /* AboutToShow first: apps (Steam included) fill the menu lazily. Sent
     * no-reply — its answer only says "layout changed", and the bus keeps
     * our two calls in order anyway. Skipped on a refresh: it is a "the user
     * is opening this" hint, and apps answer it by emitting LayoutUpdated —
     * which would make every refresh ask for the next one. */
    if (!refresh) {
        W a = {0};
        wu32(&a, (uint32_t)parent);
        Msg s = { .type = DBUS_TYPE_METHOD_CALL, .flags = 1, .path = path,
                  .interface = DBM_IFACE, .member = "AboutToShow",
                  .destination = service, .signature = "i",
                  .body = a.b, .body_len = a.pos };
        send_msg(&s);
        free(a.b);
    }

    static const char *PROPS[] = { "label", "enabled", "visible",
                                   "type", "children-display",
                                   "toggle-type", "toggle-state",
                                   "icon-name", "icon-data" };
    W b = {0};
    wu32(&b, (uint32_t)parent);
    wu32(&b, 1);                         /* depth: this level only */
    int lp = b.pos;
    wu32(&b, 0);
    int start = b.pos;
    for (size_t i = 0; i < sizeof PROPS / sizeof PROPS[0]; i++) wstr(&b, PROPS[i]);
    uint32_t alen = (uint32_t)(b.pos - start);
    memcpy(b.b + lp, &alen, 4);
    Msg m = { .type = DBUS_TYPE_METHOD_CALL, .path = path,
              .interface = DBM_IFACE, .member = "GetLayout",
              .destination = service, .signature = "iias",
              .body = b.b, .body_len = b.pos };
    int sent = dbus_call(&m, on_layout, u) != 0;
    if (!sent) free(u);
    free(b.b);
    return sent;
}

/* LayoutUpdated(u,i) / ItemsPropertiesUpdated(a(ia{sv}),a(ias)): the app
 * changed its menu while it is on screen. The AddMatch rule carries neither
 * sender nor path, so these arrive from every menu-exporting app on the bus —
 * everything but the open popup's own owner+path is dropped without a round
 * trip or a repaint.
 * ItemsPropertiesUpdated is handled as a bare "something changed" nudge: its
 * payload duplicates what a refetch returns, so re-running GetLayout costs one
 * round trip instead of a second nested-variant parser on untrusted input. */
static void on_menu_changed(const char *sender, const char *path,
                            const uint8_t *body, int body_len, const char *sig) {
    (void)body; (void)body_len; (void)sig;
    /* Coalesce: a signal arriving while our GetLayout is on the wire is
     * already answered by that reply (the app computes it after the signal),
     * so spam costs one fetch, not one per signal. */
    if (open_item < 0 || refresh_inflight) return;
    if (!sender || !path || strcmp(path, open_path)) return;
    if (strcmp(open_service, sender) && !tray_slot_is_sender(open_item, sender)) return;
    refresh_inflight = dbm_open(open_service, open_path, open_parent, 1);
}

void tray_menu(int i) {
    const char *service = tray_service(i);
    if (!service[0]) return;
    if (open_item == i) { menu_cancel_all(); return; }              /* toggle */
    if (closed_item == i && now_ms() - closed_ms < 400) {           /* toggle, click-off ran first */
        closed_item = -1; return;
    }
    const char *path = tray_menu_path(i);
    if (!path[0]) { tray_click(i, "SecondaryActivate"); return; }
    menu_anchor = click_anchor;
    dbm_open(service, path, 0, 0);
}

void dbusmenu_init(void) {
    dbus_subscribe(DBM_IFACE, "LayoutUpdated", on_menu_changed);
    dbus_subscribe(DBM_IFACE, "ItemsPropertiesUpdated", on_menu_changed);
}

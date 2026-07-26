/* Deep libFuzzer+ASan harness: full D-Bus messages end to end, exercising the
 * code that USES parsed values — the notification + tray attack surface any
 * session-bus peer reaches with attacker-chosen summary/body/hints/pixmaps.
 *
 * Where fuzz_dbus.c stops at the side-effect-free readers, this drives
 * dispatch_one() (dbus.c's top-level router) and, because the reply-side
 * parsers (tray Properties.GetAll, IconPixmap, dbusmenu GetLayout) are only
 * reachable behind a matching pending serial, the static parse entry points
 * directly. It compiles the real dbus.c/notify.c/tray.c into one TU (so every
 * static — the hints loop, pixmap/props/layout parsing, the member[64]/
 * iface[96]/body_sig[16]/id[64]/title[64]/label[] snprintf clamps — runs on
 * fuzz input) and stubs only the leaf side effects: the socket, the OSD/menu
 * render surfaces, and the icon-theme disk decode. None of the parse or bounds
 * logic under test depends on those leaves.
 *
 *   make fuzz-dispatch
 *   ./build/fuzz/fuzz_dispatch -max_len=1024 fuzz/corpus fuzz/seeds
 *
 * NOT exercised (out of scope, deliberately): osd_post()'s OSD_BODY_MAX clamp
 * and load_named_icon()'s channel swizzle live in osd.c / on real PNG files —
 * behind the parse, and pulling the render/image stack in would drag the whole
 * pool/widget machinery into the harness. The SNI *pixmap* path (icon_scale on
 * attacker bytes) IS driven, via mode 3.
 *
 * Input layout: byte 0 selects the target; the rest is the payload.
 *   0 dispatch_one(payload)              full message router (seeded)
 *   1 dbus_signal_decode_notify(sig,body) [siglen:1][sig][body]
 *   2 parse_item_props(payload as a{sv})
 *   3 parse_pixmap(payload as a(iiay))
 *   4 parse_layout_item(payload as (ia{sv}av))  dbusmenu recursion
 *   5 parse_row_props(payload as a{sv})         label clamp + strip_mnemonic
 */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "wisp.h"
#include "dbus.h"
#include "image.h"

/* ---- globals the compiled TUs expect from the runtime ---- */
ClickAnchor click_anchor;
int         menu_clickoff;
int         dnd_on;

/* ---- leaf stubs: socket / render / disk. Confined to this TU. ---- */
void msg(const char *fmt, ...) { (void)fmt; }
void epoll_add_fd(int fd) { (void)fd; }
int64_t now_ms(void) { return 0; }

/* OSD render surface — notify.c posts here; we only care that the parse ran. */
uint32_t osd_post(uint32_t replace_id, const char *summary, const char *body,
                  uint32_t icon_cp, uint32_t *image, int progress, int urgency,
                  int muted, int timeout) {
    (void)summary; (void)body; (void)icon_cp; (void)progress;
    (void)urgency; (void)muted; (void)timeout;
    free(image);   /* the real osd_post owns it; leaking would trip ASan */
    return replace_id ? replace_id : 1;
}
void osd_close(uint32_t id) { (void)id; }

/* Menu surface — tray.c's dbusmenu popup. */
Widget *menu_create(const char *title, char items[][ITEM_MAX], int n, int sel) {
    (void)title; (void)items; (void)n; (void)sel; return NULL;
}
void menu_set_pick_hook(void (*fn)(int)) { (void)fn; }
void menu_set_geom(const WispMenuGeom *g) { (void)g; }
void menu_cancel_all(void) {}
const WispMenu *wisp_menu_find(const char *name) { (void)name; return NULL; }

/* image.c is linked in whole rather than stubbed: notification cover art
 * feeds image_scale_square attacker-chosen dimensions, and the icon lookup
 * only ever stats paths a fuzzer will not guess. */

/* ---- the code under test, real sources, one TU ---- */
#include "dbus_wire.c"
#include "dbus.c"
#include "notify.c"
#include "tray.c"

/* Reset the file-static state the compiled TUs accumulate across persistent
 * runs, so one input can't smuggle state into the next. */
static void reset_state(void) {
    memset(items, 0, sizeof items);
    memset(pending, 0, sizeof pending);
    n_rows = 0;
    open_item = closed_item = -1;
    open_service[0] = open_path[0] = 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) return 0;
    reset_state();
    dbus_fd = -1;                 /* send_msg() no-ops onto a dead fd */

    uint8_t mode = data[0] % 6;
    const uint8_t *p = data + 1;
    int plen = (int)(size - 1);

    switch (mode) {
    case 0:
        dispatch_one(p, plen);
        break;
    case 1: {
        size_t siglen = p[0] % 16;
        if (1 + siglen > (size_t)plen) return 0;
        char sig[16];
        memcpy(sig, p + 1, siglen); sig[siglen] = 0;
        DbusNotifyFields out;
        dbus_signal_decode_notify(p + 1 + siglen, plen - 1 - (int)siglen, sig, &out);
        break;
    }
    case 2: {
        Item it; memset(&it, 0, sizeof it);
        R r = { .b = p, .len = plen, .pos = 0, .ok = 1 };
        parse_item_props(&r, &it);
        break;
    }
    case 3: {
        Item it; memset(&it, 0, sizeof it);
        R r = { .b = p, .len = plen, .pos = 0, .ok = 1 };
        parse_pixmap(&r, &it);
        break;
    }
    case 4: {
        R r = { .b = p, .len = plen, .pos = 0, .ok = 1 };
        Row root = { .visible = 1, .enabled = 1 };
        parse_layout_item(&r, &root, 1);
        break;
    }
    case 5: {
        Row row; memset(&row, 0, sizeof row);
        R r = { .b = p, .len = plen, .pos = 0, .ok = 1 };
        parse_row_props(&r, &row);
        break;
    }
    }
    return 0;
}

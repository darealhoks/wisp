/* Deep libFuzzer+ASan harness for the Wayland wire parser — the
 * compositor-facing attack surface. Where fuzz_dbus/fuzz_dispatch cover the
 * session bus, this drives the bytes a (malicious or buggy) compositor and its
 * IPC peers push at us: the wl_display event router and the workspace / river /
 * foreign-toplevel event handlers, plus mango's and Hyprland's text IPC.
 *
 * It compiles the real wire-facing sources into ONE TU so every bounds check,
 * wire-string reader (u32 len + pad4 + NUL), array walk (i+4 <= alen) and tag
 * clamp runs on fuzz input:
 *     wl.c workspace.c river.c wl_toplevel.c mango.c hyprland.c
 * and stubs only the leaf side effects — the render/widget/gamma entry points,
 * the seat input handlers, and epoll registration. NONE of the stubbed leaves
 * touch untrusted wire bytes, so the stubbing can't mask a parser bug.
 *
 * The real wl.c handle()/wl_send stay live: outbound requests (registry binds,
 * xdg pong, toplevel destroy) go to a socketpair that is drained each run, so
 * wl_req/wl_req_str's own length clamps are exercised too.
 *
 *   make fuzz-wl
 *   ./build/fuzz/fuzz_wl -max_len=2048 fuzz/corpus fuzz/seeds
 *
 * Input layout: byte 0 selects the target; the rest is the payload.
 *   0 wl frame stream → handle()  — carved into [obj:4][op:2][blen:2][body]
 *       frames, one call to the real router each; builds object state across
 *       frames so the id-routed extws/river/toplevel handlers are reachable.
 *   1 mango_dispatch()   line reassembly + mg_parse_line (JSON scan, name copy)
 *   2 hyprland_dispatch() line reassembly + event split
 *   3 hl_parse_monitors()+hl_parse_workspaces() the Hyprland JSON array walkers
 *
 * Out of scope (deliberately): on_pointer_event/on_keyboard_event and xkb.c's
 * keymap parser live in wisp.c/xkb.c behind the render+timerfd stack; stubbed
 * here, worth their own harness. hl_request()'s real socket returns NULL under
 * fuzz, so mode 1/2 reseeds no-op — mode 3 drives those JSON parsers directly.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "wisp.h"
#include "proto.h"

/* ---- globals the compiled TUs expect from the runtime ---- */
Widget widgets[MAX_WIDGETS];

/* toplevel match table (normally codegen output): one entry so app_id matching
 * has something to compare against. */
const char *const tl_match_app_ids[] = { "foo", "bar" };
const int         tl_n_matches = 2;

/* ---- leaf stubs: render / widget / gamma / seat / epoll. Confined here. ---- */
void on_pointer_event(uint16_t op, uint8_t *b, uint32_t n)  { (void)op; (void)b; (void)n; }
void on_keyboard_event(uint16_t op, uint8_t *b, uint32_t n) { (void)op; (void)b; (void)n; }
void on_ls_event(Widget *w, uint16_t op, uint8_t *b, uint32_t n) { (void)w; (void)op; (void)b; (void)n; }
void on_frame_done(Widget *w, uint32_t cb) { (void)w; (void)cb; }
void on_buffer_release(uint32_t buf) { (void)buf; }
Widget *widget_by_ls(uint32_t id) { (void)id; return NULL; }
void widget_destroy(Widget *w) { (void)w; }
void widget_rescale_output(Output *o) { (void)o; }
void cutout_drop_output(Output *o) { (void)o; }
void bar_set_tags_on(Output *o, uint32_t m, uint32_t a, uint32_t u) { (void)o; (void)m; (void)a; (void)u; }
void epoll_add_fd(int fd) { (void)fd; }
void epoll_del_fd(int fd) { (void)fd; }
void output_init_widgets(Output *o) { (void)o; }   /* codegen'd in the real build */
#ifdef WISP_HAS_GAMMA
void gamma_bind_output(Output *o) { (void)o; }
void gamma_on_size(Output *o, uint32_t sz) { (void)o; (void)sz; }
void gamma_on_failed(Output *o) { (void)o; }
#endif

/* ---- the code under test, real sources, one TU ---- */
#include "wl.c"
#include "workspace.c"
#include "river.c"
#include "wl_toplevel.c"
#include "mango.c"
#include "hyprland.c"

/* Pre-armed object ids so mode 0 can address the id-routed handlers directly. */
#define OID_EXTWS_MGR   0xE0000001u
#define OID_RIVER_MGR   0xE0000002u
#define OID_TL_MGR      0xE0000003u
#define OID_SEAT        0xE0000004u
#define OID_WM_BASE     0xE0000005u
#define OID_RIVER_OUT   0xE1000000u
#define OID_WL_OUTPUT   0xE0000010u

static int sp[2] = { -1, -1 };   /* sp[0] = wl_fd (outbound sink), sp[1] drained */

/* Reset the file-static + global state the compiled TUs accumulate, so one
 * input can't smuggle state into the next persistent run. */
static void reset_state(void) {
    /* wl.c wire buffer + object table */
    wl_rlen = 0;
    memset(wl_rbuf, 0, sizeof wl_rbuf);
    memset(outputs, 0, sizeof outputs);
    memset(widgets, 0, sizeof widgets);
    focused_output = NULL;
    id_compositor = id_shm = id_pointer = id_keyboard = 0;
    id_gamma_mgr = id_slock_mgr = id_slock = 0;
    id_layer_shell = id_wm_base = 0;
    wl_next_id = 0xF0000000u;   /* binds allocate here, clear of our fixed ids */

    id_registry          = ID_REGISTRY;
    id_seat              = OID_SEAT;
    id_wm_base           = OID_WM_BASE;
    id_extws_mgr         = OID_EXTWS_MGR;
    id_river_status_mgr  = OID_RIVER_MGR;
    id_river_control     = 0;
    id_toplevel_mgr      = OID_TL_MGR;
    tl_mgr_name          = 0;

    /* one addressable output so output-name/mode and river publish have a home */
    outputs[0].active = 1;
    outputs[0].wl_output = OID_WL_OUTPUT;
    outputs[0].scale120 = 120;
    snprintf(outputs[0].name, sizeof outputs[0].name, "eDP-1");

    /* workspace.c / river.c / wl_toplevel.c tables */
    memset(wss, 0, sizeof wss);
    memset(groups, 0, sizeof groups);
    for (int i = 0; i < MAX_WS; i++) wss[i].coord = -1;
    memset(rvs, 0, sizeof rvs);
    rvs[0].status = OID_RIVER_OUT;
    rvs[0].out = &outputs[0];
    memset(slots, 0, sizeof slots);
    memset(cur_count, 0, sizeof cur_count);
    memset(pub_count, 0, sizeof pub_count);
    memset(cur_title, 0, sizeof cur_title);
    memset(pub_title, 0, sizeof pub_title);

    /* mango.c / hyprland.c line buffers */
    mg_len = 0;
    hl_len = 0;
    memset(hmons, 0, sizeof hmons);
}

/* A blocking socketpair swallows every outbound request; drained each run so it
 * never fills. wl_fd blocking => wl_send never hits the EAGAIN/poll path. */
static void ensure_sink(void) {
    if (sp[0] >= 0) return;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0) { sp[0] = -1; return; }
    fcntl(sp[1], F_SETFL, O_NONBLOCK);
}
static void drain_sink(void) {
    if (sp[1] < 0) return;
    char b[4096];
    while (read(sp[1], b, sizeof b) > 0) ;
}

/* Feed a body into a fresh nonblocking socketpair end and hand it to a
 * dispatch() that reads its module fd, so line reassembly runs on fuzz bytes. */
static int feed_fd(const uint8_t *p, size_t len) {
    int s[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, s) < 0) return -1;
    fcntl(s[0], F_SETFL, O_NONBLOCK);
    (void)!write(s[1], p, len);
    close(s[1]);   /* EOF after the bytes: dispatch reads all, then sees close */
    return s[0];
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    ensure_sink();
    reset_state();
    wl_fd = sp[0];

    uint8_t mode = data[0] % 4;
    const uint8_t *p = data + 1;
    size_t len = size - 1;

    switch (mode) {
    case 0: {
        /* carve [obj:4][op:2][blen:2][body:blen] frames; skip wl_display so the
         * fatal error/bad-size paths (real die()) don't kill the fuzzer. */
        size_t i = 0;
        while (i + 8 <= len) {
            uint32_t obj  = (uint32_t)p[i] | (uint32_t)p[i+1] << 8 |
                            (uint32_t)p[i+2] << 16 | (uint32_t)p[i+3] << 24;
            uint16_t op   = (uint16_t)(p[i+4] | p[i+5] << 8);
            uint16_t blen = (uint16_t)(p[i+6] | p[i+7] << 8);
            i += 8;
            if (blen > len - i) blen = (uint16_t)(len - i);
            if (obj != ID_DISPLAY)
                handle(obj, op, (uint8_t *)(p + i), blen);
            i += blen;
        }
        break;
    }
    case 1: {
        int fd = feed_fd(p, len);
        if (fd >= 0) { mango_fd = fd; mango_dispatch(); if (mango_fd >= 0) close(mango_fd); mango_fd = -1; }
        break;
    }
    case 2: {
        int fd = feed_fd(p, len);
        if (fd >= 0) { hyprland_fd = fd; hyprland_dispatch(); if (hyprland_fd >= 0) close(hyprland_fd); hyprland_fd = -1; }
        break;
    }
    case 3: {
        /* hl_request() always NUL-terminates its reply (buf[len]=0) and the
         * parsers rely on it (atoi/atoll are unbounded); honor that contract. */
        char *b = malloc(len + 1);
        if (b) {
            memcpy(b, p, len);
            b[len] = 0;
            const char *e = b + len;
            hl_parse_monitors(b, e);
            hl_parse_workspaces(b, e);
            free(b);
        }
        break;
    }
    }

    drain_sink();
    return 0;
}

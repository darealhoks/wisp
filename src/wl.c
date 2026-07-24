/* Wayland wire I/O. */
#include "wisp.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>

int      wl_fd = -1;
uint32_t wl_next_id = 2;
/* Fixed only on a fresh connect; a reload-adopted session allocates a new
 * registry above the old process's ids (the old registry@2 is still bound). */
uint32_t id_registry = ID_REGISTRY;
uint8_t  wl_rbuf[8192];
int      wl_rlen = 0;

static int  pending_fds[16];
static int  n_pending_fds = 0;

uint32_t id_compositor, id_shm, id_seat;
uint32_t compositor_ver;
uint32_t id_layer_shell, id_wm_base;
uint32_t layer_shell_ver;
uint32_t id_pointer, id_keyboard;
uint32_t id_gamma_mgr;
uint32_t id_slock_mgr, id_slock;
uint32_t id_extws_mgr;
uint32_t id_river_status_mgr, id_river_control;
#ifdef WISP_FRACTIONAL
uint32_t id_viewporter, id_frac_mgr;
#endif

Output  outputs[MAX_OUTPUTS];
Output *focused_output;

uint32_t ptr_focus, kbd_focus;
int      ptr_x, ptr_y;
uint32_t enter_serial;

static int  globals_synced = 0;
static uint32_t sync_id = 0;
static uint32_t rt_id = 0;
static int  rt_done = 0;

/* ============================================================ */
/* Output registry                                               */
/* ============================================================ */

int output_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_OUTPUTS; i++) if (outputs[i].active) n++;
    return n;
}

Output *output_alloc(uint32_t registry_name) {
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        if (!outputs[i].active) {
            memset(&outputs[i], 0, sizeof outputs[i]);
            outputs[i].active = 1;
            outputs[i].registry_name = registry_name;
            outputs[i].last_applied_k = 0;
            outputs[i].scale120 = 120;
            return &outputs[i];
        }
    }
    return NULL;
}

Output *output_by_wl(uint32_t wl_output) {
    if (!wl_output) return NULL;
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (outputs[i].active && outputs[i].wl_output == wl_output)
            return &outputs[i];
    return NULL;
}

Output *output_by_name(const char *name) {
    if (!name || !*name) return NULL;
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (outputs[i].active && !strcmp(outputs[i].name, name))
            return &outputs[i];
    return NULL;
}

Output *output_by_gamma(uint32_t gamma_ctrl) {
    if (!gamma_ctrl) return NULL;
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (outputs[i].active && outputs[i].gamma_ctrl == gamma_ctrl)
            return &outputs[i];
    return NULL;
}

Output *output_by_registry_name(uint32_t name) {
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (outputs[i].active && outputs[i].registry_name == name)
            return &outputs[i];
    return NULL;
}

int pad4(int x) { return (x + 3) & ~3; }
uint32_t wl_new_id(void) { return ++wl_next_id; }

void msg(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
}
void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("wisp: ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap); exit(1);
}

/* wl_fd is O_NONBLOCK, so a full socket buffer (request burst against a busy
 * compositor) returns EAGAIN or a short count. Block on POLLOUT and retry
 * rather than die()/corrupt the stream. The ancillary fd rides only the first
 * byte, so it's attached until at least one byte is accepted, then dropped. */
void wl_send(const void *buf, unsigned len, int fd) {
    const char *p = buf;
    unsigned off = 0;
    union { char b[CMSG_SPACE(sizeof(int))]; struct cmsghdr a; } c = {0};
    while (off < len) {
        struct iovec iov = { (void *)(p + off), len - off };
        struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1 };
        if (fd >= 0 && off == 0) {
            m.msg_control = c.b; m.msg_controllen = CMSG_LEN(sizeof(int));
            struct cmsghdr *h = CMSG_FIRSTHDR(&m);
            h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS;
            h->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(h), &fd, sizeof(int));
        }
        ssize_t n = sendmsg(wl_fd, &m, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                struct pollfd pfd = { wl_fd, POLLOUT, 0 };
                if (poll(&pfd, 1, -1) < 0 && errno != EINTR) die("wl poll: %s", strerror(errno));
                continue;
            }
            die("sendmsg: %s", strerror(errno));
        }
        off += (unsigned)n;
    }
}

void wl_req(uint32_t obj, uint16_t op, const uint32_t *args, int n, int fd) {
    uint32_t b[32];
    if (n + 2 > (int)(sizeof b / 4)) die("req too large");
    b[0] = obj;
    uint32_t size = 8 + n * 4;
    b[1] = (size << 16) | op;
    for (int i = 0; i < n; i++) b[2 + i] = args[i];
    wl_send(b, size, fd);
}

/* Request with string embedded between pre[] and post[] arg arrays. */
void wl_req_str(uint32_t obj, uint16_t op, const uint32_t *pre, int npre,
                const char *s, const uint32_t *post, int npost) {
    uint32_t b[64];
    int sl = (int)strlen(s) + 1, pl = pad4(sl);
    /* s can be compositor-controlled (registry.global iface name); bound it
     * against b[] like wl_req does, instead of smashing the stack. */
    if (2 + npre + 1 + pl / 4 + npost > (int)(sizeof b / 4))
        die("wl_req_str too large (%d B string)", sl);
    int p = 2;
    b[0] = obj;
    for (int i = 0; i < npre; i++) b[p++] = pre[i];
    b[p++] = sl;
    memset((char *)&b[p], 0, pl);
    memcpy((char *)&b[p], s, sl);
    p += pl / 4;
    for (int i = 0; i < npost; i++) b[p++] = post[i];
    uint32_t size = p * 4;
    b[1] = (size << 16) | op;
    wl_send(b, size, -1);
}

void wl_registry_bind(uint32_t name, const char *iface, uint32_t version,
                      uint32_t new_oid) {
    uint32_t pre[1] = { name };
    uint32_t post[2] = { version, new_oid };
    wl_req_str(id_registry, REGISTRY_REQ_BIND, pre, 1, iface, post, 2);
}

int wl_take_pending_fd(void) {
    if (!n_pending_fds) return -1;
    int fd = pending_fds[0];
    for (int i = 1; i < n_pending_fds; i++) pending_fds[i - 1] = pending_fds[i];
    n_pending_fds--;
    return fd;
}
void wl_close_pending_fds(void) {
    for (int i = 0; i < n_pending_fds; i++) close(pending_fds[i]);
    n_pending_fds = 0;
}

int wl_recv(int block) {
    struct iovec iov = { wl_rbuf + wl_rlen, sizeof wl_rbuf - wl_rlen };
    union { char b[CMSG_SPACE(8 * sizeof(int))]; struct cmsghdr a; } c;
    struct msghdr m = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = c.b, .msg_controllen = sizeof c.b,
    };
    ssize_t n = recvmsg(wl_fd, &m, MSG_CMSG_CLOEXEC | (block ? 0 : MSG_DONTWAIT));
    /* EAGAIN returns 1, not 0: a partial message parked in wl_rbuf keeps the
     * drain loop's wl_rlen >= 8, so "no data" must break that loop or it
     * busy-spins until the compositor sends the rest. */
    if (n < 0) return errno == EAGAIN ? 1 : -1;
    if (n == 0) return -1;
    wl_rlen += n;
    for (struct cmsghdr *h = CMSG_FIRSTHDR(&m); h; h = CMSG_NXTHDR(&m, h))
        if (h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS) {
            int nf = (h->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            int *fds = (int *)CMSG_DATA(h);
            for (int i = 0; i < nf; i++) {
                if (n_pending_fds < (int)(sizeof pending_fds / sizeof *pending_fds))
                    pending_fds[n_pending_fds++] = fds[i];
                else close(fds[i]);
            }
        }
    return 0;
}

void wl_connect(void) {
    const char *dir  = getenv("XDG_RUNTIME_DIR");
    const char *disp = getenv("WAYLAND_DISPLAY");
    if (!dir || !disp) die("XDG_RUNTIME_DIR / WAYLAND_DISPLAY not set");

    struct sockaddr_un a = { .sun_family = AF_UNIX };
    int n = snprintf(a.sun_path, sizeof a.sun_path,
                     disp[0] == '/' ? "%s" : "%s/%s",
                     disp[0] == '/' ? disp : dir,
                     disp[0] == '/' ? ""   : disp);
    if (n <= 0 || n >= (int)sizeof a.sun_path) die("wl path too long");

    wl_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (wl_fd < 0) die("wl socket: %s", strerror(errno));
    /* ponytail: retry ~2s. Compositors (mango) fork autostart before the
       socket accepts, so a one-shot connect races and dies. */
    for (int tries = 0; connect(wl_fd, (struct sockaddr *)&a, sizeof a) < 0; tries++) {
        if (tries >= 40 || (errno != ENOENT && errno != ECONNREFUSED))
            die("wl connect %s: %s", a.sun_path, strerror(errno));
        struct timespec d = { 0, 50 * 1000 * 1000 };
        nanosleep(&d, NULL);
    }
    int fl = fcntl(wl_fd, F_GETFL); fcntl(wl_fd, F_SETFL, fl | O_NONBLOCK);

    uint32_t rid = ID_REGISTRY;
    wl_req(ID_DISPLAY, DISPLAY_REQ_GET_REGISTRY, &rid, 1, -1);
    sync_id = wl_new_id();
    uint32_t sa = sync_id;
    wl_req(ID_DISPLAY, DISPLAY_REQ_SYNC, &sa, 1, -1);

    /* Block until sync.done so globals are bound before we do anything. */
    while (!globals_synced) {
        int fl2 = fcntl(wl_fd, F_GETFL); fcntl(wl_fd, F_SETFL, fl2 & ~O_NONBLOCK);
        int rc = wl_recv(1);
        fcntl(wl_fd, F_SETFL, fl2);
        if (rc < 0) die("wl recv: %s", strerror(errno));
        wl_dispatch();
    }

    /* wlroots-family only: without these we'd connect, draw nothing and never
     * exit (output_init_widgets returns silently). Fail loud instead. */
    if (!id_compositor) die("compositor lacks wl_compositor");
    if (!id_shm)        die("compositor lacks wl_shm");
    if (!id_layer_shell) die("compositor lacks zwlr_layer_shell_v1 (GNOME is unsupported)");
}

/* Block on one display.sync round-trip, so events the compositor queued in
 * response to requests already sent (e.g. wl_output.mode after the bind during
 * wl_connect) are all received and dispatched before we return. */
void wl_roundtrip(void) {
    rt_done = 0;
    rt_id = wl_new_id();
    uint32_t sa = rt_id;
    wl_req(ID_DISPLAY, DISPLAY_REQ_SYNC, &sa, 1, -1);
    while (!rt_done) {
        int fl = fcntl(wl_fd, F_GETFL); fcntl(wl_fd, F_SETFL, fl & ~O_NONBLOCK);
        int rc = wl_recv(1);
        fcntl(wl_fd, F_SETFL, fl);
        if (rc < 0) die("wl recv: %s", strerror(errno));
        wl_dispatch();
    }
}

/* Adopt an inherited wl_display fd (--reload-fds). The compositor still has
 * the prior connection's objects bound and its half of the id namespace
 * survives the exec, so we allocate strictly above the old high-water mark
 * (`hi=`) and bind a fresh registry there. Long-lived objects ride the arg
 * for IN-PLACE reuse — `adopt=` surfaces get new buffers attached without
 * ever unmapping (a create+destroy cycle would be animated by compositors
 * with layer animations, and would double the bar's exclusive zone for a
 * frame), `gamma=` controls are exclusive per output so releasing them blips
 * the screen to neutral. `old=` (plus any adopt record nothing claimed) is
 * destroyed by wl_reap_old() once our own surfaces are up. Events still
 * arriving on other old ids (frame done, pointer enter, buffer release) miss
 * every dispatch match below the new watermark and fall through harmlessly;
 * remaining old sub-objects leak until exit (single reload's worth). */
typedef struct { int kind; char out[32]; uint32_t surf, ls, vp, frac; int w, h; } AdoptRec;
static AdoptRec adopts[MAX_WIDGETS];
static int      n_adopt;
static struct { char out[32]; uint32_t ctrl, size; } gadopts[MAX_OUTPUTS];
static int      n_gadopt;
static uint32_t old_ls[MAX_WIDGETS], old_surf[MAX_WIDGETS];
static int      n_old;

static const char *arg_field(const char *s, const char *key) {
    const char *p = s ? strstr(s, key) : NULL;
    return p ? p + strlen(key) : NULL;
}

void wl_adopt(int fd, const char *args) {
    wl_fd = fd;
    int fl = fcntl(wl_fd, F_GETFL); fcntl(wl_fd, F_SETFL, fl | O_NONBLOCK);
    /* re-arm CLOEXEC dropped by ctl_reload_exec(); without it every spawned
     * app inherits the live wl socket and keeps our surfaces/gamma alive
     * after we die. */
    fcntl(wl_fd, F_SETFD, FD_CLOEXEC);

    unsigned hi = 0;
    const char *p = arg_field(args, "hi=");
    if (p) sscanf(p, "%u", &hi);
    if (hi > wl_next_id) wl_next_id = hi;
    p = arg_field(args, "adopt=");
    while (p && *p && *p != ',' && n_adopt < MAX_WIDGETS) {
        AdoptRec *r = &adopts[n_adopt];
        int c;
        if (sscanf(p, "%d:%31[^:]:%u:%u:%u:%u:%d:%d%n", &r->kind, r->out,
                   &r->surf, &r->ls, &r->vp, &r->frac, &r->w, &r->h, &c) < 8)
            break;
        n_adopt++; p += c;
        if (*p == '+') p++;
    }
    p = arg_field(args, "gamma=");
    while (p && *p && *p != ',' && n_gadopt < MAX_OUTPUTS) {
        int c;
        if (sscanf(p, "%31[^:]:%u:%u%n", gadopts[n_gadopt].out,
                   &gadopts[n_gadopt].ctrl, &gadopts[n_gadopt].size, &c) < 3)
            break;
        n_gadopt++; p += c;
        if (*p == '+') p++;
    }
    p = arg_field(args, "old=");
    while (p && *p && n_old < MAX_WIDGETS) {
        unsigned ls, sf; int c;
        if (sscanf(p, "%u/%u%n", &ls, &sf, &c) < 2) break;
        old_ls[n_old] = ls; old_surf[n_old++] = sf;
        p += c;
        if (*p == '+') p++;
    }

    id_registry = wl_new_id();
    uint32_t rid = id_registry;
    wl_req(ID_DISPLAY, DISPLAY_REQ_GET_REGISTRY, &rid, 1, -1);
    sync_id = wl_new_id();
    uint32_t sa = sync_id;
    wl_req(ID_DISPLAY, DISPLAY_REQ_SYNC, &sa, 1, -1);
    while (!globals_synced) {
        int fl2 = fcntl(wl_fd, F_GETFL); fcntl(wl_fd, F_SETFL, fl2 & ~O_NONBLOCK);
        int rc = wl_recv(1);
        fcntl(wl_fd, F_SETFL, fl2);
        if (rc < 0) die("wl recv: %s", strerror(errno));
        wl_dispatch();
    }
    /* wl_output name events trail the sync above (the binds that solicit them
     * are only sent while dispatching it); adoption matches by output name, so
     * one more roundtrip before anyone creates widgets. */
    wl_roundtrip();
}

/* In-place adoption for widget_setup_surface: if the pre-exec process handed
 * over a same-kind surface on this output, reuse its objects and report
 * configured at its size — the caller re-sends anchor/size/exclusive on the
 * mapped ls, which the compositor treats as a plain state change. */
int wl_take_adopted(Widget *w, Output *o) {
    if (!o || !o->name[0]) return 0;
    for (int i = 0; i < n_adopt; i++) {
        AdoptRec *r = &adopts[i];
        if (!r->surf || r->kind != (int)w->kind || strcmp(r->out, o->name))
            continue;
        w->surface       = r->surf;
        w->layer_surface = r->ls;
#ifdef WISP_FRACTIONAL
        w->viewport   = r->vp;
        w->frac_scale = r->frac;
#endif
        w->w = r->w; w->h = r->h;
        w->configured = 1;
        r->surf = 0;   /* claimed */
        return 1;
    }
    return 0;
}

/* Gamma-control adoption for gamma_bind_output; the old ramp stays applied,
 * the 1 Hz auto tick re-derives the same kelvin within a second. */
uint32_t wl_take_adopted_gamma(const char *outname, uint32_t *size) {
    for (int i = 0; i < n_gadopt; i++) {
        if (!gadopts[i].ctrl || strcmp(gadopts[i].out, outname)) continue;
        uint32_t ctrl = gadopts[i].ctrl;
        *size = gadopts[i].size;
        gadopts[i].ctrl = 0;   /* claimed */
        return ctrl;
    }
    return 0;
}

/* Destroy the previous process's surfaces. Called after our own widgets have
 * been created AND a roundtrip has let their configure→attach→commit go out,
 * so the destroys land behind fully-mapped replacements in the request
 * stream — reaping any earlier is just the unmap flash again. */
void wl_reap_old(void) {
    for (int i = 0; i < n_old; i++) {
        if (old_ls[i])   wl_req(old_ls[i],   LS_REQ_DESTROY,      NULL, 0, -1);
        if (old_surf[i]) wl_req(old_surf[i], SURFACE_REQ_DESTROY, NULL, 0, -1);
    }
    n_old = 0;
    /* Adoption records nothing claimed (preset dropped a surface, output name
     * changed): same fate as old=. Unclaimed gamma controls likewise — the
     * failed bind already marked that output gamma_failed, lost until restart. */
    for (int i = 0; i < n_adopt; i++)
        if (adopts[i].surf) {
            if (adopts[i].ls) wl_req(adopts[i].ls, LS_REQ_DESTROY, NULL, 0, -1);
            wl_req(adopts[i].surf, SURFACE_REQ_DESTROY, NULL, 0, -1);
            adopts[i].surf = 0;
        }
    n_adopt = 0;
    for (int i = 0; i < n_gadopt; i++)
        if (gadopts[i].ctrl) {
            wl_req(gadopts[i].ctrl, GAMMA_CTRL_REQ_DESTROY, NULL, 0, -1);
            gadopts[i].ctrl = 0;
        }
    n_gadopt = 0;
}

/* Forward decls of input handlers in their respective modules. */
extern void on_pointer_event(uint16_t op, uint8_t *body, uint32_t bodylen);
extern void on_keyboard_event(uint16_t op, uint8_t *body, uint32_t bodylen);
extern void on_ls_event(Widget *w, uint16_t op, uint8_t *body, uint32_t bodylen);
extern void on_frame_done(Widget *w, uint32_t cb_id);
extern void on_buffer_release(uint32_t buf_id);

/* Bind the per-output gamma sub-object and spawn the per-output
 * widgets (bar/wall/hud). Called once the global registry has been synced
 * AND we have an Output. For hotplug adds after startup, called immediately. */
#ifndef WISP_GENERATED_OUTPUT_INIT
void output_init_widgets(Output *o) {
    if (!o || o->widgets_created) return;
    if (!id_compositor || !id_layer_shell) return;   /* prerequisites missing */
#ifdef WISP_HAS_GAMMA
    if (id_gamma_mgr && !o->gamma_ctrl && !o->gamma_failed)
        gamma_bind_output(o);
#endif

    /* The widgets themselves. wall first → it sits on the BACKGROUND layer
     * and we want it painted under any other surface. */
#ifdef WISP_HAS_WALL
    wall_create_on(o);
#endif
#ifdef WISP_HAS_BAR
    bar_create_on(o);
#endif
    o->widgets_created = 1;

#ifdef WISP_HAS_LOCK
    /* If a session lock is already in effect, also drop a lock surface on
     * this newly-attached output so it isn't a visible escape hatch. */
    if (lock_active()) lock_on_output_added(o);
#endif

    /* ponytail: focused_output = first output; mango ipc no longer reports
     * kbd focus. Fine on one monitor — `watch all-monitors` if that changes. */
    if (!focused_output) focused_output = o;
}
#endif

/* Compositor-side teardown ordering: layer surfaces are auto-invalidated
 * when their wl_output goes away (and the compositor will send LS_EV_CLOSED
 * ahead of the global_remove). We still need to destroy our object IDs and
 * free the slot. Idempotent. */
void output_destroy(Output *o) {
    if (!o || !o->active) return;

    /* Destroy widgets (sends layer_surface.destroy + surface.destroy). */
    if (o->bar)  { widget_destroy(o->bar);  o->bar  = NULL; }
    if (o->wall) { widget_destroy(o->wall); o->wall = NULL; }
    if (o->hud)  { widget_destroy(o->hud);  o->hud  = NULL; }
#ifdef WISP_HAS_LOCK
    if (o->lock) { lock_on_output_removed(o); }   /* destroys via widget_destroy */
#endif

    if (o->gamma_ctrl) {
        wl_req(o->gamma_ctrl, GAMMA_CTRL_REQ_DESTROY, NULL, 0, -1);
        o->gamma_ctrl = 0;
    }
    /* wl_output: just stop referencing the id. */

    if (focused_output == o) {
        focused_output = NULL;
        for (int i = 0; i < MAX_OUTPUTS; i++)
            if (outputs[i].active && &outputs[i] != o) { focused_output = &outputs[i]; break; }
    }

#ifdef WISP_HAS_OSD
    /* If an OSD widget (stack or pct pill) was anchored to this output, drop
     * it; the next post will re-anchor to whatever is focused now. */
    for (int i = 0; i < MAX_WIDGETS; i++)
        if (widgets[i].kind == W_OSD && widgets[i].output == o)
            widget_destroy(&widgets[i]);
    extern void osd_on_output_destroyed(Output *o) __attribute__((weak));
    if (osd_on_output_destroyed) osd_on_output_destroyed(o);
#endif

    /* Drop any cutouts scoped to this output before the slot can be reused. */
    cutout_drop_output(o);

    o->active = 0;
}

static void handle_registry_global(uint32_t name, const char *iface, uint32_t ver) {
    if (!id_compositor && !strcmp(iface, "wl_compositor")) {
        compositor_ver = ver < 4 ? ver : 4;
        id_compositor = wl_new_id(); wl_registry_bind(name, iface, compositor_ver, id_compositor);
    } else if (!id_shm && !strcmp(iface, "wl_shm")) {
        id_shm = wl_new_id(); wl_registry_bind(name, iface, 1, id_shm);
    } else if (!id_seat && !strcmp(iface, "wl_seat")) {
        id_seat = wl_new_id(); wl_registry_bind(name, iface, ver < 5 ? ver : 5, id_seat);
    } else if (!strcmp(iface, "wl_output")) {
        Output *o = output_alloc(name);
        if (!o) { msg("wisp: too many outputs (>%d), ignoring extra", MAX_OUTPUTS); return; }
        o->wl_output = wl_new_id();
        /* v4 for the `name` event — mango's ipc keys tag state by it. */
        wl_registry_bind(name, iface, ver < 4 ? ver : 4, o->wl_output);
        /* During the startup sync we wait until everything is bound before
         * spawning per-output widgets (output_init_widgets). For post-sync
         * hotplugs, spawn immediately. */
        if (globals_synced) output_init_widgets(o);
    } else if (!id_layer_shell && !strcmp(iface, "zwlr_layer_shell_v1")) {
        id_layer_shell = wl_new_id();
        layer_shell_ver = ver < 4 ? ver : 4;
        wl_registry_bind(name, iface, layer_shell_ver, id_layer_shell);
    } else if (!id_wm_base && !strcmp(iface, "xdg_wm_base")) {
        id_wm_base = wl_new_id(); wl_registry_bind(name, iface, 1, id_wm_base);
    } else if (!id_gamma_mgr && !strcmp(iface, "zwlr_gamma_control_manager_v1")) {
        id_gamma_mgr = wl_new_id();
        wl_registry_bind(name, iface, 1, id_gamma_mgr);
    } else if (!id_slock_mgr && !strcmp(iface, "ext_session_lock_manager_v1")) {
        id_slock_mgr = wl_new_id();
        wl_registry_bind(name, iface, 1, id_slock_mgr);
#ifdef WISP_FRACTIONAL
    } else if (!id_viewporter && !strcmp(iface, "wp_viewporter")) {
        id_viewporter = wl_new_id(); wl_registry_bind(name, iface, 1, id_viewporter);
    } else if (!id_frac_mgr && !strcmp(iface, "wp_fractional_scale_manager_v1")) {
        id_frac_mgr = wl_new_id(); wl_registry_bind(name, iface, 1, id_frac_mgr);
#endif
    } else if (!id_extws_mgr && !strcmp(iface, "ext_workspace_manager_v1")) {
        id_extws_mgr = wl_new_id();
        wl_registry_bind(name, iface, 1, id_extws_mgr);
    } else if (!id_river_status_mgr && !strcmp(iface, "zriver_status_manager_v1")) {
        id_river_status_mgr = wl_new_id();
        wl_registry_bind(name, iface, ver < 4 ? ver : 4, id_river_status_mgr);
    } else if (!id_river_control && !strcmp(iface, "zriver_control_v1")) {
        id_river_control = wl_new_id();
        wl_registry_bind(name, iface, 1, id_river_control);
#ifdef WISP_HAS_TOPLEVEL
    } else if (!id_toplevel_mgr && !strcmp(iface, "zwlr_foreign_toplevel_manager_v1")) {
        tl_bind(name, ver);
#endif
    }
}

static void on_seat_capabilities(uint32_t caps) {
    if ((caps & SEAT_CAP_POINTER) && !id_pointer) {
        id_pointer = wl_new_id();
        uint32_t a = id_pointer; wl_req(id_seat, SEAT_REQ_GET_POINTER, &a, 1, -1);
    }
    if ((caps & SEAT_CAP_KEYBOARD) && !id_keyboard) {
        id_keyboard = wl_new_id();
        uint32_t a = id_keyboard; wl_req(id_seat, SEAT_REQ_GET_KEYBOARD, &a, 1, -1);
    }
}

static void handle(uint32_t obj, uint16_t op, uint8_t *body, uint32_t bodylen) {
    if (obj == ID_DISPLAY) {
        if (op == DISPLAY_EV_ERROR) {
            if (bodylen < 12) return;
            uint32_t bad = *(uint32_t *)body;
            uint32_t code = *(uint32_t *)(body + 4);
            uint32_t mlen = *(uint32_t *)(body + 8);
            if (mlen > bodylen - 12) mlen = bodylen - 12;
            die("wl error obj=%u code=%u: %.*s", bad, code, (int)mlen, body + 12);
        }
        if (op == DISPLAY_EV_DELETE_ID) {
            /* Server destroyed this object (e.g. gamma_ctrl when its output
             * went away). Clear any tracked references so we don't send a
             * request on a now-invalid id. */
            if (bodylen < 4) return;
            uint32_t id = *(uint32_t *)body;
            for (int i = 0; i < MAX_OUTPUTS; i++) {
                Output *o = &outputs[i];
                if (o->gamma_ctrl == id) o->gamma_ctrl = 0;
            }
        }
        return;
    }
    if (obj == id_registry && op == REGISTRY_EV_GLOBAL) {
        if (bodylen < 12) return;
        uint32_t name = *(uint32_t *)body;
        uint32_t slen = *(uint32_t *)(body + 4);
        /* slen is compositor-controlled; bound it first (so pad4 can't overflow
         * int), then ensure the NUL terminator and the trailing version u32 are
         * both inside the body before dereferencing. */
        if (slen == 0 || slen > bodylen - 12 || body[8 + slen - 1] != 0) return;
        const char *iface = (const char *)(body + 8);
        if (12 + (uint32_t)pad4(slen) > bodylen) return;
        uint32_t ver = *(uint32_t *)(body + 8 + pad4(slen));
        handle_registry_global(name, iface, ver);
        return;
    }
    if (obj == id_registry && op == REGISTRY_EV_GLOBAL_REM) {
        if (bodylen < 4) return;
        uint32_t name = *(uint32_t *)body;
#ifdef WISP_HAS_TOPLEVEL
        tl_on_registry_remove(name);
#endif
        Output *o = output_by_registry_name(name);
        if (o) {
            output_destroy(o);
        }
        return;
    }
    if (obj == sync_id && sync_id && op == CALLBACK_EV_DONE) {
        sync_id = 0; globals_synced = 1; return;
    }
    if (obj == rt_id && rt_id && op == CALLBACK_EV_DONE) {
        rt_id = 0; rt_done = 1; return;
    }
    if (obj == id_wm_base && op == WM_BASE_EV_PING) {
        if (bodylen < 4) return;
        uint32_t serial = *(uint32_t *)body;
        wl_req(id_wm_base, WM_BASE_REQ_PONG, &serial, 1, -1);
        return;
    }
    if (obj == id_seat && op == SEAT_EV_CAPABILITIES) {
        if (bodylen < 4) return;
        on_seat_capabilities(*(uint32_t *)body); return;
    }
    if (id_pointer && obj == id_pointer) {
        on_pointer_event(op, body, bodylen); return;
    }
    if (id_keyboard && obj == id_keyboard) {
        on_keyboard_event(op, body, bodylen); return;
    }
    {
        /* wl_output events. We only care about the current mode's pixel size,
         * stashed so the lock can pre-render its background before engaging. */
        Output *mo = output_by_wl(obj);
        if (mo) {
            if (op == OUTPUT_EV_MODE && bodylen >= 16 && (*(uint32_t *)body & 1)) {
                mo->mode_w = *(int32_t *)(body + 4);
                mo->mode_h = *(int32_t *)(body + 8);
            } else if (op == OUTPUT_EV_SCALE && bodylen >= 4) {
                int32_t sc = *(int32_t *)body;
                /* Clamp: the pool sizing below multiplies by this, and a
                 * hostile/absurd factor must not overflow the int math. */
                if (sc < 1) sc = 1; else if (sc > 4) sc = 4;
                if (sc * 120 != mo->scale120) {
                    mo->scale120 = sc * 120;
                    if (mo->widgets_created) widget_rescale_output(mo);
                }
            } else if (op == OUTPUT_EV_NAME && bodylen >= 4) {
                /* wire string: uint32 len (incl. NUL) + padded bytes */
                uint32_t slen = *(uint32_t *)body;
                if (slen == 0 || slen > bodylen - 4) return;
                snprintf(mo->name, sizeof mo->name, "%.*s", (int)slen, body + 4);
            }
            return;
        }
    }
#ifdef WISP_HAS_GAMMA
    {
        Output *go = output_by_gamma(obj);
        if (go) {
            if (op == GAMMA_CTRL_EV_GAMMA_SIZE) {
                if (bodylen < 4) return;
                gamma_on_size(go, *(uint32_t *)body);
            }
            else if (op == GAMMA_CTRL_EV_FAILED) gamma_on_failed(go);
            return;
        }
    }
#endif
#ifdef WISP_HAS_LOCK
    if (id_slock && obj == id_slock) {
        if (op == SLOCK_EV_LOCKED)        lock_on_locked();
        else if (op == SLOCK_EV_FINISHED) lock_on_finished();
        return;
    }
    {
        Widget *lw = widget_by_slock_surf(obj);
        if (lw && op == SLOCK_SURF_EV_CONFIGURE) {
            if (bodylen < 12) return;
            uint32_t serial = *(uint32_t *)body;
            uint32_t nw = *(uint32_t *)(body + 4);
            uint32_t nh = *(uint32_t *)(body + 8);
            lock_on_surf_configure(lw, serial, (int)nw, (int)nh);
            return;
        }
    }
#endif
    /* ext-workspace handles: routed by object id before the op-0 fallback
     * below, since their id/capabilities events are also opcode 0. Bar-gated —
     * the bar is the only consumer of tag state, and wisp-lock links neither. */
#ifdef WISP_HAS_BAR
    if (extws_handle_event(obj, op, body, bodylen)) return;
    if (river_handle_event(obj, op, body, bodylen)) return;
#endif
#ifdef WISP_HAS_TOPLEVEL
    if (tl_handle_event(obj, op, body, bodylen)) return;
#endif
    /* Opcode 0 is overloaded across interfaces (layer-surface.configure,
       buffer.release, callback.done, ...). Disambiguate by object id —
       layer-surface and frame-callback first, then buffer release as the
       remaining op==0 destination. */
#ifdef WISP_FRACTIONAL
    /* preferred_scale is also opcode 0 — route by object id first. */
    {
        Widget *fw = widget_by_frac(obj);
        if (fw && op == FRAC_EV_PREFERRED_SCALE && bodylen >= 4)
            { widget_set_scale(fw, (int)*(uint32_t *)body); return; }
    }
#endif
    Widget *w = widget_by_ls(obj);
    if (w) { on_ls_event(w, op, body, bodylen); return; }
    if (op == CALLBACK_EV_DONE) {
        for (int i = 0; i < MAX_WIDGETS; i++) {
            if (widgets[i].kind != W_NONE && widgets[i].frame_cb == obj) {
                on_frame_done(&widgets[i], obj);
                return;
            }
        }
    }
    if (op == BUFFER_EV_RELEASE) on_buffer_release(obj);
}

/* Drain the read buffer using a read cursor; compact once at the end instead
 * of memmove-after-every-event. dwl tag-update bursts and pointer-motion
 * bursts deliver many small events per packet; the old per-event memmove was
 * O(n²) in bytes moved. */
void wl_dispatch(void) {
    int pos = 0;
    while (wl_rlen - pos >= 8) {
        uint32_t obj  = *(uint32_t *)(wl_rbuf + pos);
        uint32_t hh   = *(uint32_t *)(wl_rbuf + pos + 4);
        uint16_t op   = hh & 0xffff;
        uint16_t size = hh >> 16;
        if (size < 8 || size > sizeof wl_rbuf) die("wl bad size %u", size);
        if (wl_rlen - pos < size) break;
        handle(obj, op, wl_rbuf + pos + 8, size - 8);
        pos += size;
    }
    if (pos > 0) {
        if (wl_rlen - pos > 0)
            memmove(wl_rbuf, wl_rbuf + pos, wl_rlen - pos);
        wl_rlen -= pos;
    }
    /* Only flush leftover fds once the buffer is fully drained. A partial
     * message kept across reads (split delivery of e.g. wl_keyboard.keymap)
     * still owns its fd; closing it here would drop or misattribute it. */
    if (wl_rlen == 0) wl_close_pending_fds();
}

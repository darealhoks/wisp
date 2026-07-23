/* pipewire.c — PipeWire native-protocol client feeding the DSL `pipewire()`
 * source: live sink/source volume + mute, no wpctl fork/poll.
 *
 * Pure control client (no media streams ⇒ no memfd/mempool path). Speaks the
 * native protocol raw over $XDG_RUNTIME_DIR/pipewire-0, libc+libm only — same
 * hand-rolled-wire discipline as wl.c (Wayland) and dbus.c (D-Bus). Shape is
 * copied from dbus.c: reader with an ok-flag on overrun, writer helpers,
 * dispatch switch, arm_reconnect/drop lifecycle, return values on the normal
 * path (die() reserved for programmer errors, of which there are none here).
 *
 * Flow: Hello → GetRegistry. Binding is incremental (no Sync/Done gating —
 * event delivery order is not guaranteed): when the "default" Metadata global
 * appears we Bind it; its Property events name the default sink/source; we map
 * name → node global (from either the metadata event or the node's own Global,
 * whichever lands second) → Bind + SubscribeParams(Props). Node::Param events
 * carry channelVolumes (linear) + mute. Displayed pct = round(cbrt(max)*100),
 * the cubic mapping wireplumber/wpctl apply. Server Pings get a Pong keepalive.
 *
 * Wire framing: 16-byte LE header {u32 id, u32 opcode<<24|size, u32 seq,
 * u32 n_fds} + one SPA POD payload. Newer servers append a footer pod inside
 * size — we parse the first pod by its own length and ignore trailing bytes.
 * fds ride SCM_RIGHTS even on messages we ignore; we consume none and close
 * every received fd immediately (a control client references no shared mem). */

#define _GNU_SOURCE
#include "wisp.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

/* ---- SPA POD base types (spa/utils/type.h) ---- */
enum {
    SPA_None = 1, SPA_Bool = 2, SPA_Id = 3, SPA_Int = 4, SPA_Long = 5,
    SPA_Float = 6, SPA_Double = 7, SPA_String = 8,
    SPA_Array = 13, SPA_Struct = 14, SPA_Object = 15, SPA_Choice = 19,
};
/* SPA object types / param ids / prop keys (spa/utils/type.h, spa/param) */
#define SPA_OBJECT_Props      0x40002u
#define SPA_OBJECT_ParamRoute 0x40009u
#define SPA_PARAM_Props       2u
#define SPA_PARAM_Route       13u
#define SPA_PROP_mute         0x10004u
#define SPA_PROP_channelVolumes 0x10008u
/* enum spa_param_route (spa/param/route.h) */
#define SPA_ROUTE_index       1u
#define SPA_ROUTE_device      3u
#define SPA_ROUTE_props       10u
#define SPA_ROUTE_save        13u

/* Wire opcodes (pipewire/core.h, node.h, ext/metadata.h) — verified against
 * the 1.6 headers on this box. ADD_LISTENER (0) is client-local, never sent. */
#define CORE_HELLO        1
#define CORE_SYNC         2
#define CORE_PONG         3
#define CORE_GET_REGISTRY 5
#define CORE_EV_INFO      0
#define CORE_EV_DONE      1
#define CORE_EV_PING      2
#define CORE_EV_ERROR     3
#define CORE_EV_REMOVE_ID 4
#define CLIENT_UPDATE_PROPERTIES 2
#define REGISTRY_BIND     1
#define REGISTRY_EV_GLOBAL        0
#define REGISTRY_EV_GLOBAL_REMOVE 1
#define NODE_SUBSCRIBE_PARAMS 1
#define NODE_SET_PARAM        3
#define NODE_EV_INFO      0
#define NODE_EV_PARAM     1
/* Device shares Node's method/event numbering (device.h / node.h). */
#define DEVICE_SUBSCRIBE_PARAMS 1
#define DEVICE_SET_PARAM        3
#define DEVICE_EV_PARAM   1
#define METADATA_EV_PROPERTY 0

/* Fixed proxy ids by convention; we allocate 2.. ourselves. */
#define PW_ID_CORE      0
#define PW_ID_CLIENT    1
#define PW_ID_REGISTRY  2

#define TYPE_NODE     "PipeWire:Interface:Node"
#define TYPE_DEVICE   "PipeWire:Interface:Device"
#define TYPE_METADATA "PipeWire:Interface:Metadata"

int pw_fd = -1;
int pw_reconnect_fd = -1;

/* Published state (what the DSL fields read). -1 pct = unknown. */
static int cur_vol = -1, cur_mute = 0, cur_mic_vol = -1, cur_mic_mute = 0, cur_ok = 0;
static int pub_vol = -1, pub_mute = 0, pub_mic_vol = -1, pub_mic_mute = 0, pub_ok = 0;

static uint32_t out_seq;
static uint32_t next_id;             /* next free proxy id (starts at 3) */

/* Resolved defaults. *_global = registry global id; *_proxy = our bound id. */
static uint32_t meta_global, meta_proxy;
static uint32_t sink_proxy, source_proxy;
static char     sink_name[128], source_name[128];
static int      sink_nchan = 2, source_nchan = 2;

/* The real mixer lives on the sink/source's card Device, driven by SPA_PARAM_Route
 * (node-level Props writes don't propagate to the route). Per side we bind that
 * Device (global = node's device.id) and SubscribeParams(Route). *_card_device is
 * the node's card.profile.device (from Node::Info) — the write picks the route
 * whose `device` field equals it. Route events and Node::Info arrive in either
 * order, so routes are cached and matched at write time. A virtual node (e.g.
 * rnnoise source) has no device.id — dev_proxy stays 0 and writes fall back to
 * node-level Props. */
static uint32_t sink_dev_proxy, source_dev_proxy;
static int      sink_card_device = -1, source_card_device = -1;
#define ROUTES_MAX 16
typedef struct { int device, index; } RouteEnt;
static RouteEnt sink_routes[ROUTES_MAX], source_routes[ROUTES_MAX];
static int      sink_nroutes, source_nroutes;

/* Node-global cache: name → global id (+ its card Device), so a
 * default.audio.sink="foo" metadata value can be mapped to the node global to
 * Bind, and to its Device for the route write path. device_id < 0 = virtual. */
#define NODE_CACHE 96
typedef struct { uint32_t global; char name[128]; int device_id; } NodeEnt;
static NodeEnt nodes[NODE_CACHE];
static int     node_n;

static void pw_drop(int delay_ms);
static void arm_reconnect(int delay_ms);

extern void wispgen_wisp_state_changed(void) __attribute__((weak));
static void publish(void) {
    if (cur_vol == pub_vol && cur_mute == pub_mute && cur_mic_vol == pub_mic_vol
        && cur_mic_mute == pub_mic_mute && cur_ok == pub_ok) return;
    pub_vol = cur_vol; pub_mute = cur_mute;
    pub_mic_vol = cur_mic_vol; pub_mic_mute = cur_mic_mute; pub_ok = cur_ok;
    if (wispgen_wisp_state_changed) wispgen_wisp_state_changed();
}

/* ================================================================== */
/* POD writer                                                          */
/* ================================================================== */

typedef struct { uint8_t *b; int pos, cap; } POD;

static void pod_ensure(POD *p, int n) {
    if (p->pos + n <= p->cap) return;
    int nc = p->cap ? p->cap : 128;
    while (nc < p->pos + n) nc *= 2;
    p->b = realloc(p->b, nc);
    p->cap = nc;
}
static void pod_raw(POD *p, const void *d, int n) {
    pod_ensure(p, n); memcpy(p->b + p->pos, d, n); p->pos += n;
}
static void pod_u32(POD *p, uint32_t v) { pod_raw(p, &v, 4); }
static void pod_pad(POD *p) {
    static const uint8_t z[8] = {0};
    int pad = (-p->pos) & 7;
    if (pad) pod_raw(p, z, pad);
}
/* Fixed-body primitive pod: 8-byte header {size,type} + body, padded to 8. */
static void pod_prim(POD *p, uint32_t type, const void *body, int n) {
    pod_u32(p, (uint32_t)n); pod_u32(p, type);
    pod_raw(p, body, n); pod_pad(p);
}
static void pw_wbool (POD *p, int v)      { uint32_t x = v ? 1 : 0; pod_prim(p, SPA_Bool, &x, 4); }
static void pw_wid   (POD *p, uint32_t v) { pod_prim(p, SPA_Id,  &v, 4); }
static void pw_wint  (POD *p, int32_t v)  { pod_prim(p, SPA_Int, &v, 4); }
static void pw_wstr  (POD *p, const char *s) {
    if (!s) s = "";
    int l = (int)strlen(s) + 1;
    pod_u32(p, (uint32_t)l); pod_u32(p, SPA_String);
    pod_raw(p, s, l); pod_pad(p);
}
/* Container begin/end: reserve the size word, backpatch it at end. */
static int pw_begin(POD *p, uint32_t type) {
    int at = p->pos; pod_u32(p, 0); pod_u32(p, type); return at;
}
static void pw_end(POD *p, int at) {
    uint32_t sz = (uint32_t)(p->pos - (at + 8));
    memcpy(p->b + at, &sz, 4);
    pod_pad(p);
}
static int pw_begin_object(POD *p, uint32_t otype, uint32_t oid) {
    int at = pw_begin(p, SPA_Object); pod_u32(p, otype); pod_u32(p, oid); return at;
}
static void pw_prop(POD *p, uint32_t key) { pod_u32(p, key); pod_u32(p, 0); }  /* + value pod follows */

/* ================================================================== */
/* Message send                                                        */
/* ================================================================== */

static void pw_send(uint32_t id, uint8_t opcode, POD *p) {
    if (pw_fd < 0) { free(p->b); return; }
    uint8_t hdr[16];
    uint32_t w0 = id;
    uint32_t w1 = ((uint32_t)opcode << 24) | ((uint32_t)p->pos & 0xffffff);
    uint32_t w2 = out_seq++;
    uint32_t w3 = 0;
    memcpy(hdr, &w0, 4); memcpy(hdr + 4, &w1, 4);
    memcpy(hdr + 8, &w2, 4); memcpy(hdr + 12, &w3, 4);
    struct iovec iov[2] = { { hdr, 16 }, { p->b, (size_t)p->pos } };
    struct msghdr m = { .msg_iov = iov, .msg_iovlen = 2 };
    ssize_t n = sendmsg(pw_fd, &m, MSG_NOSIGNAL);
    free(p->b);
    if (n != 16 + p->pos) pw_drop(2000);
}

/* ================================================================== */
/* POD reader (ok-flag on any overrun, mirrors dbus.c's R)             */
/* ================================================================== */

typedef struct { const uint8_t *b; int len, pos, ok; } RD;

static uint32_t rd_u32(RD *r) {
    if (!r->ok || r->pos + 4 > r->len) { r->ok = 0; return 0; }
    uint32_t v; memcpy(&v, r->b + r->pos, 4); r->pos += 4; return v;
}

/* Read one pod header at r->pos, unwrap a Choice to its first child, hand back
 * the effective type + body location, and advance r->pos past the whole
 * (padded) outer pod. Returns 0 (and trips ok) on any bounds failure. */
static int pod_open(RD *r, uint32_t *type, int *body_pos, uint32_t *body_size) {
    int start = r->pos;
    if (!r->ok || start + 8 > r->len) { r->ok = 0; return 0; }
    uint32_t size, t;
    memcpy(&size, r->b + start, 4);
    memcpy(&t, r->b + start + 4, 4);
    int64_t bodyend = (int64_t)start + 8 + (int64_t)size;
    if (bodyend > r->len) { r->ok = 0; return 0; }
    int next = (int)((bodyend + 7) & ~(int64_t)7);
    if (next > r->len) next = (int)bodyend;   /* last pod in a tight slice */
    if (t == SPA_Choice) {
        int chp = start + 8 + 8;               /* skip choice {type,flags} */
        if (chp + 8 > r->len) { r->ok = 0; return 0; }
        uint32_t csize, ctype;
        memcpy(&csize, r->b + chp, 4);
        memcpy(&ctype, r->b + chp + 4, 4);
        if ((int64_t)chp + 8 + csize > r->len) { r->ok = 0; return 0; }
        *type = ctype; *body_pos = chp + 8; *body_size = csize;
    } else {
        *type = t; *body_pos = start + 8; *body_size = size;
    }
    r->pos = next;
    return 1;
}
static void rd_skip(RD *r) { uint32_t t, s; int bp; pod_open(r, &t, &bp, &s); }

static int32_t rd_int(RD *r) {
    uint32_t t, s; int bp;
    if (!pod_open(r, &t, &bp, &s) || s < 4) { r->ok = 0; return 0; }
    int32_t v; memcpy(&v, r->b + bp, 4); return v;
}
static void rd_str(RD *r, char *out, int cap) {
    uint32_t t, s; int bp;
    if (!pod_open(r, &t, &bp, &s) || t != SPA_String) { r->ok = 0; if (cap) out[0] = 0; return; }
    int n = (int)s; if (n > cap) n = cap;
    if (n <= 0) { if (cap) out[0] = 0; return; }
    memcpy(out, r->b + bp, n);
    out[n - 1] = 0;   /* body includes NUL; force-terminate a truncated copy */
}
/* Open a Struct/Object, giving a sub-reader bounded to its body. */
static int rd_struct(RD *r, RD *sub) {
    uint32_t t, s; int bp;
    if (!pod_open(r, &t, &bp, &s) || t != SPA_Struct) { r->ok = 0; return 0; }
    if (bp + (int)s > r->len) { r->ok = 0; return 0; }
    *sub = (RD){ r->b, bp + (int)s, bp, 1 };
    return 1;
}
static int rd_object(RD *r, RD *sub, uint32_t *otype) {
    uint32_t t, s; int bp;
    if (!pod_open(r, &t, &bp, &s) || t != SPA_Object) { r->ok = 0; return 0; }
    if (bp + 8 > r->len || bp + (int)s > r->len) { r->ok = 0; return 0; }
    memcpy(otype, r->b + bp, 4);
    *sub = (RD){ r->b, bp + (int)s, bp + 8, 1 };   /* skip object {type,id} */
    return 1;
}
/* Max channel of a channelVolumes Array<Float>, and its channel count. */
static float rd_chanvols(RD *r, int *nchan) {
    uint32_t t, s; int bp;
    if (!pod_open(r, &t, &bp, &s) || t != SPA_Array) { r->ok = 0; return 0; }
    RD a = { r->b, bp + (int)s, bp, 1 };
    uint32_t csize = rd_u32(&a), ctype = rd_u32(&a);
    if (!a.ok || ctype != SPA_Float || csize != 4) return 0;
    float mx = 0; int n = 0;
    while (a.pos + 4 <= a.len) {
        float f; memcpy(&f, a.b + a.pos, 4); a.pos += 4;
        if (f > mx) mx = f;
        n++;
    }
    *nchan = n;
    return mx;
}

/* ================================================================== */
/* Outbound protocol messages                                          */
/* ================================================================== */

static void send_hello(void) {
    POD p = {0};
    int st = pw_begin(&p, SPA_Struct);
    pw_wint(&p, 3);                    /* protocol version 3 */
    pw_end(&p, st);
    pw_send(PW_ID_CORE, CORE_HELLO, &p);
}
static void send_client_props(void) {
    POD p = {0};
    int st = pw_begin(&p, SPA_Struct);
    int dict = pw_begin(&p, SPA_Struct);   /* spa_dict as a struct */
    pw_wint(&p, 1);                        /* n_items */
    pw_wstr(&p, "application.name");
    pw_wstr(&p, "wisp");
    pw_end(&p, dict);
    pw_end(&p, st);
    pw_send(PW_ID_CLIENT, CLIENT_UPDATE_PROPERTIES, &p);
}
static void send_get_registry(void) {
    POD p = {0};
    int st = pw_begin(&p, SPA_Struct);
    pw_wint(&p, 3);                    /* registry version */
    pw_wint(&p, (int32_t)PW_ID_REGISTRY);
    pw_end(&p, st);
    pw_send(PW_ID_CORE, CORE_GET_REGISTRY, &p);
}
static void send_pong(uint32_t id, uint32_t seq) {
    POD p = {0};
    int st = pw_begin(&p, SPA_Struct);
    pw_wint(&p, (int32_t)id);
    pw_wint(&p, (int32_t)seq);
    pw_end(&p, st);
    pw_send(PW_ID_CORE, CORE_PONG, &p);
}
static void send_bind(uint32_t global, const char *type, uint32_t new_id) {
    POD p = {0};
    int st = pw_begin(&p, SPA_Struct);
    pw_wint(&p, (int32_t)global);
    pw_wstr(&p, type);
    pw_wint(&p, 3);                    /* interface version */
    pw_wint(&p, (int32_t)new_id);
    pw_end(&p, st);
    pw_send(PW_ID_REGISTRY, REGISTRY_BIND, &p);
}
static void send_subscribe(uint32_t proxy, uint32_t param_id) {
    POD p = {0};
    int st = pw_begin(&p, SPA_Struct);
    int ar = pw_begin(&p, SPA_Array);
    pod_u32(&p, 4);                    /* child size */
    pod_u32(&p, SPA_Id);               /* child type */
    pod_u32(&p, param_id);
    pw_end(&p, ar);
    pw_end(&p, st);
    pw_send(proxy, NODE_SUBSCRIBE_PARAMS, &p);   /* == DEVICE_SUBSCRIBE_PARAMS */
}

/* ================================================================== */
/* Default resolution                                                  */
/* ================================================================== */

static NodeEnt *node_by_name(const char *name) {
    for (int i = 0; i < node_n; i++)
        if (!strcmp(nodes[i].name, name)) return &nodes[i];
    return NULL;
}

/* Extract "name":"<value>" from wireplumber's default.audio.* JSON value.
 * Bounded scan, no JSON parser (see dbus.c: hand-rolled over pulling a lib). */
static int json_name(const char *json, char *out, int cap) {
    const char *k = strstr(json, "\"name\"");
    if (!k) return 0;
    k = strchr(k + 6, ':'); if (!k) return 0;
    k = strchr(k, '"'); if (!k) return 0;
    k++;
    int i = 0;
    while (*k && *k != '"' && i < cap - 1) out[i++] = *k++;
    out[i] = 0;
    return i > 0;
}

/* Bind the desired default node once its global is known. Idempotent: a no-op
 * until the name is set (from metadata) AND its global is enumerated, so it's
 * safe to call from both the metadata-property and the node-global paths —
 * whichever arrives second wins the race. */
static void try_bind(int is_sink) {
    const char *name = is_sink ? sink_name : source_name;
    uint32_t *proxy = is_sink ? &sink_proxy : &source_proxy;
    if (!name[0] || *proxy) return;
    NodeEnt *e = node_by_name(name);
    if (!e) return;
    *proxy = next_id++;
    send_bind(e->global, TYPE_NODE, *proxy);
    send_subscribe(*proxy, SPA_PARAM_Props);   /* reads: node channelVolumes+mute */
    /* Writes go to the card Device's route; virtual nodes have no device.id. */
    if (e->device_id >= 0) {
        uint32_t dev = next_id++;
        if (is_sink) { sink_dev_proxy = dev; sink_nroutes = 0; }
        else         { source_dev_proxy = dev; source_nroutes = 0; }
        send_bind((uint32_t)e->device_id, TYPE_DEVICE, dev);
        send_subscribe(dev, SPA_PARAM_Route);
    }
}

/* ================================================================== */
/* Inbound event decode                                                */
/* ================================================================== */

static void on_core_ping(RD *r) {
    RD s; if (!rd_struct(r, &s)) return;
    uint32_t id = (uint32_t)rd_int(&s);
    uint32_t seq = (uint32_t)rd_int(&s);
    if (s.ok) send_pong(id, seq);
}
static void on_core_error(RD *r) {
    RD s; if (!rd_struct(r, &s)) return;
    (void)rd_int(&s); (void)rd_int(&s);
    int res = rd_int(&s);
    char m[160]; rd_str(&s, m, sizeof m);
    if (s.ok) msg("wisp: pipewire error res=%d: %s", res, m);
}

/* Registry::Global: Struct{ Int id, Int perm, String type, Int version,
 * Struct{ Int n_items, (String key, String value)*n } }. The props dict is a
 * nested Struct (not inline). Cache Node globals by node.name; remember the
 * "default" Metadata global. */
static void on_global(RD *r) {
    RD s; if (!rd_struct(r, &s)) return;
    uint32_t id = (uint32_t)rd_int(&s);
    (void)rd_int(&s);                  /* permissions */
    char type[64]; rd_str(&s, type, sizeof type);
    (void)rd_int(&s);                  /* version */
    RD d; if (!rd_struct(&s, &d)) return;   /* props dict */
    int n = rd_int(&d);
    if (!s.ok || n < 0 || n > 1024) return;

    int is_node = !strcmp(type, TYPE_NODE);
    int is_meta = !strcmp(type, TYPE_METADATA);
    char nodename[128] = "", metaname[64] = "";
    int devid = -1;   /* card.profile.device isn't in the registry global — Node::Info carries it */
    for (int i = 0; i < n && d.ok; i++) {
        char key[64], v[64];
        rd_str(&d, key, sizeof key);
        if (is_node && !strcmp(key, "node.name")) rd_str(&d, nodename, sizeof nodename);
        else if (is_node && !strcmp(key, "device.id")) { rd_str(&d, v, sizeof v); devid = atoi(v); }
        else if (is_meta && !strcmp(key, "metadata.name")) rd_str(&d, metaname, sizeof metaname);
        else rd_skip(&d);              /* value pod we don't need */
    }
    s.ok = d.ok;
    if (!s.ok) return;
    if (is_node && nodename[0] && node_n < NODE_CACHE) {
        nodes[node_n].global = id;
        snprintf(nodes[node_n].name, sizeof nodes[node_n].name, "%s", nodename);
        nodes[node_n].device_id = devid;
        node_n++;
        /* A default may have been named before its node global arrived. */
        try_bind(1); try_bind(0);
    } else if (is_meta && !meta_proxy && !strcmp(metaname, "default")) {
        /* Bind the "default" metadata now; its Property events name the
         * current sink/source. No need to wait for enumeration to finish. */
        meta_global = id;
        meta_proxy = next_id++;
        send_bind(meta_global, TYPE_METADATA, meta_proxy);
    }
}
static void on_global_remove(RD *r) {
    RD s; if (!rd_struct(r, &s)) return;
    uint32_t id = (uint32_t)rd_int(&s);
    if (!s.ok) return;
    for (int i = 0; i < node_n; i++)
        if (nodes[i].global == id) { nodes[i] = nodes[--node_n]; break; }
}

/* Metadata::Property: Struct{ Int subject, String key, String type,
 * String value }. default.audio.sink/source name the current defaults. */
static void on_metadata_property(RD *r) {
    RD s; if (!rd_struct(r, &s)) return;
    (void)rd_int(&s);                  /* subject */
    char key[64], mtype[48], val[256];
    rd_str(&s, key, sizeof key);
    rd_str(&s, mtype, sizeof mtype);
    rd_str(&s, val, sizeof val);
    if (!s.ok) return;
    int sink = !strcmp(key, "default.audio.sink");
    int src  = !strcmp(key, "default.audio.source");
    if (!sink && !src) return;
    char name[128];
    if (!json_name(val, name, sizeof name)) return;
    if (sink && strcmp(name, sink_name)) {
        snprintf(sink_name, sizeof sink_name, "%s", name);
        sink_proxy = sink_dev_proxy = 0;   /* default switched: rebind (old abandoned) */
        try_bind(1);
    } else if (src && strcmp(name, source_name)) {
        snprintf(source_name, sizeof source_name, "%s", name);
        source_proxy = source_dev_proxy = 0;
        try_bind(0);
    }
}

/* Node::Param: Struct{ Int seq, Id id, Int index, Int next, Object param }.
 * param is Props with channelVolumes (linear) + mute. */
static void on_node_param(RD *r, int is_sink) {
    RD s; if (!rd_struct(r, &s)) return;
    (void)rd_int(&s);                  /* seq */
    uint32_t pid = (uint32_t)rd_int(&s);
    (void)rd_int(&s); (void)rd_int(&s);   /* index, next */
    if (!s.ok || pid != SPA_PARAM_Props) return;
    uint32_t otype; RD o;
    if (!rd_object(&s, &o, &otype) || otype != SPA_OBJECT_Props) return;

    int have_vol = 0, mute = is_sink ? cur_mute : cur_mic_mute;
    int vol = is_sink ? cur_vol : cur_mic_vol, nchan = is_sink ? sink_nchan : source_nchan;
    while (o.pos < o.len && o.ok) {
        uint32_t key = rd_u32(&o);
        (void)rd_u32(&o);              /* prop flags */
        if (!o.ok) break;
        if (key == SPA_PROP_mute) {
            mute = rd_int(&o) ? 1 : 0;
        } else if (key == SPA_PROP_channelVolumes) {
            int nc = 0;
            float mx = rd_chanvols(&o, &nc);
            if (o.ok && nc > 0) { vol = (int)lroundf(cbrtf(mx) * 100.0f); nchan = nc; have_vol = 1; }
        } else {
            rd_skip(&o);
        }
    }
    if (!o.ok) return;
    if (is_sink) { cur_vol = vol; cur_mute = mute; sink_nchan = nchan; if (have_vol || cur_vol >= 0) cur_ok = 1; }
    else         { cur_mic_vol = vol; cur_mic_mute = mute; source_nchan = nchan; }
    publish();
}

/* Node::Info: Struct{ Int id, Int max_in, Int max_out, Long change_mask,
 * Int n_in, Int n_out, Id state, String error, Struct props, ... }. props is a
 * nested dict Struct{ Int n_items, (String key, String value)* } — same shape as
 * a Registry Global's props. We want card.profile.device: the route's `device`
 * field the write must target. */
static void on_node_info(RD *r, int is_sink) {
    RD s; if (!rd_struct(r, &s)) return;
    rd_skip(&s);                       /* id */
    rd_skip(&s); rd_skip(&s);          /* max_in, max_out */
    rd_skip(&s);                       /* change_mask (Long) */
    rd_skip(&s); rd_skip(&s); rd_skip(&s);   /* n_in, n_out, state */
    rd_skip(&s);                       /* error string */
    RD d; if (!rd_struct(&s, &d)) return;    /* props dict */
    int n = rd_int(&d);
    if (!s.ok || !d.ok || n < 0 || n > 4096) return;
    int carddev = -1;
    for (int i = 0; i < n && d.ok; i++) {
        char key[64], val[64];
        rd_str(&d, key, sizeof key);
        if (!strcmp(key, "card.profile.device")) { rd_str(&d, val, sizeof val); carddev = atoi(val); }
        else rd_skip(&d);
    }
    if (!d.ok || carddev < 0) return;
    if (is_sink) sink_card_device = carddev;
    else         source_card_device = carddev;
}

/* Device::Param(Route): Struct{ Int seq, Id id, Int index, Int next, Object route }.
 * Each event carries one route; cache (device, index) so a later write can pick
 * the route whose `device` equals the node's card.profile.device (the two events
 * arrive in either order). route.props volume/mute are ignored — reads come from
 * the node subscription; we only need the index for writes. */
static void on_device_param(RD *r, int is_sink) {
    RD s; if (!rd_struct(r, &s)) return;
    (void)rd_int(&s);                  /* seq */
    uint32_t pid = (uint32_t)rd_int(&s);
    (void)rd_int(&s); (void)rd_int(&s);   /* index, next */
    if (!s.ok || pid != SPA_PARAM_Route) return;
    uint32_t otype; RD o;
    if (!rd_object(&s, &o, &otype) || otype != SPA_OBJECT_ParamRoute) return;

    int route_index = -1, route_device = -2;
    while (o.pos < o.len && o.ok) {
        uint32_t key = rd_u32(&o);
        (void)rd_u32(&o);              /* prop flags */
        if (!o.ok) break;
        if (key == SPA_ROUTE_index)       route_index = rd_int(&o);
        else if (key == SPA_ROUTE_device) route_device = rd_int(&o);
        else rd_skip(&o);
    }
    if (!o.ok || route_index < 0 || route_device < 0) return;
    RouteEnt *tbl = is_sink ? sink_routes : source_routes;
    int *nr = is_sink ? &sink_nroutes : &source_nroutes;
    for (int i = 0; i < *nr; i++)
        if (tbl[i].device == route_device) { tbl[i].index = route_index; return; }
    if (*nr < ROUTES_MAX) { tbl[*nr].device = route_device; tbl[*nr].index = route_index; (*nr)++; }
}

static void handle_msg(uint32_t id, uint32_t opcode, const uint8_t *body, int len) {
    RD r = { body, len, 0, 1 };
    if (id == PW_ID_CORE) {
        switch (opcode) {
        case CORE_EV_PING:      on_core_ping(&r); break;
        case CORE_EV_ERROR:     on_core_error(&r); break;
        case CORE_EV_INFO: case CORE_EV_REMOVE_ID: default: break;
        }
    } else if (id == PW_ID_REGISTRY) {
        if (opcode == REGISTRY_EV_GLOBAL) on_global(&r);
        else if (opcode == REGISTRY_EV_GLOBAL_REMOVE) on_global_remove(&r);
    } else if (id == meta_proxy && meta_proxy) {
        if (opcode == METADATA_EV_PROPERTY) on_metadata_property(&r);
    } else if (id == sink_proxy && sink_proxy) {
        if (opcode == NODE_EV_PARAM) on_node_param(&r, 1);
        else if (opcode == NODE_EV_INFO) on_node_info(&r, 1);
    } else if (id == source_proxy && source_proxy) {
        if (opcode == NODE_EV_PARAM) on_node_param(&r, 0);
        else if (opcode == NODE_EV_INFO) on_node_info(&r, 0);
    } else if (id == sink_dev_proxy && sink_dev_proxy) {
        if (opcode == DEVICE_EV_PARAM) on_device_param(&r, 1);
    } else if (id == source_dev_proxy && source_dev_proxy) {
        if (opcode == DEVICE_EV_PARAM) on_device_param(&r, 0);
    }
}

/* ================================================================== */
/* Socket dispatch                                                     */
/* ================================================================== */

static uint8_t *rb;
static int      rcap, rlen;

void pw_dispatch(void) {
    if (pw_fd < 0) return;
    for (;;) {
        if (rcap - rlen < 4096) {
            int nc = rcap ? rcap * 2 : 8192;
            uint8_t *nn = realloc(rb, nc);
            if (!nn) { pw_drop(2000); return; }
            rb = nn; rcap = nc;
        }
        struct iovec iov = { rb + rlen, (size_t)(rcap - rlen) };
        union { char c[CMSG_SPACE(16 * sizeof(int))]; struct cmsghdr a; } cbuf;
        struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1,
                            .msg_control = cbuf.c, .msg_controllen = sizeof cbuf.c };
        ssize_t n = recvmsg(pw_fd, &m, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (n < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) break; pw_drop(2000); return; }
        if (n == 0) { pw_drop(2000); return; }
        for (struct cmsghdr *h = CMSG_FIRSTHDR(&m); h; h = CMSG_NXTHDR(&m, h))
            if (h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS) {
                int nf = (int)((h->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                int *fds = (int *)(void *)CMSG_DATA(h);
                for (int i = 0; i < nf; i++) close(fds[i]);   /* control client: never used */
            }
        rlen += (int)n;
    }
    int off = 0;
    while (rlen - off >= 16) {
        uint32_t id, w1, size;
        memcpy(&id, rb + off, 4);
        memcpy(&w1, rb + off + 4, 4);
        size = w1 & 0xffffff;
        if ((int64_t)(rlen - off - 16) < (int64_t)size) break;
        handle_msg(id, w1 >> 24, rb + off + 16, (int)size);
        off += 16 + (int)size;
        if (pw_fd < 0) return;         /* a decoder dropped us */
    }
    if (off) { memmove(rb, rb + off, rlen - off); rlen -= off; }
}

/* ================================================================== */
/* Writes                                                              */
/* ================================================================== */

/* Emit the shared inner Object Props (channelVolumes and/or mute), the payload
 * both a node SetParam(Props) and a device SetParam(Route) carry. pct < 0 skips
 * volume; mute < 0 skips mute. channelVolumes is linear = (pct/100)^3, the cubic
 * mapping wireplumber applies (holds above 1.0: 150% → 3.375). */
static void emit_props(POD *p, int pct, int nchan, int mute) {
    if (nchan < 1) nchan = 1;
    if (nchan > 64) nchan = 64;
    int ob = pw_begin_object(p, SPA_OBJECT_Props, SPA_PARAM_Props);
    if (mute >= 0) { pw_prop(p, SPA_PROP_mute); pw_wbool(p, mute); }
    if (pct >= 0) {
        float lin = powf((float)pct / 100.0f, 3.0f);
        pw_prop(p, SPA_PROP_channelVolumes);
        int ar = pw_begin(p, SPA_Array);
        pod_u32(p, 4); pod_u32(p, SPA_Float);
        for (int i = 0; i < nchan; i++) pod_raw(p, &lin, 4);
        pw_end(p, ar);
    }
    pw_end(p, ob);
}

/* The real mixer: SetParam(Route) on the card Device. Node-level Props writes
 * set only software volume and wireplumber does NOT propagate them to the route
 * (wpctl and every other mixer reader would ignore them, and the next route
 * change reverts us) — verified live. save=true persists across restarts, as
 * wpctl does. */
static void device_set_route(uint32_t dev_proxy, int route_index, int card_device,
                             int pct, int nchan, int mute) {
    if (!dev_proxy || route_index < 0) return;
    POD p = {0};
    int st = pw_begin(&p, SPA_Struct);
    pw_wid(&p, SPA_PARAM_Route);
    pw_wint(&p, 0);                    /* flags */
    int ob = pw_begin_object(&p, SPA_OBJECT_ParamRoute, SPA_PARAM_Route);
    pw_prop(&p, SPA_ROUTE_index);  pw_wint(&p, route_index);
    pw_prop(&p, SPA_ROUTE_device); pw_wint(&p, card_device);
    pw_prop(&p, SPA_ROUTE_props);  emit_props(&p, pct, nchan, mute);
    pw_prop(&p, SPA_ROUTE_save);   pw_wbool(&p, 1);
    pw_end(&p, ob);
    pw_end(&p, st);
    pw_send(dev_proxy, DEVICE_SET_PARAM, &p);
}

/* Node-level fallback for virtual nodes (no card Device, e.g. rnnoise source):
 * they ARE the graph endpoint, so a node Props write is the real control. */
static void node_set_param(uint32_t proxy, int pct, int nchan, int mute) {
    if (!proxy) return;
    POD p = {0};
    int st = pw_begin(&p, SPA_Struct);
    pw_wid(&p, SPA_PARAM_Props);
    pw_wint(&p, 0);
    emit_props(&p, pct, nchan, mute);
    pw_end(&p, st);
    pw_send(proxy, NODE_SET_PARAM, &p);
}

/* The route index for a device index, from the cached Route params (-1 = none). */
static int route_index_for(int is_sink, int card) {
    const RouteEnt *tbl = is_sink ? sink_routes : source_routes;
    int nr = is_sink ? sink_nroutes : source_nroutes;
    for (int i = 0; i < nr; i++)
        if (tbl[i].device == card) return tbl[i].index;
    return -1;
}

/* Route where the sink has a hardware Device + a resolved route, node otherwise. */
static void set_side(int is_sink, int pct, int mute) {
    uint32_t dev = is_sink ? sink_dev_proxy : source_dev_proxy;
    int card = is_sink ? sink_card_device : source_card_device;
    int nchan = is_sink ? sink_nchan : source_nchan;
    uint32_t node = is_sink ? sink_proxy : source_proxy;
    int ridx = (dev && card >= 0) ? route_index_for(is_sink, card) : -1;
    if (dev && ridx >= 0) device_set_route(dev, ridx, card, pct, nchan, mute);
    else                  node_set_param(node, pct, nchan, mute);
}

void pw_set_volume(int pct) {
    if (pct < 0) pct = 0;
    set_side(1, pct, 0);               /* unmute on volume change, as media.c did */
}
void pw_set_mute(int on) {
    set_side(1, -1, on < 0 ? !cur_mute : on);
}
void pw_set_mic_mute(int on) {
    set_side(0, -1, on < 0 ? !cur_mic_mute : on);
}

/* ================================================================== */
/* Public getters                                                      */
/* ================================================================== */

int pw_ok(void)          { return cur_ok; }
int pw_vol_pct(void)     { return cur_vol < 0 ? 0 : cur_vol; }
int pw_vol_muted(void)   { return cur_mute; }
int pw_mic_vol_pct(void) { return cur_mic_vol < 0 ? 0 : cur_mic_vol; }
int pw_mic_muted(void)   { return cur_mic_mute; }

/* ================================================================== */
/* Connect / drop / reconnect                                          */
/* ================================================================== */

static void reset_state(void) {
    out_seq = 0; next_id = 3;
    meta_global = meta_proxy = sink_proxy = source_proxy = 0;
    sink_dev_proxy = source_dev_proxy = 0;
    sink_card_device = source_card_device = -1;
    sink_nroutes = source_nroutes = 0;
    sink_name[0] = source_name[0] = 0;
    sink_nchan = source_nchan = 2;
    node_n = 0;
    rlen = 0;
    cur_vol = cur_mic_vol = -1;
    cur_mute = cur_mic_mute = cur_ok = 0;
}

static int pw_connect(void) {
    reset_state();
    const char *rt = getenv("XDG_RUNTIME_DIR");
    const char *rem = getenv("PIPEWIRE_REMOTE");
    if (!rem || !rem[0]) rem = "pipewire-0";
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    if (rem[0] == '/') {
        snprintf(path, sizeof path, "%s", rem);
    } else {
        if (!rt) { msg("wisp: pipewire: no XDG_RUNTIME_DIR"); arm_reconnect(5000); return -1; }
        snprintf(path, sizeof path, "%s/%s", rt, rem);
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) { msg("wisp: pipewire socket: %s", strerror(errno)); arm_reconnect(5000); return -1; }
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0 && errno != EINPROGRESS) {
        msg("wisp: pipewire connect %s: %s", path, strerror(errno));
        close(fd); arm_reconnect(5000); return -1;
    }
    pw_fd = fd;
    /* Fire the handshake; replies are consumed asynchronously in pw_dispatch.
     * A SOCK_NONBLOCK connect still lets these queue in the socket buffer. */
    send_hello();
    send_client_props();
    send_get_registry();
    if (pw_fd < 0) return -1;              /* a send failed and dropped us */
    msg("wisp: pipewire connected");
    return fd;
}

static void arm_reconnect(int delay_ms) {
    if (pw_reconnect_fd < 0) return;
    struct itimerspec ts = {
        .it_value = { .tv_sec = delay_ms / 1000,
                      .tv_nsec = (long)(delay_ms % 1000) * 1000000L },
    };
    timerfd_settime(pw_reconnect_fd, 0, &ts, NULL);
}
static void pw_drop(int delay_ms) {
    if (pw_fd >= 0) { epoll_del_fd(pw_fd); close(pw_fd); pw_fd = -1; }
    reset_state();
    publish();                            /* ok=0 → widget shows its !ok state */
    arm_reconnect(delay_ms);
}

void pw_reconnect(void) {
    if (pw_reconnect_fd < 0) return;
    uint64_t e; (void)!read(pw_reconnect_fd, &e, sizeof e);
    if (pw_fd >= 0) return;
    int fd = pw_connect();
    if (fd < 0) return;                   /* pw_connect armed the retry */
    epoll_add_fd(fd);
}

void pw_init(void) {
    if (pw_reconnect_fd < 0)
        pw_reconnect_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    pw_connect();                         /* best effort; failure arms reconnect */
}

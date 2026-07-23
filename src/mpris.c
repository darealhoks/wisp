/* mpris.c — MPRIS2 media-player client, feeding the DSL `mpris()` source.
 *
 * Pure client: owns no bus name. Players are discovered once at bus-up
 * (ListNames) and tracked live via NameOwnerChanged; state arrives via
 * Properties.GetAll on discovery and PropertiesChanged after that. No
 * polling anywhere — the idle-zero-CPU invariant holds by construction.
 *
 * A player is addressed by its well-known name (org.mpris.MediaPlayer2.foo)
 * for outbound calls, and recognised on inbound signals by its unique owner
 * (":1.42"), which we learn from the sender of its GetAll reply. */

#define _GNU_SOURCE
#include "wisp.h"
#include "dbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MPRIS_PREFIX "org.mpris.MediaPlayer2."
#define MPRIS_PATH   "/org/mpris/MediaPlayer2"
#define MPRIS_PLAYER "org.mpris.MediaPlayer2.Player"

/* ponytail: 4 slots. More concurrent players than that is a browser-tab
 * pathology; the 5th is ignored until one goes away. */
#define MPRIS_MAX 4

typedef struct {
    char name[80];      /* well-known name; empty = free slot */
    char owner[64];     /* unique name, learned from the GetAll reply */
    char title[160];
    char artist[96];
    char status[16];    /* "Playing" / "Paused" / "Stopped" */
} Player;

static Player players[MPRIS_MAX];

extern void wispgen_wisp_state_changed(void) __attribute__((weak));
static void changed(void) {
    if (wispgen_wisp_state_changed) wispgen_wisp_state_changed();
}

/* The one players[] entry the DSL fields expose: whatever is playing, else
 * the first known player. */
static Player *active(void) {
    Player *any = NULL;
    for (int i = 0; i < MPRIS_MAX; i++) {
        if (!players[i].name[0]) continue;
        if (!strcmp(players[i].status, "Playing")) return &players[i];
        if (!any) any = &players[i];
    }
    return any;
}

const char *mpris_title(void)  { Player *p = active(); return p ? p->title  : ""; }
const char *mpris_artist(void) { Player *p = active(); return p ? p->artist : ""; }
const char *mpris_status(void) { Player *p = active(); return p ? p->status : "Stopped"; }
const char *mpris_player(void) {
    Player *p = active();
    return p ? p->name + sizeof MPRIS_PREFIX - 1 : "";
}

static Player *by_name(const char *name) {
    for (int i = 0; i < MPRIS_MAX; i++)
        if (!strcmp(players[i].name, name)) return &players[i];
    return NULL;
}
static Player *by_owner(const char *owner) {
    for (int i = 0; i < MPRIS_MAX; i++)
        if (players[i].name[0] && !strcmp(players[i].owner, owner)) return &players[i];
    return NULL;
}

/* ================================================================== */
/* Property parsing                                                    */
/* ================================================================== */

/* First element of an `as`, then skip to the end of the array. */
static void read_first_str(R *r, char *out, int cap) {
    uint32_t len = ru32(r);
    if (!r->ok) return;
    ralign(r, 4);
    int64_t end = (int64_t)r->pos + (int64_t)len;
    if (end > r->len) { r->ok = 0; return; }
    if (r->pos < end) {
        const char *s = rstr(r);
        if (r->ok) snprintf(out, cap, "%s", s);
    }
    r->pos = (int)end;
}

static void parse_metadata(R *r, Player *p) {
    uint32_t len = ru32(r);
    if (!r->ok) return;
    ralign(r, 8);
    int64_t end = (int64_t)r->pos + (int64_t)len;
    if (end > r->len) { r->ok = 0; return; }
    p->title[0] = p->artist[0] = 0;
    while (r->pos < end && r->ok) {
        ralign(r, 8);
        const char *key = rstr(r);
        const char *vs  = rsig(r);
        if (!r->ok) return;
        if (!strcmp(key, "xesam:title") && !strcmp(vs, "s"))
            snprintf(p->title, sizeof p->title, "%s", rstr(r));
        else if (!strcmp(key, "xesam:artist") && !strcmp(vs, "as"))
            read_first_str(r, p->artist, sizeof p->artist);
        else { const char *s = vs; skip_val(r, &s, 0); }
    }
    if (r->ok) r->pos = (int)end;
}

/* a{sv} of player properties — the body of GetAll and the changed-props arg
 * of PropertiesChanged are the same shape. */
static void parse_props(R *r, Player *p) {
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
        if (!strcmp(key, "PlaybackStatus") && !strcmp(vs, "s"))
            snprintf(p->status, sizeof p->status, "%s", rstr(r));
        else if (!strcmp(key, "Metadata") && !strcmp(vs, "a{sv}"))
            parse_metadata(r, p);
        else { const char *s = vs; skip_val(r, &s, 0); }
    }
}

/* ================================================================== */
/* Discovery                                                           */
/* ================================================================== */

static void on_getall_reply(const char *sender, R *r, const char *sig,
                            int is_err, void *ud) {
    int slot = (int)(long)ud;
    if (slot < 0 || slot >= MPRIS_MAX || !players[slot].name[0]) return;
    if (is_err) { players[slot].name[0] = 0; changed(); return; }
    if (!sig || strcmp(sig, "a{sv}")) return;
    /* ponytail: the slot could in principle have been recycled to a different
     * player before this reply landed; the next PropertiesChanged corrects it. */
    snprintf(players[slot].owner, sizeof players[slot].owner, "%s", sender ? sender : "");
    parse_props(r, &players[slot]);
    changed();
}

static void query_player(const char *name) {
    if (by_name(name)) return;
    int slot = -1;
    for (int i = 0; i < MPRIS_MAX; i++) if (!players[i].name[0]) { slot = i; break; }
    if (slot < 0) return;
    Player *p = &players[slot];
    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", name);
    snprintf(p->status, sizeof p->status, "Stopped");

    W b = {0};
    wstr(&b, MPRIS_PLAYER);
    Msg m = { .type = DBUS_TYPE_METHOD_CALL,
              .path = MPRIS_PATH,
              .interface = "org.freedesktop.DBus.Properties",
              .member = "GetAll",
              .destination = p->name,
              .signature = "s",
              .body = b.b, .body_len = b.pos };
    if (!dbus_call(&m, on_getall_reply, (void *)(long)slot)) p->name[0] = 0;
    free(b.b);
}

static void on_listnames_reply(const char *sender, R *r, const char *sig,
                               int is_err, void *ud) {
    (void)sender; (void)ud;
    if (is_err || !sig || strcmp(sig, "as")) return;
    uint32_t len = ru32(r);
    if (!r->ok) return;
    ralign(r, 4);
    int64_t end = (int64_t)r->pos + (int64_t)len;
    if (end > r->len) return;
    while (r->pos < end && r->ok) {
        const char *n = rstr(r);
        if (!r->ok) return;
        if (!strncmp(n, MPRIS_PREFIX, sizeof MPRIS_PREFIX - 1)) query_player(n);
    }
}

void mpris_on_bus_up(void) {
    memset(players, 0, sizeof players);
    Msg m = { .type = DBUS_TYPE_METHOD_CALL,
              .path = "/org/freedesktop/DBus",
              .interface = "org.freedesktop.DBus",
              .member = "ListNames",
              .destination = "org.freedesktop.DBus" };
    dbus_call(&m, on_listnames_reply, NULL);
}

/* ================================================================== */
/* Signals                                                             */
/* ================================================================== */

static void on_name_owner_changed(const char *sender, const char *path,
                                  const uint8_t *body, int body_len, const char *sig) {
    (void)sender; (void)path;
    if (!sig || strcmp(sig, "sss")) return;
    R r = { .b = body, .len = body_len, .pos = 0, .ok = 1 };
    const char *name = rstr(&r);
    rstr(&r);                       /* old owner */
    const char *new_owner = rstr(&r);
    if (!r.ok) return;
    if (strncmp(name, MPRIS_PREFIX, sizeof MPRIS_PREFIX - 1)) return;
    if (new_owner[0]) { query_player(name); return; }
    Player *p = by_name(name);
    if (p) { p->name[0] = 0; changed(); }
}

static void on_props_changed(const char *sender, const char *path,
                             const uint8_t *body, int body_len, const char *sig) {
    if (!sig || strncmp(sig, "sa{sv}", 6)) return;
    if (strcmp(path, MPRIS_PATH)) return;
    Player *p = by_owner(sender);
    if (!p) return;
    R r = { .b = body, .len = body_len, .pos = 0, .ok = 1 };
    if (strcmp(rstr(&r), MPRIS_PLAYER) || !r.ok) return;
    parse_props(&r, p);
    changed();
}

void mpris_init(void) {
    dbus_subscribe("org.freedesktop.DBus", "NameOwnerChanged", on_name_owner_changed);
    dbus_subscribe("org.freedesktop.DBus.Properties", "PropertiesChanged", on_props_changed);
}

/* ================================================================== */
/* Controls                                                            */
/* ================================================================== */

void mpris_control(const char *member) {
    Player *p = active();
    if (!p) return;
    /* Fire-and-forget: the state change comes back as PropertiesChanged. */
    Msg m = { .type = DBUS_TYPE_METHOD_CALL,
              .flags = 1,
              .path = MPRIS_PATH,
              .interface = MPRIS_PLAYER,
              .member = member,
              .destination = p->name };
    send_msg(&m);
}

/* workspace.c — tag/workspace seam. Owns the ext_workspace_v1 client and
 * picks the backend: ext-workspace if the compositor advertises the global
 * (sway >= 1.11, niri, hyprland, labwc, cosmic, kwin, dwl with the patch),
 * else mango's unix-socket IPC (mango.c).
 *
 * ext-workspace rides the existing wl_display fd, so it adds no poll source
 * and idle stays at 0 ticks. State is double-buffered per the protocol: events
 * accumulate and only manager.done publishes them to the bar. */
#include "wisp.h"

#include <stdio.h>
#include <string.h>

int tags_fd = -1;

#define MAX_WS     64
#define MAX_GROUPS 8

typedef struct {
    uint32_t id;        /* server-allocated object id; 0 = free slot */
    uint32_t group;     /* owning group object id; 0 = unassigned */
    uint32_t state;     /* EXTWS_STATE_* bitfield */
    int      coord;     /* first coordinate, -1 if none sent */
    int      bit;       /* 0-based tag bit, resolved at done */
    char     name[16];
} Ws;

typedef struct {
    uint32_t id;        /* server-allocated object id; 0 = free slot */
    Output  *out;
} Group;

static Ws     wss[MAX_WS];
static Group  groups[MAX_GROUPS];

static Ws    *ws_by_id(uint32_t id) {
    for (int i = 0; i < MAX_WS; i++) if (wss[i].id == id) return &wss[i];
    return NULL;
}
static Group *grp_by_id(uint32_t id) {
    for (int i = 0; i < MAX_GROUPS; i++) if (groups[i].id == id) return &groups[i];
    return NULL;
}

/* Wire string: uint32 len (incl. NUL) + padded bytes. Returns 0 on a malformed
 * or out-of-bounds string (compositor-controlled — never trusted). */
static int wire_str(uint8_t *body, uint32_t bodylen, char *out, size_t outsz) {
    if (bodylen < 4) return 0;
    uint32_t slen = *(uint32_t *)body;
    if (slen == 0 || slen > bodylen - 4 || body[4 + slen - 1] != 0) return 0;
    snprintf(out, outsz, "%.*s", (int)(slen - 1), body + 4);
    return 1;
}

/* "3" -> 3. Returns 0 unless the whole string is a 1..32 decimal — workspace
 * names are free-form, so anything else falls through to positional numbering. */
static int name_to_tag(const char *s) {
    int n = 0;
    if (!*s) return 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        n = n * 10 + (*p - '0');
        if (n > 32) return 0;
    }
    return n;
}

/* Publish the accumulated state: one (occupied, active, urgent) mask per
 * output.
 *
 * ponytail: ext-workspace has no client count, so "occupied" degrades to "the
 * workspace exists and isn't hidden" — the compositor's own answer to which
 * workspaces are worth showing. Verified against niri: it reports no `hidden`
 * and no occupancy, and materializes workspaces on demand, so the bar tracks
 * its list exactly (incl. niri's trailing empty workspace, as its own bars do).
 * No upgrade path in v1: ext-foreign-toplevel-list lists toplevels but never
 * says which workspace they're on. */
static void extws_publish(void) {
    for (int g = 0; g < MAX_GROUPS; g++) {
        if (!groups[g].id || !groups[g].out) continue;
        uint32_t occ = 0, act = 0, urg = 0;
        int seq = 0;

        for (int i = 0; i < MAX_WS; i++) {
            Ws *w = &wss[i];
            if (!w->id || w->group != groups[g].id) continue;
            /* "clients should not display such workspaces" — protocol text. */
            if (w->state & EXTWS_STATE_HIDDEN) continue;
            /* Prefer the name as a tag number (1..32), then the compositor's
             * coordinate, then arrival order — so "1".."9" bars line up on
             * sway/niri/hyprland while unnamed workspaces still get a slot. */
            int n = name_to_tag(w->name);
            if (!n && w->coord >= 0 && w->coord < 32) n = w->coord + 1;
            if (!n) n = ++seq;
            if (n < 1 || n > 32) continue;
            w->bit = n - 1;
            uint32_t bit = 1u << w->bit;
            occ |= bit;
            if (w->state & EXTWS_STATE_ACTIVE) act |= bit;
            if (w->state & EXTWS_STATE_URGENT) urg |= bit;
        }
#ifdef WISP_HAS_BAR
        bar_set_tags_on(groups[g].out, occ, act, urg);
#else
        (void)occ; (void)act; (void)urg;
#endif
    }
}

int extws_handle_event(uint32_t obj, uint16_t op, uint8_t *body, uint32_t bodylen) {
    if (!id_extws_mgr) return 0;

    if (obj == id_extws_mgr) {
        if (op == EXTWS_MGR_EV_GROUP || op == EXTWS_MGR_EV_WORKSPACE) {
            if (bodylen < 4) return 1;
            uint32_t nid = *(uint32_t *)body;
            if (op == EXTWS_MGR_EV_GROUP) {
                Group *g = grp_by_id(0);
                if (g) { g->id = nid; g->out = NULL; }
            } else {
                Ws *w = ws_by_id(0);
                if (w) {
                    memset(w, 0, sizeof *w);
                    w->id = nid; w->coord = -1;
                }
            }
        } else if (op == EXTWS_MGR_EV_DONE) {
            extws_publish();
        } else if (op == EXTWS_MGR_EV_FINISHED) {
            id_extws_mgr = 0;
            msg("wisp: ext-workspace finished (no tags)");
        }
        return 1;
    }

    Group *g = grp_by_id(obj);
    if (g) {
        if (bodylen >= 4) {
            uint32_t arg = *(uint32_t *)body;
            if (op == EXTWS_GRP_EV_OUTPUT_ENTER)      g->out = output_by_wl(arg);
            else if (op == EXTWS_GRP_EV_OUTPUT_LEAVE) g->out = NULL;
            else if (op == EXTWS_GRP_EV_WS_ENTER) {
                Ws *w = ws_by_id(arg); if (w) w->group = g->id;
            } else if (op == EXTWS_GRP_EV_WS_LEAVE) {
                Ws *w = ws_by_id(arg); if (w && w->group == g->id) w->group = 0;
            }
        }
        if (op == EXTWS_GRP_EV_REMOVED) {
            for (int i = 0; i < MAX_WS; i++)
                if (wss[i].group == g->id) wss[i].group = 0;
            g->id = 0; g->out = NULL;
        }
        return 1;
    }

    Ws *w = ws_by_id(obj);
    if (w) {
        switch (op) {
        case EXTWS_WS_EV_NAME:
            wire_str(body, bodylen, w->name, sizeof w->name);
            break;
        case EXTWS_WS_EV_COORDINATES:
            /* array: uint32 byte-length + padded u32 elements. Only the first
             * (most significant) axis maps onto a linear tag bar. */
            if (bodylen >= 8 && *(uint32_t *)body >= 4) w->coord = (int)*(uint32_t *)(body + 4);
            break;
        case EXTWS_WS_EV_STATE:
            if (bodylen >= 4) w->state = *(uint32_t *)body;
            break;
        case EXTWS_WS_EV_REMOVED:
            w->id = 0;
            break;
        default: break;   /* id, capabilities: unused */
        }
        return 1;
    }
    return 0;
}

static void extws_view(Output *o, int idx) {
    for (int i = 0; i < MAX_WS; i++) {
        Ws *w = &wss[i];
        if (!w->id || w->bit != idx - 1) continue;
        Group *g = grp_by_id(w->group);
        /* Prefer the workspace on the requested output; a bar click passes the
         * clicked monitor, so the right one switches on multi-head. */
        if (o && g && g->out && g->out != o) continue;
        wl_req(w->id, EXTWS_WS_REQ_ACTIVATE, NULL, 0, -1);
        wl_req(id_extws_mgr, EXTWS_MGR_REQ_COMMIT, NULL, 0, -1);
        return;
    }
}

/* mango first: its IPC reports client_count, so the bar can tell an empty tag
 * from an occupied one — ext-workspace has no equivalent. ext-workspace is the
 * portable fallback for every other compositor (mango also advertises it). */
void tags_init(void) {
    mango_init();
    tags_fd = mango_fd;
    if (mango_fd >= 0) { id_extws_mgr = 0; return; }
    if (id_extws_mgr) {
        for (int i = 0; i < MAX_WS; i++) wss[i].coord = -1;
        return;   /* handles stream in on their own; no fd to poll */
    }
    msg("wisp: no workspace backend (no ext_workspace_v1, no mango ipc)");
}

void tags_dispatch(void) {
    if (mango_fd < 0) return;   /* ext-workspace has no fd of its own */
    mango_dispatch();
    tags_fd = mango_fd;   /* mango.c clears it to -1 when the socket closes */
}

void tags_view(Output *o, int idx) {
    if (idx < 1 || idx > 32) return;
    if (id_extws_mgr) extws_view(o, idx);
    else              mango_view_tag(o, idx);
}

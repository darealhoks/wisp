/* wl_toplevel.c — zwlr_foreign_toplevel_management client feeding the DSL
 * `toplevel()` source: event-driven app-aliveness (is app_id X open?), with no
 * fd, poll, or timer of its own — it rides the main wl_display connection
 * (wl.c) as one more bound global. Idle stays at 0 ticks.
 *
 * The compositor streams a `toplevel` (new handle) event per open window, then
 * per-handle `app_id`/`title`/…/`done` events, and a `closed` event when a
 * window goes away. State is double-buffered per the protocol: per-handle
 * fields accumulate and are only made visible to the DSL on `done` (atomicity).
 *
 * We track every toplevel minimally (app_id arrives *after* the handle is
 * created, so we can't pre-filter), match each against a fixed table codegen
 * emits (stage 2) — `tl_match_app_ids` / `tl_n_matches` — and publish, per
 * match slot, a live count and the title of the most recent matching window.
 * On any published change we ping wispgen_wisp_state_changed() (same repaint
 * path as mpris.c / pipewire.c). Absent global ⇒ all exists=0, one stderr line;
 * loud, not fatal (gamma is the precedent for an optional manager). */
#include "wisp.h"
#include "proto.h"

#include <stdio.h>
#include <string.h>

/* The match table — emitted by wispc (stage 2) alongside WISP_HAS_TOPLEVEL,
 * so it always exists whenever this TU is compiled. */
extern const char *const tl_match_app_ids[];
extern const int tl_n_matches;

#define TL_MAX        64    /* tracked live toplevels (all apps) */
#define TL_MATCH_MAX  16    /* published match slots; caps tl_n_matches */
#define TL_TITLE_CAP  128

uint32_t id_toplevel_mgr;
static uint32_t tl_mgr_name;   /* registry global name, for the remove path */

typedef struct {
    uint32_t id;                 /* handle object id; 0 = free slot */
    int      match;              /* index into the match table, -1 = no match */
    char     title[TL_TITLE_CAP];
} TLSlot;
static TLSlot slots[TL_MAX];

/* Published (post-`done`) per-match state the getters read. */
static int  cur_count[TL_MATCH_MAX], pub_count[TL_MATCH_MAX];
static char cur_title[TL_MATCH_MAX][TL_TITLE_CAP];
static char pub_title[TL_MATCH_MAX][TL_TITLE_CAP];

extern void wispgen_wisp_state_changed(void) __attribute__((weak));

static int nmatch(void) {
    return tl_n_matches > TL_MATCH_MAX ? TL_MATCH_MAX : tl_n_matches;
}

static TLSlot *slot_by_id(uint32_t id) {
    for (int i = 0; i < TL_MAX; i++)
        if (slots[i].id == id) return &slots[i];
    return NULL;
}

/* Recompute cur_* from the slot table and ping the repaint path if anything a
 * DSL field exposes actually moved. Cheap full scan (TL_MAX * matches). */
static void publish(void) {
    int n = nmatch();
    for (int m = 0; m < n; m++) { cur_count[m] = 0; cur_title[m][0] = 0; }
    uint32_t best[TL_MATCH_MAX] = {0};   /* highest handle id per match = most recent */
    for (int i = 0; i < TL_MAX; i++) {
        TLSlot *s = &slots[i];
        if (!s->id || s->match < 0 || s->match >= n) continue;
        cur_count[s->match]++;
        if (s->id > best[s->match]) {
            best[s->match] = s->id;
            snprintf(cur_title[s->match], TL_TITLE_CAP, "%s", s->title);
        }
    }
    int changed = 0;
    for (int m = 0; m < n; m++) {
        if (cur_count[m] != pub_count[m] || strcmp(cur_title[m], pub_title[m])) {
            pub_count[m] = cur_count[m];
            snprintf(pub_title[m], TL_TITLE_CAP, "%s", cur_title[m]);
            changed = 1;
        }
    }
    if (changed && wispgen_wisp_state_changed) wispgen_wisp_state_changed();
}

void tl_bind(uint32_t name, uint32_t ver) {
    if (id_toplevel_mgr) return;
    id_toplevel_mgr = wl_new_id();
    tl_mgr_name = name;
    uint32_t v = ver < 3 ? ver : 3;   /* we read only <=v1 events; bind current */
    wl_registry_bind(name, "zwlr_foreign_toplevel_manager_v1", v, id_toplevel_mgr);
}

static void tl_reset(void) {
    memset(slots, 0, sizeof slots);
    id_toplevel_mgr = 0;
    publish();   /* all exists=0 */
}

void tl_on_registry_remove(uint32_t name) {
    if (!id_toplevel_mgr || name != tl_mgr_name) return;
    tl_reset();
    msg("wisp: foreign-toplevel manager gone");
}

/* Match a handle's app_id against the table; -1 if none. */
static int match_app_id(const char *app_id) {
    int n = nmatch();
    for (int i = 0; i < n; i++)
        if (tl_match_app_ids[i] && !strcmp(tl_match_app_ids[i], app_id)) return i;
    return -1;
}

int tl_handle_event(uint32_t obj, uint16_t op, uint8_t *body, uint32_t bodylen) {
    if (!id_toplevel_mgr) return 0;

    if (obj == id_toplevel_mgr) {
        if (op == TL_MGR_EV_TOPLEVEL) {
            if (bodylen < 4) return 1;
            uint32_t nid = *(uint32_t *)body;   /* server-allocated handle id */
            TLSlot *s = slot_by_id(0);
            if (s) { s->id = nid; s->match = -1; s->title[0] = 0; }
            /* Table full: the toplevel is untracked, so it can never match —
             * count stays correct for the apps we do track. */
        } else if (op == TL_MGR_EV_FINISHED) {
            tl_reset();
        }
        return 1;
    }

    TLSlot *s = slot_by_id(obj);
    if (!s) return 0;

    switch (op) {
    case TL_HANDLE_EV_TITLE: {
        /* wire string: u32 len (incl NUL) + padded bytes; truncate to cap. */
        if (bodylen < 4) break;
        uint32_t slen = *(uint32_t *)body;
        if (slen == 0 || slen > bodylen - 4 || body[4 + slen - 1] != 0) break;
        snprintf(s->title, TL_TITLE_CAP, "%.*s", (int)(slen - 1), body + 4);
        break;
    }
    case TL_HANDLE_EV_APP_ID: {
        if (bodylen < 4) break;
        uint32_t slen = *(uint32_t *)body;
        if (slen == 0 || slen > bodylen - 4 || body[4 + slen - 1] != 0) break;
        s->match = match_app_id((const char *)(body + 4));
        break;
    }
    case TL_HANDLE_EV_DONE:
        publish();   /* atomic commit point */
        break;
    case TL_HANDLE_EV_CLOSED:
        /* Finalize per the protocol: destroy the handle, free the slot. */
        wl_req(s->id, TL_HANDLE_REQ_DESTROY, NULL, 0, -1);
        s->id = 0; s->match = -1; s->title[0] = 0;
        publish();
        break;
    default:
        break;   /* state / output_enter/leave / parent: unused */
    }
    return 1;
}

/* ---- getters consumed by generated DSL field expressions (stage 2) ---- */
int tl_count(int m)  { return (m >= 0 && m < nmatch()) ? cur_count[m] : 0; }
int tl_exists(int m) { return tl_count(m) > 0; }
const char *tl_title(int m) {
    return (m >= 0 && m < nmatch()) ? cur_title[m] : "";
}

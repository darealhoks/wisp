/* river.c — river's zriver_status_manager_v1 workspace source. river tags are
 * already 32-bit bitmasks, near 1:1 with wisp's occ/act/urg, so no
 * name-to-number guessing (unlike ext-workspace). Like ext-workspace it rides
 * the main wl_display fd — one status object per output, compositor-pushed
 * events, no extra poll source, idle stays at 0 ticks. Tag switches go out
 * through zriver_control_v1 (`set-focused-tags`). */
#include "wisp.h"
#include "proto.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t status;          /* zriver_output_status_v1 id; 0 = free slot */
    Output  *out;
    uint32_t occ, act, urg;
} RvOut;
static RvOut rvs[MAX_OUTPUTS];

static RvOut *rv_by_status(uint32_t id) {
    for (int i = 0; i < MAX_OUTPUTS; i++) if (rvs[i].status == id) return &rvs[i];
    return NULL;
}

/* Ask river for one output_status per live output. Called from tags_init after
 * the registry sync, so every wl_output is already bound.
 * ponytail: enumerates outputs present at startup only; a monitor hot-plugged
 * later won't report tags. Add a river_on_output_added() hook off
 * output_init_widgets if multi-head hotplug on river ever matters. */
void river_init(void) {
    if (!id_river_status_mgr) return;
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        Output *o = &outputs[i];
        if (!o->active || !o->wl_output) continue;
        RvOut *r = rv_by_status(0);
        if (!r) break;
        r->status = wl_new_id();
        r->out = o;
        r->occ = r->act = r->urg = 0;
        uint32_t args[2] = { r->status, o->wl_output };
        wl_req(id_river_status_mgr, RIVER_STATUS_MGR_REQ_GET_OUTPUT, args, 2, -1);
    }
}

static void river_publish(RvOut *r) {
#ifdef WISP_HAS_BAR
    if (r->out) bar_set_tags_on(r->out, r->occ, r->act, r->urg);
#else
    (void)r;
#endif
}

int river_handle_event(uint32_t obj, uint16_t op, uint8_t *body, uint32_t bodylen) {
    if (!id_river_status_mgr) return 0;
    RvOut *r = rv_by_status(obj);
    if (!r) return 0;

    switch (op) {
    case RIVER_OUTPUT_EV_FOCUSED_TAGS:
        if (bodylen >= 4) r->act = *(uint32_t *)body;
        break;
    case RIVER_OUTPUT_EV_VIEW_TAGS: {
        /* array: uint32 byte-length + that many bytes of padded u32 elements,
         * one tag mask per view on this output; OR them for occupancy. */
        if (bodylen < 4) break;
        uint32_t n = *(uint32_t *)body;
        if (n > bodylen - 4) break;
        uint32_t occ = 0;
        for (uint32_t off = 0; off + 4 <= n; off += 4)
            occ |= *(uint32_t *)(body + 4 + off);
        r->occ = occ;
        break;
    }
    case RIVER_OUTPUT_EV_URGENT_TAGS:
        if (bodylen >= 4) r->urg = *(uint32_t *)body;
        break;
    default:
        return 1;   /* layout_name / layout_name_clear: consumed, no tag change */
    }
    river_publish(r);
    return 1;
}

/* ponytail: river's `set-focused-tags` acts on the focused output, so `o` is
 * advisory-only — single-laptop. Prefix a `set-focused-output` command keyed
 * off o->name if a second monitor ever shows up. */
void river_view_tag(Output *o, int idx) {
    (void)o;
    if (!id_river_control || idx < 1 || idx > 32) return;
    char mask[12];
    snprintf(mask, sizeof mask, "%u", 1u << (idx - 1));
    wl_req_str(id_river_control, RIVER_CONTROL_REQ_ADD_ARGUMENT, NULL, 0,
               "set-focused-tags", NULL, 0);
    wl_req_str(id_river_control, RIVER_CONTROL_REQ_ADD_ARGUMENT, NULL, 0,
               mask, NULL, 0);
    /* run_command(seat, callback). ponytail: leak one callback id per switch,
     * we don't read the success/failure reply. */
    uint32_t args[2] = { id_seat, wl_new_id() };
    wl_req(id_river_control, RIVER_CONTROL_REQ_RUN_COMMAND, args, 2, -1);
}

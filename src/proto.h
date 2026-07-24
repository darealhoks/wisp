/* Raw Wayland wire opcodes — hand-extracted from the protocol XML.
 * Keep this file small; only what wisp actually uses. */
#ifndef WISP_PROTO_H
#define WISP_PROTO_H

/* Fixed object ids */
#define ID_DISPLAY  1u
#define ID_REGISTRY 2u

/* wl_display events */
#define DISPLAY_EV_ERROR     0
#define DISPLAY_EV_DELETE_ID 1

/* wl_display requests */
#define DISPLAY_REQ_SYNC         0
#define DISPLAY_REQ_GET_REGISTRY 1

/* wl_registry */
#define REGISTRY_REQ_BIND      0
#define REGISTRY_EV_GLOBAL     0
#define REGISTRY_EV_GLOBAL_REM 1

/* wl_callback */
#define CALLBACK_EV_DONE 0

/* wl_compositor */
#define COMPOSITOR_REQ_CREATE_SURFACE 0
#define COMPOSITOR_REQ_CREATE_REGION  1

/* wl_region */
#define REGION_REQ_DESTROY  0
#define REGION_REQ_ADD      1
#define REGION_REQ_SUBTRACT 2

/* wl_shm */
#define SHM_REQ_CREATE_POOL 0

/* wl_shm_pool */
#define POOL_REQ_CREATE_BUFFER 0
#define POOL_REQ_DESTROY       1
#define POOL_REQ_RESIZE        2

/* wl_buffer */
#define BUFFER_REQ_DESTROY 0
#define BUFFER_EV_RELEASE  0

/* wl_surface */
#define SURFACE_REQ_DESTROY        0
#define SURFACE_REQ_ATTACH         1
#define SURFACE_REQ_DAMAGE         2
#define SURFACE_REQ_FRAME          3
#define SURFACE_REQ_COMMIT         6
#define SURFACE_REQ_SET_BUFFER_SCALE 8   /* since wl_surface v3 */
#define SURFACE_REQ_DAMAGE_BUFFER  9

/* wl_seat */
#define SEAT_REQ_GET_POINTER  0
#define SEAT_REQ_GET_KEYBOARD 1
#define SEAT_EV_CAPABILITIES  0
#define SEAT_EV_NAME          1
#define SEAT_CAP_POINTER  1
#define SEAT_CAP_KEYBOARD 2

/* wl_pointer */
#define POINTER_REQ_SET_CURSOR 0

/* wl_output */
#define OUTPUT_EV_GEOMETRY 0
#define OUTPUT_EV_MODE     1
#define OUTPUT_EV_DONE     2
#define OUTPUT_EV_SCALE    3
#define OUTPUT_EV_NAME     4   /* since wl_output v4 */

/* zwlr_layer_shell_v1 */
#define LAYER_SHELL_REQ_GET_LAYER_SURFACE 0
#define LAYER_SHELL_REQ_DESTROY           1
#define LAYER_BACKGROUND 0
#define LAYER_BOTTOM     1
#define LAYER_TOP        2
#define LAYER_OVERLAY    3

/* zwlr_layer_surface_v1 */
#define LS_REQ_SET_SIZE                  0
#define LS_REQ_SET_ANCHOR                1
#define LS_REQ_SET_EXCLUSIVE_ZONE        2
#define LS_REQ_SET_MARGIN                3
#define LS_REQ_SET_KEYBOARD_INTERACTIVITY 4
#define LS_REQ_ACK_CONFIGURE             6
#define LS_REQ_DESTROY                   7
#define LS_EV_CONFIGURE 0
#define LS_EV_CLOSED    1
#define LS_ANCHOR_TOP    1
#define LS_ANCHOR_BOTTOM 2
#define LS_ANCHOR_LEFT   4
#define LS_ANCHOR_RIGHT  8

/* xdg_wm_base */
#define WM_BASE_REQ_DESTROY          0
#define WM_BASE_REQ_CREATE_POSITIONER 1
#define WM_BASE_REQ_GET_XDG_SURFACE  2
#define WM_BASE_REQ_PONG             3
#define WM_BASE_EV_PING              0

/* wl_shm pixel format */
#define WL_SHM_FORMAT_XRGB8888 1
#define WL_SHM_FORMAT_ARGB8888 0

/* zwlr_gamma_control_manager_v1 — night-mode color temperature. */
#define GAMMA_MGR_REQ_GET_GAMMA_CONTROL 0
#define GAMMA_MGR_REQ_DESTROY           1
/* zwlr_gamma_control_v1 */
#define GAMMA_CTRL_REQ_SET_GAMMA 0
#define GAMMA_CTRL_REQ_DESTROY   1
#define GAMMA_CTRL_EV_GAMMA_SIZE 0
#define GAMMA_CTRL_EV_FAILED     1

/* ext_session_lock_manager_v1 / ext_session_lock_v1 / ext_session_lock_surface_v1 */
#define SLOCK_MGR_REQ_DESTROY 0
#define SLOCK_MGR_REQ_LOCK    1
#define SLOCK_REQ_DESTROY            0
#define SLOCK_REQ_GET_LOCK_SURFACE   1
#define SLOCK_REQ_UNLOCK_AND_DESTROY 2
#define SLOCK_EV_LOCKED   0
#define SLOCK_EV_FINISHED 1
#define SLOCK_SURF_REQ_DESTROY        0
#define SLOCK_SURF_REQ_ACK_CONFIGURE  1
#define SLOCK_SURF_EV_CONFIGURE       0

/* ext_workspace_v1 — the cross-compositor workspace protocol (workspace.c).
 * Server-created handles arrive with server-allocated ids (>= 0xff000000), so
 * they can't collide with our wl_new_id() space. */
#define EXTWS_MGR_REQ_COMMIT    0
#define EXTWS_MGR_REQ_STOP      1
#define EXTWS_MGR_EV_GROUP      0
#define EXTWS_MGR_EV_WORKSPACE  1
#define EXTWS_MGR_EV_DONE       2
#define EXTWS_MGR_EV_FINISHED   3
/* ext_workspace_group_handle_v1 */
#define EXTWS_GRP_EV_CAPABILITIES 0
#define EXTWS_GRP_EV_OUTPUT_ENTER 1
#define EXTWS_GRP_EV_OUTPUT_LEAVE 2
#define EXTWS_GRP_EV_WS_ENTER     3
#define EXTWS_GRP_EV_WS_LEAVE     4
#define EXTWS_GRP_EV_REMOVED      5
/* ext_workspace_handle_v1 */
#define EXTWS_WS_REQ_ACTIVATE  1
#define EXTWS_WS_EV_ID          0
#define EXTWS_WS_EV_NAME        1
#define EXTWS_WS_EV_COORDINATES 2
#define EXTWS_WS_EV_STATE       3
#define EXTWS_WS_EV_CAPABILITIES 4
#define EXTWS_WS_EV_REMOVED     5
#define EXTWS_STATE_ACTIVE 1
#define EXTWS_STATE_URGENT 2
#define EXTWS_STATE_HIDDEN 4

/* river-status-unstable-v1 / river-control-unstable-v1 (river.c). We request one
 * zriver_output_status_v1 per output (client-allocated id) and switch tags via
 * zriver_control_v1. Both interfaces open with a `destroy` destructor at opcode
 * 0, so the getters/commands start at 1. */
#define RIVER_STATUS_MGR_REQ_GET_OUTPUT  1   /* get_river_output_status(new_id, wl_output) */
#define RIVER_OUTPUT_EV_FOCUSED_TAGS     0
#define RIVER_OUTPUT_EV_VIEW_TAGS        1   /* array: one tag mask per view */
#define RIVER_OUTPUT_EV_URGENT_TAGS      2   /* since status-manager v2 */
#define RIVER_CONTROL_REQ_ADD_ARGUMENT   1
#define RIVER_CONTROL_REQ_RUN_COMMAND    2

/* zwlr_foreign_toplevel_management_unstable_v1 (v3) — wl_toplevel.c. Handle
 * objects arrive with server-allocated ids (>= 0xff000000), same as ext-ws. */
#define TL_MGR_REQ_STOP       0
#define TL_MGR_EV_TOPLEVEL    0
#define TL_MGR_EV_FINISHED    1
/* zwlr_foreign_toplevel_handle_v1 (destroy is the 8th request, index 7) */
#define TL_HANDLE_REQ_DESTROY 7
#define TL_HANDLE_EV_TITLE        0
#define TL_HANDLE_EV_APP_ID       1
#define TL_HANDLE_EV_OUTPUT_ENTER 2
#define TL_HANDLE_EV_OUTPUT_LEAVE 3
#define TL_HANDLE_EV_STATE        4
#define TL_HANDLE_EV_DONE         5
#define TL_HANDLE_EV_CLOSED       6
#define TL_STATE_ACTIVATED        2   /* enum value in the state array */

#endif

#ifdef WISP_FRACTIONAL
/* wp_viewporter / wp_viewport (v1) + wp_fractional_scale_manager_v1 (v1).
 * Fractional scaling means: buffer_scale stays 1, the buffer is round(logical *
 * scale/120) physical pixels, and the viewport maps it back onto the logical
 * size. preferred_scale is the compositor's per-surface hint, in 120ths. */
#define VIEWPORTER_REQ_GET_VIEWPORT   1
#define VIEWPORT_REQ_DESTROY          0
#define VIEWPORT_REQ_SET_DESTINATION  2
#define FRAC_MGR_REQ_GET_FRACTIONAL_SCALE 1
#define FRAC_REQ_DESTROY              0
#define FRAC_EV_PREFERRED_SCALE       0
#endif

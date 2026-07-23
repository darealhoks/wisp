/* wisp — user-facing knobs. Self-contained (does NOT include dwlarp config). */
#ifndef WISP_CONFIG_H
#define WISP_CONFIG_H

/* ---------- Font ----------
 * Backend + font file are build-time choices, not runtime knobs: pick them via
 * `//! font_backend = …` / `//! font = …` directives in the `.wisp`, or
 * `make FONT_BACKEND={truetype,bitmap} FONT=/path/to/font`. bitmap rasterizes
 * PSF/BDF at build time into the generated bake.h; truetype (default)
 * rasterizes TTF/OTF at runtime (src/tt/) and reads $WISP_FONT, falling back
 * to the FONT= path baked in as WISP_FONT_DEFAULT_PATH.
 * Glyph/Font types live in src/font.h. */

/* ---------- Bar ---------- */
#define BAR_HEIGHT      28

/* ---------- HUD (generic slide-in shell) ----------
 * Contents are DSL-driven (codegen-emitted render/click per reveal_on_hover
 * surface). Only animation/timing knobs live here. */
#define HUD_HIDE_DELAY_MS  30
#define HUD_CLICK_GRACE_MS 100
#define HUD_ANIM_TAU_MS    50.0
#define HUD_ANIM_EPSILON   0.5

/* ---------- Menu (dmenu-wl clone, horizontal top bar) ---------- */

#ifndef MENU_HEIGHT
#define MENU_HEIGHT     28            /* dmenu-wl -h 28 */
#endif

/* Vertical (launcher) mode — enabled by `axis = vertical;` on the menu
 * template. The menu becomes a top-centered float instead of a full-width
 * strip; all values overridable from the DSL via gen_overrides.h. */
#ifndef MENU_VERTICAL
#define MENU_VERTICAL   0
#endif
#ifndef MENU_W
#define MENU_W          560           /* float width */
#endif
#ifndef MENU_MARGIN
#define MENU_MARGIN     0             /* gap below the top screen edge */
#endif
#ifndef MENU_MAX_VIS
#define MENU_MAX_VIS    5             /* rows shown before scrolling */
#endif
#ifndef MENU_ROW_H
#define MENU_ROW_H      0             /* 0 = font line_h + 10 */
#endif
#ifndef MENU_PAD_Y
#define MENU_PAD_Y      6             /* top/bottom body inset (mirrors the template's pad_y) */
#endif
#ifndef MENU_GAP
#define MENU_GAP        0             /* gap between the clicked bar cell and the popup */
#endif
#ifndef MENU_HDR_H
#define MENU_HDR_H      0             /* body height above the rows (query line); 0 = rows start at pad_y */
#endif
#ifndef MENU_TERMINAL
#define MENU_TERMINAL   "foot -e"     /* prefix for .desktop Terminal=true apps */
#endif
#ifndef MENU_ICONS
#define MENU_ICONS      0             /* app-icon decode: off by default, it is the launcher's fattest RAM/IO cost */
#endif
#ifndef MENU_SORT_FREQ
#define MENU_SORT_FREQ  1             /* apps launcher: most-used first (0 = alphabetical) */
#endif

/* ---------- Status sampling ---------- */
#define VPN_STALE_S        180
#define STATUS_CADENCE_WIFI   5
#define STATUS_CADENCE_BAT   30
#define STATUS_CADENCE_DISK 300
/* CPU temp and VPN state change on the order of seconds, not the 1Hz status
 * tick — coarser cadence cuts idle syscalls without affecting visible state. */
#define STATUS_CADENCE_TEMP   2
#define STATUS_CADENCE_VPN    5

/* Low-battery notification thresholds. Fires once when bat_pct crosses each
 * threshold downward while discharging; charging back above clears the latch
 * so the next discharge will fire again. */
#define BAT_WARN_PCT  15
#define BAT_CRIT_PCT   5

/* Tag labels (9). */
#define TAG_LABELS { "1","2","3","4","5","6","7","8","9" }

#define CTL_SOCK_NAME "wisp.sock"


/* ---------- Wallpaper ----------
 * PNG decoded once at first configure. If the file is missing or the decoder
 * rejects it, the background falls back to WALL_BG (solid). */
#ifndef WALL_PATH
#define WALL_PATH "~/.local/share/dwl/wallpaper.png"
#endif
#ifndef WALL_BG
#define WALL_BG   0xff0f1219u
#endif
/* Crossfade length for `wispctl wall <path>` (ms). */
#ifndef WALL_FADE_MS
#define WALL_FADE_MS 300
#endif
/* How the two frames are combined per tick: cross-dissolve or block reveal. */
#define WALL_TRANSITION_FADE   0
#define WALL_TRANSITION_DITHER 1
#ifndef WALL_TRANSITION
#define WALL_TRANSITION WALL_TRANSITION_FADE
#endif
#ifndef WALL_DITHER_PX
#define WALL_DITHER_PX 16
#endif

/* ---------- OSD / notifications (mako + dwl-osd replacement) ----------
 * Top-center stack. Each slab fixed-size, rendered into one tall surface;
 * inactive slots are transparent so the unused area is click-through.
 * Colors transcribed from the prior mako config to keep a consistent look. */
#ifndef OSD_W
#define OSD_W              340
#endif
#ifndef OSD_SLAB_H
#define OSD_SLAB_H          60
#endif
#ifndef OSD_GAP
#define OSD_GAP              8
#endif
#ifndef OSD_TOP_MARGIN
/* >0 selects the floating top-center stack (gap below the bar zone);
 * 0 keeps the flush-under-bar layout with fillets + bar cutout. */
#define OSD_TOP_MARGIN     0
#endif
#ifndef OSD_PROG_H
#define OSD_PROG_H          10   /* thicker, easier to see at a glance */
#endif
#ifndef OSD_PROG_TRACK_BG
#define OSD_PROG_TRACK_BG   0xff1c2733u  /* dim trough behind the fill */
#endif
#ifndef OSD_PAD_X
#define OSD_PAD_X           14
#endif
#ifndef OSD_ICON_GAP
#define OSD_ICON_GAP        12
#endif

#ifndef OSD_BG
#define OSD_BG          0xff0f1219u   /* solid; wallpaper shadow tone */
#endif
#ifndef OSD_FG
#define OSD_FG          0xffc8e8f0u
#endif
#ifndef OSD_BORDER
#define OSD_BORDER      0xff5f8a93u   /* wallpaper misty teal */
#endif
#ifndef OSD_RADIUS
#define OSD_RADIUS      10
#endif
#ifndef OSD_PROG_FG
#define OSD_PROG_FG     0xff84a7b3u   /* wallpaper bright teal */
#endif

/* Default timeouts (ms). Critical sticky (caller passes 0). */
#ifndef OSD_TIMEOUT_LOW
#define OSD_TIMEOUT_LOW       3000
#endif
#ifndef OSD_TIMEOUT_NORMAL
#define OSD_TIMEOUT_NORMAL    5000
#endif
#ifndef OSD_TIMEOUT_OSD
#define OSD_TIMEOUT_OSD       1200
#endif

/* ---------- Gamma / night mode (wlsunset replacement) ----------
 * Hard-step at NIGHT_HOUR (warm) and DAY_HOUR (cool). FLAT_K is the
 * HUD-button override (manual warm; intentionally warmer than NIGHT_K
 * so the toggle is visually distinct from scheduled night). */
#ifndef GAMMA_DAY_K
#define GAMMA_DAY_K     6500
#endif
#ifndef GAMMA_NIGHT_K
#define GAMMA_NIGHT_K   2800
#endif
#ifndef GAMMA_FLAT_K
#define GAMMA_FLAT_K    2400
#endif
#ifndef GAMMA_DAY_HOUR
#define GAMMA_DAY_HOUR  7
#endif
#ifndef GAMMA_NIGHT_HOUR
#define GAMMA_NIGHT_HOUR 20
#endif

/* ---------- Session lock (swaylock replacement) ----------
 * PAM service to authenticate against. "system-auth" is the conventional
 * shared stack used by login/sudo/su. "swaylock" works too if it exists. */
#ifndef LOCK_PAM_SERVICE
#define LOCK_PAM_SERVICE "system-auth"
#endif
#ifndef LOCK_HELPER_BIN
#define LOCK_HELPER_BIN  "wisp-lock-helper"
#endif
#ifndef LOCK_BG
#define LOCK_BG          0xff000000u
#endif
#ifndef LOCK_RING
#define LOCK_RING        0xff5f8a93u
#endif
#ifndef LOCK_RING_WRONG
#define LOCK_RING_WRONG  0xffd06878u
#endif
#ifndef LOCK_FG
#define LOCK_FG          0xffa8d5ccu
#endif
#ifndef LOCK_DIM
#define LOCK_DIM         0xff7a808bu
#endif
#ifndef LOCK_CAPS
#define LOCK_CAPS        0xffe0c060u
#endif
/* Font pixel sizes; defaults are the two sizes every bake always includes.
 * Override via `lock { clock_size / font_size }` — wispc feeds those into the
 * baked size list, so the requested size actually exists at runtime. */
#ifndef LOCK_CLOCK_SIZE
#define LOCK_CLOCK_SIZE  22
#endif
#ifndef LOCK_FONT_SIZE
#define LOCK_FONT_SIZE   14
#endif
#ifndef LOCK_PROMPT
#define LOCK_PROMPT      "Password:"
#endif
#ifndef LOCK_WRONG_MS
#define LOCK_WRONG_MS    1200
#endif

#endif

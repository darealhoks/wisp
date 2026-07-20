/* wisp — user-facing knobs. Self-contained (does NOT include dwlarp config). */
#ifndef WISP_CONFIG_H
#define WISP_CONFIG_H

/* ---------- Font ----------
 * Backend + font file are build-time choices, not runtime knobs: pick them via
 * `./configure` (or `make config`), or `make FONT_BACKEND={baked,bitmap,freetype}
 * FONT=/path/to/font`. baked/bitmap rasterize at build time into src/bake.h;
 * freetype dlopen()s libfreetype.so.6 at runtime (src/font_ft.c) and reads
 * $WISP_FONT, falling back to the FONT= path baked in as WISP_FONT_DEFAULT_PATH.
 * Glyph/Font types live in src/font.h. */

/* ---------- Bar ---------- */
#define BAR_HEIGHT      28
#define BAR_PAD_X       16   /* L/R inset (was 10 — felt cramped against logo) */
#define BAR_TAG_PAD_X   10   /* horizontal padding inside each tag cell (dwlb-ish) */
#define BAR_TAG_GAP      0   /* dwlb runs tags edge-to-edge */

#define BAR_BG          0xff0f1219u  /* solid; wallpaper shadow tone */
#define BAR_FG          0xffffffffu
#define BAR_DIM         0xff7a808bu  /* separators */
#define BAR_ACTIVE_BG   0xff2a2f3au  /* active workspace tag */
#define BAR_URGENT_BG   0xffee3300u

/* VPN colors (match dwlarp WS_STATUS_VPN_*_FG) */
#define VPN_ON_FG       0xff7fbf9fu
#define VPN_STALE_FG    0xffff5050u
#define VPN_OFF_FG      0xffffaa20u

/* ---------- HUD (generic slide-in shell) ----------
 * Contents are DSL-driven (codegen-emitted render/click per reveal_on_hover
 * surface). Only animation/timing knobs live here. */
#define HUD_HIDE_DELAY_MS  30
#define HUD_CLICK_GRACE_MS 100
#define HUD_ANIM_TAU_MS    50.0
#define HUD_ANIM_EPSILON   0.5

/* ---------- Menu (dmenu-wl clone, horizontal top bar) ---------- */
#ifndef MENU_BG
#define MENU_BG         0xff0f1219u   /* wallpaper shadow tone (matches bar/osd) */
#endif
#ifndef MENU_FG
#define MENU_FG         0xffffffffu   /* dmenu-wl -nf */
#endif
#ifndef MENU_SEL_BG
#define MENU_SEL_BG     0xff2a2f3au   /* = active-workspace tag (BAR_ACTIVE_BG) */
#endif
#ifndef MENU_SEL_FG
#define MENU_SEL_FG     0xffffffffu   /* white text on the navy-grey slab */
#endif
#ifndef MENU_DIM
#define MENU_DIM        0xff7a808bu
#endif

#ifndef MENU_HEIGHT
#define MENU_HEIGHT     28            /* dmenu-wl -h 28 */
#endif
#ifndef MENU_PAD_X
#define MENU_PAD_X      8             /* L/R inset */
#endif
#ifndef MENU_ITEM_PAD_X
#define MENU_ITEM_PAD_X 8             /* horizontal padding inside each item slab */
#endif
#ifndef MENU_GAP
#define MENU_GAP        12            /* prompt → first item */
#endif
#ifndef MENU_PROMPT
#define MENU_PROMPT     "run:"
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
#ifndef MENU_RADIUS
#define MENU_RADIUS     0
#endif
#ifndef MENU_BORDER
#define MENU_BORDER     0x00000000u   /* alpha 0 = off */
#endif
#ifndef MENU_BORDER_W
#define MENU_BORDER_W   1
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

#define TIME_FMT "%H:%M"
#define DATE_FMT "%b %d"

/* Tag labels (9). */
#define TAG_LABELS { "1","2","3","4","5","6","7","8","9" }

/* Show modules */
#define SHOW_DISK    1
#define SHOW_CPU     1
#define SHOW_MEM     1
#define SHOW_BAT     1
#define SHOW_VPN     1
#define SHOW_WIFI    1

#define CTL_SOCK_NAME "wisp.sock"

/* ---------- Powermenu (built-in `wispctl powermenu`) ----------
 * Each entry: { icon-codepoint, label, shell-command }. Icons must also
 * appear in tools/bake.c's ICONS[]. */
/* ponytail: sleep/hibernate are `true` no-ops — not set up on this machine yet.
 * Swap in `loginctl suspend` / `loginctl hibernate` once configured. */
#define POWERMENU_INIT { \
    { 0xf011, "Poweroff",  "loginctl poweroff" }, \
    { 0xf021, "Reboot",    "loginctl reboot"   }, \
    { 0xf08b, "Logout",    "pkill -x mango"    }, \
    { 0xf186, "Sleep",     "true"              }, \
    { 0xf28d, "Hibernate", "true"              }, \
}
/* Emoji picker (`wispctl emoji`) data lives in src/emoji_data.h (EMOJI_INIT),
 * generated from gemoji — too long to inline here. Freetype-only: the baked
 * bitmap backend has no emoji glyphs. */

/* ---------- Wallpaper ----------
 * PNG decoded once at first configure. If the file is missing or the decoder
 * rejects it, the background falls back to WALL_BG (solid). */
#ifndef WALL_PATH
#define WALL_PATH "~/.local/share/dwl/wallpaper.png"
#endif
#ifndef WALL_BG
#define WALL_BG   0xff0f1219u
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
#ifndef OSD_SEPARATOR
#define OSD_SEPARATOR   0xff202020u
#endif
#ifndef OSD_SEPARATOR_FRAC
/* Separator width as a fraction of OSD_W (×100, so 80 == 80%). */
#define OSD_SEPARATOR_FRAC 80
#endif
#define OSD_BORDER_CRIT 0xffffffffu
#define OSD_BORDER_MUTE 0xffa04050u
#define OSD_BG_MUTE     0xff3a1418u   /* solid */
#define OSD_FG_MUTE     0xfff0c8c8u
#define OSD_BORDER_WARN 0xffe0c060u   /* low-battery warn slab (yellow) */
#define OSD_BG_WARN     0xff332a14u   /* solid */
#define OSD_FG_WARN     0xfff0e0a8u
#ifndef OSD_PROG_FG
#define OSD_PROG_FG     0xff84a7b3u   /* wallpaper bright teal */
#endif
#define OSD_PROG_FG_MUTE 0xffd06070u
#define OSD_PROG_FG_WARN 0xffe0c060u

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

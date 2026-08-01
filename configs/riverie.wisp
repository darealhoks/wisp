//! font = ~/.local/share/fonts/MapleMono-NF-Bold.ttf
//! font_fallback = /usr/share/fonts/noto-emoji/NotoColorEmoji.ttf

// ==================================
//               BAR
// ==================================

source time   = clock("%H:%M");
source date_s = clock("%b %-d");
source tags   = tags();
source cpu_s  = cpu(every="2s");
source mem_s  = mem(every="2s");
source bat_s  = bat("BAT0");
source temp_s = temp(every="2s");
source wifi_s = net("");
source tray_s = tray(icon_size=20);
source vol_s  = pipewire();

source hid    = ui_hidden();
source notif_s = notifications(history=64);

const TEXT   = #ffdbe2ee;
const SUBTXT = #ffa5adbb;
const CRUST  = #ff0e131c;
const REST   = #ff141a26;
const WSACT  = #ff64799c;
const WSBORD = #ff2e3a4e;
const BORD   = #ff2e3a4e;
const YELLOW = #ffa8bfdd;
const ORANGE = #ffddab64;
const RED    = #ffe0603f;
const GREEN  = #ff97bb90;
const PRIM   = #ff64799c;
const TERT   = #ff92aed2;

const TGREEN  = #ffc0e0b8;
const TBLUE   = #ffb3ccec;
const TVIOLET = #ffcbb8ee;
const TRED    = #ffeeb9b3;
const TTEAL   = #ffaedfdf;

const TRAY_ICONS_ONLY = true; // icon-less tray items aren't shown
const BLACK  = #ff000000;

surface bar {
	layer = top;
	anchor = top | left | right;
	height = 34;
	margin = 6;
	exclusive_zone = 34;
	visible = hid.value == "0";

	bg = #00000000;
	radius = 0;

	/* left side, left > right */

		widget edge_l {
		align = left;
		pad = 2;
	}

	group distrogrp {
		align = left;
		widget distro {
			icon = 0xf32e;
			fg   = TBLUE;
		}
	}
	group batgrp {
		align = left;
		widget bat {
			icon = bat_s.charging  ? 0xf0084
				: bat_s.pct >= 75 ? 0xf240
				: bat_s.pct >= 50 ? 0xf241
				: bat_s.pct >= 25 ? 0xf242
				: bat_s.pct >= 10 ? 0xf243
				:                   0xf244;
			text = "{bat_s.pct}%";
			fg   = bat_s.pct < 15 ? RED
				: bat_s.pct < 25 ? ORANGE : TEXT;
			icon_fg = bat_s.charging ? GREEN
				: bat_s.pct < 15 ? RED
				: bat_s.pct < 25 ? ORANGE : TGREEN;
		}
	}
	group clockgrp {
		align = left;
		widget time {
			text = time;
		}
		widget date.dim {
			text = date_s;
		}
	}

	for tag in tags.list {
		cell.ws {
			text         = tag.label;
			visible      = tag.pinned || tag.occupied || tag.active || tag.urgent;
			on_click()   = exec("wispctl tag {tag.index} {tag.output}");
		}
	}

	/* right side, right > left */
		widget edge_r {
		align = right;
		pad = 2;
	}

	group conngrp {
		align = right;
		widget wifi {
			icon = wifi_s.signal >= 3 ? 0xf0928
				: wifi_s.signal >= 2 ? 0xf0925
				: wifi_s.signal >= 1 ? 0xf0922
				:                      0xf091f;
			fg = wifi_s.signal >= 1 ? TTEAL : RED;
			on_click() = exec("foot -T ws-hud-wifi --app-id=ws-hud-wifi -e impala");
		}
		widget sep_conn.sep {
			text = "/";
		}
		widget audio {
			icon = !vol_s.ok   ? 0xf0581
				: vol_s.mute     ? 0xf0581
				: vol_s.vol < 34 ? 0xf026
				: vol_s.vol < 67 ? 0xf027
				:                  0xf028;
			fg = !vol_s.ok     ? RED
				: vol_s.mute     ? ORANGE : TVIOLET;
			on_click() = exec("foot -T ws-hud-vol --app-id=ws-hud-vol -e wiremix");
		}
		widget sep_conn2.sep {
			text = "/";
		}
		// the panel is anchored top|right, so the bell belongs on the right
		// edge of the bar — in the HUD it opened a surface nowhere near itself
		widget notif {
			icon = 0xf0f3;
			text = notif_s.count > 0 ? "{notif_s.count}" : "";
			fg = notif_s.open ? WSACT
				: notif_s.count > 0 ? TERT : TEXT;
			on_click() = exec("wispctl notif toggle");
		}
	}

	group sysgrp {
		align = right;
		widget cpu    {
			icon = 0xf4bc;
			text = "{cpu_s.pct}%";
			tooltip = "CPU load";
			fg = cpu_s.pct >= 90 ? RED
				: cpu_s.pct >= 75 ? ORANGE : TEXT;
			icon_fg = cpu_s.pct >= 90 ? RED
				: cpu_s.pct >= 75 ? ORANGE : TBLUE;
		}
		widget sep_ct.sep {
			text = "/";
		}
		widget temp   {
			icon = 0xf06d;
			text = "{temp_s.c}°C";
			tooltip = "Package temperature";
			fg = temp_s.c >= 85 ? RED
				: temp_s.c >= 70 ? ORANGE : TEXT;
			icon_fg = temp_s.c >= 85 ? RED
				: temp_s.c >= 70 ? ORANGE : TRED;
		}
		widget sep_tr.sep {
			text = "/";
		}
		widget mem    {
			icon = 0xefc5;
			tooltip = "Memory in use";
			text = mem_s.used_mb >= 1024
				? "{mem_s.used_mb / 1024}.{mem_s.used_mb * 10 / 1024 % 10} GB"
				: "{mem_s.used_mb} MB";
			fg = mem_s.pct >= 90 ? RED
				: mem_s.pct >= 75 ? ORANGE : TEXT;
			icon_fg = mem_s.pct >= 90 ? RED
				: mem_s.pct >= 75 ? ORANGE : TGREEN;
		}
	}

	group traygrp {
		align = right;
		for tray_item in tray_s.items {
			cell.tray {
				icon       = tray_item.icon;
				// an app with its own attention artwork says it itself
				bg         = tray_item.status == "NeedsAttention"
					&& !tray_item.has_attention_icon ? RED
					: tray_item.menu_open ? REST : #00000000;
				text       = tray_item.has_icon || TRAY_ICONS_ONLY ? "" : tray_item.id;
				visible    = tray_item.status != "Passive"
					&& (tray_item.has_icon || !TRAY_ICONS_ONLY);
				on_click()       = exec("wispctl tray activate {tray_item.index}");
				on_right_click()  = exec("wispctl tray menu {tray_item.index}");
				on_middle_click() = exec("wispctl tray secondary {tray_item.index}");
			}
		}
	}
}

group {
	bg = CRUST;
	border = BORD;
	border_width = 2;
	radius = 8;
	pad = 8;
	pad_x = 12;
	gap = 14;
} // no height → fills the bar row
#distrogrp {
	pad_x = 18;
	gap = 0;
}
#clockgrp  {
	gap = 10;
}
#traygrp   {
	pad_x = 4;
	gap = 0;
}
#conngrp   {
	pad_x = 18;
}

widget {
	fg = TEXT;
	icon_gap = 5; // icon column → label gap
}
.dim   {
	fg = SUBTXT;
}
.sep   {
	fg = BORD;
}

.ws {
	align = left;
	fg = TEXT;
	bg = CRUST;
	border = BORD;
	border_width = 2;
	radius = 8;
	pad = 6;
	width = 28;
	height = 28;
	transition_size = 160ms;
	enter_anim = 160ms;
	exit_anim = 160ms;
}
.ws:active {
	fg = TEXT;
	border = BORD;
	width = 34;
	height = 34;
}

.tray {
	align = right;
	fg = TEXT;
	radius = 6;
	width = 28;
	height = 26;
	enter_anim = 160ms;
	exit_anim = 160ms;
}

.tray:pressed {
	bg = REST;
}

// ==================================
//               HUD
// ==================================

source gamma_warm = gamma_warm();
source dnd_on     = dnd();
source mirror_on  = toplevel(app_id="at.yrlf.wl_mirror");

surface hud {
	layer = overlay;
	anchor = top;
	width  = 244;          // 5 × 32px button + 4 × 8px separator, each +6 gap
	height = 40;
	font_size = 14;
	reveal_on_hover = 20;
	reveal_gutter   = 3;
	reveal_anim_ms  = 200;
	reveal_easing   = ease_out;
	visible = hid.value == "0";

	widget gamma_btn.btn {
		icon = 0xf186;
		fg = gamma_warm.value == "1" ? WSACT : TEXT;
		on_click() = exec("sh -c 'wispctl gamma is-warm && wispctl gamma off || wispctl gamma flat'");
	}
	widget sep1.sep {
		text = "/";
	}
	widget dnd_btn.btn {
		icon = 0xf1f6;
		fg = dnd_on.value == "on" ? TERT : TEXT;
		on_click() = exec("wispctl dnd toggle");
	}
	widget sep2.sep {
		text = "/";
	}
	widget vol_btn.btn {
		icon = 0xf028;
		on_click() = exec("foot -T ws-hud-vol --app-id=ws-hud-vol -e wiremix");
	}
	widget sep3.sep {
		text = "/";
	}
	widget wifi_btn.btn {
		icon = 0xf1eb;
		on_click() = exec("foot -T ws-hud-wifi --app-id=ws-hud-wifi -e impala");
	}
	widget sep4.sep {
		text = "/";
	}
	widget mirror_btn.btn {
		icon = 0xf24d;
		fg = mirror_on.exists ? PRIM : TEXT;
		on_click() = {
			exec("mirror toggle")
		}
		;
	}
}

#hud widget {
	align = center;
	pad = 6;
}

.btn {
	width = 32;
	height = 32;
	radius = 8;
}

#gamma_btn, #dnd_btn, #mirror_btn, #notif {
	transition_fg = 180ms;
}
.btn:pressed {
	bg = REST;
}

// ==================================
//       NOTIFICATION CENTER
// ==================================

// Persistent panel over the history ring. `wispctl notif toggle` (bar bell)
// flips notif_s.open, which creates/destroys the surface — closed costs no
// pool and no timer.
surface notifs {
	layer   = overlay;
	anchor  = top | right;
	margin  = 46;          // clears the bar (height 34 + its own margin 6)
	margin_x = 6;          // flush with the bar's right edge
	width   = 380;
	height  = 420;
	exclusive_zone = -1;
	axis    = vertical;
	scroll  = rows;        // one wheel notch = exactly one card, whatever its height
	font_size = 14;
	visible = notif_s.open;
	output  = active;      // one copy, on the monitor whose bell was clicked
	pad_x   = 10;          // one inset for the whole panel; rows fill what's left
	pad_y   = 8;
	on_escape = "wispctl notif close";
	dismiss_on_unfocus;    // clicking anywhere else closes it, like a dropdown
	                       // (kbd is on_demand, so an open panel never eats typing)

	bg = CRUST;
	radius = 8;
	border = BORD;
	border_width = 2;

	// bell / title / trash: the two 26px squares balance, so the title is
	// actually centered instead of merely left of the button
	group nhead {
		sticky;                // stays pinned; the cards scroll beneath it
		height = 30;
		widget nbell.dim {
			width = 26;
			icon  = 0xf0f3;
		}
		widget nhead_t.dim {
			width = 304;    // 356 content width − the two 26px squares
			text  = "notifications";
		}
		widget nclear {
			width = 26;
			icon  = 0xf1f8;
			fg    = notif_s.count > 0 ? SUBTXT : WSBORD;
			on_click() = exec("wispctl notif clear");
		}
	}
	widget nrule {
		sticky;
		height = 1;
		bg     = BORD;
		pad    = 8;
	}
	widget nempty {
		width   = 356;         // declared width → the text centers across it too
		height  = 330;         // fills the card area, so the text centers in it
		text    = "nothing yet…";
		fg      = #ff5c6678;
		visible = notif_s.count == 0;
	}
	for note in notif_s.history {
		cell.note {
			icon = note.icon;
			text = "{note.summary}\n{note.body}";
			body_lines = 4;
			body_fit;
			wrap;
			text_align = start;
			fg      = TEXT;
			body_fg = SUBTXT;   // summary reads first, body recedes
			icon_fg = note.urgent ? RED : TERT;
			border  = note.urgent ? RED : #00000000;
			// click a card to dismiss it
			on_click() = exec("wispctl notif dismiss {note.id}");
		}
	}
}

#nhead {
	pad_x = 0;
	gap   = 0;
	bg    = #00000000;
	border = #00000000;    // don't inherit the panel frame around the header
}
#nclear {
	height = 26;           // else the press fill spans the whole header band
	radius = 6;
}
#nclear:hover {
	bg = REST;
}
#nclear:pressed {
	bg = WSBORD;
}
.note {
	bg     = REST;
	radius = 8;
	pad_x  = 12;
	pad_y  = 10;
	pad    = 8;
	border_width = 1;
	icon_gap = 8;
}
.note:hover {
	bg = #ff1b2233;
}
.note:pressed {
	bg = WSBORD;
}

// ==================================
//           OSD (notifs)
// ==================================

surface osd {
	spawned_by = osd;
	layer = overlay;
	anchor = top;
	max = 4;
	body_lines = 4;
	body_max = 256;
	dismiss_on_click = true;
	focus_follow = true;
	dbus_close = true;

	width = 340;
	height = 60;
	margin = 6;
	gap = 0;
	pad_x = 14;
	icon_gap = 12;
	image = 32; // cover art square; falls back to the icon glyph
	prog_h = 10;

	timeout_low = 3000;
	timeout_normal = 5000;
	timeout = 1200;

	radius = 10;
	border_width = 2;
	separator_frac = 80;

	prog_fg = WSACT;
	prog_track = REST;
	bg = CRUST;
	border = BORD;
	separator = REST;

	widget icon  {
		align = left;
		width = 58;
		icon = $image;
		visible = $has_icon;   // collapse the column, don't indent past an empty box
	}

	widget title {
		align = left;
		text = $nbody > 0 ? "{$summary}\n{$body}" : $summary;
		body_lines = 1 + $nbody;
		elide;
		pad = 12;
		x_offset = $has_icon ? 0 : 14;  // no icon column → keep the pad_x inset
		y_offset = $progress >= 0 ? -9 : 0;
	}

	widget pct   {
		align = right;
		text = "{$pct}%";
		pad = 12;
		y_offset = $progress >= 0 ? -9 : 0;
		visible = $progress >= 0;
	}

	widget prog  {
		slider;
		align = left;
		width = 312;
		height = 10;
		visible = $progress >= 0;
		value = $progress;
		value_max = 100;
		track_bg = REST;
		track_fg = WSACT;
		track_radius = 5;
	}
}

#osd widget {
	fg = TEXT;
}

// text-only: a per-slab bg breaks the seamless chain the stacked slabs read as
#osd widget:warn {
	fg = ORANGE;
}
#prog:warn {
	track_fg = ORANGE;
}

#osd widget:urgent {
	fg = RED;
}
#prog:mute {
	track_fg = RED;
}

surface pill {
	spawned_by = osd_pill;
	layer = overlay;
	anchor = top;
	width = 220;
	height = 40;
	margin = 3;
	radius = 8;
	font_size = 20;

	widget icon {
		align = left;
		width = 40;
		pad = 0;
		x_offset = 3;
		icon = $icon;
	}
	widget prog {
		slider;
		align = left;
		width = 164;
		height = 10;
		value = $progress;
		value_max = 100;
		track_bg = REST;
		track_fg = WSACT;
		track_radius = 5;
	}
}

#pill {
	bg = CRUST;
	border = BORD;
	border_width = 2;
}
#pill widget {
	fg = TEXT;
}
#pill:warn {
	bg = REST;
}
#pill widget:warn {
	fg = ORANGE;
}

// ==================================
//            Subsystems
// ==================================

lock {
	bg         = BLACK;
	ring       = PRIM;
	ring_bad   = RED;
	fg         = YELLOW;
	dim        = #60000000;   // scrim over the wallpaper, not a text color
	caps       = ORANGE;
	prompt     = "Password";
	pam        = "system-auth";
	font_size  = 20;
	wall       = true;

	// swaylock's default face: one centered ring, nothing else. No anchor bits
	// → centered on both axes. Per-state colour is just another ring with a
	// different show=, drawn in order: base, then PAM, then wrong on top.
	// swaylock's proportions (radius:thickness 5:1), scaled up from its 50/10.
	ring dial { radius = 80; thickness = 16; bg = #cc0e131c;
	            border = BORD; border_width = 2; fg = PRIM; show = !wrong; }
	// The 60° arc that jumps to a new angle on every keystroke. Its own element
	// with a transparent stroke, so it only exists while there is input —
	// swaylock shows no highlight on an empty buffer.
	ring keys { radius = 80; thickness = 16; fg = #00000000; show = typing;
	            highlight = TERT; highlight_bs = SUBTXT; separator = CRUST; }
	ring busy { radius = 80; thickness = 16; fg = TERT; show = verifying; }
	ring bad  { radius = 80; thickness = 16; fg = RED;  show = wrong; }

	text verify { text = "Verifying"; show = verifying; fg = SUBTXT; font_size = 14; }
	text bad_t  { text = "Wrong";     show = wrong;     fg = RED;    font_size = 14; }
	text caps_t { text = "Caps Lock"; show = caps;      fg = ORANGE; font_size = 14; y = 110; }
}

gamma {
	day_k     = 6500;
	night_k   = 2800;
	flat_k    = 2400;
	day_hour  = 7;
	night_hour = 20;
	fade_min   = 30;
	transition_ms = 300;
}

wallpaper {
	path = "~/next/rice/walls/riverie.png";
	transition = wipe;
	wipe_dir   = down_right;
	wipe_soft  = 200;
	fade_ms    = 300;
	bg         = BLACK;
}

media {
	// media keys
}

// ==================================
//              Tooltip
// ==================================

// Decoration only — never focusable, never clickable (tooltip.c pins
// keyboard_interactivity 0 + a zero-area input region). `width` is a clamp:
// the surface auto-widths to $text and elides past it.
surface tooltip {
	spawned_by = tooltip;
	layer = overlay;
	exclusive_zone = -1;

	font_size  = 14;
	width      = 320;
	height     = 26;
	pad_x      = 8;
	pad_y      = 4;
	anchor_gap = 4;
	delay_ms   = 500;   // hover dwell before it appears

	bg           = CRUST;
	border       = BORD;
	border_width = 1;
	radius       = 6;

	widget label {
		align = left;
		text  = $text;
		fg    = SUBTXT;
		elide;
	}
}

// ==================================
//              Menus
// ==================================

surface menu {
	spawned_by = menu;
	layer = overlay;
	exclusive_zone = -1;

	axis        = vertical;
	width       = 320;
	margin      = 6;
	max_visible = 5;
	row_h       = 34;

	prompt = "run: ";
	sort   = "most_used";
	icons  = true;
	hover;

	pad_x = 8;
	pad_y = 6;

	group query {
		height = 34;
		pad = 0;
		pad_x = 8;
		gap = 0;
		bg = #00000000;
		border = #00000000;
		cell {
			text = menu.prompt;
			fg = SUBTXT;
		}
		cell {
			text = menu.query;
			fg = TEXT;
		}
		cell {
			text = "_";
			fg = TEXT;
		}
	}
	for row in rows {
		cell {
			height = 34;
			icon   = row.icon;
			text   = row.label;
			fg     = TEXT;
			bg     = row.selected ? REST : #00000000;
			radius = 4;
			pad_x  = 8;
			icon_gap = 5;
			elide;
		}
	}
}

#hud, #osd, #menu {
	bg = CRUST;
	fg = TEXT;
	border = BORD;
	border_width = 2;
	radius = 8;
}

menu power {
	hover;
	item {
		icon = 0xf011;
		label = "Poweroff";
		exec = "loginctl poweroff";
	}
	item {
		icon = 0xf021;
		label = "Reboot";
		exec = "loginctl reboot";
	}
	item {
		icon = 0xf08b;
		label = "Logout";
		exec = "pkill -x mango";
	}
	item {
		icon = 0xf186;
		label = "Sleep";
		exec = "true";
	}
	item {
		icon = 0xf28d;
		label = "Hibernate";
		exec = "true";
	}
}

menu emoji {
	preset = emoji;
	hover;
}

menu tray {
	width       = 200;
	row_h       = 24;
	separator_h = 13;       // 1px hairline + 6px either side
	separator   = REST;     // same near-invisible line the OSD stack uses
	separator_frac = 92;
	max_visible = 24;
	anchor_gap  = 6;
	hover;
	font_size   = 12;

	bg = CRUST;
	border = BORD;
	border_width = 2;
	radius = 8;
	pad_x = 6;
	pad_y = 6;

	for row in rows {
		cell {
			height = 24;
			text   = row.label;
			// one icon column: the app's own raster wins it, the checkmark
			// only shows on rows that brought no icon (a checkbox row
			// virtually never does). 0 = no glyph, so a menu where no row
			// has an icon loses the column instead of indenting every label
			icon   = row.has_icon ? row.icon : (row.checked ? 0xf00c : 0);
			icon_box = 12;   // == the square menu_icons_load decodes for row_h 24
			icon_gap = 6;
			fg     = row.enabled ? TEXT : SUBTXT;
			bg     = row.selected ? BORD : #00000000;
			radius = 4;
			pad_x  = 8;
			elide;
		}
	}
}

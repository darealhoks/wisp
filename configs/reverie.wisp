//! font = ~/.local/share/fonts/MapleMono-NF-Bold.ttf
//! font_fallback = /usr/share/fonts/noto-emoji/NotoColorEmoji.ttf

// bar

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
source notif_s = notifications(history=64, image=22);

include "theme.wisp";

const TRAY_ICONS_ONLY = true; // icon-less tray items are hidden, not labelled

surface bar {
	layer = top;
	anchor = top | left | right;
	height = 34;
	margin = 6;
	exclusive_zone = 34;
	visible = hid.value == "0";

	bg = #00000000;
	radius = 0;

	// align=left packs left→right in declaration order

		widget edge_l {
		align = left;
		pad = 2;
	}

	group distrogrp {
		align = left;
		widget distro {
			icon = 0xf08e8;
			fg   = TVIOLET;
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

	// align=right packs right→left in declaration order
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
		// notifs panel is anchored top|right, so the bell must stay on the right
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
#bar group {
	shadow = #14000000;
	shadow_y = 0;
	shadow_blur = 6; // 6 = bar top margin, keeps the even halo unclipped
}
#distrogrp {
	pad_x = 12;
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
	icon_gap = 5;
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
	shadow = #14000000;  // matches the `#bar group` pills
	shadow_y = 0;
	shadow_blur = 6; // 6 = bar top margin, keeps the even halo unclipped
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

// hud

source gamma_warm = gamma_warm();
source dnd_on     = dnd();
source mirror_on  = toplevel(app_id="at.yrlf.wl_mirror");
// written by ~/next/rice/mango/caffeine; inotify wants a literal absolute path
source caffeine_on = inotify(path="/run/user/1000/caffeine");

surface hud {
	layer = overlay;
	anchor = top;
	width  = 297;          // 6×(32+6) + 5×(9+6) − 6: pad is a trailing advance, one per
	                       // widget, last one dropped on a centered run. "/" is 9 at 14px.
	                       // too small does not shrink, it clips off both edges
	height = 40;
	font_size = 14;
	reveal_on_hover = 20;
	reveal_gutter   = 3;
	reveal_anim_ms  = 200;
	reveal_easing   = ease_out;
	shadow = #14000000;
	shadow_y = 0;
	shadow_blur = 6;
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
	widget sep5.sep {
		text = "/";
	}
	widget caff_btn.btn {
		icon = 0xf0f4;
		fg = caffeine_on.value == "1" ? PRIM : TEXT;
		on_click() = exec("/home/hoks/.local/bin/caffeine toggle");
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

#gamma_btn, #dnd_btn, #mirror_btn, #caff_btn, #notif {
	transition_fg = 180ms;
}
.btn:pressed {
	bg = REST;
}

// notifications

surface notifs {
	layer   = overlay;
	anchor  = top | right;
	margin  = 46;          // bar height 34 + bar margin 6, keep in sync
	margin_x = 6;          // matches the bar margin
	width   = 380;
	height  = 420;
	exclusive_zone = -1;
	axis    = vertical;
	scroll  = rows;
	font_size = 14;
	visible = notif_s.open;
	output  = active;
	pad_x   = 10;
	pad_y   = 8;
	on_escape = "wispctl notif close";
	dismiss_on_unfocus;

	bg = NOTIFBG;
	radius = 8;
	border = BORD;
	shadow = #14000000;
	shadow_y = 0;
	shadow_blur = 6;
	border_width = 2;

	group nhead {
		sticky;
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
		height  = 330;
		text    = "nothing yet…";
		fg      = EMPTY;
		visible = notif_s.count == 0;
	}
	for note in notif_s.history {
		cell.note {
			icon = note.image;   // pixmap thumbnail; note.icon glyph is the fallback
			text = "{note.summary}\n{note.body}";
			body_lines = 4;
			body_fit;
			wrap;
			text_align = start;
			fg      = TEXT;
			body_fg = SUBTXT;
			icon_fg = note.urgent ? RED : TERT;
			border  = note.urgent ? RED : #00000000;
			on_click()       = exec("wispctl notif invoke {note.id}");
			on_right_click() = exec("wispctl notif dismiss {note.id}");
		}
	}
}

#nhead {
	pad_x = 0;
	gap   = 0;
	bg    = #00000000;
	border = #00000000;    // else the header inherits the panel frame
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
	icon_box = 22;   // == notifications(image=): glyph fallback shares the thumbnail square
	icon_gap = 8;
}
.note:hover {
	bg = HOVER;
}
.note:pressed {
	bg = WSBORD;
}

// osd

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
	image = 32;
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
	shadow = #14000000;
	shadow_y = 0;
	shadow_blur = 6;

	widget icon  {
		align = left;
		width = 58;
		icon = $image;
		icon_fg = $icon == 0xf019 ? TERT       // f019 = em's download glyph, matches /usr/local/bin/em
			: $icon == 0xf0e7 ? ORANGE     // power-mode performance
			: $icon == 0xf24e ? TBLUE      // power-mode balanced
			: $icon == 0xf240 ? GREEN      // power-mode battery
			: #00000000;
		visible = $has_icon;
	}

	widget title {
		align = left;
		text = $nbody > 0 ? "{$summary}\n{$body}" : $summary;
		body_lines = 1 + $nbody;
		elide;
		pad = 12;
		x_offset = $has_icon ? 0 : 14;  // no icon column → stands in for the pad_x 14 inset
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

// fg only, a per-slab bg breaks the seamless stacked-slab chain
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
	shadow = #14000000;
	shadow_y = 0;
	shadow_blur = 6;

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

// subsystems

lock {
	bg        = BLACK;
	dim       = #00000000;   // the default scrim is opaque grey
	pam       = "system-auth";
	font_size = 20;

	text dots   { text = "{dots}";    show = typing; fg = TEXT; }
	text caps_t { text = "Caps Lock"; show = caps;   fg = ORANGE; font_size = 14; y = 40; }
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
	path = WALL;
	transition = wipe;
	wipe_dir   = down_right;
	wipe_soft  = 200;
	fade_ms    = 300;
	bg         = BLACK;
}

media {
}

idle {
	timeout blank {
		after  = 300s;
		run    = "caffeine blank || wispctl dpms off";
		resume = "wispctl dpms on";
	}
	timeout suspend {
		after = 720s;
		run   = "caffeine sleep || loginctl suspend";
	}

	before_sleep = lock;
}

// tooltip

// width is a clamp, the surface auto-widths to $text and elides past it
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
	delay_ms   = 500;

	bg           = CRUST;
	border       = BORD;
	border_width = 1;
	radius       = 6;
	shadow = #14000000;
	shadow_y = 0;
	shadow_blur = 6;

	widget label {
		align = left;
		text  = $text;
		fg    = SUBTXT;
		elide;
	}
}

// menus

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
	shadow = #14000000;
	shadow_y = 0;
	shadow_blur = 6;
}

// no anchor: an axis with neither edge bitted is centred by layer-shell
surface polkit {
	spawned_by = polkit;
	layer      = overlay;
	keyboard   = exclusive;
	exclusive_zone = -1;

	axis   = vertical;
	width  = 420;
	height = 150;   // 28 pad + 22 + 6 + 34 + 6 + 38 rows, then the origin at the far edge
	pad_x  = 16;
	pad_y  = 14;
	font_size = 14;

	bg = CRUST;
	fg = TEXT;
	border = BORD;
	border_width = 2;
	radius = 8;
	shadow = #14000000;
	shadow_y = 0;
	shadow_blur = 6;

	// this body advances by height+`pad` per row: the surface `gap` never
	// reaches it, so the row spacing is each row's own trailing pad
	widget pk_title {
		height = 22;
		pad    = 6;
		text   = "Authentication required";
		fg     = TEXT;
	}
	group pk_entry {
		height = 34;
		pad    = 6;
		pad_x  = 10;
		gap    = 0;
		bg     = REST;
		border = #00000000;
		radius = 8;
		cell { text = polkit.prompt; fg = SUBTXT; }
		cell { text = polkit.dots;   fg = TEXT; }
		cell { text = "_";           fg = TEXT; }
	}
	widget pk_msg {
		// two fixed rows, not body_fit: the height above is constant, so a
		// short message must not shrink the stack under the bottom row
		height     = 38;
		text       = polkit.message;
		fg         = SUBTXT;
		body_lines = 2;
		wrap;
		text_align = start;
	}
	// last row does double duty so the origin can sit flush at the bottom:
	// a failed attempt swaps it for PAM's error instead of adding a row
	widget pk_user {
		align  = end;   // only row out of the start bucket, so it can't collide
		height = 18;
		text   = polkit.failed ? polkit.error : "{polkit.user} · {polkit.action}";
		fg     = polkit.failed ? RED : EMPTY;
		elide;
	}
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
	separator   = REST;
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
	shadow = #14000000;
	shadow_y = 0;
	shadow_blur = 6;

	for row in rows {
		cell {
			height = 24;
			text   = row.label;
			// icon 0 = no glyph, so an icon-less menu loses the column instead of indenting every label
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

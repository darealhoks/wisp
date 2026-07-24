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

const TEXT   = #ffe9e6dd;
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
			text = " {bat_s.pct}%";
			fg   = bat_s.charging ? GREEN
				: bat_s.pct < 15 ? RED
				: bat_s.pct < 25 ? ORANGE
				: bat_s.pct < 40 ? YELLOW : TEXT;
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
			fg = wifi_s.signal >= 1 ? TEXT : RED;
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
				: vol_s.mute     ? YELLOW : TEXT;
			on_click() = exec("foot -T ws-hud-vol --app-id=ws-hud-vol -e wiremix");
		}
	}

	group sysgrp {
		align = right;
		widget cpu    {
			icon = 0xf4bc;
			text = " {cpu_s.pct}%";
			fg = cpu_s.pct >= 90 ? RED
				: cpu_s.pct >= 75 ? ORANGE
				: cpu_s.pct >= 50 ? YELLOW : TEXT;
		}
		widget sep_ct.sep {
			text = "/";
		}
		widget temp   {
			icon = 0xf06d;
			text = " {temp_s.c}°C";
			fg = temp_s.c >= 85 ? RED
				: temp_s.c >= 70 ? ORANGE
				: temp_s.c >= 55 ? YELLOW : TEXT;
		}
		widget sep_tr.sep {
			text = "/";
		}
		widget mem    {
			icon = 0xefc5;
			text = mem_s.used_mb >= 1024
				? " {mem_s.used_mb / 1024}.{mem_s.used_mb * 10 / 1024 % 10} GB"
				: " {mem_s.used_mb} MB";
			fg = mem_s.pct >= 90 ? RED
				: mem_s.pct >= 75 ? ORANGE
				: mem_s.pct >= 60 ? YELLOW : TEXT;
		}
	}

	group traygrp {
		align = right;
		for tray_item in tray_s.items {
			cell.tray {
				icon       = tray_item.icon;
				bg         = tray_item.menu_open ? REST : #00000000;
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
	width  = 242;
	height = 40;
	font_size = 14;
	reveal_on_hover = 20;
	reveal_gutter   = 3;
	reveal_anim_ms  = 200;
	reveal_easing   = ease-out;
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

#gamma_btn, #dnd_btn, #mirror_btn {
	transition_fg = 180ms;
}
.btn:pressed {
	bg = REST;
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
		width = 40;
		icon = $icon;
	}

	widget title {
		align = left;
		text = $nbody > 0 ? "{$summary}\n{$body}" : $summary;
		body_lines = 1 + $nbody;
		elide;
		pad = 12;
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

#osd:warn {
	bg = REST;
}
#osd widget:warn {
	fg = ORANGE;
}
#prog:warn {
	track_fg = ORANGE;
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
	dim        = PRIM;
	caps       = RED;
	prompt     = "Password";
	pam        = "system-auth";
	font_size  = 20;
}

gamma {
	day_k     = 6500;
	night_k   = 2800;
	flat_k    = 2400;
	day_hour  = 7;
	night_hour = 20;
}

wallpaper {
	path = "~/next/rice/themes/current/wallpaper.png";
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
//              Menus
// ==================================

surface menu {
	spawned_by = menu;
	layer = overlay;
	exclusive_zone = -1;
	keyboard = exclusive;

	axis        = vertical;
	width       = 320;
	margin      = 6;
	max_visible = 5;
	row_h       = 34;

	prompt = "run: ";
	sort   = "most_used";
	icons  = true;

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
}

menu tray {
	width       = 200;
	row_h       = 24;
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
			fg     = TEXT;
			bg     = row.selected ? BORD : #00000000;
			radius = 4;
			pad_x  = 8;
			elide;
		}
	}
}

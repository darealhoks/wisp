//! font = ~/.local/share/fonts/MapleMono-NF-Bold.ttf
//! font_fallback = /usr/share/fonts/noto-emoji/NotoColorEmoji.ttf

include "theme.wisp";

// bar

source time   = clock("%H:%M");
source date_s = clock("%b %d");
source tags   = tags();
source cpu_s  = cpu(every="2s");
source mem_s  = mem(every="2s");
source bat_s  = bat("BAT0");
source temp_s = temp(every="2s");
source wifi_s = net("");
source disk_s = disk("/", every="600s");

surface bar {
	layer = top;
	anchor = top | left | right;
	height = 28;
	exclusive_zone = 28;
	bg = CRUST;

	armpit_inner = 10;

	widget edge_l {
		align = left;
		pad = 12;
		y_offset = -34;
	}
	widget logo   {
		align = left;
		icon = 0xf32e;
	}
	widget time   {
		align = left;
		text = time;
	}
	widget date.dim {
		align = left;
		text = date_s;
		pad = 16;
	}

	for tag in tags.list {
		cell {
			align   = left;
			text    = tag.label;
			visible = tag.pinned || tag.exists || tag.active || tag.urgent;
			on_click() = exec("wispctl tag {tag.index} {tag.output}");
		}
	}

	widget title.dim {
		align = left;
		text = tags.title;
		pad = 16;
	}

	widget edge_r {
		align = right;
		pad = 12;
		y_offset = -34;
	}
	widget wifi   {
		align = right;
		icon = wifi_s.wired        ? 0xf0200
			: wifi_s.signal >= 3 ? 0xf0928
			: wifi_s.signal >= 2 ? 0xf0925
			: wifi_s.signal >= 1 ? 0xf0922
			:                      0xf091f;
		pad = 16;
		visible = wifi_s.wired || wifi_s.signal >= 0;
	}
	widget bat    {
		align = right;
		icon = bat_s.charging  ? 0xf0084
			: bat_s.pct >= 75 ? 0xf240
			: bat_s.pct >= 50 ? 0xf241
			: bat_s.pct >= 25 ? 0xf242
			: bat_s.pct >= 10 ? 0xf243
			:                   0xf244;
		text = "{bat_s.pct}%";
		fg   = bat_s.pct < 15 ? WARN : (bat_s.charging ? CHARGE : FG);
	}
	widget sep.dim {
		align = right;
		text = "/";
	}
	widget mem    {
		align = right;
		icon = 0xefc5;
		text = "{mem_s.pct}%";
	}
	widget temp   {
		align = right;
		icon = 0xf0238;
		text = "{temp_s.c} C";
	}
	widget cpu    {
		align = right;
		icon = 0xf4bc;
		text = "{cpu_s.pct}%";
		fg = cpu_s.pct > 80 ? WARN : FG;
	}
	widget disk   {
		align = right;
		icon = 0xf02ca;
		text = "{disk_s.pct}%";
	}
}

widget {
	fg = FG;
	pad = 22;
	icon_gap = 8;
}
.dim        {
	fg = DIM;
}

#bar cell          {
	fg = DIM;
	bg = CLEAR;
	width = 28;
	height = 28;
	pad = 4;
}
#bar cell:active   {
	fg = FG;
	bg = ACT;
}
#bar cell:urgent   {
	fg = FG;
	bg = URG;
}

// corners

surface screen_corners {
	layer  = top;
	anchor = bottom | left | right;
	height = 10;
	exclusive_zone = 0;
	bg = CLEAR;
	armpit_outer = 10;
	armpit_color = CRUST;
	input = none;
}

// hud

source gamma_warm = gamma_warm();
source dnd_on     = dnd();

// width 244 = 4×48 buttons + 3×12 gaps + 2×8 pad
// buttons straddle the bar edge (.btn y_offset -14), clip_top 28 hides the overhang
surface hud {
	layer = overlay;
	anchor = top;
	width  = 244;
	height = 36;
	font_size = 22;
	clip_top = 28;
	reveal_on_hover = 28;
	reveal_anim_ms  = 200;
	reveal_easing   = ease_out;
	bg = CRUST;
	border_width    = 0;
	radius_bl       = 14;
	radius_br       = 14;
	// lo..hi is an animated fillet range
	fillet_tl       = 0..18;
	fillet_tr       = 0..18;
	fillet_offset_y = 0;

	widget gamma_btn.btn.toggle {
		icon = 0xf186;
		bg = gamma_warm.value == "1" ? PEACH : SURFACE;
		fg = gamma_warm.value == "1" ? DARK  : FG;
		on_click() = exec("sh -c 'wispctl gamma is-warm && wispctl gamma off || wispctl gamma flat'");
	}
	widget dnd_btn.btn.toggle {
		icon = 0xf1f6;
		bg = dnd_on.value == "on" ? SAGE : SURFACE;
		fg = dnd_on.value == "on" ? DARK : FG;
		on_click() = exec("wispctl dnd toggle");
	}
	widget vol_btn.btn.pop {
		icon = 0xf028;
		on_click() = exec("foot -T ws-hud-vol --app-id=ws-hud-vol -e wiremix");
	}
	widget wifi_btn.btn.pop {
		icon = 0xf1eb;
		on_click() = exec("foot -T ws-hud-wifi --app-id=ws-hud-wifi -e impala");
	}
}

#hud widget  {
	align = center;
}
.btn         {
	width = 48;
	height = 48;
	pad = 12;
	y_offset = -14;
	border_width = 0;
	radius = 14;
	bg = SURFACE;
	fg = FG;
}
.toggle      {
	transition_bg = 180ms;
	transition_fg = 180ms;
}
.pop:pressed {
	bg = TEAL;
}

// osd

surface osd {
	spawned_by = osd;
	layer = overlay;
	anchor = top;

	max = 8;
	width = 340;
	height = 60;
	margin = 0;
	gap = 0;
	pad_x = 14;
	icon_gap = 12;
	prog_h = 10;
	body_lines = 4;
	body_max = 256;
	timeout_low = 3000;
	timeout_normal = 5000;
	timeout = 1200;
	bg = CRUST;
	fg = FG;
	border = CLEAR;
	prog_fg = ACCENT;
	prog_track = SUNK;
	radius = 10;
	fillet_r = 14;
	separator = SUNK;
	separator_frac = 80;
	dismiss_on_click = true;
	focus_follow = true;
	dbus_close = true;

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
		track_bg = SUNK;
		track_fg = ACCENT;
		track_radius = 5;
	}
}

#osd widget {
	fg = FG;
	pad = 0;
}

#osd widget:warn {
	fg = AMBER;
}
#prog:warn {
	track_fg = AMBER;
}
#prog:mute {
	track_fg = WARN;
}

// pct posts render as this pill instead of joining the osd stack
surface pill {
	spawned_by = osd_pill;
	layer = overlay;
	anchor = top;
	width = 220;
	height = 40;
	margin = -20;
	radius = 8;
	fillet_r = 14;
	font_size = 20;

	widget icon {
		align = left;
		width = 40;
		pad = 4;
		icon = $icon;
	}
	widget prog {
		slider;
		align = left;
		width = 162;
		height = 10;
		value = $progress;
		value_max = 100;
		track_bg = SUNK;
		track_fg = ACCENT;
		track_radius = 5;
	}
}

#pill {
	bg = CRUST;
}
#pill widget {
	fg = FG;
}
#pill widget:warn {
	fg = AMBER;
}

// subsystems

lock {
	bg       = BLACK; // only shows when the wallpaper is missing
	ring     = LOCKRING;
	ring_bad = LOCKBAD;
	fg       = LOCKFG;
	dim      = SCRIM;
	caps     = LOCKCAPS;
	prompt   = "Password";
	pam      = "system-auth";
	font_size = 16;

	// x/y are insets from the anchored edges, not absolute coords
	text clock { anchor = top; y = 120; text = "{time}"; format = "%H:%M";
	             fg = LOCKFG; font_size = 64; }
	text date  { anchor = top; y = 210; text = "{time}"; format = "%A %e %B";
	             fg = DIM; font_size = 16; }

	// show holds one condition only, hence the split caps/non-caps ring pair
	ring dial      { anchor = bottom | right; x = 160; y = 48; radius = 44;
	                 thickness = 10; bg = LOCKBG; border = LOCKED;
	                 highlight = LOCKFG; highlight_bs = DIM;
	                 separator = LOCKED; show = !caps; }
	ring dial_caps { anchor = bottom | right; x = 160; y = 48; radius = 44;
	                 thickness = 10; bg = LOCKBG; border = LOCKED;
	                 highlight = LOCKCAPS; highlight_bs = DIM;
	                 separator = LOCKED; show = caps; }
	ring dial_bad  { anchor = bottom | right; x = 160; y = 48; radius = 44;
	                 thickness = 10; fg = LOCKBAD; show = wrong; }

	frame card { anchor = bottom | left; x = 48; y = 48;
	             width = 360; height = 96; radius = 12;
	             bg = LOCKBG; border = LOCKRING; border_width = 1; }
	text label { anchor = bottom | left; x = 72; y = 108; text = "{prompt}";
	             fg = DIM; font_size = 14; }
	text dots  { anchor = bottom | left; x = 72; y = 72; text = "{dots}";
	             show = !wrong; fg = LOCKFG; }
	text bad   { anchor = bottom | left; x = 72; y = 72; text = "wrong password";
	             show = wrong; fg = LOCKBAD; }
	text caps_ind { anchor = bottom | right; x = 48; y = 48; text = "CAPS";
	                show = caps; fg = LOCKCAPS; font_size = 14; }
	text kbd   { anchor = bottom | right; x = 48; y = 72; text = "{layout}";
	             fg = DIM; font_size = 14; }
}

gamma {
	day_k     = 6500;
	night_k   = 2800;
	flat_k    = 2400;
	day_hour  = 7;
	night_hour = 20;
	fade_min   = 30; // 0 = hard step
	transition_ms = 300;
}

wallpaper {
	path = WALL;
	bg   = CRUST;
	transition = wipe;
	wipe_dir   = down_right;
	wipe_soft  = 200;
	fade_ms    = 300;
}

media {
}

// menus

surface menu {
	spawned_by = menu;
	layer = overlay;
	exclusive_zone = -1;

	axis   = horizontal;
	prompt = "run:";
	sort   = "most_used";

	bg = CRUST;
	pad_x = 8;

	group query {
		pad = 8;
		gap = 6;
		bg = CLEAR;
		border = CLEAR;
		cell {
			text = menu.prompt;
			fg = DIM;
		}
		cell {
			text = menu.query;
			fg = FG;
		}
		cell {
			text = "_";
			fg = FG;
		}
	}
	for row in rows {
		cell {
			text  = row.label;
			fg    = FG;
			bg    = row.selected ? ACT : CLEAR;
			pad_x = 8;
		}
	}
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
		exec = "loginctl terminate-session \"\"";
	}
	item {
		icon = 0xf186;
		label = "Sleep";
		exec = "loginctl suspend";
	}
}

menu emoji {
	preset = emoji;
}

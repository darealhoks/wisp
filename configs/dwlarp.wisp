// ==================================
//               BAR
// ==================================

source time   = clock("%H:%M");
source date_s = clock("%b %d");
source tags   = tags();
source cpu_s  = cpu();
source mem_s  = mem();
source bat_s  = bat("BAT0");
source temp_s = temp();
source wifi_s = net("wlan0");
source disk_s = disk("/");

const FG    = #ffffffff;
const DIM   = #ff7a808b;
const URG   = #ffee3300;
const ACT   = #ff2a2f3a;
const WARN  = #ffff5050;

surface bar {
	layer = top;
	anchor = top | left | right;
	height = 28;
	exclusive_zone = 28;
	bg = #ff0f1219;

	// concave feet curl down the screen edges; buffer grows 10px below but
	// exclusive_zone stays 28 so windows tile flush under the bar body
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
			visible = tag.pinned || tag.occupied || tag.active || tag.urgent;
			on_click() = exec("wispctl tag {tag.index}");
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
		icon = wifi_s.signal >= 3 ? 0xf0928
			: wifi_s.signal >= 2 ? 0xf0925
			: wifi_s.signal >= 1 ? 0xf0922
			:                      0xf091f;
		pad = 16;
		visible = wifi_s.signal >= 0;
	}
	widget bat    {
		align = right;
		icon = bat_s.charging  ? 0xf0084
			: bat_s.pct >= 75 ? 0xf240
			: bat_s.pct >= 50 ? 0xf241
			: bat_s.pct >= 25 ? 0xf242
			: bat_s.pct >= 10 ? 0xf243
			:                   0xf244;
		text = " {bat_s.pct}%";
		fg   = bat_s.pct < 15 ? WARN : (bat_s.charging ? #ff7fbf9f : FG);
	}
	widget sep.dim {
		align = right;
		text = "/";
	}
	widget mem    {
		align = right;
		icon = 0xefc5;
		text = " {mem_s.pct}%";
	}
	widget temp   {
		align = right;
		icon = 0xf0238;
		text = " {temp_s.c} C";
	}
	widget cpu    {
		align = right;
		icon = 0xf4bc;
		text = " {cpu_s.pct}%";
		fg = cpu_s.pct > 80 ? WARN : FG;
	}
	widget disk   {
		align = right;
		icon = 0xf02ca;
		text = " {disk_s.pct}%";
	}
}

// 22px module rhythm; edge insets and labels override inline
widget {
	fg = FG;
	pad = 22;
}
.dim        {
	fg = DIM;
}

#bar cell          {
	fg = DIM;
	bg = #00000000;
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

// ==================================
//          SCREEN CORNERS
// ==================================

// click-through strip that rounds the bottom two screen corners to match the
// bar feet up top; armpit_outer rounds in place, input=none passes clicks through
surface screen_corners {
	layer  = top;
	anchor = bottom | left | right;
	height = 10;
	exclusive_zone = 0;
	bg = #00000000;
	armpit_outer = 10;
	armpit_color = #ff0f1219; // must match bar bg
	input = none;
}

// ==================================
//               HUD
// ==================================

// native event-driven sources: flip on the real state change, no polling
source gamma_warm = gamma_warm();
source dnd_on     = dnd();

const SURFACE = #26ffffff;
const PEACH   = #ffe6b89c;
const SAGE    = #ff8fb3a3;
const TEAL    = #ff7fb0bb;
const DARK    = #ff1a2530;

// width 244 = 4·48 buttons + 3·12 gaps + 2·8 pad. buttons straddle the bar
// bottom edge (y_offset=-14) and clip_top=28 hides their pixels above the bar
surface hud {
	layer = overlay;
	anchor = top;
	width  = 244;
	height = 36;
	font_size = 22;
	clip_top = 28;
	reveal_on_hover = 28;
	reveal_anim_ms  = 200;
	reveal_easing   = ease-out;
	bg = #ff0f1219;
	border_width    = 0;
	radius_bl       = 14;
	radius_br       = 14;
	// lo..hi animated fillet; outward top fillets flare into the bar so the
	// panel reads as pulled out of it, not a detached pill
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
		on_click() = exec("foot -T ws-hud-vol --app-id=ws-hud-vol -e pulsemixer");
	}
	widget wifi_btn.btn.pop {
		icon = 0xf1eb;
		on_click() = exec("foot -T ws-hud-wifi --app-id=ws-hud-wifi -e impala");
	}
}

// .toggle recolors on state, .pop flashes on press
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

// ==================================
//           OSD (notifs)
// ==================================

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
	bg = #ff0f1219;
	fg = FG;
	border = #00000000;
	prog_fg = #ff84a7b3;
	prog_track = #ff1c2733;
	// margin 0 → flush under the bar; only bottom corners round, fillets claw
	// up into the bar row at the junction
	radius = 10;
	fillet_r = 14;
	separator = #ff1c2733;
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
		track_bg = #ff1c2733;
		track_fg = #ff84a7b3;
		track_radius = 5;
	}
}

#osd widget {
	fg = FG;
	pad = 0;
}

// :warn / :mute pseudos read the derived per-slab $warn / $mute bindings
#osd widget:warn {
	fg = #ffffaa20;
}
#prog:warn {
	track_fg = #ffffaa20;
}
#prog:mute {
	track_fg = WARN;
}

// pct posts (volume/brightness) render as this pill instead of joining the
// stack; negative margin rests it inside the bar row so all four corners round
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
		track_bg = #ff1c2733;
		track_fg = #ff84a7b3;
		track_radius = 5;
	}
}

#pill {
	bg = #ff0f1219;
}
#pill widget {
	fg = FG;
}
#pill widget:warn {
	fg = #ffffaa20;
}

// ==================================
//            Subsystems
// ==================================

lock {
	bg       = #ff000000; // fallback fill when the wallpaper is missing
	ring     = #ff5f8a93;
	ring_bad = #ffd06878;
	fg       = #ffa8d5cc;
	dim      = #ff7a808b;
	caps     = #ffe0c060;
	scrim    = #5c0e1622; // alpha = base dim strength
	road     = #ffd9a24b;
	prompt   = "Password";
	pam      = "system-auth";
}

gamma {
	day_k     = 6500;
	night_k   = 2800;
	flat_k    = 2400;
	day_hour  = 7;
	night_hour = 20;
}

wallpaper {
	path = "~/.local/share/dwl/wallpaper.png";
	bg   = #ff0f1219;
	transition = wipe;
	wipe_dir   = down_right;
	wipe_soft  = 200;
	fade_ms    = 300;
}

media {
}

// ==================================
//              Menus
// ==================================

// dmenu strip: full-width top band, prompt + query then items left→right
surface menu {
	spawned_by = menu;
	layer = overlay;
	exclusive_zone = -1;
	keyboard = exclusive;

	axis   = horizontal;
	prompt = "run:";
	sort   = "most_used";

	bg = #ff0f1219;
	pad_x = 8;

	group query {
		pad = 8;
		gap = 6;
		bg = #00000000;
		border = #00000000;
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
			bg    = row.selected ? ACT : #00000000;
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

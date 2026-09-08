//! font = /usr/share/fonts/liberation-fonts/LiberationMono-Regular.ttf

// dwm's default bar, ported: tags, layout symbol, window title, status text.
// Everything below the config is a commented starting point for the other
// subsystems — uncomment what you want.

const NORM_BG  = #ff222222;
const NORM_BRD = #ff444444;
const NORM_FG  = #ffbbbbbb;
const SEL_FG   = #ffeeeeee;
const SEL_BG   = #ff005577;
const BLACK    = #ff000000;

// dwm's layout symbol is local state, not the compositor's: clicking cycles it
mut layout = "[]=";

source tags   = tags();
source time   = clock("%a %d %b %H:%M");
source cpu_s  = cpu(every="2s");
source mem_s  = mem(every="2s");

surface bar {
	layer = top;
	anchor = top | left | right;
	height = 22;
	exclusive_zone = 22;
	font_size = 14;
	bg = NORM_BG;

	for tag in tags.list {
		cell.tag {
			text = tag.label;
			fg   = tag.occupied ? NORM_FG : NORM_BRD;
		}
	}

	widget lt {
		align = left;
		text  = layout;
		fg    = NORM_FG;
		pad_x = 7;
		on_click() = set(layout, layout == "[]=" ? "><>"
		                       : layout == "><>" ? "[M]"
		                       :                   "[]=");
		// or drive your compositor's own layouts through its CLI, mango:
		//   source layout = exec_line("mmsg get layouts", every="2s");
		//   on_click() = exec("mmsg dispatch switch_layout");
		// then drop the `mut layout` above
	}

	widget title {
		align = left;
		text  = tags.title;
		fg    = SEL_FG;
		pad_x = 7;
		elide;
	}

	widget clock {
		align = right;
		text  = time;
		fg    = NORM_FG;
		pad_x = 7;
	}
	widget mem {
		align = right;
		text  = "mem {mem_s.pct}%";
		fg    = NORM_FG;
		pad_x = 7;
	}
	widget cpu {
		align = right;
		text  = "cpu {cpu_s.pct}%";
		fg    = NORM_FG;
		pad_x = 7;
	}
}

.tag        { bg = NORM_BG; pad_x = 7; }
.tag:active { fg = SEL_FG; bg = SEL_BG; }
.tag:urgent { fg = SEL_BG; bg = SEL_FG; }

// path is required and a missing file falls back to bg: dwm has no wallpaper
wallpaper {
	path = "";
	bg   = BLACK;
	fade_ms = 0;
}

// ---------------------------------------------------------------------------
// Template: every subsystem, commented out. Uncomment what you want, delete
// the rest. Full reference: docs/modules.md, docs/builtins.md.
// ---------------------------------------------------------------------------

// -- more sources ------------------------------------------------------------
// source bat_s   = bat("BAT0");            // .pct .charging
// source temp_s  = temp(every="2s");       // .c
// source disk_s  = disk("/", every="600s");// .pct
// source wifi_s  = net("");                // .ssid .up .signal .wired
// source vol_s   = pipewire();             // .vol .mute .mic_vol .mic_mute
// source bt_s    = bluez();                // .device .powered .connected .battery
// source pp_s    = power_profile();        // .profile
// source bl_s    = backlight();            // .pct
// source vpn_s   = vpn();                  // .state .ok
// source mpd_s   = mpris();                // .title .artist .status .player
// source notes   = notifications();        // .count .open, .history for a list
// source tray_s  = tray(icon_size=16);     // .count, .items for a list
// source warm    = gamma_warm();           // "1" / "0"
// source dnd_s   = dnd();                  // "on" / "off"
// source shell_s = exec_line("uname -r", every="60s");
// source watch_s = inotify(path="/tmp/x");

// -- notification popups (replaces mako) --------------------------------------
// surface osd {
// 	spawned_by = osd;
// 	layer = overlay;
// 	anchor = top | right;
// 	max = 5;
// 	width = 340;
// 	height = 60;
// 	margin = 8;
// 	gap = 6;
// 	timeout_normal = 5000;
// 	bg = NORM_BG;
// 	border = NORM_BRD;
// 	dismiss_on_click = true;
//
// 	widget icon  { align = left; width = 40; icon = $icon; }
// 	widget title { align = left; text = $nbody > 0 ? "{$summary}\n{$body}" : $summary;
// 	               body_lines = 1 + $nbody; fg = SEL_FG; elide; pad = 12; }
// }

// -- volume/brightness pill (wispctl volume, wispctl backlight) ---------------
// surface pill {
// 	spawned_by = osd_pill;
// 	layer = overlay;
// 	anchor = bottom;
// 	width = 220;
// 	height = 40;
// 	margin = 60;
// 	bg = NORM_BG;
//
// 	widget icon { align = left; width = 40; icon = $icon; fg = SEL_FG; }
// 	widget prog { slider; align = left; width = 162; height = 8;
// 	              value = $progress; value_max = 100;
// 	              track_bg = NORM_BRD; track_fg = SEL_BG; }
// }

// -- run launcher (wispctl menu run) ------------------------------------------
// surface menu {
// 	spawned_by = menu;
// 	layer = overlay;
// 	exclusive_zone = -1;
// 	axis = vertical;
// 	width = 400;
// 	row_h = 22;
// 	prompt = "run:";
// 	sort = "most_used";
// 	bg = NORM_BG;
//
// 	group query {
// 		height = 22;
// 		cell { text = menu.prompt; fg = NORM_BRD; }
// 		cell { text = menu.query;  fg = SEL_FG; }
// 	}
// 	for row in rows {
// 		cell { text = row.label; fg = row.selected ? SEL_FG : NORM_FG;
// 		       bg = row.selected ? SEL_BG : NORM_BG; pad_x = 7; }
// 	}
// }

// -- static menus (wispctl menu power) ----------------------------------------
// menu power {
// 	item { icon = 0xf011; label = "Poweroff"; exec = "loginctl poweroff"; }
// 	item { icon = 0xf021; label = "Reboot";   exec = "loginctl reboot"; }
// }
// menu emoji { preset = emoji; }

// -- hover tooltips (widget tooltip = "text") ---------------------------------
// surface tooltip {
// 	spawned_by = tooltip;
// 	layer = overlay;
// 	width = 200;
// 	height = 22;
// 	delay_ms = 500;
// 	bg = NORM_BG;
// 	widget t { align = center; text = $text; fg = NORM_FG; }
// }

// -- lock screen (wispctl lock) ------------------------------------------------
// lock {
// 	pam    = "system-auth";
// 	prompt = "Password";
// 	bg     = BLACK;
// 	dim    = #59000000;
// 	fg     = SEL_FG;
// 	text clock { anchor = top; y = 120; text = "{time}"; format = "%H:%M";
// 	             fg = SEL_FG; font_size = 64; }
// 	text dots  { anchor = bottom; y = 120; text = "{dots}"; show = !wrong; fg = SEL_FG; }
// 	text bad   { anchor = bottom; y = 120; text = "wrong password"; show = wrong; fg = #ffcc3333; }
// }

// -- idle timeouts (replaces swayidle) ----------------------------------------
// idle {
// 	timeout dim   { after = 240s; run = "wispctl backlight set 10"; resume = "wispctl backlight set 80"; }
// 	timeout blank { after = 300s; run = "wispctl dpms off"; resume = "wispctl dpms on"; }
// 	timeout sleep { after = 900s; run = "loginctl suspend"; }
// 	before_sleep = lock;
// }

// -- night light (wispctl gamma auto|day|night|flat|off) ----------------------
// gamma {
// 	day_k = 6500;
// 	night_k = 3200;
// 	day_hour = 7;
// 	night_hour = 20;
// }

// -- media keys + PipeWire volume (body must stay empty) ----------------------
// media { }

// -- extra panel: a dock, a tray strip, anything not named `bar` --------------
// surface dock {
// 	layer = top;
// 	anchor = bottom;
// 	height = 40;
// 	exclusive_zone = 0;
// 	bg = NORM_BG;
// 	for it in tray_s.items {
// 		cell { icon = it.icon; width = 24;
// 		       on_click()       = exec("wispctl tray activate {it.index}");
// 		       on_right_click() = exec("wispctl tray menu {it.index}"); }
// 	}
// }

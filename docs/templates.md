# Template library

Copy-paste starting points. Every block on this page is a complete config that
passes `wispc --check` and `wispc --emit`. Combine them by pasting them into one
file; the top level takes declarations in any order.

## Generic bar

```wisp
source time = clock("%H:%M");
source bat_s = bat("BAT0");

const TEXT = #ffdbe2ee;
const CRUST = #ff0e131c;
const RED = #ffe0603f;

surface bar {
	layer = top;
	anchor = top | left | right;
	height = 28;
	exclusive_zone = 28;
	bg = CRUST;

	widget distro {
		align = left;
		icon = 0xf32e;
		fg = #ffa8bfdd;
		pad = 8;
	}
	widget bat {
		align = right;
		icon = bat_s.charging ? 0xf0084 : 0xf241;
		text = "{bat_s.pct}%";
		fg = bat_s.pct < 15 ? RED : TEXT;
	}
	widget clock {
		align = right;
		text = time;
		fg = TEXT;
		pad = 8;
	}
}
```

- Naming it `bar` is what the tag accumulator and lock-on-output path look for.
- `exclusive_zone` defaults to `height`; use `0` to overlay, `-1` to overlay *and* flip on the menu feature.
- Adding radius, border, fillet, armpit, `bg_bottom`, `clip_widgets`, a cutout or a slider disables partial repaint for the whole surface.
- Every OSD or menu property you paste in by accident is inert here.

## Workspace tags

```wisp
source tags = tags();

surface bar {
	anchor = top | left | right;
	height = 28;
	exclusive_zone = 28;
	bg = #ff0e131c;

	for tag in tags.list {
		cell.ws {
			text = tag.label;
			visible = tag.pinned || tag.occupied || tag.active || tag.urgent;
			on_click() = exec("wispctl tag {tag.index} {tag.output}");
		}
	}
	widget title {
		align = left;
		text = tags.title;
		fg = #ffa5adbb;
		elide;
	}
}

.ws        { fg = #ffdbe2ee; bg = #ff0e131c; width = 28; height = 28; radius = 8; }
.ws:active { fg = #ff64799c; width = 34; }
.ws:urgent { bg = #ffe0603f; }
```

- `tags.list` unrolls to 9 cells at compile time. Cell fields: `label` `index` `active` `urgent` `occupied` `pinned` `output`.
- Always interpolate `{tag.output}` into the click, or clicks retarget the focused monitor.
- Only `tags.title` reads as a scalar; per-tag state comes from the `for` cell (`tag.occupied` etc.).
- `tag.pinned` is a compile-time mask from `tags(pinned=…)`, not compositor state.

## HUD

```wisp
source g = gamma_warm();

surface hud {
	layer = overlay;
	anchor = top;
	width = 120;
	height = 40;
	reveal_on_hover = 20;
	reveal_gutter = 3;
	reveal_anim_ms = 200;
	reveal_easing = ease_out;
	bg = #ff0e131c;

	widget gamma_btn {
		align = center;
		icon = 0xf186;
		fg = g.value == "1" ? #ff64799c : #ffdbe2ee;
		on_click() = exec("wispctl gamma flat");
	}
}
```

- `reveal_on_hover` alone is what makes a surface a HUD; it is the width of the input trigger strip.
- `reveal_gutter` defaults to `reveal_on_hover` and is unpainted; set `0` to paint from the anchored edge, which is what a floating bar wants.
- `exclusive_zone` defaults to `-1` here so the trigger can overlap the bar; declare `0` to sit below it.
- `margin` is inert on a HUD. Corner anchors slide diagonally, and `anim.emerged_h`/`anim.emerged_w` are readable in the body.

## OSD stack

```wisp
surface osd {
	spawned_by = osd;
	layer = overlay;
	anchor = top;
	max = 4;
	width = 340;
	height = 60;
	margin = 6;
	pad_x = 14;
	body_lines = 4;
	body_max = 256;
	bg = #ff0e131c;
	fg = #ffdbe2ee;

	widget icon {
		align = left;
		width = 58;
		icon = $image;
	}
	widget title {
		align = left;
		text = $nbody > 0 ? "{$summary}\n{$body}" : $summary;
		body_lines = 1 + $nbody;
		elide;
		pad = 12;
	}
	widget prog {
		slider;
		align = left;
		width = 312;
		height = 10;
		visible = $progress >= 0;
		value = $progress;
		value_max = 100;
		track_bg = #ff141a26;
		track_fg = #ff64799c;
	}
}

#osd widget { fg = #ffdbe2ee; }
#osd widget:warn { fg = #ffe0603f; }
#prog:mute { track_fg = #ffa5adbb; }
```

- The surface **must** be named `osd`; both the name and `spawned_by = osd` are load-bearing, and renaming it silently removes the notification engine.
- `$image` needs `image = N` on the surface to decode cover art at all; it falls back to the `$icon` glyph.
- `body_lines` and `body_max` have no compiled-in default, set them.
- A widget named `icon` donates its `width` as the reserved text column, so icon-less slabs stay aligned.
- Layer, exclusive zone, visibility and input are all inert; `osd.c` creates and sizes the surface itself.

## Tooltip

```wisp
surface tooltip {
	spawned_by = tooltip;
	layer = overlay;
	exclusive_zone = -1;

	font_size  = 14;
	width      = 320;   // clamp; the surface auto-widths to $text and elides past it
	height     = 26;
	pad_x      = 8;
	anchor_gap = 4;
	delay_ms   = 500;   // hover dwell before it appears

	bg = #ff0e131c;
	border = #ff2e3a4e;
	border_width = 1;
	radius = 6;

	widget label { align = left; text = $text; fg = #ffa5adbb; elide; }
}
```

- Bindings: `$text` only. Triggered by `tooltip = "…"` on any bar or HUD cell, or manually with `wispctl tooltip <x> <width> <below> "text"` / `wispctl tooltip hide`.
- Decoration only: `keyboard_interactivity` is 0 and the input region is zero-area, so a tooltip can never take focus or a click.
- It always hangs below the anchoring cell, x-clamped to the output; there is no flip-above near a screen edge.
- Declaring `tooltip` on a cell with no `spawned_by = tooltip` surface is a compile error, not a silent no-op.

## OSD pill

```wisp
surface pill {
	spawned_by = osd_pill;
	layer = overlay;
	anchor = top;
	width = 220;
	height = 40;
	margin = -20;
	radius = 8;
	fillet_r = 14;
	bg = #ff0e131c;

	widget icon {
		align = left;
		width = 40;
		icon = $icon;
		fg = #ffdbe2ee;
	}
	widget prog {
		slider;
		align = left;
		width = 164;
		height = 10;
		value = $progress;
		value_max = 100;
		track_bg = #ff141a26;
		track_fg = #ff64799c;
	}
}
```

- Declaring it is the gate: once a `pill` exists, progress-only posts route here instead of joining the stack.
- `width` defaults to 0, which means the pill is not compiled in at all.
- A negative `margin` rests the pill that many pixels inside the bar row and rounds all four corners; 0 or less sits it flush with fillet claws.
- No `pad_x` on a slab, so the leading gap is the first widget's own `width`. No cover art, `$image` is not wired here.

## Generic vertical menu

```wisp
surface menu {
	spawned_by = menu;
	layer = overlay;
	exclusive_zone = -1;

	axis = vertical;
	width = 320;
	margin = 6;
	max_visible = 5;
	row_h = 34;
	prompt = "run: ";
	sort = "most_used";
	icons = true;

	bg = #ff0e131c;
	pad_x = 8;
	pad_y = 6;

	group query {
		height = 34;
		pad = 0;
		pad_x = 8;
		gap = 0;
		bg = #00000000;
		cell { text = menu.prompt; fg = #ffa5adbb; }
		cell { text = menu.query;  fg = #ffdbe2ee; }
		cell { text = "_";         fg = #ffdbe2ee; }
	}
	for row in rows {
		cell {
			height = 34;
			icon = row.icon;
			text = row.label;
			fg = #ffdbe2ee;
			bg = row.selected ? #ff141a26 : #00000000;
			radius = 4;
			pad_x = 8;
			elide;
		}
	}
}

menu power {
	item { icon = 0xf011; label = "Poweroff"; exec = "loginctl poweroff"; }
	item { icon = 0xf021; label = "Reboot";   exec = "loginctl reboot"; }
}
menu emoji { preset = emoji; }
```

- Row fields: `row.label`, `row.icon`, `row.has_icon`, `row.index`, `row.selected`, plus `row.enabled`, `row.separator`, `row.toggle` and `row.checked` — the last four are populated by tray dropdowns from dbusmenu. A separator can never be selected: click, hover and the arrow keys all step over it. Give the menu `separator_h` and a `separator` colour and separator rows shrink to that slot and draw a 1px line instead of a cell, `separator_frac` percent of the content width.
- `axis = vertical` is the top-centred launcher float; the header row must declare a `height` or the header measures as zero.
- `row_h` sizing is computed against font size 14, so set it explicitly whenever `font_size` is not 14.
- `icons = true` decodes app icons and is the launcher's biggest RAM and IO cost.
- Surface-level `bg`, `border`, `radius` and `layer` are inert; the body carries the look. `exclusive_zone = -1` is still what flips on the menu feature.

## Generic horizontal menu

```wisp
surface menu {
	spawned_by = menu;
	layer = overlay;
	exclusive_zone = -1;

	axis = horizontal;
	prompt = "run:";
	sort = "most_used";
	bg = #ff0f1219;
	pad_x = 8;

	group query {
		pad = 8;
		gap = 6;
		bg = #00000000;
		border = #00000000;
		cell { text = menu.prompt; fg = #ffa5adbb; }
		cell { text = menu.query;  fg = #ffdbe2ee; }
		cell { text = "_";         fg = #ffdbe2ee; }
	}
	for row in rows {
		cell {
			text = row.label;
			fg = #ffdbe2ee;
			bg = row.selected ? #ff64799c : #00000000;
			pad_x = 8;
		}
	}
}
```

- Anything other than `axis = vertical` is the full-width dmenu strip, items left to right.
- Row fields are `label` `icon` `selected` `index`; `icon` reserves its column even on rows without one, unless no row in the menu has one at all.
- Per-menu overrides on a `menu NAME {}` declaration (`width`, `row_h`, `max_visible`, `anchor_gap`, `pad_y`) inherit when 0.
- A bare `hover;` in a menu declaration makes pointer motion move the selection, at the cost of repainting on motion.

## Tray

```wisp
source tray_s = tray(icon_size=20);

surface bar {
	anchor = top | left | right;
	height = 34;
	exclusive_zone = 34;
	bg = #ff0e131c;

	group traygrp {
		align = right;
		pad_x = 4;
		gap = 0;
		bg = #ff141a26;
		radius = 8;
		for it in tray_s.items {
			cell.tray {
				icon = it.icon;
				bg = it.menu_open ? #ff141a26 : #00000000;
				text = it.has_icon ? "" : it.id;
				visible = it.status != "Passive";
				on_click()        = exec("wispctl tray activate {it.index}");
				on_right_click()  = exec("wispctl tray menu {it.index}");
				on_middle_click() = exec("wispctl tray secondary {it.index}");
			}
		}
	}
}

.tray { align = right; fg = #ffdbe2ee; width = 28; height = 26; radius = 6; }
```

- `icon_size` is an integer literal from 8 to 64, default 16, and bakes the tray icon size for the whole build.
- Item fields: `icon` `has_icon` `title` `id` `status` `index` `menu_open`. The runtime compacts the list to 8.
- wisp owns the StatusNotifier watcher name, so the daemon has to start before the tray apps do.
- Icons resolve through hicolor and `/usr/share/pixmaps`, PNG only, decoded once per name.

## Graph

```wisp
source cpu_s = cpu();

surface bar {
	anchor = top | left | right;
	height = 28;
	exclusive_zone = 28;
	bg = #ff0e131c;

	widget cpugraph {
		align = right;
		graph = cpu_s.pct;
		graph_max = 100;
		graph_samples = 60;
		graph_fg = #ff64799c;
		width = 60;
		height = 20;
		bg = #ff141a26;
		radius = 4;
	}
}
```

- The field must be a numeric status field from `cpu`, `mem`, `temp`, `bat`, `disk` or `backlight`. Not `net()` rates, not `exec_line()`.
- The ring is sampled inside that source's existing tick, so a graph costs nothing extra at idle.
- `width` is the main-axis extent, roughly one pixel per sample, and the newest sample is flush right.
- A graph inside a `group` is a hard error. Max 32 graph widgets.

## Slider

```wisp
mut vol = 0.5;

surface panel {
	layer = overlay;
	anchor = top | right;
	width = 200;
	height = 40;
	margin = 8;
	exclusive_zone = 0;
	bg = #ff0e131c;

	widget volume {
		slider;
		align = left;
		orientation = horizontal;
		width = 160;
		height = 12;
		value = vol;
		track_bg = #ff141a26;
		track_fg = #ff64799c;
		track_radius = 6;
		thumb_size = 12;
		thumb_shape = circle;
		thumb_color = #ffdbe2ee;
		show_value;
		value_format = "%.0f";
		value_scale = 100;
		value_fg = #ffdbe2ee;
		on_change() = exec("wispctl volume {vol}");
	}
}
```

- `value` is required; a bare `mut` identifier makes the slider read-write and dragging writes it. Missing `value` fails `--emit`.
- `on_change()` takes no parameter. `on_change(p)` fails `--emit`; the thumb writes the `mut` as a 0..1 double before the body runs.
- Track length is the main-axis dimension, thickness the other; omit the thickness to fill.
- Knob travel is inset by half the thumb at each end so the physical ends still give exactly 0 and 1.

## Compound

```wisp
source time = clock("%H:%M");

compound frame {
	layer = top;
	anchor = top | left;
	width = 1920;
	height = 1080;
	bg = #ff0e131c;
	radius_inner = 12;

	region top {
		edge = top;
		size = 28;
		widget clock { align = right; text = time; fg = #ffdbe2ee; pad = 12; }
	}
	region left {
		edge = left;
		size = 28;
		widget logo { align = top; icon = 0xf32e; fg = #ffa8bfdd; }
	}
}
```

- `width` and `height` are required and are the bounding box regions are carved from; 0 or less is a hard error.
- Every region needs `edge` (a single identifier that is a bit of the compound `anchor`) and an integer literal `size`.
- One region per edge, at least one, at most 8, 256 items across all of them.
- `group` is rejected inside a region, and there is no per-region animation, nesting or independent visibility.

## Gamma

```wisp
gamma {
	day_k = 6500;
	night_k = 2800;
	flat_k = 2400;
	day_hour = 7;
	night_hour = 20;
	fade_min = 30;
	transition_ms = 300;
}

surface bar {
	anchor = top | left | right;
	height = 24;
	exclusive_zone = 24;
	bg = #ff0e131c;
	widget g {
		align = right;
		icon = 0xf186;
		fg = #ffdbe2ee;
		on_click() = exec("wispctl gamma flat");
	}
}
```

- `flat_k` is the manual override and is deliberately warmer than `night_k`.
- `fade_min` is minutes of linear crossfade centred on each schedule edge; 0 is a hard step.
- `transition_ms` tweens manual switches at 60 Hz; 0 compiles the tween out entirely.
- A `gamma {}` config is never idle-zero-CPU: without a polled status source it gets a dedicated 1 Hz timer.

## Lock

```wisp
lock {
	pam = "system-auth";
	prompt = "Password";
	bg = #ff000000;
	wall = true;
	dim = #60000000;
	fg = #ffa8bfdd;
	ring_bad = #ffe0603f;
	caps = #ffe0603f;
	font_size = 20;
	wrong_ms = 1200;
	retry_ms = 1000;
	lockout_after = 10;

	text clock { anchor = top; y = 120; text = "{time}"; format = "%H:%M";
	             fg = #ffa8bfdd; font_size = 64; }

	// One ring per state — that is how per-state colour is spelled.
	ring halo { y = -120; radius = 52; thickness = 6; segments = 12; gap = 4;
	            bg = #d90e131c; fg = #ffa8bfdd; show = !wrong; }
	// Keypress arc only while something is typed: a stroke-less ring of the
	// same geometry, gated on `typing`.
	ring keys { y = -120; radius = 52; thickness = 6; fg = #00000000;
	            show = typing; highlight = #ffdbe2ee; highlight_bs = #ffa5adbb;
	            separator = #ff0e131c; }
	ring halo_bad { y = -120; radius = 52; thickness = 6; segments = 12; gap = 4;
	                bg = #d90e131c; fg = #ffe0603f; show = wrong; }

	// No anchor bit on an axis means centred; x/y are then nudges.
	frame card { width = 320; height = 96; radius = 8;
	             bg = #d90e131c; border = #ff64799c; border_width = 2; }
	text label { y = -24; text = "{prompt}"; fg = #ffa5adbb; font_size = 14; }
	text dots  { text = "{dots}"; show = !wrong; fg = #ffa8bfdd; }
	text bad   { text = "wrong password"; show = wrong; fg = #ffe0603f; }
	text caps_ind { y = 80; text = "CAPS LOCK"; show = caps;
	                fg = #ffe0603f; font_size = 14; }
	text kbd   { anchor = bottom | right; x = 48; y = 48; text = "{layout}";
	             show = layout_alt; fg = #ffa5adbb; font_size = 14; }
}

surface bar {
	anchor = top | left | right;
	height = 24;
	exclusive_zone = 24;
	bg = #ff0e131c;
	widget lockbtn {
		align = right;
		icon = 0xf023;
		fg = #ffdbe2ee;
		on_click() = exec("wispctl lock");
	}
}
```

- The lock is a separate binary, `wisp-lock`, which links PAM. The daemon never does, and `wispctl lock` execs it rather than talking to the socket.
- Every visible piece is a declared `frame`/`text`/`ring` element. Declare none and you get the legacy layout: centred dots plus a wrong and a CAPS line.
- A `ring`'s `radius` is the ring's own radius and `gap` is in degrees; there are no per-state ring colours, only more `ring` elements with different `show`.
- `highlight` is the per-keypress arc; it is opt-in, jumps angle on every character and backspace, and needs no timer because the keystroke already repaints. It draws whenever its element's `show` passes, so gate it on `typing` for swaylock's behaviour of no arc on an empty buffer.
- `wall = true` draws the compiled `wallpaper { path }` (default `~/.local/share/dwl/wallpaper.png`), mapping the daemon's on-disk cover-fit cache and decoding it once itself if that cache is cold. A runtime `wispctl wall` override is not followed, and a missing file falls back to `bg`.
- `dim` is composited over the background, so an opaque colour hides the wallpaper entirely.
- `font_size` on the block and on each element feeds the baked font size list.

## Wallpaper and media

```wisp
source time = clock("%H:%M");

surface bar {
	anchor = top | left | right;
	height = 24;
	exclusive_zone = 24;
	bg = #ff0e131c;
	widget clock { align = right; text = time; fg = #ffdbe2ee; }
}

wallpaper {
	path = "~/wall.png";
	bg = #ff000000;
	transition = wipe;
	wipe_dir = down_right;
	wipe_soft = 200;
	fade_ms = 300;
}

media { }
```

- PNG only, decoded once at first configure; a missing file falls back to the solid `bg`.
- `wipe_dir` names where the edge travels **to**, so `right` reveals the new wallpaper from the left. Diagonals sweep on x+y.
- `wipe_soft` is the lerp band width in px; 1 is a hard line. `dither_px` (default 16) is the block size for `transition = dither`.
- `media { }` must be empty; any property inside is an error. It enables both the media keys and PipeWire.

## Gotchas

- Every block here is a whole config, not a fragment. Pasting two of them into one file is fine, but two `surface bar` declarations is a duplicate-declaration error.
- The `osd`, `pill`, `menu` and `tooltip` template names are load-bearing; `spawned_by` alone does not select the engine.
- Run `wispc --emit` as well as `--check` after editing any of these, several classes of error only appear at codegen.
- Combining the vertical and horizontal menu templates is not possible; there is one `surface menu` per config.

//! font_backend = freetype
//! font = ~/.local/share/fonts/MapleMono-NF-Bold.ttf
//! font_fallback = /usr/share/fonts/noto-emoji/NotoColorEmoji.ttf

source time   = clock("%H:%M");
source date_s = clock("%b %-d");
source tags   = tags();
source cpu_s  = cpu();
source mem_s  = mem();
source bat_s  = bat("BAT0");
source temp_s = temp();
source wifi_s = wifi("wlan0");
/* SUPER+b (mango) → `wispctl hide toggle`: bar + HUD gate on this. */
source hid    = ui_hidden();
source vol_s  = exec_line("v=$(wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null); case x$v in x) echo err;; *MUTED*) echo mute;; *) p=$(echo $v | tr -cd 0-9); if [ 0$p -lt 34 ]; then echo low; elif [ 0$p -lt 67 ]; then echo med; else echo high; fi;; esac", every="2s");

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
const SOLID  = #ff0e131c;   // opaque: translucent CRUST must not leak through the lock

surface bar {
  layer = top;
  anchor = top | left | right;
  height = 34;
  margin = 6;
  exclusive_zone = 34;         // reserve strip + top gap
  visible = hid.value == "0";  // destroy releases the zone

  bg = #00000000;
  radius = 0;

  widget edge_l { align = left; pad = 2; }
  group distrogrp {
    align = left;
    widget distro { icon = 0xf32e; }
  }
  group clockgrp {
    align = left;
    widget time { text = time; }
    widget date.dim { text = date_s; }
  }

  for tag in tags.list {
    cell.ws {
      text         = tag.label;
      visible      = tag.pinned || tag.occupied || tag.active || tag.urgent;
      on_click() = exec("wispctl tag {tag.index} {tag.output}");
    }
  }

  /* Right side is declared right-to-left → first group is rightmost. */
  widget edge_r { align = right; pad = 2; }

  group conngrp {
    align = right;
    widget wifi { icon = wifi_s.signal >= 3 ? 0xf0928
                       : wifi_s.signal >= 2 ? 0xf0925
                       : wifi_s.signal >= 1 ? 0xf0922
                       :                      0xf091f;
                  fg = wifi_s.signal >= 1 ? TEXT : RED;
                  on_click() = exec("foot -T ws-hud-wifi --app-id=ws-hud-wifi -e impala"); }
    widget sep_conn.sep { text = "/"; }
    widget audio { icon = vol_s.value == "mute" ? 0xf0581
                        : vol_s.value == "err"  ? 0xf0581
                        : vol_s.value == "low"  ? 0xf026
                        : vol_s.value == "med"  ? 0xf027
                        :                         0xf028;
                   fg = vol_s.value == "err"  ? RED
                      : vol_s.value == "mute" ? YELLOW : TEXT;
                   on_click() = exec("foot -T ws-hud-vol --app-id=ws-hud-vol -e wiremix"); }
  }

  group batgrp {
    align = right;
    widget bat { icon = bat_s.charging  ? 0xf0084
                      : bat_s.pct >= 75 ? 0xf240
                      : bat_s.pct >= 50 ? 0xf241
                      : bat_s.pct >= 25 ? 0xf242
                      : bat_s.pct >= 10 ? 0xf243
                      :                   0xf244;
                 text = " {bat_s.pct}%";
                 fg   = bat_s.charging ? GREEN
                      : bat_s.pct < 15 ? RED
                      : bat_s.pct < 25 ? ORANGE
                      : bat_s.pct < 40 ? YELLOW : TEXT; }
  }

  group sysgrp {
    align = right;
    widget cpu    { icon = 0xf4bc;  text = " {cpu_s.pct}%";
                    fg = cpu_s.pct >= 90 ? RED
                       : cpu_s.pct >= 75 ? ORANGE
                       : cpu_s.pct >= 50 ? YELLOW : TEXT; }
    widget sep_ct.sep { text = "/"; }
    widget temp   { icon = 0xf06d;  text = " {temp_s.c}°C";
                    fg = temp_s.c >= 85 ? RED
                       : temp_s.c >= 70 ? ORANGE
                       : temp_s.c >= 55 ? YELLOW : TEXT; }
    widget sep_tr.sep { text = "/"; }
    widget mem    { icon = 0xefc5;
                    text = mem_s.used_mb >= 1024
                         ? " {mem_s.used_mb / 1024}.{mem_s.used_mb * 10 / 1024 % 10} GB"
                         : " {mem_s.used_mb} MB";
                    fg = mem_s.pct >= 90 ? RED
                       : mem_s.pct >= 75 ? ORANGE
                       : mem_s.pct >= 60 ? YELLOW : TEXT; }
  }
}

group      { bg = CRUST; border = BORD; border_width = 2; radius = 8;
             pad = 8; pad_x = 12; gap = 14; }   // no height → fills the bar row
#distrogrp { pad_x = 18; gap = 0; }
#clockgrp  { gap = 10; }
#conngrp   { pad_x = 18; }

widget { fg = TEXT; }
.dim   { fg = SUBTXT; }
.sep   { fg = BORD; }

/* Active tag grows 4px */
.ws        { align = left; fg = TEXT; bg = CRUST; border = BORD; border_width = 2;
             radius = 8; pad = 6; width = 28; height = 28;
             transition_size = 160ms;
             enter_anim = 160ms; exit_anim = 160ms; }
.ws:active { fg = TEXT; border = BORD; width = 34; height = 34; }


// ============================================================================
// HUD — hover-revealed button panel.
// ============================================================================

/* In-process state — no poll timer, no fork; the daemon pings on mutation. */
source gamma_warm = gamma_warm();
source dnd_on     = dnd();
source mirror_on  = exec_line("mirror status", every="2s", refresh="instant");

/* Geometry: 32-px hit targets, 6-px gaps → row = 5·(32+6) − 6 = 184 (layout
 * adds `pad` per widget then drops the trailing one). The row is centred in
 * `width`, putting the glyph edge ≈18 px in — the bar groups' pad_x. The bar
 * row spans y 6..40, so a 40-px body under a 3-px gutter shares its centre.
 * The surface stays anchored to the screen top (the hover strip must sit at
 * the true edge); `reveal_gutter` leaves that band unpainted so the body
 * floats clear. */
surface hud {
  layer = overlay;
  anchor = top;
  width  = 242;
  height = 40;
  font_size = 14;
  reveal_on_hover = 20;        // input only — does not displace paint
  reveal_gutter   = 3;
  reveal_anim_ms  = 200;
  reveal_easing   = ease-out;
  visible = hid.value == "0";

  widget gamma_btn.btn {
    icon = 0xf186;
    fg = gamma_warm.value == "1" ? WSACT : TEXT;
    on_click() = exec("sh -c 'wispctl gamma is-warm && wispctl gamma off || wispctl gamma flat'");
  }
  widget sep1.sep { text = "/"; }
  widget dnd_btn.btn {
    icon = 0xf1f6;
    fg = dnd_on.value == "on" ? TERT : TEXT;
    on_click() = exec("wispctl dnd toggle");
  }
  widget sep2.sep { text = "/"; }
  widget vol_btn.btn {
    icon = 0xf028;
    on_click() = exec("foot -T ws-hud-vol --app-id=ws-hud-vol -e wiremix");
  }
  widget sep3.sep { text = "/"; }
  widget wifi_btn.btn {
    icon = 0xf1eb;
    on_click() = exec("foot -T ws-hud-wifi --app-id=ws-hud-wifi -e impala");
  }
  widget sep4.sep { text = "/"; }
  widget mirror_btn.btn {
    icon = 0xf24d;
    fg = mirror_on.value == "on" ? PRIM : TEXT;
    on_click() = {
      set(mirror_on, mirror_on.value == "on" ? "off" : "on");
      exec("mirror toggle");
    };
  }
}

#hud widget { align = center; pad = 6; }
.btn        { width = 32; height = 32; radius = 8; }
/* Only the state-carrying glyphs recolor — a transition on a constant fg would
 * cost a live TransSlot for an animation that can never run. */
#gamma_btn, #dnd_btn, #mirror_btn { transition_fg = 180ms; }
.btn:pressed { bg = REST; }

// ============================================================================
// OSD — notification stack.
// ============================================================================
surface osd {
  spawned_by = osd;
  layer = overlay;
  anchor = top;
  max = 8; width = 340; height = 60; margin = 6; gap = 0;   // margin > 0 → float layout
  pad_x = 14; icon_gap = 12; prog_h = 10;
  body_lines = 4; body_max = 256;
  timeout_low = 3000; timeout_normal = 5000; timeout = 1200;
  prog_fg = WSACT; prog_track = REST;
  bg = CRUST; radius = 10; border_width = 2; border = BORD;
  separator = REST; separator_frac = 80;
  dismiss_on_click = true; focus_follow = true; dbus_close = true;

  widget icon  { align = left;   width = 40; icon = $icon; }
  /* Summary + wrapped body as one block: $body arrives "\n"-joined, so
     body_lines is 1 (summary) + however many lines it wrapped to. The block
     centers in the slab; a progress band steals the bottom prog_h + 8 px, so
     lift by half that to stay centered in what's left. elide keeps a long
     summary (attacker-controlled, it comes off D-Bus) inside the slab. */
  widget title { align = left;  text = $nbody > 0 ? "{$summary}\n{$body}" : $summary;
                 body_lines = 1 + $nbody; elide;
                 pad = 12; y_offset = $progress >= 0 ? -9 : 0; }
  /* progress is -1 on a plain notification — no pct column then. */
  widget pct   { align = right;  text = "{$pct}%";         pad = 12;
                 y_offset = $progress >= 0 ? -9 : 0;
                 visible = $progress >= 0; }
  /* Progress band. $progress is 0..100, hence value_max. */
  widget prog  { slider; align = left; width = 312; height = 10;
                 visible = $progress >= 0; value = $progress; value_max = 100;
                 track_bg = REST; track_fg = WSACT; track_radius = 5; }
}

#osd widget { fg = TEXT; }

/* Category styling. `muted` arrives as 1 (mute) / 2 (warn) per slab; the
 * pseudos read the derived $mute / $warn bindings, so the render path has no
 * C branch on it. */
#osd:warn { bg = REST; }
#osd widget:warn { fg = ORANGE; }
#prog:warn { track_fg = ORANGE; }
#prog:mute { track_fg = RED; }

/* Pct posts (volume / brightness) skip the stack and render as this pill —
   one fixed-height slab, {icon}{slider}, top-centered like a bar module.
   margin > 0 floats it below the bar, so it rounds and outlines all round.
   The icon's leading gap is its widget width (the slab has no pad_x). */
surface pill {
  spawned_by = osd_pill;
  layer = overlay;
  anchor = top;
  width = 220; height = 40; margin = 3; radius = 8; font_size = 20;

  widget icon { align = left; width = 40; pad = 4; icon = $icon; }
  widget prog { slider; align = left; width = 162; height = 10;
                value = $progress; value_max = 100;
                track_bg = REST; track_fg = WSACT; track_radius = 5; }
}

#pill { bg = CRUST; border = BORD; border_width = 2; }
#pill widget { fg = TEXT; }
#pill:warn { bg = REST; }
#pill widget:warn { fg = ORANGE; }

// ============================================================================
// Optional subsystems.
// ============================================================================

lock {
  bg         = SOLID;
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
  transition = fade;
  fade_ms    = 300;
  bg         = SOLID;
}

media {}

surface menu {
  spawned_by = menu;
  layer = overlay;
  exclusive_zone = -1;
  keyboard = exclusive;

  axis        = vertical;      // top-centered launcher float, query row on top
  width       = 560;
  margin      = 6;
  max_visible = 5;
  row_h       = 34;

  prompt = "run:";
  sort   = "most_used";
  icons  = true;

  pad_x = 8;   // legacy MENU_PAD_X
  pad_y = 6;   // legacy MENU_VPAD

  // query row: dim prompt, the typed text, then a caret
  group query {
    height = 34; pad = 0; pad_x = 8; gap = 6;
    bg = #00000000; border = #00000000;
    cell { text = menu.prompt; fg = SUBTXT; }
    cell { text = menu.query;  fg = TEXT; }
    cell { text = "_";         fg = TEXT; }
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
    }
  }
}

#hud, #osd, #menu { bg = CRUST; fg = TEXT; border = BORD; border_width = 2; radius = 8; }

menu power {
  item { icon = 0xf011; label = "Poweroff";  exec = "loginctl poweroff"; }
  item { icon = 0xf021; label = "Reboot";    exec = "loginctl reboot"; }
  item { icon = 0xf08b; label = "Logout";    exec = "pkill -x mango"; }
  item { icon = 0xf186; label = "Sleep";     exec = "true"; }
  item { icon = 0xf28d; label = "Hibernate"; exec = "true"; }
}

menu emoji { preset = emoji; }

source time   = clock("%H:%M");
source date_s = clock("%b %d");
source tags   = tags();
source cpu_s  = cpu();
source mem_s  = mem();
source bat_s  = bat("BAT0");
source temp_s = temp();
source vpn_s  = vpn("mullvad");
source wifi_s = wifi("wlan0");
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

  /* Concave armpit feet: the bar's underside curls down the left/right screen
   * edges with a concave quarter-arc so it hugs the display corners. Buffer
   * grows 10 px below (feet only); exclusive_zone stays 28 so windows tile
   * flush under the bar body and the feet just overlap the wallpaper. */
  armpit_inner = 10;

  widget edge_l { align = left; pad = 12; y_offset = -34; }
  widget logo   { align = left; icon = 0xf32e; }
  widget time   { align = left; text = time;   }
  widget date.dim { align = left; text = date_s; pad = 16; }

  for tag in tags.list {
    cell {
      align   = left;
      text    = tag.label;
      visible = tag.pinned || tag.occupied || tag.active || tag.urgent;
      on_click() = exec("wispctl tag {tag.index}");
    }
  }

  widget title.dim { align = left; text = tags.title; pad = 16; }

  widget edge_r { align = right; pad = 12; y_offset = -34; }
  widget wifi   { align = right;
                  icon = wifi_s.signal >= 3 ? 0xf0928
                       : wifi_s.signal >= 2 ? 0xf0925
                       : wifi_s.signal >= 1 ? 0xf0922
                       :                      0xf091f;
                  pad = 16;
                  visible = wifi_s.signal >= 0; }
  widget vpn    { align = right;
                  icon = vpn_s.state == "on"    ? 0x25cf
                       : vpn_s.state == "stale" ? 0x25b2
                       :                          0x25cb;
                  fg   = vpn_s.state == "on"    ? #ff7fbf9f
                       : vpn_s.state == "stale" ? #ffff5050
                       :                          #ffffaa20; }
  widget bat    { align = right;
                  icon = bat_s.charging  ? 0xf0084
                       : bat_s.pct >= 75 ? 0xf240
                       : bat_s.pct >= 50 ? 0xf241
                       : bat_s.pct >= 25 ? 0xf242
                       : bat_s.pct >= 10 ? 0xf243
                       :                   0xf244;
                  text = " {bat_s.pct}%";
                  fg   = bat_s.pct < 15 ? WARN : (bat_s.charging ? #ff7fbf9f : FG); }
  widget sep.dim { align = right; text = "/"; }
  widget mem    { align = right; icon = 0xefc5;  text = " {mem_s.pct}%"; }
  widget temp   { align = right; icon = 0xf0238; text = " {temp_s.c} C"; }
  widget cpu    { align = right; icon = 0xf4bc;  text = " {cpu_s.pct}%"; fg = cpu_s.pct > 80 ? WARN : FG; }
  widget disk   { align = right; icon = 0xf02ca; text = " {disk_s.pct}%"; }
}

/* 22 px is the module rhythm; edge insets and labels override it inline. */
widget { fg = FG; pad = 22; }
.dim        { fg = DIM; }

#bar cell          { fg = DIM; bg = #00000000; width = 28; height = 28; pad = 4; }
#bar cell:active   { fg = FG;  bg = ACT; }
#bar cell:urgent   { fg = FG;  bg = URG; }

// ============================================================================
// Screen-corner round-overs — bottom two corners.
// ============================================================================

/* A thin, click-through strip pinned to the bottom screen edge. It paints
 * nothing but two concave wedges at its bottom-left/right corners — the same
 * #cc000000 round-over the bar feet give the top corners — so all four display
 * corners read as rounded. `armpit_outer` rounds the corners ON the anchored
 * (bottom) edge in place; `exclusive_zone = 0` reserves no space and
 * `input = none` lets pointer events pass through to windows beneath. */
surface screen_corners {
  layer  = top;
  anchor = bottom | left | right;
  height = 10;
  exclusive_zone = 0;
  bg = #00000000;
  armpit_outer = 10;
  armpit_color = #ff0f1219;   // must match bar bg
  input = none;
}

// ============================================================================
// HUD — hover-revealed button panel.
// ============================================================================

/* refresh="instant" → on_click() re-polls synchronously, no delay, so the
 * toggle flips in the same frame as the click. `every=...` still governs the
 * steady-state cadence for state changes that happen out-of-band. */
source gamma_warm = exec_line("wispctl gamma is-warm",                                   every="2s", refresh="instant");
source dnd_on     = exec_line("wispctl dnd status",                                      every="2s", refresh="instant");
source mvpn_on    = exec_line("mullvad-vpn health >/dev/null 2>&1 && echo 1 || echo 0", every="2s", refresh="instant");

const SURFACE = #26ffffff;
const PEACH   = #ffe6b89c;
const SAGE    = #ff8fb3a3;
const TEAL    = #ff7fb0bb;
const DARK    = #ff1a2530;

/* Geometry: BTN_W=BTN_H=48, BTN_GAP=12, PAD=8, BTN_BORDER=2.
 *   WIDGET_W = 5·48 + 4·12 + 2·8 = 304.
 * `reveal_on_hover = 28` adds a 28-px trigger gutter at the top that sits
 * over the bar (exclusive_zone = -1 lets the HUD ignore the bar's
 * reservation). `y_offset = -14` straddles each 48-px button across the bar
 * bottom edge; `clip_top = 28` hides border pixels above y=28 so the chiclet
 * outlines read as extending out of the bar. */
surface hud {
  layer = overlay;
  anchor = top;
  width  = 304;
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
  /* Animated fillet: `lo..hi` opts in. Lo is informational (radius is driven
   * by the body's visible extent past the bar gutter); hi caps the resting
   * round-over. Outward top fillets flare into the bar instead of reading as
   * a detached pill. */
  fillet_tl       = 0..18;
  fillet_tr       = 0..18;
  fillet_offset_y = 0;

  /* Optimistic toggle: `set <source>` writes the polled-source line buffer
   * and marks the surface dirty so bg flips in the same frame as the click.
   * The exec() then runs the real command; the next poll confirms (or, if
   * the command failed, reverts). transition_bg/fg smooth the recolor. */
  widget gamma_btn.btn.toggle {
    icon = 0xf186;
    bg = gamma_warm.value == "1" ? PEACH : SURFACE;
    fg = gamma_warm.value == "1" ? DARK  : FG;
    on_click() = {
      set(gamma_warm, gamma_warm.value == "1" ? "0" : "1");
      exec("sh -c 'wispctl gamma is-warm && wispctl gamma off || wispctl gamma flat'");
    };
  }
  widget dnd_btn.btn.toggle {
    icon = 0xf1f6;
    bg = dnd_on.value == "on" ? SAGE : SURFACE;
    fg = dnd_on.value == "on" ? DARK : FG;
    on_click() = {
      set(dnd_on, dnd_on.value == "on" ? "off" : "on");
      exec("wispctl dnd toggle");
    };
  }
  widget mvpn_btn.btn.toggle.pop {
    icon = 0xf132;
    bg = mvpn_on.value == "1" ? TEAL : SURFACE;
    fg = mvpn_on.value == "1" ? DARK : FG;
    on_click() = exec("foot -T ws-hud-mullvad --app-id=ws-hud-mullvad -e mullvad-menu");
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

/* --- HUD style --- `.toggle` recolors on state, `.pop` flashes on press. */
#hud widget  { align = center; }
.btn         { width = 48; height = 48; pad = 12; y_offset = -14;
               border_width = 0; radius = 14; bg = SURFACE; fg = FG; }
.toggle      { transition_bg = 180ms; transition_fg = 180ms; }
.pop:pressed { bg = TEAL; }

// ============================================================================
// OSD — notification stack.
// ============================================================================

surface osd {
  spawned_by = osd;
  layer = overlay;
  anchor = top;

  max = 8; width = 340; height = 60; margin = 0; gap = 0;
  pad_x = 14; icon_gap = 12; prog_h = 10;
  body_lines = 4; body_max = 256;
  timeout_low = 3000; timeout_normal = 5000; timeout = 1200;
  bg = #ff0f1219; fg = FG; border = #00000000;
  prog_fg = #ff84a7b3; prog_track = #ff1c2733;
  /* margin 0 → flush under the bar: only the bottom corners round, and the
     fillets claw up into the bar row at the junction. */
  radius = 10; fillet_r = 14;
  separator = #ff1c2733; separator_frac = 80;
  dismiss_on_click = true; focus_follow = true; dbus_close = true;

  widget icon  { align = left;   width = 40; icon = $icon; }
  widget title { align = left;  text = $nbody > 0 ? "{$summary}\n{$body}" : $summary;
                 body_lines = 1 + $nbody; elide;
                 pad = 12; y_offset = $progress >= 0 ? -9 : 0; }
  widget pct   { align = right;  text = "{$pct}%";         pad = 12;
                 y_offset = $progress >= 0 ? -9 : 0;
                 visible = $progress >= 0; }
  widget prog  { slider; align = left; width = 312; height = 10;
                 visible = $progress >= 0; value = $progress; value_max = 100;
                 track_bg = #ff1c2733; track_fg = #ff84a7b3; track_radius = 5; }
}

#osd widget { fg = FG; pad = 0; }

/* Category styling: `muted` arrives 1 (mute) / 2 (warn) per slab; the pseudos
 * read the derived $mute / $warn bindings. */
#osd widget:warn { fg = #ffffaa20; }
#prog:warn { track_fg = #ffffaa20; }
#prog:mute { track_fg = WARN; }

/* Pct posts (volume / brightness) skip the stack and render as this pill —
   without it every volume tick lands in the 8-slab stack. A negative margin
   rests the body that far INSIDE the bar row — straddling the bar edge like
   the old OSD did, so all four corners round — with the fillet_r claw where
   it pokes out below. The icon's leading gap is its widget width (a slab has
   no pad_x). */
surface pill {
  spawned_by = osd_pill;
  layer = overlay;
  anchor = top;
  width = 220; height = 40; margin = -20; radius = 8; fillet_r = 14;
  font_size = 20;

  widget icon { align = left; width = 40; pad = 4; icon = $icon; }
  widget prog { slider; align = left; width = 162; height = 10;
                value = $progress; value_max = 100;
                track_bg = #ff1c2733; track_fg = #ff84a7b3; track_radius = 5; }
}

#pill { bg = #ff0f1219; }
#pill widget { fg = FG; }
#pill widget:warn { fg = #ffffaa20; }

// ============================================================================
// Optional subsystems.
// ============================================================================

lock {
  bg       = #ff000000;        // fallback fill when the wallpaper is missing
  ring     = #ff5f8a93;
  ring_bad = #ffd06878;
  fg       = #ffa8d5cc;
  dim      = #ff7a808b;
  caps     = #ffe0c060;
  scrim    = #5c0e1622;        // alpha = base dim strength
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
}

media {}

surface menu {
  spawned_by = menu;
  layer = overlay;
  anchor = top | left | right;
  height = 28;
  exclusive_zone = -1;
  keyboard = exclusive;
  bg = #ff0f1219;
  prompt = "run:";
  pad_x = 8;    // legacy MENU_PAD_X

  widget prompt { text = menu.prompt; fg = #ffffffff; pad = 4; }
  widget query { text = menu.query;  fg = #ffffffff; }
  widget caret { text = "_";         fg = #ffffffff; pad = 12; }  // legacy MENU_GAP
  for row in rows {
    cell {
      text  = row.label;
      fg    = #ffffffff;
      bg    = row.selected ? #ff2a2f3a : #00000000;
      pad_x = 8;   // legacy MENU_ITEM_PAD_X
    }
  }
}

// Menus, formerly compiled in unconditionally (POWERMENU_INIT / EMOJI_INIT).
// ponytail: sleep/hibernate are `true` no-ops — not set up on this machine.
menu power {
  item { icon = 0xf011; label = "Poweroff";  exec = "loginctl poweroff"; }
  item { icon = 0xf021; label = "Reboot";    exec = "loginctl reboot"; }
  item { icon = 0xf08b; label = "Logout";    exec = "pkill -x mango"; }
  item { icon = 0xf186; label = "Sleep";     exec = "true"; }
  item { icon = 0xf28d; label = "Hibernate"; exec = "true"; }
}

menu emoji { preset = emoji; }

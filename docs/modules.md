# Modules

Eleven things the runtime treats differently. The DSL picks between them by
**which properties are present**, not by a keyword.

| kind | selected by |
|---|---|
| bar / panel | `surface NAME { … }` with none of the below |
| HUD | `reveal_on_hover = N;` with N greater than 0 |
| menu popup | `exclusive_zone = -1`, or any `menu NAME {}` declaration |
| OSD stack | `surface osd { spawned_by = osd; … }` |
| OSD pill | `surface pill { spawned_by = osd_pill; … }` |
| menu template | `surface menu { spawned_by = menu; … }` |
| polkit prompt | `surface polkit { spawned_by = polkit; … }` |
| tooltip | `surface tooltip { spawned_by = tooltip; … }` |
| greeter | `surface NAME { spawned_by = greet; … }` |
| compound | `compound NAME { region … }` |
| blocks | `lock {}` `gamma {}` `wallpaper {}` `media {}` `idle {}` |

Property **names** are validated per kind, but the `surface` schema is one union
across every surface flavour. An OSD-only property on a bar passes `--check` and
is never read. That is where every "inert" list below comes from.

Limits: 16 surfaces, 8 declared menus, 128 items per plain surface, 8 widgets
per surface instance.

## Bar and panel

`surface bar { … }`. The name `bar` is the one special panel name: it is what
the tag accumulator and the lock-on-output path look for. Any other name is a
plain panel with identical behaviour otherwise.

| property | default | behaviour |
|---|---|---|
| `layer` | `top` | layer-shell layer |
| `anchor` | `top\|left\|right` | edges, OR-combined |
| `height` | `24` | body height |
| `width` | `0` (stretch) | if set and `exclusive_zone` is not, the zone is forced to 0 |
| `margin` | `0` | uniform layer-shell margin, all four sides |
| `margin_x` | `= margin` | left/right override, so a panel can clear the bar vertically and keep the bar's side inset |
| `exclusive_zone` | `= height` | positive reserves space, `-1` overlays and flips on the menu feature |
| `bg` | `#ff000000` | body clear colour |
| `bg_bottom` | off | vertical gradient toward it; disables partial repaint |
| `font_size` | `14` | picks the baked strike |
| `axis` | `horizontal` | `vertical` stacks along Y |
| `input` | full region | only `none`, which makes the surface click-through |
| `visible` | true | expression, gates create and destroy per output |
| `output` | all | only `active`: one copy, on the monitor whose bar cell was just clicked |
| `scroll` | `0` (off) | wheel-scrollable stack, `N` px per notch or the keyword `rows`; needs `axis = vertical` |
| `pad_x` / `pad_y` | `2` | scrollable surfaces only: gap between the frame and the rows |
| `on_escape` | - | shell command run when Escape is pressed here; declaring it turns on keyboard interactivity |
| `keyboard` | `on_demand` with `on_escape`, else `none` | `none`, `on_demand`, `exclusive` |
| `dismiss_on_unfocus` | marker | run the `on_escape` command when keyboard focus goes elsewhere |
| `dismiss_on_click` | false | any click closes the surface |
| `radius` | `0` | uniform corner radius |
| `radius_inner` / `radius_outer` | `= radius` | desktop-facing and anchored-edge pair |
| `radius_tl/tr/bl/br` | derived | absolute, wins over the pair |
| `border` | `0` (off) | alpha 0 disables |
| `border_width` | `1` | 0 disables |
| `border_top/bottom/left/right` | `1` | per-side suppression |
| `clip_top` | `0` | wipes background and border above y=N after drawing |
| `clip_widgets` | absent | presence alone disables partial repaint |
| `armpit_inner` | `0` | concave feet at desktop-facing corners; horizontal non-HUD only |
| `armpit_outer` | `0` | concave rounds on the anchored screen edge |
| `armpit_tl/tr/bl/br` | derived | absolute override |
| `armpit_color` | `= bg` | paints the arcs |
| `fillet_tl/tr/bl/br` | `0` | convex arc into the empty quadrant; accepts `lo..hi` |
| `fillet_inner_*` / `fillet_outer_*` | `0` | anchor-relative aliases, absolute names win |
| `fillet_offset_y` | `0` | shifts the junction along the slide axis |
| `cutout_into` | - | punch a transparent rect into another surface |
| `cutout_x/y/width/height` | centre / own extent | the punched rect |

**Inert here** (accepted, never read): `spawned_by`, `reveal_*`,
`max`, `gap`, `pad`, `icon_gap`, `image`, `prog_h`, `prog_fg`,
`prog_track`, `body_lines`, `body_max`, `timeout*`, `slide_ms`, `fillet_r`,
`separator`, `separator_frac`, `focus_follow`, `dbus_close`,
`prompt`, `row_h`, `max_visible`, `anchor_gap`, `sort`,
`terminal`, `icons`, `hover`, `edge`, `size`, `fg`. `pad_x` and `pad_y` are
inert too unless the surface scrolls.

**Lifecycle.** Created once per active output at startup and on output-add. With
`visible = <expr>`, a false-to-true edge creates on every active output and the
reverse destroys them all. Repaint is per-surface: any source read by a widget
property joins that surface's dependency set, printable with `wispc --deps`.

A flat horizontal bar with no radius, border, fillet, armpit, gradient, clip,
cutout or slider takes the **partial repaint** path, where only the cells whose
source ticked are redrawn. Any one of those features turns it off.

Skeleton: [[templates#generic-bar]].

### Scrollable panel

A panel with `scroll` and `axis = vertical` is a wheel-scrollable container: a
notch shifts the stack by `scroll` px, or by exactly one row with
`scroll = rows`, which is what variable-height rows (wrapped text, `body_fit`)
want. Content past the body rect is clipped, never drawn off-screen. The offset
is discrete and unanimated, and a wheel at either end repaints nothing.

Up and Down move a selection through the rows and Enter activates it exactly as
a click would, on any scrollable surface, with no declaration. There is one
indicator for both inputs: an arrow press starts from the row under the pointer,
and pointer motion takes the selection back.

A leading `sticky;` on a widget or group pins it above the scrolled region — a
header. Only the leading run may be sticky; a `sticky` below a scrolling row is
an error. The scrolled stack starts where that run ends.

Panels are usually keyboard-dismissable and per-monitor:

- `on_escape = "…"` runs a shell command on Escape and gives the surface
  keyboard interactivity. `keyboard = exclusive` holds the whole session's
  keyboard while mapped, which is right for a modal and wrong for a panel; the
  default `on_demand` is what you want.
- `dismiss_on_unfocus;` reuses that same command when keyboard focus goes
  elsewhere, so clicking away closes the panel. It needs `on_escape`. A click
  landing on the panel itself does not dismiss it, and the verdict is taken at
  the end of the Wayland event batch, so focus moving between this surface's own
  per-output copies is not a click-away either.
- `output = active;` creates exactly one copy, on the monitor whose bar cell was
  just clicked (the same one-second click anchor menus use, and reading it
  spends it). Without it a panel opens on every monitor at once. A monitor
  hotplugged afterwards gets no copy.

The notification centre is this panel plus a `notifications()` source: gate
`visible` on `<src>.open`, loop `for note in <src>.history`, and drive it with
`wispctl notif open|close|toggle|clear|dismiss <id>`.

## HUD

Any surface with `reveal_on_hover = N;` becomes a hover-revealed slide-in and
links `hud.c`.

| property | default | behaviour |
|---|---|---|
| `reveal_on_hover` | `0` = not a HUD | width of the input trigger strip on the slide edge |
| `reveal_gutter` | `= reveal_on_hover` | unpainted band on that edge; the buffer is `height + gutter`. `0` paints from the anchored edge, which is what a floating bar wants |
| `reveal_anim_ms` | `0` = snap | slide duration |
| `reveal_easing` | `ease_out` | `linear` `ease_in` `ease_out` `ease_in_out` |
| `exclusive_zone` | `-1` for a HUD | declare `0` to sit below the bar instead of overlapping it |

The slide edge comes from `anchor` with priority top, bottom, left, right.
Corner anchors slide diagonally. Inside a HUD body, `anim.emerged_h` and
`anim.emerged_w` read the emerged extent past the gutter in pixels.

**Inert on a HUD:** `margin`, because the slide is a render offset and not a
margin tween, plus everything inert on a plain bar.

**Lifecycle.** Hidden until pointer-enter, then the input region swaps to the
full surface and `cur_off` tweens to 0. On leave (100 ms click grace, 30 ms
hide delay) it tweens back and then frees its SHM pool, which is what keeps a
parked HUD off the idle RAM budget. Without `reveal_anim_ms` the motion is
exponential decay. `wispctl hud-cancel` force-hides.

Skeleton: [[templates#hud]].

## OSD stack

`surface osd { spawned_by = osd; … }`. **Both** the name and the property
matter: the name is what sets `has_osd` and `has_dbus` and what makes codegen
include the engine. Rename the surface and notifications silently disappear.

It is not created at startup. `osd.c` owns one widget, a ring of slabs, the
tweens, the bar cutout and the geometry. The widget blocks in the body **are**
the slab layout.

| property | default | meaning |
|---|---|---|
| `max` | 8 | slabs in the ring |
| `width` | 340 | slab width |
| `height` | 60 | slab height |
| `margin` | 0 | 0 sits flush under the bar, above 0 floats it top-centre |
| `gap` | 8 | between slabs |
| `pad_x` | 14 | |
| `icon_gap` | 12 | |
| `image` | 0 | cover-art square in px; 0 compiles the decoder out |
| `prog_h` | 10 | |
| `body_lines` | none, set it | body line ceiling |
| `body_max` | none, set it | body byte cap |
| `timeout_low` | 3000 | low-urgency notification lifetime, ms |
| `timeout_normal` | 5000 | normal-urgency lifetime |
| `timeout` | 1200 | `wispctl osd` lifetime |
| `slide_ms` | 200 | slide in and out |
| `radius` | 10 | |
| `fillet_r` | 18 | claw wedges into the bar |
| `border_width` | 0 | |
| `bg` | `0xff0f1219` | |
| `fg` | `0xffc8e8f0` | |
| `border` | `0xff5f8a93` | |
| `prog_fg` | `0xff84a7b3` | |
| `prog_track` | `0xff1c2733` | |
| `focus_follow` | 1 | re-anchor to the focused output |
| `dbus_close` | 1 | emit `NotificationClosed` |
| `dismiss_on_click` | 1 | |
| `separator` | 0 (off) | read by the renderer, not a macro |
| `separator_frac` | 0 | |
| `font_size` | 14 | |
| `anchor` | `top` | only top-centre and bottom-right are wired |

A widget named `icon` contributes its `width` as the reserved left column, so
text lines up across slabs. Put `visible = $has_icon` on it and the column
collapses on a slab that carried neither cover art nor an icon, instead of
indenting its text past an empty box; the body wraps against the same rule.

**Template arguments**, readable as `$name` in the body:

| binding | type |
|---|---|
| `$summary` | string |
| `$body` | string |
| `$nbody` | int, real line count |
| `$icon` | codepoint |
| `$image` | pixmap, falls back to the `$icon` glyph; needs `image = N` |
| `$pct` | display string |
| `$progress` | 0..100 int, what a slider `value` wants |
| `$has_icon` | bool, true when the post carried an image or an icon |
| `$muted`, `$urgency` | int |
| `$mute`, `$warn` | derived, read by the `:mute` and `:warn` pseudos |

**Inert on the OSD template:** `layer`, `exclusive_zone`, `visible`,
`input`, `reveal_*`, `clip_top`, `armpit_*`, `cutout_*`, `bg_bottom`,
`radius_tl..br`, `radius_inner/outer`, the per-side border flags, and every menu
property. `osd.c` creates and sizes the surface itself, so none of the declared
surface lifecycle runs.

**Lifecycle.** A slab is posted by a D-Bus `Notify`, by `wispctl
osd|notify|volume|mic|backlight|mpris`, or by an `emit(osd, …)` handler. A post
with a matching replace id overwrites its slot instead of stacking. Expiry
drives the epoll timeout, so an empty stack costs nothing; when the ring empties
the pool is released. Cover art resolves `image-data`, then `image-path`, then
`app_icon`, PNG only.

Skeleton: [[templates#osd-stack]].

## OSD pill

`surface pill { spawned_by = osd_pill; … }`. The name must be `pill`. Declaring
it is what compiles the pill in; once present, **progress-only posts** (a
`$progress` of 0 or more with no body) route here instead of joining the stack.

| property | default | meaning |
|---|---|---|
| `width` | 0, meaning disabled | pill width |
| `height` | 36 | |
| `margin` | 0 | see below |
| `fillet_r` | `= OSD_FILLET_R` (18) | |

A `margin` of 0 or less sits the pill flush against the bar with fillet claws. A
**negative** margin rests it that many pixels inside the bar row, painting over
the bar, and all four corners round. Everything else (`bg`, `border`, `radius`,
`font_size`, `separator*`, widget bodies) goes through the same renderer as the
stack, over a one-slab ring, with the same `$` bindings.

A slab has no `pad_x`, so the leading gap before the first widget is that
widget's own `width`.

**Inert:** everything inert on the OSD template, plus `max`, `gap`, `timeout*`,
`image`, `prog_h`, `body_*`, `focus_follow`, `dbus_close`, `dismiss_on_click`,
which are stack-only and read off the `osd` template. The pill has no cover art,
`$image` is not wired to it.

Skeleton: [[templates#osd-pill]].

## Menu

Two halves, both needed for a working launcher.

1. `surface menu { spawned_by = menu; … }` is the look **every** menu gets.
2. `menu NAME { … }` declares a named menu that `wispctl menu <name>` can open. A declaration carrying its own body gets its own renderer instead.

`menu.c` owns state, keys, sizing and scrolling, and draws nothing.

| template property | default | meaning |
|---|---|---|
| `height` | 28 | header row height |
| `width` | 560 | |
| `margin` | 0 | |
| `max_visible` | 5 | rows on screen |
| `row_h` | 0 = font line height + 10 | sizing is measured against font size 14, so declare `row_h` whenever `font_size` is not 14 |
| `pad_y` | 6 | |
| `anchor_gap` | 0 | gap between a clicked bar cell and the popup |
| `separator_h` | 0 = a separator row gets a full `row_h` slot | non-zero gives `row.separator` rows their own shorter slot, so the row grid stops being uniform |
| `separator` | 0 (off) | with `separator_h > 0`, a separator row draws no cell at all: a 1px line of this colour, centred in its slot |
| `separator_frac` | 100 | percent of the content width that line spans |
| `terminal` | `"foot -e"` | used for `Terminal=true` desktop entries |
| `icons` | 0 | app-icon decode, the launcher's biggest RAM and IO cost |
| `axis` | `horizontal` | `vertical` is a top-centred launcher float, anything else a full-width dmenu strip |
| `sort` | `"most_used"` | only the literal `"alphabetical"` changes it |
| `prompt` | - | prompt string |

Body bindings: `for row in rows { cell { … } }` with fields `label` (string),
`icon` (pixmap, which reserves its column even on rows without one, so labels
stay aligned — but a menu whose owner decoded no icon at all gets no icon table
and the column collapses instead), `has_icon` (bool), `selected` (bool),
`index` (int) and the flag bools `enabled`, `separator`, `toggle`, `checked`.
Also `menu.prompt`, `menu.query` and `menu.count`.

The header height is computed by summing the declared `height` of every
non-`for` top-level body item, so **anything above the rows must declare a
`height`**.

`menu NAME { … }` takes two shapes, freely mixed:

```wisp
menu power {
	item { icon = 0xf011; label = "Poweroff"; exec = "loginctl poweroff"; }
	item { icon = 0xf021; label = "Reboot";   exec = "loginctl reboot"; }
}
menu emoji { preset = emoji; }
```

`item` needs `icon` as an integer literal codepoint plus `label` and `exec` as
strings, all three required. `preset = emoji` is the only preset that exists.

Per-menu overrides, where 0 means inherit: `width`, `row_h`, `max_visible`,
`separator_h`, `anchor_gap`, `pad_y`. A bare `hover;` marker makes pointer motion move the
selection, for pointer-driven tray dropdowns; menus without it never repaint on
motion, which is the idle-zero-CPU rule.

**Inert on the menu template:** `layer`, `exclusive_zone` (though `-1` is still
what flips on the menu feature), and surface-level `bg`, `border`
and `radius`, because the body carries the look and `menu.c` owns the surface.
Also all OSD props, `reveal_*`, `armpit_*`, `fillet_*`, `clip_top`, `cutout_*`,
`visible`, `input`.

**Lifecycle.** Opened by `wispctl menu <name>` or `wispctl apps`, closed by
Escape, a selection, focus loss, or `wispctl menu-cancel`.
`emit(menu_reply, index = …)` routes a selection back over the socket. Max 32
rows on screen, 8 declared menus.

Skeletons: [[templates#generic-vertical-menu]] and
[[templates#generic-horizontal-menu]].

## polkit

`surface polkit { spawned_by = polkit; … }` and no block: declaring the surface
is the whole configuration, and it is what makes wisp register as the session's
polkit authentication agent on the system bus. Without it an `auth_admin`
action has no prompt. The surface is created per authentication and destroyed
after, so nothing is mapped and no pool exists at idle.

| property | default | note |
|---|---|---|
| `width` | — | modal width |
| `height` | — | modal height; a compile-time constant, the body never resizes it |
| `layer` | `overlay` | |
| `keyboard` | — | use `exclusive`, or keystrokes leak to whatever is behind the prompt |
| `anchor` | none | an axis with neither edge set is centred by layer-shell, which is what a modal wants |
| `shadow`, `shadow_*` | off | same shadow props as any surface |

The body is one vertical stack of widgets and groups that advances by each
row's `height` **plus that row's own `pad`**: the surface `gap` and a widget's
`y_offset` are not applied here, so row spacing is the trailing `pad`. `align`
picks the start/center/end bucket, and start and center both originate at the
top of the content area — declaring rows in both makes them overlap. `align =
end` pins a row to the bottom edge.

Body bindings are seven read-only self-locals, [[state#polkit-self-locals]].
The typed password is never reachable from the DSL.

Skeleton: [[templates#polkit]].

## Tooltip

`surface tooltip { spawned_by = tooltip; … }`. One transient layer surface,
created on hover and destroyed on leave, so nothing is mapped at idle. A cell's
`tooltip = "…"` arms it after `delay_ms`; `wispctl tooltip <x> <width> <below>
<text>` and `wispctl tooltip hide` drive it by hand. Declaring `tooltip` on a
cell with no such surface is a compile error.

| property | default | note |
|---|---|---|
| `width` | — | clamp, not a fixed width; the surface auto-widths to `$text` and elides past it |
| `height` | — | |
| `pad_x` | 0 | horizontal inset of the label |
| `anchor_gap` | 0 | vertical gap from the anchoring cell |
| `delay_ms` | — | hover dwell before it appears |
| `layer` | `overlay` | |
| `exclusive_zone` | — | set `-1` |
| `bg`, `border`, `border_width`, `radius`, `shadow*` | | as any surface |

Body binding is `$text` only. It always hangs below the anchor, x-clamped to
the output — there is no flip-above near a screen edge. `keyboard_interactivity`
is 0 and the input region is zero-area, so it can never take focus or a click;
a text change tears the surface down and re-shows it rather than swapping in
place. No timer is armed unless a tooltip is pending.

Skeleton: [[templates#tooltip]].

## greet

`surface NAME { spawned_by = greet; … }` and no block: declaring the surface
is the whole configuration, and it is what makes the binary a greetd greeter.
The name is free — this template is found by its `spawned_by` value, not by
its name. The surface is created at startup and lives until the session
starts, at which point wisp exits.

greetd keeps everything privileged: PAM as root, the VT, exec'ing the session.
wisp only speaks greetd's client protocol on `$GREETD_SOCK`. That variable
being unset is fatal, not a degraded mode — a greet surface outside greetd is
a config error.

| property | default | note |
|---|---|---|
| `user` | `""` | username to log in; a compile-time string literal |
| `sessions` | `/etc/greetd/environments` | one command line per session, blanks and `#` skipped, first 8 kept, first selected |
| `width` | — | |
| `height` | — | a compile-time constant, the body never resizes it |
| `layer` | `overlay` | |
| `keyboard` | `exclusive` | the one surface where exclusive is the default |
| `anchor` | none | centred, as a modal wants |
| `shadow`, `shadow_*` | off | same shadow props as any surface |

The body is the same vertical stack polkit's is, advancing by each row's
`height` plus that row's own `pad`, with the same start/center/end buckets.
Body bindings are nine read-only self-locals, [[state#greet-self-locals]],
plus the list `for s in greet.sessions`. The typed secret is never reachable
from the DSL.

Keys: printable characters append, Backspace deletes a codepoint, Enter
submits, Escape clears the field, Up/Down (Tab = Down) cycle the session.
A left click on a `for s in greet.sessions` row selects that session — those
rows are the one place a `for` cell gets a hit rect without a declared
`on_click`. Nothing else in the surface reacts to the pointer, and there are
no timers, so the greeter idles at 0 ticks.

Skeleton: [[templates#greet]]. End-to-end setup: [[greeter]].

## Compound

`compound NAME { region NAME { edge = …; size = N; … } … }`. Each region becomes
its own edge-anchored thin layer surface, which means much smaller SHM pools
than one full-screen buffer.

| compound property | default | note |
|---|---|---|
| `width` / `height` | none | **required**, and 0 or less is a hard error. This is the bounding box regions are carved from |
| `bg` | `#ff000000` | |
| `layer` | `top` | |
| `anchor` | `top\|left` | every region's `edge` must be a bit of this mask |
| `margin` | `0` | |
| `radius` | `0` | |
| `radius_inner` | `0` | fills the L notch between two perpendicular regions with a quarter disc |
| `exclusive_zone` | largest region `size` on a primary anchor edge | |

Region properties: `edge` is required and a single identifier; `size` is
required and an integer literal. Rectangles are carved from the bounding box:
top and bottom span the width, left and right span the height.

Hard limits: one region per edge, at least one region, at most 8 regions, 256
items across all of them.

**Unsupported:** `group` inside a region (the parser rejects it), nested
compounds, per-region animation, more than one region per edge, per-region
`visible`. All regions are created and destroyed as a unit.

Skeleton: [[templates#compound]].

## lock

`lock { … }` is not a surface. It sets `has_lock`, which flows only into the
generated overrides. The lock state machine lives in a separate binary,
`wisp-lock`, and the daemon never links it. `wispctl lock` execs that binary
directly, it is not a socket command.

| property | default | note |
|---|---|---|
| `pam` | `"system-auth"` | PAM service name |
| `prompt` | `"Password:"` | the `{prompt}` token, nothing draws it on its own |
| `bg` | `0xff000000` | flat fill under everything; used when `wall` is off or the cache is missing |
| `wall` | `false` | draw the compiled wallpaper path as the background, from the daemon's cover-fit cache |
| `dim` | `0xff7a808b` | scrim composited **over** the background, so give it an alpha |
| `fg` | `0xffa8d5cc` | default-layout text colour |
| `ring_bad` | `0xffd06878` | default-layout "wrong password" colour |
| `caps` | `0xffe0c060` | default-layout caps indicator colour |
| `ring` | `0xff5f8a93` | default `fg` for `ring` elements that declare none |
| `font_size` | 14 | fallback size for elements with no `font_size` |
| `wrong_ms` | 1200 | how long the `wrong` state stays up |
| `retry_ms` | 0 | first backoff after a wrong password; 0 disables throttling |
| `retry_growth` | 2 | multiplier per further failure |
| `retry_max_ms` | 300000 | backoff clamp |
| `lockout_after` | 0 | failures after which every attempt waits `retry_max_ms`; 0 disables |
| `privacy` | `false` | `{dots}` shows at most one mark and `{count}` shows nothing |
| `wipe_on_backspace` | `false` | one backspace clears the whole buffer |

Backoff after the Nth wrong password is `retry_ms * retry_growth^(N-1)`, clamped
to `retry_max_ms` and never shorter than `wrong_ms`. `lockout_after` latches, but
only until the right password: the lock is the one door back into the session,
so there is deliberately no permanent brick.

`font_size` is scraped for the bake, clamped to 4..256, on the block **and** on
every element.

### lock elements

The whole layout is declared. `frame NAME { … }`, `text NAME { … }` and
`ring NAME { … }` inside the block lower to a table the locker walks in
declaration order; the name is optional and is documentation only. With no
elements declared you get the legacy layout: centred dots, a wrong line and a
CAPS line under it.

| property | frame | text | ring | note |
|---|---|---|---|---|
| `anchor` | yes | yes | yes | `top` `bottom` `left` `right`, OR-combined; an axis with no bit is **centred** |
| `x` / `y` | yes | yes | yes | inset from the anchored edge, or a nudge on a centred axis |
| `width` / `height` | yes | - | - | text is sized by its own content |
| `radius` | yes | - | yes | corner radius on a frame; the **ring's own radius** on a ring, default 50 |
| `border`, `border_width` | yes | - | yes | `border_width` defaults to 1 when `border` is set |
| `bg` | yes | - | yes | on a ring this is the disc inside it, skipped when its alpha is 0 |
| `fg` | - | yes | yes | the ring stroke; a ring with no `fg` uses the block's `ring` |
| `font_size` | - | yes | - | |
| `thickness` | - | - | yes | radial width of the stroke in px, default 8 |
| `segments` | - | - | yes | equal arcs the ring splits into, 1..360, default 1 |
| `gap` | - | - | yes | **degrees** blanked between arcs, default 0 |
| `highlight` | - | - | yes | keypress arc colour; absent or alpha 0 = no highlight |
| `highlight_bs` | - | - | yes | the same arc after a backspace, default = `highlight` |
| `highlight_arc` | - | - | yes | its length in **degrees**, default 60 (swaylock's `M_PI/3`) |
| `separator` | - | - | yes | lines at both ends of the highlight arc, 2px |
| `text` | - | yes | - | template, see below |
| `format` | - | yes | - | strftime format behind `{time}`, default `"%H:%M"` |
| `show` | yes | yes | yes | state condition, see below |

A ring is placed by its bounding box, `2 * radius + thickness + 2 *
border_width` on a side. `border` draws a line on **both** faces of the ring,
inside and outside, like swaylock's. Sectors run clockwise from 12 o'clock and
the first gap is centred there, so `segments = 1` with a `gap` is a full ring
with one notch at the top, and `gap` at or above `360 / segments` draws nothing.

`highlight` is swaylock's per-keypress arc: it jumps to a new pseudo-random
angle at least a quarter turn away on every character and every backspace, and
`wisp-lock` repaints on that keystroke anyway, so it costs no timer. The only
runtime state behind it is the angle and whether the last input was a
backspace. It draws whenever its element's `show` passes, including on an empty
buffer at wherever the angle was left — put it on its own `show = typing` ring
with `fg = #00000000` for swaylock's behaviour of nothing lit until something is
typed.

There are no per-state ring colours, and none for caps lock either. Declare one
`ring` per state with the same geometry and different colours — `show = !wrong`, `show = verifying`,
`show = wrong` — which covers swaylock's ring-color / ring-ver-color /
ring-wrong-color / ring-clear-color in the layer the rest of the lock already
uses. `show` holds one condition, so swaylock's
`caps-lock-key-highlight-color` is the same trick on the other axis: a
`show = !caps` ring and a `show = caps` ring with different `highlight`, and a
`show = wrong` ring painting over both.

`text` interpolates a closed token set and nothing else: `{dots}` the typed
mask, `{count}` the character count, `{prompt}` the block's `prompt`, `{layout}`
the active keyboard layout name, `{time}` `format` through strftime. Any other
interpolation is a compile error.

`show =` takes one of `always` `typing` `empty` `wrong` `caps` `verifying`
`layout_alt` `throttled` `locked_out`, optionally negated with `!`
(`show = !wrong;`). `layout_alt` is true whenever the active XKB group is not
the first one.

A declared `{time}` is the only thing on the lock that changes without input, so
it arms a timer in `wisp-lock`: per-minute, or 1 Hz if `format` contains `%S`,
`%T` or `%s`. Without `{time}` the locker sleeps between keystrokes.

Skeleton: [[templates#lock]].

## gamma

`gamma { … }` links the gamma client, binds a `zwlr_gamma_control_v1` per output
and adds a 1 Hz tick.

| property | default | note |
|---|---|---|
| `day_k` | 6500 | |
| `night_k` | 2800 | |
| `flat_k` | 2400 | manual override, deliberately warmer than night |
| `day_hour` | 7 | |
| `night_hour` | 20 | |
| `fade_min` | 30 | minutes of linear crossfade centred on each schedule edge; 0 is a hard step |
| `transition_ms` | 0 | tween for manual switches at 60 Hz; 0 compiles the tween out |

Driven at runtime by `wispctl gamma auto|day|night|flat|off|state|is-warm`. The
`gamma_warm()` source reads daemon state directly, never poll it with
`exec_line`.

Skeleton: [[templates#gamma]].

## wallpaper

`wallpaper { … }` links the wallpaper and image code and runs per output before
any other surface. The PNG is decoded once at first configure; a missing or
rejected file falls back to a solid `bg`.

| property | default | note |
|---|---|---|
| `path` | *(required)* | PNG only |
| `bg` | `0xff0f1219` | fallback fill |
| `fade_ms` | 300 | nonzero also enables the animation subsystem |
| `transition` | `fade` | `fade`, `dither` or `wipe` |
| `dither_px` | 16 | block size for the pseudo-random reveal |
| `wipe_dir` | `right` | names where the **edge travels to**, so `right` reveals from the left. Diagonals sweep on x+y |
| `wipe_soft` | 160 | lerp band at the edge in px; 1 is a hard line |

`wispctl wall <path.png>` switches at runtime over `fade_ms`; the override lasts
until reload.

## media

`media { }` and nothing else. The body must be empty, any property inside is an
error. It sets both the media and the PipeWire features, so the media keys read
and write through PipeWire. MPRIS control still needs an `mpris()` source or
`wispctl mpris`.

Skeleton for all four blocks: [[templates#wallpaper-and-media]].

## idle

`idle { … }` replaces swayidle and wlopm. It links the idle client, binds
`ext-idle-notify-v1`, and adds **no timer**: every countdown belongs to the
compositor.

```
idle {
    timeout blank {
        after  = 300s;
        run    = "wispctl dpms off";
        resume = "wispctl dpms on";
    }
    timeout sleep { after = 720s; run = "loginctl suspend"; }

    before_sleep = lock;
}
```

| property | where | default | note |
|---|---|---|---|
| `after` | `timeout` | required | duration before the timeout fires; must be positive (`300s`, `5000ms`) |
| `run` | `timeout` | — | `/bin/sh -c` command run when the seat goes idle |
| `resume` | `timeout` | — | command run when activity comes back |
| `before_sleep` | block | — | `lock` (the builtin) or a command string, run before the system suspends |

Timeout names must be unique; they are documentation. Any number of timeouts.

`before_sleep` takes a logind `delay` sleep inhibitor on the system bus at
startup and holds it until the action is **up**, not merely started — the
builtin `lock` releases it the moment the compositor confirms the lock surface,
a command string when it exits. That is swayidle's `-w`, and it is what stops a
suspend from beating the lock to the screen.

Declaring the block also enables [[wispctl#screen-power]] (`wispctl dpms
on|off`), which is what `run` strings use instead of `wlopm`.

If the compositor does not advertise `ext_idle_notifier_v1` (a nested session,
a greeter) wisp warns once on stderr and the timeouts never fire; nothing else
is affected.

## Widgets, groups and cells

The contents of every surface kind.

| property | default | note |
|---|---|---|
| `align` | start | `left`=start, `right`=end, plus `top` `bottom` `center` |
| `width` / `height` | content fit / row height | both accept a runtime expression |
| `pad` | 0 | trailing gap toward centre, mirrored on both sides |
| `pad_x` / `pad_y` | 0 | inner padding |
| `x_offset` / `y_offset` | 0 | post-layout shift, negative is left and up |
| `text` | - | UTF-8; `\n` splits body lines; any type is coerced |
| `icon` | - | integer codepoint or a pixmap expression |
| `icon_box` | 0 = auto | icon column width |
| `icon_gap` | 2 | gap between icon column and text |
| `fg` | `0xffffffff` | text and icon |
| `icon_fg` | 0 = absent | icon only; alpha 0 falls back to `fg`; no transition slot |
| `body_fg` | 0 = absent | text lines after the first; line 1 stays `fg`. Dims a `for` cell's body against its summary, which a second widget cannot do — a `for` block holds exactly one cell |
| `bg` | 0 = off | slab |
| `border` | 0 = off | |
| `border_width` | 1 | |
| `border_top/bottom/left/right` | 1 | |
| `radius`, `radius_tl/tr/br/bl` | 0 | any nonzero uses the antialiased rounded path |
| `press_bg` | 0 | background while pressed and over |
| `hover_bg` | 0 | background while the pointer is over; `press_bg` wins over it |
| `tooltip` | none | literal string; hovering the cell for `delay_ms` pops the `spawned_by = tooltip` surface. Not interpolatable — the pointer outlives the frame |
| `body_lines` | 1 | integer expression |
| `body_fit` | marker | slab height tracks the real line count, `body_lines` becomes a ceiling |
| `text_align` | center | pins the multi-line block |
| `elide` | marker | clamp each line and append an ellipsis |
| `wrap` | marker | word-wrap before measuring; not available in groups |
| `visible` | true | layout skip, or plays `exit_anim` |
| `sticky` | marker | on a scrollable surface, pin this leading row above the scrolled stack |
| `shadow` | 0 = off | drop shadow colour |
| `shadow_x` / `shadow_y` | 0 / 2 | |
| `shadow_blur` | **0** | |
| `shadow_spread` | 0 | |
| `transition_bg/fg/border/size` | 0 | ms |
| `transition_easing` | `ease_out` | |
| `enter_anim` / `exit_anim` | 0 | ms, **require `visible`**; scale alpha and geometry together |
| `enter_easing` / `exit_easing` | `ease_out` | |

### Slider

Marker `slider;`. `value` is required, either a bare `mut` identifier (which
makes dragging write it) or any expression from 0 to `value_max`.

| property | default |
|---|---|
| `value_max` | 1.0 |
| `orientation` | `horizontal` |
| `track_bg` | `0xff202020` |
| `track_fg` | `0xff808080` |
| `track_radius` | 0 |
| `thumb_size` | 0 |
| `thumb_shape` | `bar`, `pill`, `circle`/`disc`/`knob`, `none` |
| `thumb_radius` | 0 |
| `thumb_color` | 0, falls back to `track_fg` |
| `thumb_border` / `thumb_border_width` | 0 / 0 |
| `show_value` | marker |
| `value_format` | `"%.0f"` |
| `value_scale` | 100.0 |
| `value_fg` | `= fg` |
| `value_gap` | 6 |
| `value_align` | `end` |

Track length is the main-axis dimension, thickness the other one. Knob travel is
inset by half the thumb at each end so dragging to the physical end still gives
exactly 0 or 1.

### Graph

`graph = <src>.<field>` where the field is a numeric status field from `cpu`,
`mem`, `temp`, `bat`, `disk` or `backlight`. Not `net()` rates, not
`exec_line()`.

| property | default |
|---|---|
| `graph_max` | 100 |
| `graph_samples` | 60, ring of 2..256 |
| `graph_fg` | `0xffffffff` |

The ring is sampled inside that source's own change path, so a graph adds zero
idle cost. Newest sample is flush right. Max 32 graph widgets, and a graph
inside a `group` is a **hard error**.

### Groups

The group schema is exactly nine properties: `align bg border border_width gap
height pad pad_x radius`. No `pad_y`, no per-side borders.

Container colours must be static, a literal or a `const` chain, never a ternary.
Member `fg`, `icon_fg`, `text`, `icon` and `bg` may be dynamic. A group is one
flex slot; on a vertical surface it becomes a band, which is the only way to put
two differently-styled texts on one line. A horizontal group whose members are
all invisible collapses entirely. Groups are not wired for compound regions.

### for blocks

Five iterables, each needing **exactly one** `cell { … }`. They live at surface,
widget or group scope.

| head | cap | cell fields |
|---|---|---|
| `for t in <tags-src>.list` | 9 | `label` `index` `active` `urgent` `occupied` `pinned` `output` |
| `for n in <dbus_signal-src>.history` | 8 | `summary` `body` `url` `urgent` |
| `for n in <notifications-src>.history` | `history=` (16) | `summary` `body` `app` `icon` `image` `urgent` `id` |
| `for i in <tray-src>.items` | 8 | `icon` `has_icon` `title` `id` `status` `index` `has_attention_icon` `menu_open` |
| `for r in rows` | 32 | `label` `icon` `has_icon` `selected` `index` `enabled` `separator` `toggle` `checked` |

Anything else is an error naming those forms. `tags.list` is unrolled at
compile time; the others are runtime loops.

A notification entry has **no `index`**: the ring can shift while a click is
still travelling over the socket, so `note.id` (a monotonic serial) is the only
stable dismiss key — `exec("wispctl notif dismiss {note.id}")`.

## Workspaces per compositor

`tags()` picks a backend in strict priority, first hit wins:

1. **mango IPC**, first because its IPC reports client counts, so an empty tag is distinguishable from an occupied one.
2. **hyprland IPC**, before the rest for the same reason.
3. **river-status**, whose `view_tags` gives true occupancy.
4. **ext-workspace-v1**, the portable fallback: sway, niri, labwc, cosmic, kwin, hyprland, patched dwl.
5. Nothing, in which case the tag row stays empty and a message is logged.

That is the closed set; Wayfire's wire format rules it out.

What differs in practice:

- **Occupancy.** mango, hyprland and river report real occupancy. ext-workspace has no client count, so `tag.occupied` degrades to "the workspace exists and is not hidden". On niri that means the bar mirrors niri's live list including its trailing empty workspace.
- **Numbering** under ext-workspace: the workspace name is parsed as 1..32 first, then the compositor's first coordinate plus 1, then arrival order. Anything outside 1..32 is dropped.
- **Multi-monitor clicks.** Always pass `tag.output`: `exec("wispctl tag {tag.index} {tag.output}")`. Without it a click switches the keyboard-focused monitor instead of the clicked one. reverie passes it, anemoia does not.
- **`tag.pinned`** reads a compile-time mask, not compositor state, and is identical on every backend.
- **Poll cost.** mango and hyprland add a real fd to epoll; river and ext-workspace add none.

## Gotchas

- Renaming the `osd`, `pill` or `menu` surface silently drops that engine; the name is load-bearing, not just `spawned_by`.
- The surface property schema is one union, so OSD properties on a bar and menu properties on an OSD pass `--check` and do nothing.
- `margin` is inert on a HUD, the slide is a render offset.
- `scroll` needs `axis = vertical`, and `sticky` only works on the leading rows.
- `dismiss_on_unfocus` without `on_escape` is an error; it has no command of its own.
- `keyboard = exclusive` on a panel eats every keystroke in the session until it is dismissed.
- A notification cell has `note.id`, not `note.index`.
- A compound without `width` and `height`, a region without `edge`, or a region without an integer literal `size` is a hard error.
- A `graph` inside a `group` is a hard error; a slider inside a group compiles fine despite older docs.
- Anything above the rows in a menu template must declare a `height`, or the header height is measured as zero.
- `media { … }` with any property inside is an error, the block is a pure marker.
- Declaring any lock `frame`/`text` element drops the built-in layout entirely; `dim` is a scrim over the background, and `ring` is drawn by nothing.

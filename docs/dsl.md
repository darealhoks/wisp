# The .wisp language

A `.wisp` file describes everything wisp puts on screen. It is compiled, not
read at runtime. `wispc` turns it into C, the C is compiled into the daemon, and
the daemon has no idea a config file ever existed.

That has one consequence worth stating up front: **every edit needs a rebuild.**

    make install && wispctl reload

Start from `configs/bee.wisp`. It is the config this rice runs and it uses most
of the language.

## Shape of a file

Six things can appear at the top level.

    source NAME = call(...);     data that changes over time
    surface NAME { ... }         a window on screen
    compound NAME { ... }        several edge-anchored surfaces as one unit
    const NAME = expr;           a compile-time value
    mut NAME = expr;             a value a handler can write
    SELECTOR { ... }             a style rule

Plus four optional blocks that configure a subsystem: `lock`, `gamma`,
`wallpaper` and `media`.

Order does not matter. `wispc` parses the whole file before it resolves
anything, so style rules can sit at the bottom and a `const` can be defined
after its use.

Every declaration and every property ends in a semicolon.

## Literals

    42  0x1a         int, optional ms/s suffix: 200ms, 2s
    3.14             float, no suffix
    "text"           string, \" \\ \n \xNN, {expr} interpolates, \{ is a brace
    #rrggbb          color, alpha becomes ff
    #aarrggbb        color with alpha
    true false       bool
    1..8             range, int endpoints only, used by animated fillets

`2s` and `2000` are the same token to the compiler. The suffix is stripped by
the lexer.

Colors are ARGB. Always write the alpha on anything you expect to see. A color
that folds to alpha 0 is not painted, and there is no warning for it.

String interpolation is the only way to build a string. There is no `+`.

    text = "{cpu.pct}% {mem.pct}%";

## Sources

A source is a value the daemon keeps up to date. Reading a field of a source
inside a surface marks that surface dirty whenever the field changes, and only
then. Nothing polls a surface that is not watching anything.

    source cpu  = cpu();
    source time = clock("%H:%M");

`NAME` on its own reads the first field. `NAME.field` reads a named one.

These are the sources that work:

| source | fields | notes |
|---|---|---|
| `clock(fmt)` | `value` | ticks per second only if the format shows seconds |
| `cpu()` | `pct` `load1` | |
| `mem()` | `pct` `used_mb` | |
| `temp(zone=)` | `c` | |
| `bat(name=)` | `pct` `charging` | |
| `wifi(iface=)` | `ssid` `signal` | |
| `disk(path=)` | `pct` `free_gb` | |
| `vpn(provider=)` | `state` `ok` | |
| `tags(labels=, pinned=)` | `title`, `list` | `list` is for `for` only |
| `gamma_warm()` | `value` | "1" or "0" |
| `dnd()` | `value` | "on" or "off" |
| `ui_hidden()` | `value` | "1" or "0", driven by `wispctl hide` |
| `exec_line(cmd, every=)` | `value` | first line of stdout, 256 bytes max |
| `dbus_signal(iface, member)` | `value`, `history` | `history` is for `for` only |

The status sources share one timer that fires once a second, and each compares
its fields against the last sample before dirtying anything. An idle bar does
not repaint.

`gamma_warm()`, `dnd()` and `ui_hidden()` read daemon state directly. Never
reach for them through `exec_line("wispctl ...")`, which forks the daemon back
into itself once a second.

The table above is the whole list — a source the compiler accepts is a source
it can build. `mpris()` and `inotify()` were removed rather than left as names
that passed `--check` and failed `--emit`; they come back with their drivers.

### exec_line

    source vol = exec_line("wpctl get-volume @DEFAULT_SINK@", every="2s");

The command runs under `sh -c`. On click, a widget reading the source re-polls
after 120 ms, which is usually enough for the external tool to have applied the
change. `refresh="instant"` makes that re-poll synchronous. A `set()` on the
source inside the handler suppresses the re-poll entirely, so you can update the
display optimistically.

## const and mut

    const BG = #ff0f1219;
    const PILL = BG;              // a const may name another const
    mut brightness = 0.6;

`const` is inlined at compile time. `mut` becomes a static variable, typed from
its initializer, that handlers write with `set()` or `animate()`.

A colour property that is given but cannot be folded to a literal is a build
error. That is deliberate. If it defaulted to 0 instead, the fill would be
skipped for alpha and you would get an invisible widget with no diagnostic.

String muts exist but cannot be animated.

## Surfaces

A surface is one layer-shell window.

    source time = clock("%H:%M");

    surface bar {
      layer = top;
      anchor = top|left|right;
      height = 24;
      exclusive_zone = 24;
      bg = #cc000000;
      widget clock { align = right; text = time; fg = #ffeeeeee; pad = 16; }
    }

That is a working bar. Everything else is refinement.

A bar, a HUD, an OSD and a menu are not four different things in wisp. They are
four surfaces with different anchors, layers and lifecycles.

Enum values are bare words, never quoted.

The properties you reach for first:

| property | default | meaning |
|---|---|---|
| `layer` | `top` | `background`, `bottom`, `top`, `overlay` |
| `anchor` | `top\|left\|right` | edges, ORed together |
| `width` / `height` | auto / 24 | required for a float, not for an edge-to-edge anchor |
| `margin` | 0 | uniform gap outside the surface |
| `exclusive_zone` | height for a bar | positive reserves space, `-1` overlays everything |
| `bg` | `#ff000000` | body colour |
| `bg_bottom` | off | makes `bg` a vertical gradient |
| `font_size` | 14 | baked at build time |
| `keyboard` | `none` | `none`, `on_demand`, `exclusive` |
| `axis` | `horizontal` | `vertical` stacks along Y |
| `input` | full | `input = none;` lets pointer events fall through |
| `visible` | always | an expression gating creation and destruction |
| `radius` | 0 | with `radius_inner` / `radius_outer` / `radius_tl` and friends |

Corner shaping goes further than `radius`. `fillet_*` draws a convex arc into an
empty quadrant so a floating bar appears to melt into the screen edge, and takes
a range (`0..8`) to animate as a HUD slides in. `armpit_inner` and
`armpit_outer` cut concave feet instead. Every variant has anchor-relative
aliases — see the `radius` / `fillet` handling in `src/tools/wispc/`.

### Hover-revealed surfaces

    reveal_on_hover = 4;      // px of hover strip along the anchored edge
    reveal_gutter = 4;        // unpainted band; height stacks below it
    reveal_anim_ms = 200;
    reveal_easing = ease_out;

The surface exists only as an input strip until the pointer enters it, then
slides out. Set `reveal_gutter = 0` when the surface floats and there is nothing
to fill the gutter with.

## Widgets

A widget is a cell of text and an icon inside a surface.

    widget cpu {
      icon = 0xf4bc;
      text = " {cpu.pct}%";
      fg = cpu.pct >= 90 ? RED : TEXT;
      pad = 8;
    }

`align` puts the widget in the `left`, `right` or `center` bucket, and widgets
flow in declaration order within their bucket. `pad` is the gap to the next
neighbour; `pad_x` and `pad_y` are inner padding that grows the widget's own
slab.

Omit `width` to fit content. Both `width` and `height` accept a runtime
expression, resolved during the measure pass:

    width = tag.active ? 34 : 30;

Icons are Nerd Font codepoints written as hex. There are no images and no icon
themes. The set of glyphs available is baked from your font by `tools/bake.c`.

`bg`, `border`, `radius`, `press_bg` and `shadow` dress the widget's own slab.
`visible` skips it in layout, or plays its exit animation if it has one.

### Sliders

Declare the marker, bind a `mut`:

    mut vol = 0.5;
    widget volslider {
      slider;
      value = vol;
      orientation = horizontal;
      track_bg = #ff202020; track_fg = #ff808080;
      thumb_shape = circle; thumb_size = 12;
      show_value; value_format = "%.0f"; value_scale = 100;
    }

The track fills the main axis of the surface. Thickness comes from the other
dimension.

### Handlers

    on_click([p])   on_press([p])   on_release([p])
    on_scroll([d])  on_drag([dx[,dy]])   on_change()

`p` is the 1-based cell index inside a `for`, `d` is +1 or -1 for a wheel step.
The body is one statement or a `{ }` block. Four statements exist:

    exec("sh command {interp}")           // runs under sh -c, failures are logged
    set(name, expr)                       // writes a mut or an exec_line source
    emit(surface, kw = val, ...)          // instantiates a template surface
    animate(mut, target, duration, easing)

Example, straight from the pattern `bee.wisp` uses:

    widget wifi_btn {
      icon = 0xf1eb;
      on_click() = exec("foot --app-id=ws-hud-wifi -e impala");
    }

## Style rules

Structure and appearance are written separately. A surface, widget or cell body
says what exists and what it does. A style rule says what it looks like.

    group      { bg = CRUST; border = BORD; border_width = 2; radius = 8; }
    #conngrp   { pad_x = 18; }
    .sep       { fg = BORD; }
    #hud widget { align = center; pad = 6; }
    .ws:active { fg = WSACT; width = 34; }

Selectors are a type keyword, an id (`#name`, or a bare name), a `.class`, a
descendant chain, or a comma-separated list of those. A class is declared as a
suffix on the node's name:

    widget mvpn_btn.btn.toggle.pop { ... }

Whitespace matters. `bar.pill` is one compound selector; `bar .pill` is a
descendant chain.

The cascade resolves entirely at compile time, before sema. Matching properties
are copied into each node and the rules are thrown away, so a style rule can
never do anything a property in the body could not.

Specificity is id 100, class 10, type 1, and higher wins. Two rules of equal
specificity setting the same property on the same node is a compile error naming
both rules. There is no silent later-wins. A property written inline in a node
body always beats every rule, which is how a widget keeps its own ternary under
a blanket `widget { fg = ... }` rule.

Three pseudo-classes exist:

    :active     on for-cells, becomes  loopvar.active ? styled : base
    :urgent     on for-cells, applied after :active so it wins
    :pressed    on any widget, may only set bg

## for blocks

There are three iterables, and each `for` needs exactly one `cell`.

    for tag in tags.list { cell { ... } }        // 9 cells, unrolled at compile time
    for note in notes.history { cell { ... } }   // ring of 8, at runtime
    for item in $items { cell { ... } }          // menu template only

Fields are `label index active urgent occupied pinned output` for a tag,
`summary body url urgent` for a notification, `label index selected` for a menu
item.

    for tag in tags.list {
      cell.ws {
        text = tag.label;
        visible = tag.occupied || tag.active;
        on_click() = exec("wispctl tag {tag.index} {tag.output}");
      }
    }
    .ws        { fg = TEXT;  bg = CRUST; width = 30; height = 30; radius = 8; }
    .ws:active { fg = WSACT; width = 34; height = 34; }

`tag.output` is the slot index of the monitor the bar is on. Passing it to
`wispctl tag` switches the clicked monitor rather than the keyboard-focused one.

`tags()` takes two optional named args, both space-separated strings:

    source tags = tags(labels = "term www chat", pinned = "1 2 3");

`labels` overrides the pill text positionally (unlisted tags keep their
number); `pinned` names tags whose `tag.pinned` is true — put it in `visible`
to keep those pills on screen even when empty. Both are compile-time
constants; occupied-driven visibility stays the dynamic path.

A `for` belongs at surface or widget scope. Written anywhere else it parses,
passes sema, and is then silently dropped by codegen. Nothing tells you.

## Groups

A group packs several widgets into one rounded container, the way waybar's
`group/` does. Members draw without their own borders inside it.

    group sysgrp {
      align = right;
      bg = CRUST; border = BORD; border_width = 2; radius = 8;
      pad_x = 12; gap = 10; pad = 8;
      widget cpu { icon = 0xf4bc; text = " {cpu.pct}%"; }
      widget sep.sep { text = "/"; }
      widget mem { icon = 0xf538; text = " {mem.pct}%"; }
    }

The group is a single flex slot. It aligns as a unit, and members flow inside
it. Container colours must be static, meaning a literal or a chain of consts; a
ternary there is a build error. Member `fg`, `bg`, `text` and `icon` may be
dynamic.

Groups take widgets and nothing else. No sliders, no `for`, no transitions, no
`body_lines`. They work on horizontal surfaces only, and a group on
`axis = vertical` is a codegen error.

## Compounds

A compound is several thin edge-anchored surfaces that share a corner.

    compound frame {
      width = 320; height = 240; bg = CRUST; radius = 8; radius_inner = 12;
      region top  { edge = top;  size = 28; widget title { text = "..."; } }
      region left { edge = left; size = 28; }
    }

One region per edge, and every edge must lie inside the compound's `anchor`.
`radius_inner` fills the L-shaped notch with a quarter disc. A region body is a
surface body.

## Templates

A surface with `spawned_by` is not created at startup. Each `emit()` makes one
instance, and `$name` inside the body reads an emit argument.

Two templates are wired end to end.

**osd** — `surface osd { spawned_by = osd; ... }` takes `$summary $body $icon
$pct $muted $urgency`, plus `$progress` (0–100, or -1 on a plain
notification), `$nbody` (how many lines the body wrapped to) and the derived
`$mute`/`$warn` booleans behind the `:mute`/`:warn` pseudo-classes. Your
widget blocks ARE the slab: the generated renderer draws them once per
notification, so a slab is composed like any other surface — text with
`body_lines = 1 + $nbody`, an `elide;` marker to truncate a long summary with
an ellipsis, `visible = $progress >= 0` gating, a `slider;` widget as the
progress bar, and `#osd:warn { bg = ...; }` styling per category. `osd.c`
keeps only what a declaration cannot say: the slab ring, the slide tweens and
the bar cutout.

Properties on the surface become compile-time overrides that osd.c reads:
`max gap pad_x icon_gap prog_h body_lines body_max timeout timeout_low
timeout_normal slide_ms fillet_r border_width separator separator_frac
prog_fg prog_track fg focus_follow dbus_close`. `anchor` and `margin` *are*
the layout: an edge named in `anchor` with `margin = 0` sits flush against it
— that side stays square, its border stays open, and under a bar `fillet_r`
flares the corner into the bar row — while `margin > 0` lifts the chain off
every edge so all four corners round and the outline closes. `anchor = top;
margin = 12` is a floating top-centre stack; `anchor = bottom | right;
margin = 0` is the corner stack.

**pill** — `surface pill { spawned_by = osd_pill; ... }`. Declare it and
progress-only posts (volume / brightness) skip the stack and render as this
surface instead: one fixed-height slab, top-centred by the compositor. The
body is composed like any other surface — `$icon` and `$progress` are bound,
and `:mute`/`:warn` style it per state (`#pill:warn { bg = ...; }`). `width
height margin fillet_r` size and anchor it; `margin > 0` floats it below the
bar so all corners round, `margin = 0` rests it flush under the bar with
`fillet_r` flaring into the bar row, and a *negative* margin tucks the body
that many px inside the bar row, straddling the edge. Without this surface,
progress posts join the stack like any notification.

**menu** — `surface menu { spawned_by = menu; ... }`. Same idea; `menu.c` owns
the render loop.

    surface menu {
      spawned_by = menu;
      layer = overlay; anchor = top|left|right;
      height = 28; exclusive_zone = -1; keyboard = exclusive;
      bg = CRUST; fg = TEXT; sel_bg = REST; sel_fg = TEXT; dim = SUBTXT;
      prompt = "run:"; sort = "most_used";
    }

Colours are `bg fg sel_bg sel_fg dim border`; sizes are `height pad_x
item_pad_x gap radius border_width width margin max_visible row_h`. `sort` is
`"most_used"` or `"alphabetical"`. `terminal = "foot -e";` is the prefix put in
front of `Terminal=true` desktop entries. `icons = true;` turns on app-icon
decode for `wispctl apps` (off by default — it is the launcher's fattest
RAM/IO cost). Add `axis = vertical;` and the full-width strip becomes a
top-centred launcher float with scrolling rows, wheel and arrow navigation.

Writing `for item in $items { cell ... }` inside a menu still parses, but
`menu.c` never consumed it. It does nothing.

### Declared menus

    menu power {
      item { icon = 0xf011; label = "Poweroff"; exec = "loginctl poweroff"; }
      item { label = "Logout"; exec = "pkill -x mango"; }
    }
    menu emoji { preset = emoji; }

`wispctl menu <name>` opens one; picking runs `exec`. `icon` is a codepoint
that must also be in `tools/bake.c`'s `ICONS[]` (baked backend). No `menu`
decl means those entries — and, for `preset = emoji`, the whole baked gemoji
table — are not compiled in. Menus share the `surface menu` styling.

## Optional blocks

    lock { pam = "login"; prompt = "..."; bg = ...; ring = ...; ring_bad = ...;
           fg = ...; dim = ...; caps = ...; font_size = 18; wrong_ms = 600; }

`lock {}` only styles the locker. It does not gate it. `wisp-lock` is built and
installed whether or not the block is present.

    gamma { day_k = 6500; night_k = 3400; flat_k = 5000;
            day_hour = 7; night_hour = 20; }

    wallpaper { path = "/path/to/wall.png"; bg = #ff0f1219; }

    media { }

`media {}` is empty on purpose. Its presence enables MPRIS and the media keys.

## Feature flags

Nothing is enabled by a switch. Using a construct compiles its module in, and
not using it leaves the code out of the binary entirely.

| what you write | what gets linked |
|---|---|
| `exec_line(...)` | the exec runner |
| `dbus_signal(...)` | the D-Bus client |
| `tags()` | the workspace backends (mango IPC + ext-workspace) |
| a surface with `reveal_on_hover` | `hud.c` |
| `surface osd { spawned_by = osd }` | `osd.c` and D-Bus |
| `surface pill { spawned_by = osd_pill }` | the progress pill path in `osd.c` |
| `exclusive_zone = -1`, or a surface named `menu` | `menu.c` |
| `lock {}` | the lock IPC |
| `gamma {}` | gamma control |
| `wallpaper {}` | `wall.c` |
| `media {}` | MPRIS and media keys |
| any `transition_*`, `enter_anim`, `exit_anim`, `animate()` | `anim.c` |

This is why binary size and memory depend on your config, and why a stripped
config produces a genuinely smaller daemon rather than a smaller code path.

## Animation

    transition_bg = 150ms;  transition_fg = 150ms;  transition_easing = ease_out;
    enter_anim = 200ms;  exit_anim = 120ms;
    enter_easing = ease_out;  exit_easing = ease_in;

`enter_anim` and `exit_anim` need the widget to have a `visible` expression, as
they play on the transition of that expression.

Easings are `linear`, `ease_in`, `ease_out`, `ease_in_out`, or
`cubic_bezier(a,b,c,d)`. Hyphenated `ease-out` is also accepted.

Animation is the one thing that makes the daemon use CPU. Nothing is running,
nothing repaints, and no timer is armed when nothing moves.

## Checking your work

    build/wispc --check configs/bee.wisp     parse and typecheck only
    build/wispc --emit build/gen-tw configs/bee.wisp

`--check` catches unknown sources and fields, undefined identifiers, `$args`
outside a template, writes to a const, bad colour literals and bad easings.

`--emit` catches the rest, and its messages are prefixed `codegen:`. The ones
you are most likely to meet: a source with no driver, a `for` over something
that is not `.list` or `.history`, a `for` without exactly one `cell`, and a
slider without a `value = <mut>`.

## What the language does not have

No functions, macros, `if`, `while` or `switch`. Use the ternary.

No arrays, structs or dicts. List data reaches the screen through `for` and
nowhere else.

No imports. One file per build.

No runtime reload of properties, no runtime cascade. `wispctl reload` re-execs
the binary you compiled.

No images, SVG or icon themes. Nerd Font codepoints only.

No string concatenation. Use interpolation.

# Gotchas

Everything in wisp that compiles and does not do what you meant, in one place.

## Parses and does nothing

| written | reality |
|---|---|
| a handler parameter on a non-`for` widget | bound to NULL, interpolating it prints garbage |
| `margin` on a HUD | inert, the slide is a render offset not a margin tween |
| any OSD property on a bar, any menu property on an OSD | the surface schema is one union, unread properties are silent |
| a `for` outside surface, widget, group or region scope | parses and is dropped |

## Passes --check, fails --emit

Always run both.

| config | error at `--emit` |
|---|---|
| more than 16 interpolated expressions in one string | `too many interp args` |
| `slider;` without `value = <mut>` | `slider widget needs value = <mut>` |
| `on_change(p)` on a slider | `unresolved identifier 'p'` |
| more than 64 `const` plus `mut` | `too many consts/muts (max 64)` |
| `every=` on `vpn`, `bat`, `disk`, `backlight` | `every= only applies to polled kinds` |
| `every=` under 250 ms | `must be >= 250ms` |
| an unknown or hyphenated easing in `animate()` | `unknown easing` |
| `emit()` to a spawn template other than `osd` | `emit() only targets the osd spawn template` |
| `animate()` on a string `mut` | `cannot animate a string mut` |
| a colour property that will not constant-fold | hard colour eval error |
| `inotify(path=)` relative, or extra keyword arguments | `inotify() …` |
| over 32 sources, 16 surfaces, 8 menus, 32 graphs, 16 `toplevel()` | the matching "too many" |
| a `graph` inside a `group` | `graph widgets are not allowed inside a group` |

## Fails at link, not at compile

`emit()` to any spawned template other than `osd`. Only `spawn_osd` is
generated, so `emit(pill, …)` and `emit(menu, …)` pass sema and codegen and then
fail to link.

## Silent truncation

| thing | cap | what happens |
|---|---|---|
| `const` plus `mut` | 64 | extras dropped, later reference is an unresolved identifier on line 1 |
| interpolation format string | 1024 B | clipped |
| interpolation output buffer | 64 to 2048 B | snprintf truncation |
| a compiled expression's text | 1024 B | clipped |
| string `mut` | 128 B | snprintf-bounded |
| baked font sizes | 64 distinct, 4..256 | extras ignored |
| dbus history ring | 8 entries | oldest lost |
| notification history ring | `history=`, 16 default, 128 max | oldest lost |
| tags unrolled | 9 cells | rest never drawn |

## Naming traps

- The OSD, pill, menu and tooltip templates must be named `osd`, `pill`, `menu` and `tooltip`. The `spawned_by` property alone is not enough; codegen looks the template up by name. Rename it and the engine silently drops out.
- The polkit and greet templates are the exception: they are found by their `spawned_by` value, so their names are free.
- A surface named `bar` is the one panel the tag accumulator and lock-on-output path find. Other names are plain panels.
- `bar.pill` is one node with a class; `bar .pill` is a descendant chain. One space changes the selector.
- `surface NAME {` is a declaration but a bare `surface {` or `surface.cls {` is a style rule. Same for `menu NAME {` versus `menu {`.
- `#name` and a bare `name` are the same id selector.
- Classes are declared as a name suffix (`widget wifi.pill`, `cell.ws`), not as a property.

## Style cascade

- Equal specificity on the same property of the same node is a **hard error** naming both rules. CSS's later-wins does not exist here.
- An inline property in a body beats every rule, always.
- `:hover` and `:pressed` may only set `bg`. They are pointer state resolved at runtime, so unlike the other pseudos they need no base value — and they do nothing on a menu row, where `hover;` moves the selection instead.
- `:pressed` beats `:hover` on the same cell.
- `:active`, `:urgent`, `:mute` and `:warn` need a base value already on the node, and only work on `for` cells or inside a spawned template.

## Layout traps

- Anything above the rows in a menu template must declare a `height`, or the header height computes as zero.
- Menu row sizing is measured against font size 14, so declare `row_h` whenever you change `font_size`.
- A `height` **expression** on a menu row cell desyncs the hit grid from the pixels. `row_h` and `separator_h` on the menu are the only supported way to vary row height.
- `separator` and `separator_frac` do nothing on a menu unless `separator_h` is non-zero and `separator` has alpha; on an OSD stack they mean the inter-slab rule instead.
- An OSD slab has no `pad_x`, so the leading gap before the first widget is that widget's own `width`.
- `enter_anim` and `exit_anim` require a `visible` expression to have anything to trigger on.
- A group's container colours must be static; a ternary there is an error. Only member properties may be dynamic.
- The group schema has exactly nine properties, no `pad_y` and no per-side borders.
- Any one of radius, border, fillet, armpit, gradient, clip, cutout or a slider turns off a bar's partial repaint path.
- `scroll` needs `axis = vertical` on the same surface, and `sticky` only holds on the leading run of rows. Both are hard errors.
- `pad_x` and `pad_y` on a surface are read only when it scrolls; elsewhere they are among the silently unread union properties.
- `margin_x` overrides `margin` on the left and right only, which is how a panel clears the bar vertically and still lines up with its side inset.

## Language traps

- No `\xNN` escape. Unknown escapes keep both bytes verbatim.
- `\{` keeps the backslash, it only suppresses interpolation.
- Time suffixes are integer-only: `1.5s` is a parse error, write `1500ms`.
- `..` binds tighter than any binary operator, so `a + 1..2` is `a + (1..2)`.
- There is no string `+`. Use `"{a}{b}"`.
- Function calls are legal only as a `source` right-hand side, plus `cubic_bezier()` in `animate()`.
- A `{ … }` handler body needs a trailing `;` after the closing brace; without it the error points at the next brace.
- Block comments do not nest.
- Single-quoted strings do not exist, only `"…"`, even though `\'` is a valid escape inside one.
- A string literal inside `{…}` interpolation terminates the outer string; move the ternary out of the interpolation.
- The unknown type never errors, which is how a misspelled `for` cell field survives `--check`.

## Runtime traps

- Never poll daemon state with `exec_line("wispctl …")`. `gamma_warm()`, `dnd()` and `ui_hidden()` read it in-process; a shell poll forks a client back into the daemon.
- Reading `net().rx_kbps` or `tx_kbps` turns an event-driven source into a polling one.
- A `gamma {}` config is never idle-zero-CPU: it gets a dedicated 1 Hz timer if no polled status source exists.
- Always pass `{tag.output}` to `wispctl tag`, or the click switches the focused monitor.
- Under `ext-workspace-v1` there is no client count, so `tag.occupied` means "exists and is not hidden".
- `make` alone is not enough. `wispctl reload` re-execs the installed binary, so use `make install`.
- The daemon must start before your tray apps do; wisp owns the StatusNotifier watcher name.
- `dismiss_on_unfocus` is an error without `on_escape`: it reuses that command rather than taking one of its own.
- A `spawned_by = greet` surface needs `$GREETD_SOCK`: outside greetd it is fatal, not a degraded mode. Preview one with `fakegreet 'mango -c configs/... -s build/greet/wisp'`.
- `keyboard` defaults to `exclusive` on a greet surface and to `on_demand` everywhere else.
- `keyboard = exclusive` holds the session's keyboard for as long as the surface is mapped, so a panel that declares it eats every keystroke until dismissed. Panels want the default `on_demand`.
- Without `output = active` a panel opens on every monitor at once, and a monitor plugged in later never gets a copy of one that has it.
- Dismiss a notification by `note.id`, never by a row index; the history ring can shift while the click is still travelling over the socket.
- `notifications(image=N)` thumbnails only decode when the OSD surface also declares `image = N`; they ride that decode.
- Icon names resolve as PNGs only, under an app-supplied dir, the XDG hicolor `apps/` sizes, loose `<data>/icons/NAME.png`, then `/usr/share/pixmaps`. No theme index, no SVG.

## Lock traps

- `lock { dim }` is a scrim drawn over the background, not a text colour. An opaque value covers the wallpaper completely; give it alpha.
- `lock { ring = … }` is only the default `fg` for `ring` elements. It draws nothing on its own — declare a `ring NAME { … }` element.
- On a `ring`, `radius` is the ring's radius, not a corner radius, and `gap` is in **degrees**. `gap >= 360 / segments` leaves nothing to draw.
- Ring colour per state is several `ring` elements with different `show`, not per-state properties. Overlapping conditions all draw, in declaration order.
- `show` holds exactly one condition, so a caps-lock highlight colour is a `show = !caps` / `show = caps` pair of rings, not a `caps_highlight` property.
- A ring with no `highlight` draws no keypress arc at all — it is opt-in, and `highlight_arc` and `separator` do nothing without it.
- The keypress arc follows its element's `show`, not the input state. On a `show = !wrong` ring it is already lit before the first keystroke; gate it on `typing` instead.
- Declaring one element replaces the whole legacy layout, not just that line. Nothing draws the prompt, the dots or the caps indicator unless you declare it.
- `text = "…"` is a property, `text NAME { … }` (or `text { … }`) is an element. The parser splits on what follows the keyword.
- A lock `text` element with no `anchor` is centred on both axes, and `x`/`y` become nudges rather than insets.
- `{layout}` is a label lifted out of the keymap's `name[groupN]` lines. Key translation stays on group 1, so an alternate layout still types the first layout's characters.
- A `{time}` element is the only reason `wisp-lock` wakes without input, and `%S`/`%T`/`%s` in its `format` turn a per-minute timer into 1 Hz.
- `lockout_after` is not permanent: the correct password still clears it. Do not treat it as a hard lockout.

## Gotchas

- This entire page is the gotchas; the per-page `## Gotchas` sections are the short version for that topic.

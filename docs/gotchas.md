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
| tags unrolled | 9 cells | rest never drawn |

## Naming traps

- The OSD, pill and menu templates must be named `osd`, `pill` and `menu`. The `spawned_by` property alone is not enough; codegen looks the template up by name. Rename it and the engine silently drops out.
- A surface named `bar` is the one panel the tag accumulator and lock-on-output path find. Other names are plain panels.
- `bar.pill` is one node with a class; `bar .pill` is a descendant chain. One space changes the selector.
- `surface NAME {` is a declaration but a bare `surface {` or `surface.cls {` is a style rule. Same for `menu NAME {` versus `menu {`.
- `#name` and a bare `name` are the same id selector.
- Classes are declared as a name suffix (`widget wifi.pill`, `cell.ws`), not as a property.

## Style cascade

- Equal specificity on the same property of the same node is a **hard error** naming both rules. CSS's later-wins does not exist here.
- An inline property in a body beats every rule, always.
- `:hover` is rejected with a pointer to the `hover;` marker, which is a menu selection behaviour, not a style state.
- `:pressed` may only set `bg`.
- `:active`, `:urgent`, `:mute` and `:warn` need a base value already on the node, and only work on `for` cells or inside a spawned template.

## Layout traps

- Anything above the rows in a menu template must declare a `height`, or the header height computes as zero.
- Menu row sizing is measured against font size 14, so declare `row_h` whenever you change `font_size`.
- An OSD slab has no `pad_x`, so the leading gap before the first widget is that widget's own `width`.
- `enter_anim` and `exit_anim` require a `visible` expression to have anything to trigger on.
- A group's container colours must be static; a ternary there is an error. Only member properties may be dynamic.
- The group schema has exactly nine properties, no `pad_y` and no per-side borders.
- Any one of radius, border, fillet, armpit, gradient, clip, cutout or a slider turns off a bar's partial repaint path.

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

## Gotchas

- This entire page is the gotchas; the per-page `## Gotchas` sections are the short version for that topic.

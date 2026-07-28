# State and handlers

Three ways to hold a value: `const` (inlined at compile time), `mut` (a static C
variable), and a `source` (driven by the outside world, see [[builtins]]).

```wisp
const ACCENT = #ff64799c;
mut  vol     = 0.5;
source bat_s = bat("BAT0");
```

## const

Inlined everywhere it is used. It may reference another `const`, and it may be
used before it is declared. Cycles are caught at depth 32.

```wisp
const CRUST = #ff0e131c;
const BORD  = #ff2e3a4e;
const TRAY_ICONS_ONLY = true;
```

Colour properties **must** fold to a constant, so a `const` chain is how you get
a palette: one `const` per colour, referenced everywhere else.

## mut

Becomes a static variable typed from its initializer. A string `mut` is a
`char[128]`. `const` and `mut` share one namespace and one budget: **64 of them
combined**, and going over truncates silently, showing up later as
"unresolved identifier" pointing at line 1.

Only a `mut` can be the target of `set()` or `animate()`, and only a `mut` (as a
bare identifier) makes a slider read-write.

## Statements

Exactly four statements exist. They appear in handler bodies and in a source's
`on_change`.

| statement | form | rules |
|---|---|---|
| `exec` | `exec("cmd {interp}")` | runs through `/bin/sh -c`; errors are logged, never fatal |
| `set` | `set(NAME, expr)` | NAME is a `mut` or an `exec_line` source; a `const` target is an error |
| `emit` | `emit(SURFACE, kw = val, …)` | SURFACE must be a declared surface with `spawned_by`, or the sink `menu_reply` |
| `animate` | `animate(MUT, to, dur [, easing] [, repeat = e] [, alternate])` | target must be a `mut`; string muts cannot animate |

A `{ … }` block groups several of them in one handler. The block still needs its
own terminating `;` after the closing brace, like any other handler body.

### set

`set()` marks every surface that depends on the target dirty. Setting an
`exec_line` source overwrites its buffer optimistically and suppresses the next
re-poll, which is the pattern for a control that both fires a command and shows
the result before the command finishes:

```wisp
source prof = exec_line("powerprofilesctl get", every="10s", refresh_ms=200);

surface bar {
	anchor = top | left | right; height = 28; exclusive_zone = 28;
	bg = #ff0e131c;
	widget pp {
		align = right;
		text = prof;
		fg = #ffdbe2ee;
		on_click() = {
			set(prof, "performance");
			exec("powerprofilesctl set performance");
		};
	}
}
```

The label flips immediately, the shell command catches up, and 200 ms later the
real value replaces the optimistic one.

### animate

```wisp
animate(pulse, 1, 200ms, ease_out);
animate(pulse, 1, 200ms, ease_out, repeat = 4, alternate);
animate(pulse, 1, 300ms, cubic_bezier(0.16, 1, 0.3, 1));
```

`repeat` is the **total** number of runs, default 1. With `alternate`, an odd
count ends at `to` and an even count back at `from`. Easings are `linear`,
`ease_in`, `ease_out`, `ease_in_out` or `cubic_bezier(a,b,c,d)`. Anything else
is an error at **codegen**, not at `--check`, and hyphenated `ease-out` lexes as
a subtraction and is rejected.

A click that animates a widget property:

```wisp
mut glow = 0.0;

surface bar {
	anchor = top | left | right; height = 28; exclusive_zone = 28;
	bg = #ff0e131c;
	widget pulse {
		align = right;
		text = "ping";
		fg = #ffdbe2ee;
		x_offset = glow * 6;
		on_click() = {
			animate(glow, 1, 160ms, ease_out, repeat = 2, alternate);
			exec("wispctl notify 1 pinged");
		};
	}
}
```

### emit

`emit()` posts to a spawned template. In practice that means the OSD:

```wisp
on_click() = emit(osd, summary = "hello", body = "from a click", icon = 0xf0eb);
```

Only `spawn_osd` is generated. Emitting to a `pill` or `menu` template compiles
and then fails at **link** time.

## Handlers

Written inside a `widget` or a `cell`, each takes zero, one or two identifier
parameters and exactly one statement.

| written | real behaviour |
|---|---|
| `on_click(p)` | left button, and any button that is not right or middle |
| `on_right_click(p)` | right button |
| `on_middle_click(p)` | middle button |
| `on_change()` | sliders only, appended to the slider's set thunk |

Any one handler alone makes a widget hit-testable. Max 64 hit entries per
widget.

**Parameters** bind to the 1-based cell index only inside a `for` cell. On a
plain widget the parameter is bound to a NULL pointer and interpolating it gives
garbage. On a slider, `on_change(p)` fails at `--emit` with "unresolved
identifier"; write `on_change()` and read the bound `mut`, which the thunk
writes as a 0..1 double before the body runs.

```wisp
for tag in tags.list {
	cell.ws {
		text = tag.label;
		on_click() = exec("wispctl tag {tag.index} {tag.output}");
	}
}
```

```wisp
widget volume {
	slider;
	value = vol;
	on_change() = exec("wispctl volume set {vol}");
}
```

## Tray click set

The three click handlers together are what a working tray cell needs:

```wisp
for it in tray_s.items {
	cell.tray {
		icon = it.icon;
		on_click()        = exec("wispctl tray activate {it.index}");
		on_right_click()  = exec("wispctl tray menu {it.index}");
		on_middle_click() = exec("wispctl tray secondary {it.index}");
	}
}
```

## Gotchas

- `const` plus `mut` over 64 is an error: "too many consts/muts (max 64)".
- A handler parameter on a non-`for` widget is NULL.
- `on_change(p)` on a slider fails `--emit`; use `on_change()`.
- A bad easing in `animate()` is a codegen error, so `--check` alone will not catch it. Easing *properties* are checked by `--check`.
- `animate()` on a string `mut` is an error.
- A slider with `slider;` but no `value = …` fails `--emit`.
- `emit()` to anything other than the `osd` template is a codegen error.
- A `{ … }` handler body without a trailing `;` is a parse error pointing at the next closing brace.

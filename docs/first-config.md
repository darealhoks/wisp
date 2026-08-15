# First config

Write a file, compile it, run it. Three commands.

```sh
mkdir -p ~/.config/wisp
$EDITOR ~/.config/wisp/mine.wisp
wispctl rebuild mine
```

## A bar on screen

Put this in `~/.config/wisp/mine.wisp`. It is the smallest thing that shows up.

```wisp
source time = clock("%H:%M");

surface bar {
	layer = top;
	anchor = top | left | right;
	height = 24;
	exclusive_zone = 24;
	bg = #cc000000;

	widget clock {
		align = right;
		text = time;
		fg = #ffeeeeee;
		pad = 16;
	}
}
```

Four things are happening.

- `source time = clock("%H:%M")` declares a value that changes over time. `clock` with no seconds in its format arms a per-minute timer, not a 1 Hz one.
- `surface bar { … }` is a layer-shell surface. Naming it `bar` is the one special name for a panel: the tag accumulator and the lock-on-output path look for it.
- `exclusive_zone = 24` reserves that strip so windows do not sit under the bar. It defaults to `height`, so you can leave it out; `0` overlays without reserving.
- `widget clock` reads `time`. That read is what puts the surface in the source's dependency set, so a minute tick repaints this widget and nothing else.

Build and run it:

```sh
wispctl rebuild mine
```

That resolves the name to `~/.config/wisp/mine.wisp`, runs `make install` in the
share dir with `WISP=` pointed at it, remembers the selection, and reloads a
running daemon. If none is running, start one with `wisp`.

When one file gets long, move it to `~/.config/wisp/mine/mine.wisp` and
`include` the rest of the pieces from beside it — `wispctl rebuild mine` still
finds it, and the pieces are not configs of their own. The full rule is
[[install#what-counts-as-a-config]].

## The edit loop

```sh
wispc --check ~/.config/wisp/mine.wisp    # fast syntax and sema pass
wispctl rebuild mine                       # compile + install + reload
```

Once a config is selected, `wispctl reload` alone re-execs the daemon in place
with the binary you already built; `wispctl rebuild <name>` is what you want
after editing the `.wisp`. There is no live reload of properties, the daemon is
recompiled every time.

For a tighter loop, `wispc` can watch the file itself:

```sh
wispc --watch --reload ~/.config/wisp/mine.wisp
```

That runs `make WISP=<file>` and then `wispctl reload` on every write, until you
interrupt it.

## Add something that reacts

```wisp
source time  = clock("%H:%M");
source bat_s = bat("BAT0");
source tags  = tags();

const TEXT = #ffdbe2ee;
const RED  = #ffe0603f;

surface bar {
	anchor = top | left | right;
	height = 28;
	exclusive_zone = 28;
	bg = #ff0e131c;

	for tag in tags.list {
		cell.ws {
			text = tag.label;
			visible = tag.occupied || tag.active;
			on_click() = exec("wispctl tag {tag.index} {tag.output}");
		}
	}

	widget bat {
		align = right;
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

.ws        { fg = TEXT; bg = #ff0e131c; width = 28; height = 28; radius = 8; }
.ws:active { fg = #ff64799c; width = 34; }
.ws:urgent { bg = RED; }
```

New pieces: a `for` over `tags.list` (unrolled to 9 cells at compile time), a
class `.ws` declared as a name suffix and styled by a rule at the bottom,
pseudo-class rules for the active and urgent states, a click handler that shells
out, and string interpolation `"{bat_s.pct}%"`.

Always pass `{tag.output}` to `wispctl tag`. Without it a click switches the
workspace on the keyboard-focused monitor instead of the one you clicked.

## Where to go next

- Every property of every surface kind: [[modules]].
- All 22 sources: [[builtins]].
- Copy-paste starting points for an OSD, a menu, a HUD, a lock: [[templates]].
- Things that compile and do nothing: [[gotchas]].

## Gotchas

- `wispc --check` passing is not proof it builds. Run `wispc --emit /tmp/out file.wisp` too.
- Handler parameters only mean something inside a `for` cell; on a plain widget the parameter is bound to NULL.
- A colour property that cannot be constant-folded is a hard build error, by design, so alpha-0 invisibility cannot happen silently.
- Equal-specificity style rules touching the same property of the same node are a compile error, not a later-wins.

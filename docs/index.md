# wisp

wisp is one Wayland process that draws a whole desktop shell: bar, hover HUD,
notifications, app menu, lock screen, gamma, wallpaper, media keys. You declare
all of it in a `.wisp` file, and the bundled compiler `wispc` lowers that file
to C and links it into the daemon. A config that never mentions notifications
produces a binary with no D-Bus client in it. It links libc and libm and
nothing else, speaks Wayland and D-Bus as raw wire, and burns zero CPU ticks
when nothing on screen is moving.

## Start here

| page | what it covers |
|---|---|
| [[install]] | install.sh, `make install`, requirements, verifying the build |
| [[first-config]] | empty file to a bar on screen, then the edit loop |
| [[templates]] | copy-paste skeletons for every surface kind |

## The language

| page | what it covers |
|---|---|
| [[syntax]] | tokens, comments, `//!` directives, strings, expressions |
| [[types]] | value types, coercions, enums, property types, units |
| [[builtins]] | all 22 sources, their fields and arguments |
| [[state]] | `const`/`mut`, `set`/`emit`/`animate`, handlers, `on_change` |

## Reference

| page | what it covers |
|---|---|
| [[modules]] | every surface kind: bar, HUD, OSD, menu, compound, blocks |
| [[wispctl]] | the control client, command by command |
| [[gotchas]] | everything that compiles and does not do what you meant |

## Shape of a config

One file, top-level declarations in any order. Sources feed expressions,
expressions feed widget properties, and a change to a source repaints exactly
the widgets that read it.

```wisp
source bat_s = bat("BAT0");
const TEXT = #ffdbe2ee;
const RED  = #ffe0603f;

surface bar {
	anchor = top | left | right;
	height = 34;
	exclusive_zone = 34;
	bg = #ff0e131c;
	widget bat {
		align = right;
		icon = bat_s.charging ? 0xf0084 : 0xf241;
		text = "{bat_s.pct}%";
		fg = bat_s.pct < 15 ? RED : TEXT;
	}
}
```

The tree ships two example configs, `configs/riverie.wisp` and
`configs/anemoia.wisp`. Both are full desktops and both are worth reading.

## Requirements in one line

A wlroots-shaped compositor with `wlr-layer-shell-unstable-v1`; workspaces come
from mango IPC, hyprland IPC, river-status or `ext-workspace-v1`. See
[[install#compositors]].

## Gotchas

- There is no live config reload: every edit means a rebuild, `wispctl reload` re-execs the daemon.
- Conditionals are the ternary and strings join with `"{interpolation}"`; `for … in` is the only repetition.
- A property that passes `wispc --check` is not necessarily built: run `--emit` too, see [[gotchas]].

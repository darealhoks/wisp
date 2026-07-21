# wisp

**W**idget **I**nterface, **S**ingle **P**rocess — one Wayland daemon that draws
a whole desktop shell. All it needs is `wlr-layer-shell-unstable-v1`, plus
`ext-workspace-v1` if you want workspace tags (sway, niri, hyprland, labwc,
cosmic, kwin, patched dwl). Developed on [mangoWM](https://github.com/DreamMaoMao/mango),
which is served by a small IPC fallback instead.

Bar, hover panels, notification slabs, app menu, session lock, night-mode gamma
and wallpaper are not separate features. They are **surfaces**: the same layer
of Wayland surface, configured differently. What you declare is what exists.

Everything visible or interactive is declared in a `.wisp` file, which the
bundled compiler `wispc` lowers to C and links into the daemon. There is no
runtime config file and no plugin loading. Writing `tags()` links the
workspace client; declaring an osd surface links the D-Bus client; a config that mentions
neither produces a binary containing neither.

The process links **libc and libm**. Wayland and D-Bus are spoken as raw wire,
so there is no `libwayland`, `libxkbcommon`, `libdbus`, `cairo`, `pango`,
`pixman` or `glib`. The default font backend bakes a TTF into const tables at
build time, leaving the daemon with no font dependency at all.

Idle costs zero ticks per second, by construction rather than by tuning:
nothing polls, animations run only while a tween is active, the bar skips
redraws by hashing its contents, and hidden surfaces release their SHM pools.
The config this repo runs (`configs/bee.wisp`, baked backend) sits at about
2.9 MB RSS idle with a 167 KB stripped binary. Your numbers depend on what you declared and which font
backend you picked.

## Docs

- [install.md](docs/install.md) — installing, build knobs, font backends
- [tutorial.md](docs/tutorial.md) — one bar from an empty file to workspace clicks
- [dsl.md](docs/dsl.md) — the language, complete
- [wispctl.md](docs/wispctl.md) — the control client, and what `reload` really does

## Quick start

```sh
curl -fsSL https://raw.githubusercontent.com/darealhoks/wisp/main/install.sh | sh
wispctl rebuild bee  # compile an example config, install, run
wisp                 # or: autostart = wisp

# from a checkout instead:
make install         # → ~/.local/bin (override with PREFIX=)
```

Then drive it: `wispctl apps`, `wispctl volume up`, `wispctl notify 1 hi`.

`configs/bee.wisp` is the config this desktop actually runs, and the best
reference once the docs run out.

---

This tool was written with the assistance of AI (Claude Opus and Fable).

# wisp <img src="wisp.png" width="28" height="28" align="top" alt="">

**W**idget **I**nterface, **S**ingle **P**rocess — one Wayland daemon that draws
a whole desktop shell. All it needs is `wlr-layer-shell-unstable-v1`, plus
`ext-workspace-v1` if you want workspace tags (sway, niri, labwc, cosmic,
kwin, patched dwl — see the support table below). Developed on [mangoWM](https://github.com/DreamMaoMao/mango),
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

## Compositor support

wisp needs `wlr-layer-shell` to run; each other feature lights up only if the
compositor speaks its protocol, else stays dark while the rest works.
Workspaces read from `ext-workspace-v1`, mango's IPC, hyprland's IPC, or
river's status protocol; compositors with just their own tag IPC (wayfire)
show no tags.

| Compositor | Bar (layer-shell) | Workspaces | Gamma | Toplevels | Lock | Fractional scale |
|---|---|---|---|---|---|---|
| **mango** (home) | ✓ | ✓ IPC + ext-ws | ✓ | ✓ | ✓ | ✓ |
| **sway** ≥1.12 | ✓ | ✓ ext-ws | ✓ | ✓ | ✓ | ✓ |
| **niri** ≥25.08 | ✓ | ✓ ext-ws | ✓ | ✓ | ✓ | ✓ |
| **labwc** ≥0.8.3 | ✓ | ✓ ext-ws | ⚠ flaky¹ | ✓ | ✓ | ✓ |
| **hyprland** | ✓ | ✓ own IPC | ✓ | ✓ | ✓ | ✓ |
| **wayfire** | ✓ | ✗ own IPC only² | ✓ | ✓ | ✓ | ✓ |
| **river** | ✓ | ✓ river-status | ✓ | ✓ | ✓ | ✓ |
| **dwl** | ✓ | ✗ patch³ | ✓ | ✗ patch³ | ✓ | ✓ |
| **COSMIC** | ✓ | ✓ ext-ws | ✗⁴ | ? | ✓ | ✓ |
| **KWin** (Plasma ≥6.6) | ✓ | ✓ ext-ws | ✗⁵ | ✗⁵ | ✓ | ✓ |
| **GNOME** (Mutter) | ✗ — unsupported⁶ | — | — | — | — | — |

¹ works but flaky on multi-output; test on your hardware.
² tags exist but only over its own IPC, which wisp doesn't speak.
³ apply `ext-workspace` / `foreign-toplevel` from [dwl-patches](https://codeberg.org/dwl/dwl-patches).
⁴ open feature request, unimplemented.
⁵ KWin declines these for its own KDE protocols.
⁶ Mutter refuses `wlr-layer-shell` by policy.

Checked against newest releases, July 2026; support moves, so check your version.

---

This tool was written with the assistance of AI (Claude Opus and Fable).

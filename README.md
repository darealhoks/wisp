# wisp <img src="wisp.png" width="32" height="32" align="absmiddle" alt="">

> [!WARNING]
> Due to the extremely minimal nature of this project, bugs can still occur. I have been daily driving wisp for a few months now so most issues should be gone but its still experimental software. 

**W**idget **I**nterface, **S**ingle **P**rocess - one Wayland daemon that draws a whole desktop shell. All it needs is `wlr-layer-shell-unstable-v1`.

Bar, hover panels, notification slabs, app menu, session lock, night-mode gamma and wallpaper are not separate features. They are **surfaces**: the same layer of Wayland surface, configured differently.

Everything is declared in a `.wisp` file, which the bundled compiler `wispc` lowers to C and links into the daemon. Writing `tags()` links the workspace client; declaring an osd surface links the D-Bus client; a config that mentions neither produces a binary containing neither.

The process links **libc and libm**. Wayland and D-Bus are spoken as raw wire, so there is no other dependency. If you wanna use the lock, `libpam` and its headers are needed (i didn't rewrite that from scratch for security reasons) but the main daemon never links it - its only used by the `wisp-lock` binary.

Idle costs nothing, my config `configs/bee.wisp` idle consumes:
**CPU:** 1 cpu tick per 10 seconds (measured on a i5-1135G7, without the cpu/temp/mem polls in the config this number is a plain 0)
**RAM:** 3.1 MB RSS (PSS is only 950KB, measured on 1080p)
**DISK:** 250 KB stripped binary
Your numbers depend on what you declared. 

## Docs

- [install.md](docs/install.md) - installing, build knobs
- [tutorial.md](docs/tutorial.md) - one bar from an empty file
- [dsl.md](docs/dsl.md) - the wisp language, complete
- [wispctl.md](docs/wispctl.md) - the control client

## Quick start

```sh
curl -fsSL https://raw.githubusercontent.com/darealhoks/wisp/main/install.sh | sh
wispctl rebuild bee  # compile an example config, install, run
wisp                 # or: autostart = wisp

# from a checkout instead:
make install         # → ~/.local/bin (override with PREFIX=)
```

Then drive it: `wispctl apps`, `wispctl volume up`, `wispctl notify 1 hi`.

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

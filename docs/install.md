# Install

```sh
curl -fsSL https://raw.githubusercontent.com/darealhoks/wisp/main/install.sh | sh
wispctl rebuild reverie   # compile an example config, install it, reload
wisp                      # or add `wisp` to your compositor autostart
```

`install.sh` installs only the two tools plus the runtime *sources*. The daemon
itself is compiled on your machine, per config, by `wispctl rebuild`, because
every translation unit includes the `features.h` that your `.wisp` generated.

## What install.sh does

1. Checks for `cc`, `make`, `git`, `pkg-config`, and PAM headers (`security/pam_appl.h`).
2. Clones the repo shallowly into a temp dir.
3. Runs `make install-tools install-share PREFIX=$PREFIX`.
4. Creates `${XDG_CONFIG_HOME:-~/.config}/wisp/`.

| path | contents |
|---|---|
| `$PREFIX/bin/wispc`, `$PREFIX/bin/wispctl` | the compiler and the control client |
| `$PREFIX/share/wisp/` | `Makefile`, `src/`, `configs/`, `docs/` |
| `~/.config/wisp/*.wisp` | your configs |

`PREFIX` defaults to `~/.local`. Re-running the script upgrades in place;
`wispctl update` does the same thing and then rebuilds your current config.

## From a checkout

```sh
make install          # builds the selected config + installs all five binaries
make check            # builds every configs/*.wisp, the validation gate
make WISP=configs/anemoia.wisp install
```

Use `make install`, not `make`. `wispctl reload` re-execs through
`/proc/self/exe`, and a plain `make` leaves the running daemon pointing at an
unlinked inode.

| target | result |
|---|---|
| `install` | `wisp`, `wispctl`, `wispc`, `wisp-lock`, `wisp-lock-helper` into `$PREFIX/bin` |
| `install-tools` | `wispc` + `wispctl` only |
| `install-share` | sources, configs and docs into `$PREFIX/share/wisp` |
| `check` | build matrix over every `configs/*.wisp` |
| `clean` | removes `build/` |
| `uninstall` | removes the binaries and the share dir |

Each config caches into its own `build/<name>/`, and `make install` warms every
config it can find (repo `configs/*.wisp` plus `~/.config/wisp/*.wisp`), so a
later `wispctl rebuild <other>` is a cache hit rather than a compile.

## Build knobs

Set them on the make command line, or as `//!` directive comments inside the
`.wisp` itself (see [[syntax#build-directives]]). Priority: command line, then
`//!`, then the sticky selection in `build/.selected`, then the default.

| knob | default | meaning |
|---|---|---|
| `WISP` | `configs/reverie.wisp` | which config to build |
| `FONT_BACKEND` | `truetype` | `truetype` (TTF/OTF rasterized in-process) or `bitmap` (PSF/BDF baked to const tables) |
| `FONT` | `~/.local/share/fonts/MapleMono-NF-Bold.ttf` | the font to bake sizes from |
| `FONT_FALLBACK` | empty | second font in the chain, truetype only; a CBDT emoji font renders in colour |
| `FRACTIONAL` | `0` | fractional scale support, requires `FONT_BACKEND=truetype` |
| `PREFIX` | `~/.local` | install prefix |
| `LINE_MAP` | `1` | `0` drops `#line` mapping back to the `.wisp` |

The selection is sticky: `make WISP=configs/anemoia.wisp && make install`
installs anemoia, it does not silently revert to the default.

## Requirements

- A C compiler, `make`, `git`, `pkg-config`.
- `libpam` and its headers, for `wisp-lock-helper` only. The daemon never links PAM.
- A compositor with `wlr-layer-shell-unstable-v1`. Without it wisp cannot map a single surface.
- A Nerd Font if you use icon codepoints; `wl-clipboard` if you want the emoji menu to copy.

Everything else is optional and degrades to "that feature is dark": no
`ext-workspace-v1` means an empty tag row, no `zwlr_gamma_control_v1` means no
gamma, and so on.

## Compositors

| compositor | bar | workspaces | gamma | toplevels | lock |
|---|---|---|---|---|---|
| mango | yes | own IPC + ext-ws | yes | yes | yes |
| sway 1.12+ | yes | ext-ws | yes | yes | yes |
| niri 25.08+ | yes | ext-ws | yes | yes | yes |
| hyprland | yes | own IPC | yes | yes | yes |
| river | yes | river-status | yes | yes | yes |
| labwc 0.8.3+ | yes | ext-ws | flaky on multi-output | yes | yes |
| wayfire | yes | no, own IPC only | yes | yes | yes |
| dwl | yes | needs ext-workspace patch | yes | needs patch | yes |
| COSMIC | yes | ext-ws | no | unknown | yes |
| KWin (Plasma 6.6+) | yes | ext-ws | no | no | yes |
| GNOME | no, Mutter refuses layer-shell | - | - | - | - |

Workspace backend selection and what differs per compositor is in
[[modules#workspaces-per-compositor]].

## Verify

```sh
wispc --check ~/.config/wisp/mine.wisp   # prints "ok"
wispc --emit /tmp/out ~/.config/wisp/mine.wisp
wispctl ping                             # prints "pong" if the daemon is up
```

Run both `--check` and `--emit`. A config that passes `--check` can still fail
codegen; the full list of check-passes-emit-fails cases is in [[gotchas]].

## Gotchas

- `make` alone is not enough after an edit, use `make install`, because `wispctl reload` re-execs the installed binary.
- `FRACTIONAL=1` with `FONT_BACKEND=bitmap` is a hard Makefile error; bitmap fonts can only pixel-double.
- `FONT_BACKEND=baked` and `=freetype` were retired and now error out.
- Only two configs ship: `reverie` and `anemoia`.
- `wispctl rebuild` needs the share dir (or `$WISP_SRC`) present, it shells out to `make -C` there.

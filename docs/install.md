# Installing wisp

## Requirements

To build, you need a C compiler, `make`, and FreeType with its `.pc` file
(`freetype2`). FreeType is used by the `bake` tool that turns a font into
`src/bake.h`, so it is required even when the daemon will not link against it.
The lock helper links PAM, so `libpam` and its headers are needed too.

At runtime wisp links only libc and libm. The one exception is the `freetype`
font backend, which `dlopen`s `libfreetype.so.6` when the daemon starts.

You also need a Wayland compositor that implements `wlr-layer-shell-unstable-v1`.
wisp is currently only developed against mangoWM.

## The quick way

    curl -fsSL https://raw.githubusercontent.com/darealhoks/wisp/main/install.sh | sh

The script checks dependencies, installs `wispc` + `wispctl` into
`$PREFIX/bin` (default `~/.local/bin`) and the runtime sources into
`$PREFIX/share/wisp`, and tells you where the example configs and docs live.
It does not compile the daemon — that happens on your machine, from your
config:

    wispctl rebuild bee          # an example from share/wisp/configs
    wispctl rebuild mybar        # ~/.config/wisp/mybar.wisp
    wispctl rebuild              # whatever you rebuilt last

`rebuild` compiles the named `.wisp`, installs, and reloads the running daemon
in place. It remembers the config in `~/.config/wisp/current`. Re-running the
install script updates the tools and sources.

## Building from a checkout

    make install

compiles the selected config and copies five binaries into `$PREFIX/bin`:

    wisp               the daemon
    wispctl            the control client
    wispc              the .wisp compiler
    wisp-lock          the session locker
    wisp-lock-helper   setuid-free PAM authentication for wisp-lock

`wispctl rebuild` works from a checkout too: point `$WISP_SRC` at it and the
share dir is ignored.

Use `make install` rather than `make`. `wispctl reload` re-executes the
*installed* binary, so a `build/`-only binary is not what a reload picks up.

Set `PREFIX` to install elsewhere:

    make install PREFIX=/usr/local

## Build knobs

Build settings live in the `.wisp` itself, as `//!` directive comments at the
top of the file (plain comments to the compiler; the Makefile reads them):

    //! font_backend = freetype
    //! font = ~/.local/share/fonts/MapleMono-NF-Bold.ttf
    //! font_fallback = /usr/share/fonts/noto-emoji/NotoColorEmoji.ttf
    //! fractional = 1

All are optional. A variable given on the make command line beats a directive:

    make FONT_BACKEND=freetype FONT=/usr/share/fonts/foo.ttf

With neither, the build falls back to the last selection recorded in
`build/.build-tag`, and then to `configs/bee.wisp` with the `baked` backend.
Changing any knob wipes `build/` before compiling, because objects from the
previous selection are not compatible.

## Configs

`WISP` selects the `.wisp` file that gets compiled into the daemon. There is no
runtime config file. Two are real presets:

    configs/bee.wisp      the Makefile default, and the config this rice actually runs
    configs/dwlarp.wisp   a second full preset (dwl-shaped bar)

Your own configs go in `~/.config/wisp/`; `wispctl rebuild <name>` finds them
there by name (and falls back to the examples in `share/wisp/configs`).

To change your config, edit the `.wisp` and rebuild:

    wispctl rebuild              # or, from a checkout: make install && wispctl reload

See [dsl.md](dsl.md) for the language and [wispctl.md](wispctl.md) for what
`reload` does and does not do.

## Font backends

One backend is compiled in per build.

`baked` is the default and the leanest. FreeType rasterizes the TTF at build
time into const tables in `src/bake.h`. The font file is not needed at runtime,
and the daemon has no font dependency at all.

`bitmap` bakes a PSF or BDF into the same tables. Pixel-exact, no
anti-aliasing, and most icon glyphs will be missing. Gzipped console fonts need
decompressing first (`gunzip -k`).

`freetype` `dlopen`s `libfreetype.so.6` and rasterizes on demand. This is the
only backend where the font can change without a rebuild:

    WISP_FONT=/path/to/other.ttf wisp
    WISP_FONT_FALLBACK=/path/to/NotoColorEmoji.ttf wisp

A fallback font covers glyphs the primary font lacks. A CBDT or sbix color font
renders in color, downscaled to text size. An outline font renders monochrome.
SVG-only color fonts do not render, because FreeType needs an external SVG
library for those.

Baked font sizes come from the `.wisp`: every `font_size = N;` it declares, plus
14 and 22. Changing a size in the config regenerates `src/bake.h` on the next
build.

## Rebuilding

`make install` is incremental. `wispc` is recompiled only when its own sources
change, and the `.wisp` is recompiled only when it or `wispc` is newer than the
generated files under `build/gen-tw/`.

    make clean       remove build/
    make distclean   also remove the generated src/bake.h
    make uninstall   remove the binaries from $PREFIX/bin and $PREFIX/share/wisp

To check that every config still builds:

    make check

Run it alone. Concurrent `make` invocations trample `build/`, since each config
needs the directory wiped.

## Running it

Start `wisp` from your compositor's autostart. In mango's `config.conf`:

    autostart = wisp

There are no options to pass. The config is already inside the binary. The one
flag it accepts, `--reload-fds`, is passed by `wispctl reload` when it re-execs
the daemon and is not meant to be typed by hand.

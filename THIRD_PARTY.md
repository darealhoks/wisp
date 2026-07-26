# Third-party notices

wisp itself is MIT (see `LICENSE`). It links only libc and libm, but three
third-party works end up compiled into the shipped binaries or checked into
this tree, and their notices travel with it.

## stb_image, stb_truetype (Sean Barrett) — public domain

`src/image.c` and `src/wall.c` compile `stb/stb_image.h` in, so every `wisp`
and `wisp-lock` binary contains it. `src/tt/raster.c` is additionally informed
by stb_truetype's rasterizer — no stb_truetype code is compiled in, but if any
of it survives in ours, this notice covers it.

Both are released under a dual public-domain-or-MIT choice; **wisp takes the
public-domain (Unlicense) option**, which carries no attribution requirement.
This section is courtesy, not obligation.

## libschrift (Thomas Oltmann) — ISC

`src/tt/raster.c` adapts code from libschrift's signed-area rasterizer. This is
the one notice here that is a hard requirement rather than courtesy.

    Copyright (c) 2019-2022 Thomas Oltmann and contributors

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

## gemoji (GitHub, Inc.) — MIT

`src/emoji_data.h` carries emoji names, aliases and tags derived from gemoji's
`db/emoji.json`.

    Copyright (c) GitHub, Inc.

    Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.

## Not third-party, listed to close the question

- **Wayland protocol opcodes** (`src/proto.h`) — hand-extracted constants from
  the upstream protocol XML (MIT/HPND). Numbers, not expression.
- **X11 keysym names** (`src/xkb.c`) — the keysymdef name→codepoint table.
- **Linux-PAM** — dynamically linked by `wisp-lock` only, never bundled.
- **FreeType** — `make ttmetrics` dlopens it for metric diffing. Debug tool,
  never built by `make install`, never shipped.
- **Fonts** — configs reference font files by path; none are vendored here.
- **TrueType** is an Apple trademark; wisp's `truetype` backend merely reads
  the format, which is descriptive use.

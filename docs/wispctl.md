# wispctl

`wispctl` is the control client. It joins its arguments with tabs, sends one
line to the daemon over a unix socket, prints the reply, and exits with a status
derived from that reply.

The socket is `$XDG_RUNTIME_DIR/wisp.sock`. Without `XDG_RUNTIME_DIR` set,
`wispctl` cannot find the daemon and fails immediately.

    $ wispctl ping
    pong

`wispctl help` prints the full command list. Two commands never touch the
socket: `help` is handled in the client, and `lock` execs `wisp-lock` directly
so the session can still be locked when the daemon is down.

## Exit status

Most commands exit 0 when the daemon replies `ok` or `pong`, and 1 otherwise.

Three commands invert that into a state probe, so you can branch on them in a
shell without parsing output:

    hide status        0 when surfaces are hidden
    dnd status         0 when do not disturb is on
    gamma is-warm      0 when the screen is being warmed

`menu` exits 1 when the user cancelled.

## Availability

A command only exists if the module behind it is declared in the `.wisp` the
running daemon was compiled from. There is no runtime plugin loading. Ask a
daemon built without a gamma block for `gamma night` and it replies `err`, and
`wispctl` exits 1.

`wispctl help` lists everything the client knows about, not what your build
supports. The two are the same only for a config that declares every module.

## reload

    make install && wispctl reload

`reload` re-executes the *installed* `wisp` binary in place. It does not
compile anything. Running it after editing a `.wisp` without rebuilding just
restarts the daemon you already had.

The exec goes through `PATH`, not `/proc/self/exe`. `install -m 755` unlinks
the destination before creating the new file, so the running daemon's
`/proc/self/exe` points at the old deleted inode. A PATH lookup picks up the
binary you just installed.

The control socket is passed across the exec, so clients stay connected. The
Wayland connection is not: the compositor still holds the previous
`wl_registry`, so the new process opens a fresh session and surfaces flash for
one frame. That flash is expected.

`quit` stops the daemon without re-execing.

## Command groups

`wispctl help` is the reference. In outline:

**daemon** — `ping`, `reload`, `quit`, `hide`, `tag <n> [output]`.

**bar** — `bar refresh` forces a redraw. `bar tags <occupied> <active> <urgent>`
sets the workspace bitmasks as hex, for driving the tag display from outside.

**menu** — `menu <title> <item>...` shows a picker and prints
`<index>\t<text>`. `apps`, `powermenu` and `emoji` are prebuilt menus; `emoji`
copies the pick with `wl-copy`. `menu-cancel` closes whatever is open.

**osd / notifications** — `osd <slot> <summary> [progress] [icon-cp] [muted]`
draws a slab; reusing a slot replaces the previous one, and a progress of -1
omits the bar. `notify <urgency> <summary> [body] [icon-cp] [timeout-ms]` takes
urgency 0, 1 or 2, with timeout -1 for the default and 0 for sticky. Icons are
hex codepoints. `osd-clear` dismisses everything. `dnd` toggles do not disturb.

**media** — `volume up|down|mute`, `mic mute`, `backlight up|down`. These show
the matching OSD as a side effect, so a keybinding needs only the one command.

**gamma** — `gamma auto|day|night|flat|off` sets the mode, `gamma state` prints
it.

## Binding it

There is nothing wisp-specific about calling `wispctl` from a compositor
keybinding. In mango's `config.conf`:

    bind=SUPER,d,spawn,wispctl apps
    bind=NONE,XF86AudioRaiseVolume,spawn,wispctl volume up

The daemon does not grab keys itself. Every interaction that starts from the
keyboard starts with your compositor spawning `wispctl`.

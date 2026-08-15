# wispctl

The control client. It joins `argv[1..]` with tabs, sends one line over
`$XDG_RUNTIME_DIR/wisp.sock`, and prints the one-line reply.

```sh
wispctl ping
wispctl apps
wispctl notify 1 "hello" "body text"
wispctl rebuild reverie
```

Four subcommands never touch the socket: `help`, `rebuild`, `update` and `lock`.

## Commands

| command | arguments | feature gate | reply |
|---|---|---|---|
| `ping` | - | always | `pong` |
| `quit` | - | always | `ok`, then exits |
| `reload` | - | always | `ok`, then re-execs in place |
| `tag` | `<n> [output-slot]`, n is 1-based | always | `ok` |
| `hide` | `on\|off\|toggle\|status` | always | `ok`, or `on`/`off` |
| `bar title` | `<string>` | bar | `ok` |
| `bar tags` | `<occ> <act> <urg>`, hex masks | bar | `ok` |
| `bar refresh` | - | bar | `ok` |
| `menu` | `<name>` | menu | deferred, replies when the menu closes |
| `menu` | `<title> <item>…` | menu | `<index>\t<text>`, or `-1\t` on cancel |
| `menu-cancel` | - | menu | `ok` |
| `apps` | - | menu | `ok` |
| `hud` | anything | hud | `ok`, kept as a no-op for compatibility |
| `hud-cancel` | - | menu | `ok` |
| `osd` | `<slot> <summary> [progress] [icon-hex] [muted]` | osd | `ok` |
| `notify` | `[-t] <urgency> <summary> [body] [icon-hex] [timeout-ms]` | osd | `ok` — `-t` = transient, skipped by the notification center |
| `osd-clear` | - | osd | `ok` |
| `notif` | `open\|close\|toggle\|status\|clear\|dismiss <id>` | osd | `ok`, or `on`/`off` for `status` |
| `dnd` | `on\|off\|toggle\|status` | osd | `ok`, or `on`/`off` |
| `volume`, `mic`, `backlight` | see below | media | `ok` |
| `mpris` | `play-pause\|next\|prev` | mpris | `ok` |
| `tray` | `activate\|secondary\|menu <index>` | tray | `ok` |
| `tooltip` | `<x> <width> <below> <text>`, or `hide` | tooltip | `ok` |
| `gamma` | `auto\|day\|night\|flat\|off\|state\|is-warm` | gamma | `ok`, the mode, or `1`/`0` |
| `wall` | `<path.png>` | wallpaper | `ok` or an error |
| `dpms` | `on\|off` | idle | `ok` |

A feature gate is a compile-time thing. A command whose feature the config never
declared is indistinguishable from a typo:

```
err: unknown command: gamma (not in this build/preset?)
```

The socket also accepts a `lock` command, but the daemon is deliberately built
without that feature defined, so it never resolves. Use `wispctl lock`.

## Notification centre

`notif open|close|toggle` flips the flag a panel surface gates on
(`visible = <notifications-src>.open`), so the surface is created and destroyed
with it. `dismiss` takes the entry serial a cell hands back as `note.id`, never
a row index — the ring can shift while the click is in flight. See
[[modules#scrollable-panel]].

## Screen power

`wispctl dpms on|off` sets `zwlr_output_power_v1` on every connected output —
`wlopm --off "*"` without the process. It exists only when the config declares
an [[modules#idle]] block, since that is what it is there to serve:

```
timeout blank { after = 300s; run = "wispctl dpms off"; resume = "wispctl dpms on"; }
```

An output whose compositor refuses power control (or whose control another
client already holds) is skipped and logged once; the rest still switch.

## Client-side subcommands

**`help`**, `-h`, `--help`. With no arguments at all, usage goes to stderr with
exit 2; asked for explicitly, it goes to stdout with exit 0.

**`rebuild [config]`** takes a config name, and only two things are configs
(see [[install#what-counts-as-a-config]]): a `.wisp` sitting directly in
`$XDG_CONFIG_HOME/wisp` (default `~/.config/wisp`), and a directory holding a
`.wisp` named after it. It resolves the name in this order:

1. a literal path, if it exists
2. a name containing `/` — a directory path under the config dir, so
   `rebuild themes/night` builds `~/.config/wisp/themes/night/night.wisp`
3. a bare name — `<confdir>/<name>.wisp` first, then a recursive search for a
   *directory* called `<name>` holding `<name>.wisp`, so
   `~/.config/wisp/themes/night/night.wisp` also resolves for `rebuild night`.
   The shallowest match wins; dot-directories and symlinked directories are
   skipped and the walk stops at 8 levels. Two matches at the same depth is an
   error listing every one of them, not a silent pick.
4. `$WISP_SRC` or the install datadir, `configs/<name>.wisp`

Any other `.wisp` — one that sits in a directory not named after it — is an
include fragment and has no name `rebuild` will answer to.

Then it remembers the choice in `<confdir>/current`, runs
`make -s -C <src> install WISP=<path>`, sends a `wall` command with the new path
read out of the freshly generated overrides, sends `reload`, and finally warms
the other configs' caches in a detached process so the switch is not blocked on
them.

**`update`** pipes `install.sh` through `curl` or `wget` into `sh`, then re-execs
`wispctl rebuild` for the current config.

**`lock`** does `execvp("wisp-lock")`. It is not a socket command on purpose:
the locker is a separate binary that links PAM, and the daemon does not.

## Exit codes

| situation | code |
|---|---|
| reply is `ok` or `pong` | 0 |
| `menu` / `menu-cancel` | 0 if the reply index is 0 or more, 1 if negative |
| `dnd status`, `hide status`, `notif status` | 0 when the reply is `on` |
| `gamma is-warm` | 0 when the reply is `1` |
| any error reply, connect failure, or empty reply | 1 |
| no arguments (usage) | 2 |

That makes the state queries usable directly in a shell test:

```sh
wispctl gamma is-warm && wispctl gamma off
wispctl dnd status || wispctl notify 1 "not in dnd"
```

## Limits

The joined command line is capped at 16384 bytes, over which the client refuses
with "command too long".

## wispc CLI

The compiler is `wispc [MODE] FILE`. Modes are last-wins and default to
`--check`.

| flag | behaviour |
|---|---|
| `--check` | parse, style cascade and sema; prints `ok` |
| `--emit DIR` | writes the generated C: `features.h`, `objects.mk`, `gen_overrides.h`, `gen_menus.h`, `gen_sources.c`, `gen_bindings.c`, `gen_surfaces.c`, `gen_outputs.c`, `gen_spawn.c`, `gen_main.c` |
| `--dump-ast` | the AST after the style cascade, sema not run |
| `--features` | the `features.h` it would emit |
| `--deps` | `surface NAME: dep dep …`, one line per surface |
| `--font-sizes` | unique font sizes ascending, then the codepoints that must be baked |
| `--includes` | resolved path of every `include`d file, one per line (build staleness) |
| `--no-line-map` | drop `#line` mapping back to the `.wisp` |
| `--watch [--reload]` | rebuild on every write to the file's directory, optionally reloading |

Exit codes: 2 for usage, 1 for any diagnostic including codegen, 0 otherwise.
Diagnostics stop after 50 errors.

## Gotchas

- `wispctl lock` execs `wisp-lock`; the socket `lock` command exists in the dispatcher but is never compiled in.
- An unknown-command error and a feature that your config never declared look identical.
- `wispctl reload` re-execs the installed binary, so `make` without `install` reloads the old one. It is also deferred while a wallpaper fade is running.
- `wispctl rebuild` writes `<confdir>/current`, so a later bare `reload` follows that selection.
- Always pass the output slot to `wispctl tag` from a bar click, or you switch the focused monitor instead of the clicked one.
- `wispc --check` exits 0 on configs that `--emit` rejects, see [[gotchas]].

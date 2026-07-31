# wispctl

The control client. It joins `argv[1..]` with tabs, sends one line over
`$XDG_RUNTIME_DIR/wisp.sock`, and prints the one-line reply.

```sh
wispctl ping
wispctl apps
wispctl notify 1 "hello" "body text"
wispctl rebuild riverie
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
| `notify` | `<urgency> <summary> [body] [icon-hex] [timeout-ms]` | osd | `ok` |
| `osd-clear` | - | osd | `ok` |
| `dnd` | `on\|off\|toggle\|status` | osd | `ok`, or `on`/`off` |
| `volume`, `mic`, `backlight` | see below | media | `ok` |
| `mpris` | `play-pause\|next\|prev` | mpris | `ok` |
| `tray` | `activate\|secondary\|menu <index>` | tray | `ok` |
| `tooltip` | `<x> <width> <below> <text>`, or `hide` | tooltip | `ok` |
| `gamma` | `auto\|day\|night\|flat\|off\|state\|is-warm` | gamma | `ok`, the mode, or `1`/`0` |
| `wall` | `<path.png>` | wallpaper | `ok` or an error |

A feature gate is a compile-time thing. A command whose feature the config never
declared is indistinguishable from a typo:

```
err: unknown command: gamma (not in this build/preset?)
```

The socket also accepts a `lock` command, but the daemon is deliberately built
without that feature defined, so it never resolves. Use `wispctl lock`.

## Client-side subcommands

**`help`**, `-h`, `--help`. With no arguments at all, usage goes to stderr with
exit 2; asked for explicitly, it goes to stdout with exit 0.

**`rebuild [config]`** resolves the name in this order:

1. a literal path, if it exists
2. `$XDG_CONFIG_HOME/wisp/<name>.wisp`
3. `$XDG_CONFIG_HOME/wisp/<name>`
4. `$WISP_SRC` or the install datadir, `configs/<name>.wisp`

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
| `dnd status`, `hide status` | 0 when the reply is `on` |
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

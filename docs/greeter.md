# Greeter

Make wisp the login screen: greetd runs a compositor, the compositor runs a
wisp binary whose config declares one `spawned_by = greet` surface. Nothing
else about wisp changes — same DSL, same build, a different binary on VT 1.

You need `greetd` and a wlroots compositor (sway, river, labwc, mango…)
installed system-wide.

## 1. Write the config

`~/.config/wisp/greet/greet.wisp` — take [[templates#greet]] verbatim and edit
the colors and `user`. It is one surface, no other module, no bar. A directory
named after the config is one of the two shapes a config may take
([[install#what-counts-as-a-config]]), so the palette lives beside it as
`theme.wisp` and any further piece you split out does too.

```wisp
surface login {
	spawned_by = greet;
	user       = "you";
	sessions   = "/etc/greetd/environments";
	axis   = vertical;
	width  = 340;
	height = 140;
	bg = #ff0e131c;
	/* … rows: greet.user, greet.prompt + greet.dots, then a `sess` group
	   holding `for s in greet.sessions` as one horizontal band … */
}
```

Leave `user` empty to be asked for the username first; set it and the greeter
goes straight to the password. Property list: [[modules#greet]].

## 2. Build it

```sh
make -C ~/.local/share/wisp WISP=~/.config/wisp/greet/greet.wisp
doas install -m755 ~/.local/share/wisp/build/greet/wisp /usr/local/bin/wisp-greeter
```

`wispctl rebuild` is the wrong tool here: it would install this greeter as your
*daemon*. Plain `make` leaves the binary in `build/<config-name>/wisp`; copy
that where the `greetd` user can execute it.

## 3. Session list

`/etc/greetd/environments`, one command line per session, `#` and blanks
skipped, first eight kept, the first one selected at startup.

```
sway
dbus-run-session river
zsh
```

Whatever you write here is exec'd by greetd after PAM succeeds, so it must be
on `PATH` or an absolute path.

## 4. Compositor + greetd

greetd cannot talk layer-shell, so it launches a compositor which autostarts
the greeter. Two small files:

`/etc/greetd/greeter.sh`

```sh
#!/bin/sh
exec sway -c /etc/greetd/sway.conf
```

`/etc/greetd/sway.conf` — your normal keyboard layout and output scale, plus
`exec /usr/local/bin/wisp-greeter`. Get the scale right: an unscaled HiDPI
login form is unreadable.

`/etc/greetd/config.toml`

```toml
[terminal]
vt = 1

[default_session]
command = "/etc/greetd/greeter.sh"
user = "greetd"
```

Then enable the service (`rc-update add greetd default`, or
`systemctl enable greetd`) and reboot.

## 5. Preview without logging out

```sh
fakegreet 'sway -c /etc/greetd/sway.conf'
```

`fakegreet` fakes a greetd socket in your current session, so you can iterate
on the config in a window. Rebuild with step 2's `make` line between edits.

## Gotchas

- `$GREETD_SOCK` unset is fatal, not a degraded mode — a greet surface outside greetd is a config error, which is why previewing needs `fakegreet`.
- The compositor does not exit when the greeter does; end `wisp-greeter` with a compositor quit command in the same wrapper script, or VT 1 never comes back.
- `/etc/pam.d/greetd` must include `login` (or your distro's equivalent session stack) or logind/elogind never assigns the seat and the compositor cannot open the DRM device.
- greetd's own `HOME` is `/var/lib/greetd`: `~`-relative paths in the config (a wallpaper, a font) resolve there. Set `HOME` in the wrapper script if you point at files in your own home, and keep `XDG_CACHE_HOME` writable by the `greetd` user.
- `keyboard` defaults to `exclusive` on a greet surface only; do not override it.
- Sessions are read once at startup, so a `/etc/greetd/environments` edit needs a greeter restart, not a rebuild. The strip widens with each entry rather than growing the surface.
- Left/Right (and Tab) cycle the session, and a click on a session cell selects it with no `on_click` of its own.
- A wrong password leaves `greet.error` holding greetd's raw `unable to create session: pam_authenticate: AUTH_ERR`. Show your own string on `greet.failed` instead; PAM's human messages arrive as the prompt with `failed` still 0.

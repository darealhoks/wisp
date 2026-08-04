# Built-in sources

A `source` is the only thing in wisp that changes on its own. There are 23 of
them and you cannot add more; declaring one links its driver into the binary,
not declaring one leaves that code out entirely.

```wisp
source bat_s = bat("BAT0");
source time  = clock("%H:%M");
```

Using the bare name reads the **primary** field: `text = bat_s;` is
`text = bat_s.pct;`. Max 32 sources per config.

## All sources

| call | primary | other fields | cost |
|---|---|---|---|
| `clock(fmt)` | `value` | none | timerfd, per-minute unless `fmt` has seconds |
| `cpu([probe][, every=])` | `pct` | - | shared 1 s status tick |
| `mem([every=])` | `pct` | `used_mb` | shared tick |
| `temp([zone][, every=])` | `c` | none | shared tick, every 2nd |
| `bat([name])` | `pct` | `charging` | uevent plus 60 s fallback |
| `net([iface])` | `ssid` | `up` `signal` `rx_kbps` `tx_kbps` | rtnetlink; rates join the shared tick only if read |
| `backlight([name])` | `pct` | none | uevent only, no timer |
| `power_profile()` | `profile` | none | system bus |
| `bluez()` | `device` | `powered` `connected` `battery` | system bus |
| `disk([path])` | `pct` | - | own 30 s timer |
| `vpn([probe])` | `state` | `ok` | rtnetlink, no poll |
| `tags([labels=][, pinned=])` | `title` | `list` (for only), `occ` `act` `urg` (fail `--emit`) | workspace backend |
| `gamma_warm()` | `value` | none | in process |
| `dnd()` | `value` | none | in process |
| `ui_hidden()` | `value` | none | in process |
| `exec_line(cmd, …)` | `value` | none | timerfd plus pipe |
| `inotify(path=, …)` | `value` | none | own inotify fd |
| `dbus_signal(iface, member)` | `value` | `history` (for only) | session bus |
| `notifications([history=][, image=])` | `count` | `open`, `history` (for only) | in process, no fd |
| `mpris()` | `title` | `artist` `status` `player` `art` | session bus |
| `tray([icon_size=])` | `count` | `items` (for only) | session bus |
| `pipewire()` | `vol` | `mute` `mic_vol` `mic_mute` `ok` | native PipeWire protocol |
| `toplevel(app_id=)` | `exists` | `count` `title` | rides the wl_display fd, max 16 |

Zero-argument sources, where any argument is an error: `power_profile`, `dnd`,
`gamma_warm`, `ui_hidden`, `mpris`, `pipewire`, `bluez`.

## Time and system

**`clock(fmt)`** takes exactly one string literal, a strftime format. It has no
readable field, writable bare or as `clock.value`. A format
containing seconds arms a 1 Hz timer, otherwise the timer is an absolute
per-minute REALTIME wakeup.

**`cpu`**, **`mem`**, **`temp`** ride one shared status tick. `every=` is only
legal on these (and on `net` when its rate fields are read) and must be at least
250 ms; anything below is an `--emit` error.

```wisp
source cpu_s = cpu(every="2s");
source mem_s = mem(every="2s");
source temp_s = temp(every="2s");
```


**`bat([name])`** takes the battery name, `"BAT0"` by default behaviour of the
driver. It runs on uevents with a 60 s fallback, not a 1 Hz tick. `charging` is
a bool.

**`backlight([name])`** is pure uevent, zero timers.

**`disk([path])`** polls on its own 30 s timer. `disk.pct` is its only field.

## Network

**`net([iface])`** with an empty string means "whichever link is up". Reading
`rx_kbps` or `tx_kbps` sets `WISP_HAS_NET_RATES` and pulls the source into the
shared poll tick; reading only `ssid`/`up`/`signal` keeps it event-driven.
`signal` refreshes on a 10 s timer while the link is up.

```wisp
source wifi_s = net("");
widget wifi {
	icon = wifi_s.signal >= 3 ? 0xf0928 : 0xf091f;
	text = wifi_s.up ? wifi_s.ssid : "offline";
}
```

**`vpn([probe])`** reports `state` as `off`, `on` or `stale`, plus a bool `ok`.

## Desktop state

**`tags([labels=][, pinned=])`** is the workspace source. Only `title` lowers as
a scalar. `list` is iterable-only, `occ`/`act`/`urg` pass `--check` and fail
`--emit`. Backend selection is in [[modules#workspaces-per-compositor]].

```wisp
source tags = tags();
for tag in tags.list { cell { text = tag.label; } }
widget title { text = tags.title; }
```

**`gamma_warm()`**, **`dnd()`**, **`ui_hidden()`** read daemon state directly
with no fd. Values are `"1"`/`"0"`, `"on"`/`"off"` and `"1"`/`"0"` respectively.
Never poll these with `exec_line("wispctl …")`, that forks a client back into
the daemon.

**`toplevel(app_id=)`** requires `app_id=`. Max 16 of them.

**`mpris()`** gives `title` `artist` `status` `player` `art` off the session bus.
`art` is the cover art decoded to a local path (`file://` only, an http cover
gives `""`); feed it to a widget's `image`.

**`notifications([history=N][, image=N])`** is the notification centre feed: the
daemon's own history of accepted notifications, newest first, one entry per post
that carried no progress hint. Entries are pushed before the DnD gate, so DnD
collects instead of dropping.

| field | meaning |
|---|---|
| `count` | number of entries held |
| `open` | the panel flag `wispctl notif open\|close\|toggle` drives |
| `history` | for-only ring; fields `summary` `body` `app` `icon` `image` `urgent` `id` |

```wisp
source notif_s = notifications(history=64, image=22);

widget bell {
	icon = 0xf0f3;
	text = notif_s.count > 0 ? "{notif_s.count}" : "";
	on_click() = exec("wispctl notif toggle");
}
```

`history=` is an int literal from 1 to 128, default 16, and is compile-time: it
also sizes the per-cell arrays of every surface that loops over the ring.
`image=` is 0 to 128 px, default 0 meaning off, and keeps a downscaled copy of
each entry's pixmap; it rides the OSD decode, so it only produces anything when
the `osd` surface also declares `image = N`. Nothing is persisted to disk.

`note.id` is the entry's serial and the only safe dismiss key — there is no
`note.index`, because the ring shifts while a click is still travelling over the
control socket. `note.image` is a pixmap for a widget's `icon`, with `note.icon`
as its fallback glyph; when the entry has neither, the icon column collapses to
zero width instead of indenting the text.

**`tray(icon_size=N)`** exposes `count` plus the `items` iterable. `icon_size`
is an int literal from 8 to 64, default 16, and bakes `TRAY_ICON_PX`.

**`bluez()`** and **`power_profile()`** each open their own system-bus fd and do
**not** imply the notification D-Bus client.

**`pipewire()`** speaks the native protocol on its own fd: `vol` and `mic_vol`
are 0..1 floats, `mute`/`mic_mute`/`ok` are bools.

## Escape hatches

**`exec_line(cmd, every=, refresh=, refresh_ms=, lines=)`** runs `sh -c cmd` and
reads its output.

| argument | default | meaning |
|---|---|---|
| first, required | - | the command, a string |
| `every=` | `1000` ms | poll interval; floored at 50 ms when nonzero, `"0"` means one shot |
| `refresh=` / `refresh_ms=` | `120` ms | debounce after a `set()`; `"instant"` or `"sync"` runs synchronously |
| `lines=` | `1` | 1 to 256, an int literal; buffer is `lines * 256` bytes |

With `lines=1` the value truncates at the first newline. With more, internal
newlines survive and trailing ones are stripped. One child is in flight at a
time and extra ticks are dropped.

**`inotify(path=, lines=, key=)`** watches the parent directory filtered by
basename, so atomic renames fire correctly. `path=` is required and must be
absolute and not end in `/`. `key=` and `lines=` are mutually exclusive.

**`dbus_signal(iface, member)`** needs at least two arguments, the first two
strings. `value` is the scalar; `history` is a for-only ring of 8 entries.

## on_change

A source may carry exactly one handler, and `on_change()` is the one it takes.

```wisp
source vol_s = pipewire() {
	on_change() = exec("wispctl osd 1 volume");
};
```

It never fires from the boot sample and never on a re-poll to the same value.
The body runs with no owning widget, so an `animate()` inside repaints through
the global path.

## Gotchas

- `every=` on `vpn`, `bat`, `disk` or `backlight` is an `--emit` error; those are not polled kinds.
- `every=` below 250 ms is an `--emit` error.
- More than 32 sources, or more than 16 `toplevel()` sources, is a hard codegen error.
- `lines=` must be an integer literal in 1..256 and `tray(icon_size=)` an integer literal in 8..64.
- `notifications(history=)` is 1..128 and `image=` 0..128, both integer literals; `history=` changes the BSS of every surface looping over the ring.
- `notifications(image=N)` alone decodes nothing: the `osd` surface has to declare `image = N` too.
- A notification row keyed on an index instead of `note.id` dismisses the wrong entry when the ring moves under the click.
- Reading `net().rx_kbps` silently converts an event-driven source into a polling one.

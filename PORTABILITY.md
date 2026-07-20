# Portability audit — 2026-07-20

Gates keeping wisp from behaving the same across Wayland compositors.
Ranked easiest → hardest to fix. Verified against src/, not just the map.

## Done (2026-07-20)

- wl_compositor / wl_seat bind versions now clamp to the advertised version.
- Missing wl_compositor / wl_shm / zwlr_layer_shell_v1 now `die()` loudly after
  the startup sync instead of drawing nothing forever.
- The dwl-autolayout spawn is gone — the script no longer exists.
- No workspace backend (no mango IPC, no `ext_workspace_manager_v1`) already
  says so on stderr instead of showing an empty bar (`workspace.c:205`).

- HiDPI phases A+B landed: `wl_output.scale` is dispatched (clamped 1..4),
  stamped onto each Widget, `wl_surface.set_buffer_scale` is sent with the
  first buffer of a new scale (guarded on wl_compositor >= 3), pools/buffers
  and the wallpaper are physical (`widget_pw/ph`), and a runtime scale change
  repools + repaints via `widget_rescale_output`.

- Phase C landed: `render.c` primitives take logical geometry (`sw`/`sh`
  included — no caller or codegen change) and multiply by a file-static scale
  armed in `widget_free_slot()`, the one choke point every render path passes
  through. `text_width()` stays logical. Scale 1 is unchanged.

- Phase D landed, smaller than planned: `strike_for()` in `render.c` picks the
  strike to blit. On the **freetype** backend `font_ft_at_scale()` hands back a
  lazily-created twin `Font` (same face, `px_size * scale`, own cache) — real
  crisp text, created only when a scaled output actually draws. On **baked** and
  **bitmap** the const table can't grow a strike, so `draw_glyph_px()` takes a
  replication factor `m` and pixel-doubles. Pen advances stay logical
  (`adv * scale`) either way, so measured and drawn widths can't drift.
  **Dropped from the plan: the `hidpi=2` DSL attr and the extra baked strike.**
  Sharp HiDPI text on baked is `./configure` → freetype, which already exists;
  a second strike is DSL + wispc + bake.c + runtime work plus permanent .rodata
  for what a build flag already buys.

- Phase E verified: `make check` clean, both font backends build under
  `-Werror`, live at `scale:2` and back to 1 at runtime.

**Non-goal:** fractional scale (wp_fractional_scale_v1 + viewporter — a second
protocol and a downscale blit; only worth it if a real 1.25/1.5 machine shows
up). At integer-only a 1.5 output gets scale-2 buffers the compositor
downsamples, still sharper than before.

## Known, accepted (no action)

- gamma/night-light needs `zwlr_gamma_control_manager_v1` — absent on
  GNOME/KDE, already degrades with a message (`gamma.c:140`).
- Linux-only syscalls (memfd_create, timerfd, epoll) — Linux is the target.
- layer-shell v3 non-nullable output arg — already handled
  (`widget.c:widget_setup_surface` always passes the real output).
- keyboard_interactivity only uses 0/1, never v4 `on_demand` — safe on v1+.
- server-allocated ext-workspace ids (>= 0xff000000) routed before the
  opcode-0 overload — handled.
- Named workspaces ("web", "chat") get positional numbers and occupancy means
  "exists and not hidden" — ext-workspace v1 carries no client count, and
  ext-foreign-toplevel-list won't say which workspace a toplevel is on. The
  name→coord→arrival fallback is the best available; see the `ponytail:` note
  at `workspace.c:65`.
- xkb.c fails soft on unparseable keymaps (dead keyboard, not a crash);
  all compositors serialize via libxkbcommon so drift is unlikely.

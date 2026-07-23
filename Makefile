CC      ?= cc
CFLAGS  ?= -Os -Wall -Wextra -Werror -Wno-unused-parameter \
           -fno-asynchronous-unwind-tables -fdata-sections -ffunction-sections
LDFLAGS ?= -Wl,--gc-sections -Wl,-s -Wl,--as-needed -lm

# Default to one job per CPU when the user didn't already pick a -j value, so
# `make` and `make install` saturate the box and finish in ~3-4s from clean
# instead of ~7s sequentially. `MAKEFLAGS` inherits into recursive sub-makes
# (notably `make check`'s per-example builds). Override with `make -j1` for
# debugging. `findstring` keeps an explicit user -j taking precedence.
JOBS    ?= $(shell nproc 2>/dev/null || echo 4)
ifeq (,$(findstring -j,$(MAKEFLAGS)))
MAKEFLAGS += --jobs=$(JOBS)
endif

PREFIX  ?= $(HOME)/.local

SRCDIR  := src
TOOLDIR := src/tools
BUILD   := build

# wispc (the .wisp compiler) compile line. Full -Werror; the switch-exhaustiveness
# part (-Wall's -Wswitch) is load-bearing: the dirty-tracking that drives every
# surface's repaint is derived in sema.c by walking the AST (walk_expr/walk_stmt/
# walk_widget) with default-less switches over the EX_*/SB_*/ST_* enums. A new
# node kind added without a matching case would silently under-collect a surface's
# source deps — the surface freezes at runtime with no diagnostic. -Werror turns
# that (and every other warning) into a wispc build error instead.
WISPC_CC := $(CC) -O2 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-format-truncation

# Preset selector. wisp is DSL-first: `make WISP=configs/foo.wisp` runs wispc to
# compile the .wisp into C (features.h + objects.mk + gen_*.c + main.c) and links
# it. Each config gets its OWN build dir (build/<name>/) so switching configs is
# a relink-nothing cache hit; the current selection lives in build/.selected and
# `wispctl rebuild` passes an explicit WISP= from the user's config dir.
SELECTED_FILE := $(BUILD)/.selected

# Selection is STICKY across separate make invocations: with no explicit WISP=
# on the command line, reuse whatever the previous build persisted in
# .selected (WISP *and* FONT_BACKEND/FONT). This is what makes the two-step
#   make WISP=configs/foo.wisp FONT_BACKEND=bitmap && make install
# install `foo` (with the chosen backend) instead of silently reverting — the
# second `make install` recovers the whole selection rather than re-defaulting.
# CLI assignments always win; `//!` directives in the .wisp override further
# below. FONT knobs are only recovered together with WISP: an explicit WISP=
# means "that config's own directives/defaults", not the old selection's fonts.
PERSISTED_TAG := $(shell [ -f $(SELECTED_FILE) ] && cat $(SELECTED_FILE) 2>/dev/null)

ifndef WISP
ifneq (,$(filter WISP=%,$(PERSISTED_TAG)))
WISP := $(patsubst WISP=%,%,$(filter WISP=%,$(PERSISTED_TAG)))
endif
ifeq ($(origin FONT_BACKEND),undefined)
ifneq (,$(filter FONT_BACKEND=%,$(PERSISTED_TAG)))
FONT_BACKEND := $(patsubst FONT_BACKEND=%,%,$(filter FONT_BACKEND=%,$(PERSISTED_TAG)))
endif
endif
ifeq ($(origin FONT),undefined)
ifneq (,$(filter FONT=%,$(PERSISTED_TAG)))
FONT := $(patsubst FONT=%,%,$(filter FONT=%,$(PERSISTED_TAG)))
endif
endif
endif

# Still no selector (fresh build/, no tag) → canonical default. bee is what
# sync.sh installs, so a tagless tree rebuilds the preset that's actually running.
WISP ?= configs/bee.wisp
# Absolute so the tag can't thrash between rel/abs spellings of one config.
override WISP := $(abspath $(WISP))
# ponytail: build dirs keyed by basename — two same-named configs from
# different dirs share (and wipe) one cache via the tag mismatch below.
CFG   := $(notdir $(basename $(WISP)))
ROOT  := $(BUILD)
BUILD := $(ROOT)/$(CFG)
BUILD_TAG_FILE := $(BUILD)/.build-tag

# Build knobs declared in the .wisp itself as `//! key = value` directive
# comments (plain // comments to wispc, so the compiler never sees them).
# Priority: CLI assignment > .wisp directive > persisted tag > default below.
# The directive block must sit between WISP resolution and the ?= defaults:
# a directive overwrites a tag-recovered value (origin "file") but an
# origin=="command line" assignment is left alone.
wispdir = $(shell sed -n 's|^//! *$(1) *= *||p' $(WISP) 2>/dev/null | tail -1)
define KNOB
ifneq ($$(origin $(1)),command line)
D_$(1) := $$(call wispdir,$(2))
ifneq ($$(D_$(1)),)
$(1) := $$(D_$(1))
endif
endif
endef
$(eval $(call KNOB,FONT_BACKEND,font_backend))
$(eval $(call KNOB,FONT,font))
$(eval $(call KNOB,FONT_FALLBACK,font_fallback))
$(eval $(call KNOB,FRACTIONAL,fractional))

# Font backend (one per build) + font path defaults — lowest priority.
#   truetype — daemon rasterizes TTF/OTF with src/tt/ (default; no FreeType).
#   bitmap   — pixel-exact PSF/BDF baked into const tables at build time.
# truetype changes the runtime (WISP_FONT_RUNTIME) + links font_cache.o.
FONT_BACKEND ?= truetype
FONT         ?= $(HOME)/.local/share/fonts/MapleMono-NF-Bold.ttf
# Optional fallback font (truetype backend only). Empty → no chain. Outline
# fonts render mono (alpha8); CBDT color-bitmap emoji fonts render in color
# (downscaled to text size). $WISP_FONT_FALLBACK overrides at runtime.
FONT_FALLBACK ?=
# Directive values can't rely on shell expansion — honor a leading ~/ here.
FONT          := $(patsubst ~/%,$(HOME)/%,$(FONT))
FONT_FALLBACK := $(patsubst ~/%,$(HOME)/%,$(FONT_FALLBACK))
# Fractional scaling needs real strikes at arbitrary sizes — the const bitmap
# tables can only pixel-double, so it's truetype-only.
FRACTIONAL ?= 0
ifeq ($(FRACTIONAL),1)
ifneq ($(FONT_BACKEND),truetype)
$(error FRACTIONAL=1 needs FONT_BACKEND=truetype (bitmap fonts can only pixel-double))
endif
CFLAGS += -DWISP_FRACTIONAL
endif
ifneq (,$(filter $(FONT_BACKEND),baked freetype))
$(error FONT_BACKEND=$(FONT_BACKEND) was retired — truetype (default) covers it; bitmap remains for PSF/BDF pixel fonts)
endif
ifeq ($(filter $(FONT_BACKEND),truetype bitmap),)
$(error FONT_BACKEND must be truetype or bitmap (got '$(FONT_BACKEND)'))
endif
ifeq ($(FONT_BACKEND),truetype)
FONT_DEFS := -DWISP_FONT_TRUETYPE -DWISP_FONT_RUNTIME
endif
CFLAGS += $(FONT_DEFS)

# Build "tag" — uniquely identifies the (WISP, FONT_BACKEND, FONT) tuple backing
# this config's build/<name>/. If the tag differs from last time, wipe the dir —
# otherwise leftover objects from a previous font selection (or a same-named
# config at another path) silently survive. FONT last so a spaced path can't
# corrupt the earlier fields during recovery. build/.selected is refreshed with
# the same tuple so a bare `make` repeats this selection — sub-makes fanning out
# over all configs (install/check) pass WISP_NOSELECT=1 to not steal it.
BUILD_TAG := WISP=$(WISP) FRACTIONAL=$(FRACTIONAL) FONT_BACKEND=$(FONT_BACKEND) FONT=$(FONT) FONT_FALLBACK=$(FONT_FALLBACK)
TAG_RESET := $(shell mkdir -p $(BUILD); \
  prev=""; [ -f $(BUILD_TAG_FILE) ] && prev=$$(cat $(BUILD_TAG_FILE)); \
  if [ "$$prev" != "$(BUILD_TAG)" ]; then \
    rm -rf $(BUILD); mkdir -p $(BUILD); \
    printf '%s' '$(BUILD_TAG)' > $(BUILD_TAG_FILE); \
  fi; \
  [ -n "$(WISP_NOSELECT)" ] || printf '%s' '$(BUILD_TAG)' > $(SELECTED_FILE); \
  echo OK)

GENDIR    := $(BUILD)/gen-tw
MAIN_FROM := $(GENDIR)
# wispc + bake are config-independent — shared at the build root across caches.
WISPC_BOOT := $(ROOT)/wispc
WISPC_BOOT_SRC := $(wildcard $(TOOLDIR)/wispc/*.c) $(TOOLDIR)/wispc/wispc.h
# Bootstrap is incremental: rebuild wispc only when its sources changed; rerun
# --emit only when the .wisp or wispc binary is newer than the generated files.
# Files touched by --emit: features.h, objects.mk, gen_*.c, main.c.
WISPC_STAMP   := $(GENDIR)/.emit-stamp
WISPC_NEED  := $(shell mkdir -p $(GENDIR) $(BUILD); \
  rebuild_wispc=0; \
  if [ ! -x $(WISPC_BOOT) ]; then rebuild_wispc=1; else \
    for f in $(WISPC_BOOT_SRC); do [ "$$f" -nt $(WISPC_BOOT) ] && rebuild_wispc=1 && break; done; \
  fi; \
  if [ $$rebuild_wispc = 1 ]; then \
    $(WISPC_CC) \
        $(wildcard $(TOOLDIR)/wispc/*.c) -o $(WISPC_BOOT) 1>&2 || { echo FAIL; exit 0; }; \
  fi; \
  if [ ! -f $(WISPC_STAMP) ] || [ $(WISP) -nt $(WISPC_STAMP) ] || [ $(WISPC_BOOT) -nt $(WISPC_STAMP) ]; then \
    $(WISPC_BOOT) --emit $(GENDIR) $(WISP) 1>&2 && touch $(WISPC_STAMP) || { echo FAIL; exit 0; }; \
  fi; \
  echo OK)
ifneq ($(WISPC_NEED),OK)
$(error wispc bootstrap failed (see stderr))
endif

# Force-include the preset's feature macros into every TU before any other
# header so module sources can #ifdef WISP_HAS_* at the top.
CFLAGS += -include $(GENDIR)/features.h -include $(GENDIR)/gen_overrides.h -I$(SRCDIR) -iquote $(GENDIR)

include $(GENDIR)/objects.mk

# The truetype backend adds the shared glyph cache + its rasterizer.
ifeq ($(FONT_BACKEND),truetype)
GEN_OBJS += $(BUILD)/font_cache.o $(BUILD)/font_tt.o $(BUILD)/tt/sfnt.o $(BUILD)/tt/raster.o $(BUILD)/tt/cff.o $(BUILD)/tt/color.o
# color.c decodes CBDT emoji PNGs with image.c's stb; sort dedupes it away when
# the preset already links image.o for the wallpaper.
GEN_OBJS := $(sort $(GEN_OBJS) $(BUILD)/image.o)
endif

HDR := $(SRCDIR)/wisp.h $(SRCDIR)/proto.h $(SRCDIR)/config.h \
       $(SRCDIR)/font.h $(GENDIR)/bake.h \
       $(GENDIR)/features.h $(GENDIR)/gen_overrides.h $(GENDIR)/gen_menus.h

WISPC_SRC := $(TOOLDIR)/wispc/arena.c $(TOOLDIR)/wispc/diag.c $(TOOLDIR)/wispc/lex.c \
            $(TOOLDIR)/wispc/parse.c $(TOOLDIR)/wispc/style.c $(TOOLDIR)/wispc/sema.c $(TOOLDIR)/wispc/dump.c \
            $(TOOLDIR)/wispc/codegen.c $(TOOLDIR)/wispc/codegen_util.c \
            $(TOOLDIR)/wispc/codegen_sources.c $(TOOLDIR)/wispc/codegen_expr.c \
            $(TOOLDIR)/wispc/codegen_items.c $(TOOLDIR)/wispc/codegen_surface.c \
            $(TOOLDIR)/wispc/codegen_surface_life.c $(TOOLDIR)/wispc/codegen_spawned.c $(TOOLDIR)/wispc/codegen_compound.c \
            $(TOOLDIR)/wispc/wispc.c

BIN := $(BUILD)/wisp $(BUILD)/wispctl $(BUILD)/wisp-lock $(BUILD)/wisp-lock-helper $(WISPC_BOOT)

# wisp-lock is a separate binary: a session-locker crash must NOT take down
# the bar/HUD, and a wisp daemon crash must NOT drop the lock. Its compile
# context uses src/lock-features.h in place of the codegen-emitted features.h,
# so only WISP_HAS_LOCK is set and every other module preprocesses out. The
# DSL-driven `lock {}` block still flows through gen_overrides.h for styling.
LOCK_SRC  := wl widget render xkb wisp lock lock-main image
ifeq ($(FONT_BACKEND),truetype)
LOCK_SRC  += font_cache font_tt tt/sfnt tt/raster tt/cff tt/color
endif
LOCK_OBJS := $(LOCK_SRC:%=$(BUILD)/lock/%.o)
LOCK_HDR  := $(SRCDIR)/wisp.h $(SRCDIR)/proto.h $(SRCDIR)/config.h \
             $(SRCDIR)/font.h $(GENDIR)/bake.h \
             $(SRCDIR)/lock-features.h $(GENDIR)/gen_overrides.h
LOCK_CFLAGS := -Os -Wall -Wextra -Werror -Wno-unused-parameter \
               -fno-asynchronous-unwind-tables -fdata-sections -ffunction-sections \
               $(FONT_DEFS) \
               -include $(SRCDIR)/lock-features.h \
               -include $(GENDIR)/gen_overrides.h -I$(SRCDIR) -iquote $(GENDIR)

all: $(BIN)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/wisp: $(GEN_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	strip --strip-all --remove-section=.comment --remove-section=.note* $@ 2>/dev/null || true

$(BUILD)/%.o: $(SRCDIR)/%.c $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# wall.c embeds stb_image.h; it trips most of our warnings. Compile with -w.
$(BUILD)/wall.o: $(SRCDIR)/wall.c $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) -w -c $< -o $@

# image.c also embeds stb_image.h (shared PNG loader for wall + lock). Same -w.
$(BUILD)/image.o: $(SRCDIR)/image.c $(SRCDIR)/image.h | $(BUILD)
	$(CC) $(CFLAGS) -w -c $< -o $@

# src/tt/ is its own subdir of objects (shared with the lock binary below).
$(BUILD)/tt $(BUILD)/lock/tt:
	mkdir -p $@

$(BUILD)/tt/%.o: $(SRCDIR)/tt/%.c $(SRCDIR)/tt/tt.h | $(BUILD)/tt
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/gen_main.o: $(MAIN_FROM)/main.c $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# WISP path also produces gen_sources.c / gen_surfaces.c / gen_bindings.c.
# Preset paths don't (their objects.mk omits these), so the rules only fire
# when a WISP build needs them.
$(BUILD)/gen_sources.o:  $(GENDIR)/gen_sources.c  $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/gen_surfaces.o: $(GENDIR)/gen_surfaces.c $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/gen_bindings.o: $(GENDIR)/gen_bindings.c $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/gen_outputs.o:  $(GENDIR)/gen_outputs.c  $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
# gen_spawn.c does `#include "osd.c"` to inline the runtime — make the include
# visible to make's dep graph so edits to osd.c actually trigger a rebuild.
$(BUILD)/gen_spawn.o:    $(GENDIR)/gen_spawn.c    $(SRCDIR)/osd.c $(SRCDIR)/osd_pill.c $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/wispctl: $(SRCDIR)/wispctl.c | $(BUILD)
	$(CC) $(CFLAGS) -Wno-format-truncation -DWISP_DATADIR='"$(PREFIX)/share/wisp"' $< -o $@ -Wl,--gc-sections -Wl,-s
	strip --strip-all --remove-section=.comment --remove-section=.note* $@ 2>/dev/null || true

$(WISPC_BOOT): $(WISPC_SRC) $(TOOLDIR)/wispc/wispc.h
	$(WISPC_CC) $(WISPC_SRC) -o $@

$(BUILD)/lock:
	mkdir -p $(BUILD)/lock

$(BUILD)/lock/%.o: $(SRCDIR)/%.c $(LOCK_HDR) | $(BUILD)/lock
	$(CC) $(LOCK_CFLAGS) -c $< -o $@

# More specific than the rule above (shorter stem), so src/tt/*.c lands here.
$(BUILD)/lock/tt/%.o: $(SRCDIR)/tt/%.c $(SRCDIR)/tt/tt.h | $(BUILD)/lock/tt
	$(CC) $(LOCK_CFLAGS) -c $< -o $@

# image.c embeds stb_image.h — suppress its warnings in the lock build too.
$(BUILD)/lock/image.o: $(SRCDIR)/image.c $(SRCDIR)/image.h | $(BUILD)/lock
	$(CC) $(LOCK_CFLAGS) -w -c $< -o $@

$(BUILD)/wisp-lock: $(LOCK_OBJS) | $(BUILD)
	$(CC) $(LOCK_CFLAGS) $^ -o $@ $(LDFLAGS)
	strip --strip-all --remove-section=.comment --remove-section=.note* $@ 2>/dev/null || true

$(BUILD)/wisp-lock-helper: $(TOOLDIR)/lock-helper.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@ -Wl,--gc-sections -Wl,-s -lpam
	strip --strip-all --remove-section=.comment --remove-section=.note* $@ 2>/dev/null || true

# bake.h is regenerated whenever the font, bake tool, .wisp unit, or build tag
# (FONT_BACKEND/FONT switch) changes. The set of sizes always includes 14 + 22
# (legacy aliases) plus any `font_size = N;` the .wisp declares.
#
# truetype: emit per-size mutable skeletons + the runtime default font path;
# no glyph data is baked and the font file need not exist at build time.
# bitmap: bake glyph data from the PSF/BDF font file.
ifeq ($(FONT_BACKEND),truetype)
$(GENDIR)/bake.h: $(ROOT)/bake $(WISPC_BOOT) $(WISP) $(SRCDIR)/font.h $(BUILD_TAG_FILE)
	$(WISPC_BOOT) --font-sizes $(WISP) > $(BUILD)/font-sizes.txt
	$(ROOT)/bake --stub $@ $(FONT) "$(FONT_FALLBACK)" $$(cat $(BUILD)/font-sizes.txt)
else
$(GENDIR)/bake.h: $(ROOT)/bake $(FONT) $(WISPC_BOOT) $(WISP) $(SRCDIR)/font.h $(BUILD_TAG_FILE)
	$(WISPC_BOOT) --font-sizes $(WISP) > $(BUILD)/font-sizes.txt
	$(ROOT)/bake $(FONT) $@ $$(cat $(BUILD)/font-sizes.txt)
endif

# Debug-only: diffs src/tt's parsing against freetype (dlopen'd). Never installed.
ttmetrics: $(TOOLDIR)/ttmetrics.c $(SRCDIR)/tt/sfnt.c $(SRCDIR)/tt/raster.c $(SRCDIR)/tt/cff.c $(SRCDIR)/tt/tt.h
	$(CC) -O1 -g -Wall -Wextra -Werror $(shell pkg-config --cflags freetype2 2>/dev/null) \
	    $(TOOLDIR)/ttmetrics.c $(SRCDIR)/tt/sfnt.c $(SRCDIR)/tt/raster.c $(SRCDIR)/tt/cff.c -o $@ -lm

$(ROOT)/bake: $(TOOLDIR)/bake.c
	$(CC) -O2 -Wall -Werror $(TOOLDIR)/bake.c -o $@

# Warm every OTHER config's cache too (repo configs + user configs; the
# share-dir examples aren't swept unless this tree IS the share dir). A broken
# non-selected config warns instead of blocking the install/switch. Each
# sub-make lands in its own build/<name>/, so with fresh caches this is a
# mtime sweep + copy — `wispctl rebuild other` then switches without compiling.
CONFDIR := $(or $(XDG_CONFIG_HOME),$(HOME)/.config)/wisp
ALL_WISP := $(sort $(abspath $(wildcard configs/*.wisp) $(wildcard $(CONFDIR)/*.wisp)))

warm-cache: $(BIN)
	@for w in $(filter-out $(WISP),$(ALL_WISP)); do \
	    out=$$($(MAKE) -s WISP=$$w WISP_NOSELECT=1 2>&1) || { \
	        echo "$$out" >&2; \
	        echo "warning: $$w failed to build (cache not updated)" >&2; }; \
	done

# WISP_NOWARM=1 skips warming the other configs' caches: `wispctl rebuild`
# reloads first and warms them in a detached make so a switch isn't blocked
# on rebuilding configs it isn't switching to.
install: $(BIN) $(if $(WISP_NOWARM),,warm-cache)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BUILD)/wisp              $(DESTDIR)$(PREFIX)/bin/wisp
	install -m 755 $(BUILD)/wispctl           $(DESTDIR)$(PREFIX)/bin/wispctl
	install -m 755 $(WISPC_BOOT)              $(DESTDIR)$(PREFIX)/bin/wispc
	install -m 755 $(BUILD)/wisp-lock         $(DESTDIR)$(PREFIX)/bin/wisp-lock
	install -m 755 $(BUILD)/wisp-lock-helper  $(DESTDIR)$(PREFIX)/bin/wisp-lock-helper

# The tools alone need no config: wispc + wispctl are what a from-share user
# gets from install.sh; `wispctl rebuild` then compiles the daemon itself.
tools: $(WISPC_BOOT) $(BUILD)/wispctl

install-tools: tools
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(WISPC_BOOT)    $(DESTDIR)$(PREFIX)/bin/wispc
	install -m 755 $(BUILD)/wispctl $(DESTDIR)$(PREFIX)/bin/wispctl

# Runtime sources for `wispctl rebuild`: the runtime is compiled per config
# (every TU #includes the generated features.h), so what ships is source, not
# a library. Wiped on reinstall so updates can't leave stale files behind.
SHAREDIR := $(DESTDIR)$(PREFIX)/share/wisp
install-share:
	rm -rf $(SHAREDIR)
	install -d $(SHAREDIR)/configs $(SHAREDIR)/docs
	cp -r Makefile src $(SHAREDIR)/
	install -m 644 configs/*.wisp $(SHAREDIR)/configs/
	install -m 644 docs/*.md $(SHAREDIR)/docs/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/wisp \
	      $(DESTDIR)$(PREFIX)/bin/wispctl \
	      $(DESTDIR)$(PREFIX)/bin/wispc \
	      $(DESTDIR)$(PREFIX)/bin/wisp-lock \
	      $(DESTDIR)$(PREFIX)/bin/wisp-lock-helper
	rm -rf $(SHAREDIR)

clean:
	rm -rf $(ROOT)

distclean: clean
	rm -f $(SRCDIR)/bake.h

.PHONY: warm-cache

# Configs present under configs/. Glob so deleting a .wisp file doesn't break
# `make check`; add new ones by dropping them in configs/ — no Makefile edit.
CONFIGS := $(patsubst configs/%.wisp,%,$(wildcard configs/*.wisp))

# Build matrix: every config present under configs/. Per-config build dirs make
# this incremental and side-effect-free: nothing to clean, and WISP_NOSELECT=1
# keeps the matrix from stealing the sticky selection.
# The nm DCE assertions run only when configs/minimal.wisp is present — it's
# the only unit whose stripped-down feature set makes the assertion meaningful.
check:
	@set -e; \
	fail=0; \
	echo "==> Build matrix"; \
	for e in $(CONFIGS); do \
	    if out=$$($(MAKE) -s WISP=configs/$$e.wisp WISP_NOSELECT=1 2>&1); then \
	        sz=$$(stat -c%s $(ROOT)/$$e/wisp); \
	        printf "  %-26s OK  %d B\n" "WISP=$$e.wisp" "$$sz"; \
	    else \
	        printf "  %-26s FAIL\n%s\n" "WISP=$$e.wisp" "$$out"; \
	        fail=1; \
	    fi; \
	done; \
	if [ -f configs/minimal.wisp ]; then \
	    echo "==> nm assertions (WISP=minimal.wisp must not link optional modules)"; \
	    for sym in dbus_ osd_ menu_ hud_ lock_ gamma_ wall_; do \
	        if nm $(ROOT)/minimal/wisp 2>/dev/null | grep -q " T $$sym\| t $$sym"; then \
	            echo "  $$sym  FAIL  found in minimal binary"; fail=1; \
	        else \
	            echo "  $$sym  OK"; \
	        fi; \
	    done; \
	else \
	    echo "==> nm assertions skipped (configs/minimal.wisp absent)"; \
	fi; \
	if [ $$fail -ne 0 ]; then echo "check: FAIL"; exit 1; fi; \
	echo "check: PASS"

.PHONY: all tools install install-tools install-share uninstall clean distclean check

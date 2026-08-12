#include "sema_internal.h"
#include <string.h>
#include <stdlib.h>

/* SrcDef + F_* live in sema_internal.h (shared with sema_types.c). */

/* A row here without a driver in codegen_sources.c makes --check pass and
 * --emit die — add the row in the same commit as the driver, never before. */
static const SrcDef SOURCES[] = {
    {"clock",                "value",  "value", F_CLOCK },
    {"cpu",                  "pct",    "pct", F_CPU },
    {"mem",                  "pct",    "pct used_mb", F_MEM },
    {"temp",                 "c",      "c", F_TEMP },
    {"bat",                  "pct",    "pct charging", F_BAT },
    {"net",                  "ssid",   "up ssid signal rx_kbps tx_kbps", F_NET },
    {"backlight",            "pct",    "pct", F_BACKLIGHT },
    {"power_profile",        "profile","profile", F_POWER },
    {"bluez",                "device", "powered connected device battery", F_BLUEZ },
    {"disk",                 "pct",    "pct", F_DISK },
    {"vpn",                  "state",  "state ok", F_VPN },
    {"tags",             "title",  "title list", F_TAGS },
    {"gamma_warm",           "value",  "value", F_NONE },
    {"dnd",                  "value",  "value", F_NONE },
    {"ui_hidden",            "value",  "value", F_NONE },
    {"exec_line",            "value",  "value", F_EXEC },
    {"inotify",              "value",  "value", F_NONE },
    {"dbus_signal",          "value",  "value history", F_DBUS },
    {"notifications",        "count",  "count open history", F_DBUS },
    {"mpris",                "title",  "title artist status player art", F_MPRIS },
    {"tray",                 "count",  "count items", F_TRAY },
    {"pipewire",             "vol",    "vol mute mic_vol mic_mute ok", F_PIPEWIRE },
    {"toplevel",             "exists", "exists count title", F_TOPLEVEL },
};

const SrcDef *find_src(const char *name, size_t n) {
    for (size_t i = 0; i < sizeof SOURCES / sizeof SOURCES[0]; i++) {
        if (strlen(SOURCES[i].name) == n && memcmp(SOURCES[i].name, name, n) == 0)
            return &SOURCES[i];
    }
    return NULL;
}

static bool field_ok(const SrcDef *s, const char *f, size_t n) {
    const char *p = s->fields;
    while (*p) {
        const char *e = p;
        while (*e && *e != ' ') e++;
        if ((size_t)(e - p) == n && memcmp(p, f, n) == 0) return true;
        p = e; while (*p == ' ') p++;
    }
    return false;
}

/* ---------- property schema ----------
 * One authoritative set of accepted prop names per container kind, derived from
 * every widget_prop()/surface_prop()/group_prop() lookup and the OSD/menu/lock/
 * gamma/wallpaper override tables in codegen*.c, plus the marker props read
 * inline (`slider;` `elide` `hover;`). Codegen is pull-based: a name it never
 * looks up was silently dropped. This table turns that typo into an error.
 *
 * The surface set is a UNION across surface flavours (plain / osd / menu / pill
 * templates / compound region) — a valid prop is never rejected for sitting on
 * the "wrong" surface flavour; only genuinely-unknown names are caught. Every
 * name here is space-delimited with a leading+trailing space for cheap search. */
typedef struct { const char *kind; const char *human; const char *props; } PropSchema;

static const PropSchema SCHEMAS[] = {
    { "widget", "widget",
      " align bg body_fg body_fit body_lines border border_bottom border_left border_right"
      " border_top border_width elide enter_anim enter_easing exit_anim"
      " exit_easing fg graph graph_fg graph_max graph_samples height hover_bg icon icon_box icon_fg icon_gap"
      " image image_size"
      " orientation pad pad_x pad_y press_bg radius"
      " radius_bl radius_br radius_tl radius_tr shadow shadow_blur shadow_spread"
      " shadow_x shadow_y show_value slider sticky text text_align thumb_border thumb_border_width"
      " thumb_color thumb_radius thumb_shape thumb_size tooltip track_bg track_fg"
      " track_radius transition_bg transition_border transition_easing"
      " transition_fg transition_size value value_align value_fg value_format"
      " value_gap value_max value_scale visible width wrap x_offset y_offset " },
    { "surface", "surface",
      " anchor anchor_gap armpit_bl armpit_br armpit_color armpit_inner"
      " armpit_outer armpit_tl armpit_tr axis bg bg_bottom body_lines body_max"
      " border border_bottom border_left border_right border_top border_width"
      " clip_top clip_widgets cutout_height cutout_into cutout_width cutout_x"
      " cutout_y dbus_close dismiss_on_click edge exclusive_zone fg fillet_bl"
      " fillet_br fillet_inner_bottom fillet_inner_left fillet_inner_right"
      " fillet_inner_top fillet_offset_y fillet_outer_bottom fillet_outer_left"
      " fillet_outer_right fillet_outer_top fillet_r fillet_tl fillet_tr"
      " focus_follow font_size gap height hover icon_gap icons image input"
      " layer margin margin_x max max_visible output pad pad_x pad_y prog_fg prog_h prog_track"
      " prompt radius radius_bl radius_br radius_inner radius_outer radius_tl"
      " radius_tr reveal_anim_ms reveal_easing reveal_gutter reveal_on_hover"
      " shadow shadow_blur shadow_spread shadow_x shadow_y"
      " delay_ms dismiss_on_unfocus keyboard on_escape row_h scroll separator separator_frac separator_h size slide_ms sort sound spawned_by terminal"
      " timeout timeout_low timeout_normal user sessions visible width " },
    { "group", "group",
      " align bg border border_width gap height pad pad_x radius shadow"
      " shadow_blur shadow_spread shadow_x shadow_y sticky " },
    { "lock", "lock block",
      " pam prompt bg ring ring_bad fg dim caps font_size wrong_ms wall"
      " retry_ms retry_growth retry_max_ms lockout_after privacy"
      " wipe_on_backspace " },
    { "lock_frame", "lock frame",
      " anchor bg border border_width height radius show width x y " },
    { "lock_text", "lock text",
      " anchor fg font_size format show text x y " },
    { "lock_ring", "lock ring",
      " anchor bg border border_width fg gap highlight highlight_arc highlight_bs radius segments separator show thickness x y " },
    { "gamma", "gamma block",
      " day_k night_k flat_k day_hour night_hour fade_min transition_ms " },
    { "wallpaper", "wallpaper block",
      " path bg fade_ms transition dither_px wipe_dir wipe_soft cache " },
    { "media", "media block", " " },
    { "idle", "idle block", " before_sleep " },
    { "idle_timeout", "idle timeout", " after run resume " },
};

static bool word_in(const char *list, const char *w, size_t n) {
    for (const char *p = list; *p;) {
        while (*p == ' ') p++;
        const char *e = p; while (*e && *e != ' ') e++;
        if ((size_t)(e - p) == n && memcmp(p, w, n) == 0) return true;
        p = e;
    }
    return false;
}

/* Levenshtein, capped — words this long never fuzzy-match a real prop name. */
static int edit_dist(const char *a, size_t an, const char *b, size_t bn) {
    if (an > 40 || bn > 40) return 99;
    int prev[41], cur[41];
    for (size_t j = 0; j <= bn; j++) prev[j] = (int)j;
    for (size_t i = 1; i <= an; i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= bn; j++) {
            int c = a[i-1] == b[j-1] ? 0 : 1;
            int m = prev[j] + 1;
            if (cur[j-1] + 1 < m) m = cur[j-1] + 1;
            if (prev[j-1] + c < m) m = prev[j-1] + c;
            cur[j] = m;
        }
        for (size_t j = 0; j <= bn; j++) prev[j] = cur[j];
    }
    return prev[bn];
}

/* Error on a prop name not in `kind`'s accepted set. did-you-mean via edit
 * distance within the kind; if the name is valid on another kind, say which. */
static void check_prop(const char *kind, Prop *p) {
    const PropSchema *sc = NULL;
    for (size_t i = 0; i < sizeof SCHEMAS / sizeof SCHEMAS[0]; i++)
        if (strcmp(SCHEMAS[i].kind, kind) == 0) { sc = &SCHEMAS[i]; break; }
    if (!sc || word_in(sc->props, p->name, p->nlen)) return;

    diag_error(p->loc, "unknown %s property '%.*s'", sc->human, (int)p->nlen, p->name);

    /* Valid on a different kind? name it — beats a fuzzy guess. */
    for (size_t i = 0; i < sizeof SCHEMAS / sizeof SCHEMAS[0]; i++) {
        if (&SCHEMAS[i] == sc) continue;
        if (word_in(SCHEMAS[i].props, p->name, p->nlen)) {
            diag_hint(p->loc, "'%.*s' is a %s property", (int)p->nlen, p->name, SCHEMAS[i].human);
            return;
        }
    }
    /* Otherwise nearest accepted name within this kind. */
    char best[64]; int bd = 3;
    for (const char *q = sc->props; *q;) {
        while (*q == ' ') q++;
        const char *e = q; while (*e && *e != ' ') e++;
        size_t wn = (size_t)(e - q);
        if (wn && wn < sizeof best) {
            int d = edit_dist(p->name, p->nlen, q, wn);
            if (d < bd) { bd = d; memcpy(best, q, wn); best[wn] = 0; }
        }
        q = e;
    }
    if (bd < 3) diag_hint(p->loc, "did you mean '%s'?", best);
}

/* Syms / ScopeEnt / S live in sema_internal.h (shared with sema_types.c). */

static bool nameq(const char *a, size_t an, const char *b) {
    size_t bn = strlen(b);
    return an == bn && memcmp(a, b, an) == 0;
}

Decl *find_decl_in(Decl **arr, int n, const char *name, size_t nlen) {
    for (int i = 0; i < n; i++)
        if (arr[i]->nlen == nlen && memcmp(arr[i]->name, name, nlen) == 0)
            return arr[i];
    return NULL;
}

static void push_local(S *s, const char *name, size_t n) {
    ScopeEnt *e = arena_alloc(s->a, sizeof *e);
    e->name = name; e->nlen = n; e->next = s->locals;
    s->locals = e;
}
static void pop_local(S *s) { s->locals = s->locals->next; }
bool is_local(S *s, const char *name, size_t n) {
    for (ScopeEnt *e = s->locals; e; e = e->next)
        if (e->nlen == n && memcmp(e->name, name, n) == 0) return true;
    return false;
}

static void add_dep(S *s, const char *src_name) {
    for (int i = 0; i < s->ndeps; i++)
        if (strcmp(s->deps[i], src_name) == 0) return;
    if (s->ndeps == s->capdeps) {
        s->capdeps = s->capdeps ? s->capdeps * 2 : 4;
        s->deps = realloc(s->deps, sizeof(char*) * s->capdeps);
    }
    s->deps[s->ndeps++] = src_name;
}

static void set_flag(SemaResult *r, int f) {
    switch (f) {
    case F_CPU:  r->has_src_cpu = 1; break;
    case F_MEM:  r->has_src_mem = 1; break;
    case F_TEMP: r->has_src_temp = 1; break;
    case F_BAT:  r->has_src_bat = 1; break;
    case F_NET:  r->has_src_net = 1; break;
    case F_BACKLIGHT: r->has_src_backlight = 1; break;
    /* power.c is a standalone system-bus client; does NOT imply has_dbus
     * (the session transport). */
    case F_POWER: r->has_power = 1; break;
    /* bluez.c is likewise a standalone system-bus client. */
    case F_BLUEZ: r->has_bluez = 1; break;
    case F_DISK: r->has_src_disk = 1; break;
    case F_VPN:  r->has_src_vpn = 1; break;
    case F_TAGS:  r->has_src_tags = 1; break;
    case F_EXEC: r->has_src_exec = 1; break;
    case F_DBUS: r->has_dbus = 1; break;
    /* mpris.c is a pure client, but it needs the transport. */
    case F_MPRIS: r->has_mpris = 1; r->has_dbus = 1; break;
    /* Same shape as mpris, except tray.c also owns a name on the bus. */
    case F_TRAY:  r->has_tray = 1; r->has_dbus = 1; break;
    /* Native PipeWire client — its own transport, does NOT imply has_dbus. */
    case F_PIPEWIRE: r->has_pipewire = 1; break;
    /* zwlr foreign-toplevel client — rides the wl_display, own transport,
     * repainted via wispgen_wisp_state_changed() (DRV_WISP-shaped). */
    case F_TOPLEVEL: r->has_toplevel = 1; break;
    default: break;
    }
}

/* `tray(icon_size=N)` — the decoded-icon square, baked into features.h. It
 * sizes a per-item buffer, so it stays compile-time; 8..64 keeps that sane. */
static void read_tray_icon_size(SemaResult *r, Expr *c) {
    for (int i = 0; i < c->call.nargs; i++) {
        const char *kn = c->call.argnames ? c->call.argnames[i] : NULL;
        if (!kn || c->call.anlen[i] != 9 || memcmp(kn, "icon_size", 9)) continue;
        if (c->call.args[i]->kind != EX_INT) {
            diag_error(c->loc, "tray icon_size takes an integer"); return;
        }
        long v = (long)c->call.args[i]->i;
        if (v < 8 || v > 64) { diag_error(c->loc, "tray icon_size must be 8..64"); return; }
        r->tray_icon_px = (int)v;
    }
}

/* `notifications(history=N)` — ring depth. Each entry is ~460 B of BSS that
 * stays unbacked until notifications arrive, and it also sizes the per-cell
 * st[]/hit arrays in every generated surface, so it stays compile-time. */
int notif_hist_cap = NOTIF_HIST_CAP;
int notif_image_px;
static void read_notif_history(Expr *c) {
    for (int i = 0; i < c->call.nargs; i++) {
        const char *kn = c->call.argnames ? c->call.argnames[i] : NULL;
        if (kn && c->call.anlen[i] == 7 && !memcmp(kn, "history", 7)) {
            if (c->call.args[i]->kind != EX_INT) {
                diag_error(c->loc, "notifications history takes an integer"); return;
            }
            long v = (long)c->call.args[i]->i;
            if (v < 1 || v > 128) { diag_error(c->loc, "notifications history must be 1..128"); return; }
            notif_hist_cap = (int)v;
        } else if (kn && c->call.anlen[i] == 5 && !memcmp(kn, "image", 5)) {
            if (c->call.args[i]->kind != EX_INT) {
                diag_error(c->loc, "notifications image takes an integer (px)"); return;
            }
            long v = (long)c->call.args[i]->i;
            if (v < 0 || v > 128) { diag_error(c->loc, "notifications image must be 0..128"); return; }
            notif_image_px = (int)v;
        }
    }
}

/* ---------- expression walk ---------- */
static void walk_expr(S *s, Expr *e);

static void walk_expr(S *s, Expr *e) {
    if (!e) return;
    switch (e->kind) {
    case EX_INT: case EX_FLOAT: case EX_STRING: case EX_BOOL: case EX_COLOR:
        return;
    case EX_INTERP:
        for (int i = 0; i < e->interp.nparts; i++)
            if (e->interp.parts[i].is_expr) walk_expr(s, e->interp.parts[i].expr);
        return;
    case EX_DOLLAR:
        if (!s->in_template) {
            diag_error(e->loc, "'$' template arg used outside spawned_by surface");
            return;
        }
        /* Record first-seen $name as a template parameter. */
        {
            const char *n = e->dollar.s; size_t L = e->dollar.n;
            int dup = 0;
            for (int i = 0; i < s->ntargs; i++)
                if (strlen(s->targs[i]) == L && memcmp(s->targs[i], n, L) == 0) { dup = 1; break; }
            if (!dup) {
                if (s->ntargs >= s->captargs) {
                    int nc = s->captargs ? s->captargs * 2 : 8;
                    const char **na = arena_alloc(s->a, sizeof(char*) * (size_t)nc);
                    for (int i = 0; i < s->ntargs; i++) na[i] = s->targs[i];
                    s->targs = na; s->captargs = nc;
                }
                s->targs[s->ntargs++] = arena_strn(s->a, n, L);
            }
        }
        return;
    case EX_CALL:
        /* A call in a value position (prop RHS, interp, ternary, emit/set/animate
         * arg) has no lowering — codegen used to reject it late. Source RHS and
         * animate easing are handled elsewhere and never reach here. */
        diag_error(e->loc, "'%.*s(...)' is a call, not a value — calls are only allowed as a `source` right-hand side",
                   (int)e->call.nlen, e->call.name);
        diag_hint(e->loc, "read a source's field instead, e.g. `source x = %.*s(...);` then use `x` or `x.field`",
                  (int)e->call.nlen, e->call.name);
        return;
    case EX_BIN: walk_expr(s, e->bin.l); walk_expr(s, e->bin.r); return;
    case EX_UN:  walk_expr(s, e->un.e); return;
    case EX_RANGE: walk_expr(s, e->range.lo); walk_expr(s, e->range.hi); return;
    case EX_TERN:walk_expr(s, e->tern.cond); walk_expr(s, e->tern.t); walk_expr(s, e->tern.e); return;
    case EX_IDENT: {
        const char *n = e->ident.s; size_t L = e->ident.n;
        if (is_local(s, n, L)) return;
        Decl *d = find_decl_in(s->s.src, s->s.nsrc, n, L);
        if (d) { add_dep(s, d->name); return; }
        d = find_decl_in(s->s.kon, s->s.nkon, n, L);
        if (d) { if (d->kind == D_MUT) add_dep(s, d->name); return; }
        if (find_decl_in(s->s.sur, s->s.nsur, n, L)) return;
        /* Built-in enum identifiers (anchor/layer/align values). */
        static const char *ENUMS[] = {
            "top","bottom","left","right","center",
            "background","overlay",
            "none",
            "vertical","horizontal",
            "start","end",                         /* alignment aliases */
            "bar","pill","circle","disc","knob",   /* slider thumb_shape */
            "active",                              /* surface `output = active` */
            NULL
        };
        /* `for row in rows` — a menu's visible filtered rows; the row source
         * is the surface's own state, not a declared source. */
        if (L == 4 && memcmp(n, "rows", 4) == 0) return;
        for (int i = 0; ENUMS[i]; i++)
            if (strlen(ENUMS[i]) == L && memcmp(ENUMS[i], n, L) == 0) return;
        diag_error(e->loc, "undefined identifier '%.*s'", (int)L, n);
        return;
    }
    case EX_MEMBER: {
        Expr *b = e->member.base;
        /* base must be IDENT — restrict to source.field or local.field */
        if (b->kind != EX_IDENT) { walk_expr(s, b); return; }
        const char *bn = b->ident.s; size_t bL = b->ident.n;
        if (is_local(s, bn, bL)) return;     /* loop var: any field ok */
        /* Built-in `anim.emerged_h` / `anim.emerged_w`: runtime body-emerged
         * extent along the slide axis (lowered in codegen_expr.c). */
        if (bL == 4 && memcmp(bn, "anim", 4) == 0) return;
        /* `menu.query` / `.prompt` / `.count` inside a menu's body. */
        if (bL == 4 && memcmp(bn, "menu", 4) == 0) return;
        /* `polkit.message` / `.prompt` / `.dots` / `.user` / `.error` /
         * `.failed` inside the `spawned_by = polkit` template's body. */
        if (bL == 6 && memcmp(bn, "polkit", 6) == 0) return;
        /* `greet.prompt` / `.dots` / `.input` / `.user` / `.error` /
         * `.session` / `.failed` / `.busy` / `.caps`, and the `.sessions` for-list,
         * inside the `spawned_by = greet` surface's body. */
        if (bL == 5 && memcmp(bn, "greet", 5) == 0) return;
        Decl *d = find_decl_in(s->s.src, s->s.nsrc, bn, bL);
        if (d) {
            /* Validate field against source schema. */
            if (d->source.call && d->source.call->kind == EX_CALL) {
                const SrcDef *sd = find_src(d->source.call->call.name, d->source.call->call.nlen);
                if (sd && !field_ok(sd, e->member.field, e->member.flen)) {
                    diag_error(e->loc, "source '%s' has no field '%.*s'", d->name, (int)e->member.flen, e->member.field);
                    diag_hint(e->loc, "%s exposes: %s", sd->name, sd->fields);
                } else if (sd) {
                    /* list/history/items are list fields — only an iterable in a
                     * `for` head, never a scalar value. Codegen's lower_member
                     * rejects them (e.g. dbus history is post-v0); catch it here
                     * so --check and --emit agree. */
                    const char *f = e->member.field; size_t fl = e->member.flen;
                    if (!s->in_for_iter &&
                        ((fl == 4 && memcmp(f, "list", 4) == 0) ||
                         (fl == 7 && memcmp(f, "history", 7) == 0) ||
                         (fl == 5 && memcmp(f, "items", 5) == 0))) {
                        diag_error(e->loc, "'%s.%.*s' is a list — use it only in `for x in %s.%.*s`",
                                   d->name, (int)fl, f, d->name, (int)fl, f);
                    }
                }
                /* net rates are the one polled thing in an otherwise
                 * event-driven source: only a config that actually reads
                 * them puts net() on the shared tick (idle = 0 CPU). */
                if (sd && !strcmp(sd->name, "net") &&
                    (nameq(e->member.field, e->member.flen, "rx_kbps") ||
                     nameq(e->member.field, e->member.flen, "tx_kbps")))
                    s->r->net_rates_used = 1;
            }
            add_dep(s, d->name);
            return;
        }
        diag_error(b->loc, "undefined identifier '%.*s'", (int)bL, bn);
        return;
    }
    }
}

/* ---------- stmt walk ---------- */
static void walk_stmt(S *s, Stmt *st) {
    if (!st) return;
    switch (st->kind) {
    case ST_EXEC: walk_expr(s, st->exec.arg); return;
    case ST_SET: {
        Decl *d = find_decl_in(s->s.kon, s->s.nkon, st->set.name, st->set.nlen);
        if (!d) {
            /* Allow set on a source: codegen handles exec_line sources as a
             * direct overwrite of the polled line buffer (optimistic update). */
            Decl *src = find_decl_in(s->s.src, s->s.nsrc, st->set.name, st->set.nlen);
            if (!src) diag_error(st->loc, "undefined identifier '%s' in set()", st->set.name);
        } else if (d->kind != D_MUT) {
            diag_error(st->loc, "'%s' is const, not mut", st->set.name);
            diag_hint(d->loc, "declare it `mut %s = ...` to allow set()", st->set.name);
        }
        walk_expr(s, st->set.val);
        return;
    }
    case ST_EMIT: {
        /* validate target surface exists (unless it's a reserved sink, e.g. menu_reply). */
        Decl *tgt = find_decl_in(s->s.sur, s->s.nsur, st->emit.name, st->emit.nlen);
        if (!tgt && !nameq(st->emit.name, st->emit.nlen, "menu_reply"))
            diag_error(st->loc, "emit target surface '%s' not declared", st->emit.name);
        else if (tgt) {
            /* Only a spawned_by template has a spawn_<name>() to call; emitting
             * to a plain surface used to fail at link, not --check. */
            bool tmpl = false;
            for (int k = 0; k < tgt->surface.n; k++) {
                SBody *sb = &tgt->surface.items[k];
                if (sb->kind == SB_PROP && nameq(sb->prop->name, sb->prop->nlen, "spawned_by"))
                    { tmpl = true; break; }
            }
            if (!tmpl) {
                diag_error(st->loc, "emit target '%s' is not a spawned_by template", st->emit.name);
                diag_hint(st->loc, "only osd/menu templates (surfaces with `spawned_by = ...`) can be emit()ed");
            }
        }
        for (int i = 0; i < st->emit.n; i++) walk_expr(s, st->emit.val[i]);
        return;
    }
    case ST_BLOCK:
        for (int i = 0; i < st->block.n; i++) walk_stmt(s, st->block.list[i]);
        return;
    case ST_ANIMATE: {
        Decl *d = find_decl_in(s->s.kon, s->s.nkon, st->anim.name, st->anim.nlen);
        if (!d) diag_error(st->loc, "undefined identifier '%s' in animate()", st->anim.name);
        else if (d->kind != D_MUT) {
            diag_error(st->loc, "animate target '%s' is const, not mut", st->anim.name);
            diag_hint(d->loc, "only `mut` values animate; declare `mut %s = ...`", st->anim.name);
        }
        walk_expr(s, st->anim.to);
        walk_expr(s, st->anim.duration);
        if (st->anim.repeat) {
            walk_expr(s, st->anim.repeat);
            Ty rt = ty_of(s, st->anim.repeat);
            if (rt == TY_STR || rt == TY_COLOR)
                diag_error(st->anim.repeat->loc,
                           "animate() repeat= must be a number, got %s", ty_name(rt));
        }
        /* easing is bare ident or a call (cubic_bezier(...)); allow without normal undef-check */
        if (st->anim.easing && st->anim.easing->kind == EX_CALL)
            for (int i = 0; i < st->anim.easing->call.nargs; i++)
                walk_expr(s, st->anim.easing->call.args[i]);
        s->r->has_anim = true;
        return;
    }
    }
}

/* ---------- containers ---------- */
static void walk_widget(S *s, Widget *w);

static void walk_for(S *s, ForBlock *f) {
    /* Only `rows` and a source's list/history/items field iterate; anything
     * else was a codegen-only error before, so --check missed it. */
    Expr *it = f->iter;
    bool iter_ok =
        (it && it->kind == EX_IDENT && nameq(it->ident.s, it->ident.n, "rows")) ||
        (it && it->kind == EX_MEMBER && it->member.base->kind == EX_IDENT &&
         find_decl_in(s->s.src, s->s.nsrc, it->member.base->ident.s, it->member.base->ident.n) &&
         (nameq(it->member.field, it->member.flen, "list") ||
          nameq(it->member.field, it->member.flen, "history") ||
          nameq(it->member.field, it->member.flen, "items"))) ||
        /* `<x>.sessions` — greeter session list, surface state like `rows` */
        (it && it->kind == EX_MEMBER && it->member.base->kind == EX_IDENT &&
         nameq(it->member.field, it->member.flen, "sessions"));
    if (it && !iter_ok)
        diag_error(it->loc, "for-iter must be `rows`, <tags-src>.list, "
                   "<dbus_signal-src>.history, <tray-src>.items or "
                   "<greet-surface>.sessions");
    s->in_for_iter = true;
    walk_expr(s, f->iter);
    s->in_for_iter = false;
    push_local(s, f->var, f->vlen);
    for (int i = 0; i < f->ncells; i++) walk_widget(s, f->cells[i]);
    pop_local(s);
}

static void walk_widget(S *s, Widget *w) {
    if (!w) return;
    for (int i = 0; i < w->nitems; i++) {
        WBody *b = &w->items[i];
        switch (b->kind) {
        case WB_PROP:
            check_prop("widget", b->prop);
            typecheck_prop(s, "widget", b->prop);
            if (b->prop->nlen == 5 && memcmp(b->prop->name, "image", 5) == 0)
                s->r->has_image = true;
            if (b->prop->nlen == 7 && memcmp(b->prop->name, "tooltip", 7) == 0) {
                if (!s->tip_prop) s->tip_prop = b->prop;
                /* The hit table holds the pointer across frames, so it has to
                 * be .rodata — an interpolated string is a render-local buffer. */
                if (!b->prop->val || b->prop->val->kind != EX_STRING)
                    diag_error(b->prop->loc, "'tooltip' must be a literal string");
            }
            /* transition_easing accepts a bare easing ident (ease_in/ease_out/
             * ease_in_out/linear); don't run the undef-ident check on it. */
            if (b->prop->nlen == 17 && memcmp(b->prop->name, "transition_easing", 17) == 0) {
                s->r->has_anim = true;
                break;
            }
            /* Step 6.3: enter_easing / exit_easing also accept bare easing idents. */
            if ((b->prop->nlen == 12 && memcmp(b->prop->name, "enter_easing", 12) == 0) ||
                (b->prop->nlen == 11 && memcmp(b->prop->name, "exit_easing",  11) == 0)) {
                s->r->has_anim = true;
                break;
            }
            /* transition_<colour> pulls in the anim module so the tween runtime
             * is linked even without an explicit animate() call. */
            if (b->prop->nlen > 11 && memcmp(b->prop->name, "transition_", 11) == 0)
                s->r->has_anim = true;
            if ((b->prop->nlen == 10 && memcmp(b->prop->name, "enter_anim", 10) == 0) ||
                (b->prop->nlen == 9  && memcmp(b->prop->name, "exit_anim",  9)  == 0))
                s->r->has_anim = true;
            walk_expr(s, b->prop->val);
            break;
        case WB_ONCLICK:
        case WB_ONCHANGE:
        case WB_ONRCLICK:
        case WB_ONMCLICK:
            if (b->click.param && b->click.plen) push_local(s, b->click.param, b->click.plen);
            if (b->click.param2 && b->click.plen2) push_local(s, b->click.param2, b->click.plen2);
            walk_stmt(s, b->click.body);
            if (b->click.param2 && b->click.plen2) pop_local(s);
            if (b->click.param && b->click.plen) pop_local(s);
            break;
        case WB_FOR:
            walk_for(s, b->forb);
            break;
        }
    }
}

/* Resolve `edge = top|bottom|left|right;` and `size = N;` on each region,
 * validate edges are distinct and consistent with the compound anchor mask. */
static int resolve_edge_ident(Expr *e) {
    if (!e || e->kind != EX_IDENT) return -1;
    const char *s = e->ident.s; size_t n = e->ident.n;
    if (n == 3 && !memcmp(s, "top",    3)) return 1;
    if (n == 6 && !memcmp(s, "bottom", 6)) return 2;
    if (n == 4 && !memcmp(s, "left",   4)) return 4;
    if (n == 5 && !memcmp(s, "right",  5)) return 8;
    return -1;
}
static int eval_anchor_mask(Expr *e) {
    if (!e) return -1;
    int v = resolve_edge_ident(e);
    if (v > 0) return v;
    if (e->kind == EX_BIN && e->bin.op == OP_BITOR) {
        int a = eval_anchor_mask(e->bin.l), b = eval_anchor_mask(e->bin.r);
        if (a < 0 || b < 0) return -1;
        return a | b;
    }
    if (e->kind == EX_INT) return (int)e->i;
    return -1;
}
static int prop_flag(WBody *items, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (items[i].kind == WB_PROP && strcmp(items[i].prop->name, name) == 0) return 1;
    return 0;
}
static int group_prop_flag(Group *g, const char *name) {
    for (int i = 0; i < g->nprops; i++)
        if (strcmp(g->props[i]->name, name) == 0) return 1;
    return 0;
}

/* `scroll` shifts the start-aligned item stack along Y, so it is meaningless
 * without `axis = vertical` — catching it here means `--check` reports it, not
 * a codegen backstop nobody runs. */
static void validate_scroll(Decl *d) {
    Prop *sc = NULL; int vertical = 0;
    Prop *unf = NULL; int has_esc = 0;
    for (int i = 0; i < d->surface.n; i++) {
        SBody *b = &d->surface.items[i];
        if (b->kind != SB_PROP) continue;
        if (strcmp(b->prop->name, "output") == 0) {
            Expr *v = b->prop->val;
            if (!(v && v->kind == EX_IDENT && v->ident.n == 6
                  && memcmp(v->ident.s, "active", 6) == 0))
                diag_error(b->prop->val ? b->prop->val->loc : d->loc,
                           "`output` takes only `active` (the monitor whose click "
                           "opened the surface); omit it for one copy per output");
        }
        if (strcmp(b->prop->name, "dismiss_on_unfocus") == 0) unf = b->prop;
        else if (strcmp(b->prop->name, "on_escape") == 0) has_esc = 1;
        if (strcmp(b->prop->name, "scroll") == 0) sc = b->prop;
        else if (strcmp(b->prop->name, "axis") == 0)
            vertical = b->prop->val && b->prop->val->kind == EX_IDENT
                    && b->prop->val->ident.n == 8
                    && memcmp(b->prop->val->ident.s, "vertical", 8) == 0;
    }
    /* Both mean "close me"; unfocus reuses the on_escape command rather than
     * duplicating it, so it needs one to run. */
    if (unf && !has_esc)
        diag_error(d->loc, "`dismiss_on_unfocus` needs `on_escape = \"<cmd>\"` — "
                           "it runs that same command when the surface loses focus");
    if (sc && !vertical)
        diag_error(sc->val ? sc->val->loc : d->loc,
                   "`scroll` needs `axis = vertical;` on the same surface — "
                   "a horizontal surface stacks along x, which scroll doesn't move");
    if (!sc) return;
    /* `sticky` pins a row above the scrolled region, which only works for a
     * LEADING run of rows: the scrolled stack starts where that run ends. */
    int seen_scrolling = 0;
    for (int i = 0; i < d->surface.n; i++) {
        SBody *b = &d->surface.items[i];
        int sticky = 0; Loc loc = d->loc;
        if (b->kind == SB_WIDGET) { sticky = prop_flag(b->widget->items, b->widget->nitems, "sticky"); loc = b->widget->loc; }
        else if (b->kind == SB_GROUP) { sticky = group_prop_flag(b->group, "sticky"); loc = b->group->loc; }
        else continue;
        if (!sticky) { seen_scrolling = 1; continue; }
        if (seen_scrolling) {
            diag_error(loc, "`sticky` only works on the leading rows of a scrollable surface — "
                            "a pinned row below scrolling ones would be overrun by them");
            return;
        }
    }
}

static void validate_compound_regions(Decl *d) {
    int anchor = -1;
    for (int i = 0; i < d->surface.n; i++) {
        SBody *b = &d->surface.items[i];
        if (b->kind == SB_PROP && strcmp(b->prop->name, "anchor") == 0)
            anchor = eval_anchor_mask(b->prop->val);
    }
    int seen = 0, n_regions = 0;
    for (int i = 0; i < d->surface.n; i++) {
        SBody *b = &d->surface.items[i];
        if (b->kind != SB_REGION) continue;
        Region *rg = b->region;
        n_regions++;
        Expr *ee = NULL, *se = NULL;
        for (int k = 0; k < rg->nitems; k++) {
            SBody *rb = &rg->items[k];
            if (rb->kind != SB_PROP) continue;
            if (strcmp(rb->prop->name, "edge") == 0) ee = rb->prop->val;
            else if (strcmp(rb->prop->name, "size") == 0) se = rb->prop->val;
        }
        int edge = resolve_edge_ident(ee);
        if (edge < 0) { diag_error(rg->loc, "region '%s' needs edge = top|bottom|left|right", rg->name); continue; }
        if (!se || se->kind != EX_INT) { diag_error(rg->loc, "region '%s' needs integer size", rg->name); continue; }
        if (seen & edge) { diag_error(rg->loc, "region '%s' edge duplicates another region (multi-region-per-edge unsupported)", rg->name); continue; }
        if (anchor >= 0 && !(anchor & edge))
            diag_error(rg->loc, "region '%s' edge not in compound anchor", rg->name);
        rg->edge = edge;
        rg->size = (int)se->i;
        seen |= edge;
    }
    if (n_regions == 0) diag_error(d->loc, "compound '%s' needs at least one region", d->name);
}

/* ---------- per-surface pass ---------- */
static void analyze_surface(S *s, Decl *d) {
    if (d->kind == D_COMPOUND) validate_compound_regions(d);
    s->cur_surface = d;
    s->ndeps = 0;
    s->in_template = false;
    /* First: detect spawned_by template + reveal_on_hover + exclusive_zone. */
    int excl = 0;
    bool has_excl_negative = false;
    bool has_hud = false;
    for (int i = 0; i < d->surface.n; i++) {
        SBody *b = &d->surface.items[i];
        if (b->kind == SB_PROP) {
            const char *pn = b->prop->name;
            if (strcmp(pn, "spawned_by") == 0) {
                s->in_template = true;
                /* `spawned_by = tooltip` compiles src/tooltip.c in — the engine
                 * exists only when a config declares the surface. */
                Expr *v = b->prop->val;
                if (v && v->kind == EX_IDENT && v->ident.n == 7
                    && memcmp(v->ident.s, "tooltip", 7) == 0)
                    s->r->has_tooltip = true;
                /* same deal for src/polkit.c: the agent registers with polkitd
                 * only when a config declares the prompt surface */
                if (v && v->kind == EX_IDENT && v->ident.n == 6
                    && memcmp(v->ident.s, "polkit", 6) == 0)
                    s->r->has_polkit = true;
                /* and src/greet.c: the greetd client connects only when a
                 * config declares the login surface */
                if (v && v->kind == EX_IDENT && v->ident.n == 5
                    && memcmp(v->ident.s, "greet", 5) == 0)
                    s->r->has_greet = true;
            }
            else if (strcmp(pn, "reveal_on_hover") == 0) has_hud = true;
            else if (strcmp(pn, "reveal_anim_ms") == 0)  s->r->has_anim = true;
            else if (strcmp(pn, "exclusive_zone") == 0) {
                if (b->prop->val && b->prop->val->kind == EX_INT) {
                    excl = (int)b->prop->val->i;
                    if (excl < 0) has_excl_negative = true;
                    else if (excl > 0) s->r->has_bar = true;
                }
            }
        }
    }
    if (has_hud)        s->r->has_hud = true;
    if (has_excl_negative) s->r->has_menu = true;
    if (nameq(d->name, d->nlen, "osd")) { s->r->has_osd = true; s->r->has_dbus = true; }

    /* Walk all properties / widgets / for blocks. */
    for (int i = 0; i < d->surface.n; i++) {
        SBody *b = &d->surface.items[i];
        switch (b->kind) {
        case SB_PROP:
            check_prop("surface", b->prop);
            typecheck_prop(s, "surface", b->prop);
            /* reveal_easing accepts a bare easing ident (Step 6.2). */
            if (b->prop->nlen == 13 && memcmp(b->prop->name, "reveal_easing", 13) == 0)
                break;
            /* spawned_by names an engine (osd / osd_pill / menu), not a decl. */
            if (b->prop->nlen == 10 && memcmp(b->prop->name, "spawned_by", 10) == 0)
                break;
            /* keyboard's idents are checked against E_KBD by typecheck_prop;
               walking them would only report on_demand/exclusive as undefined */
            if (b->prop->nlen == 8 && memcmp(b->prop->name, "keyboard", 8) == 0)
                break;
            walk_expr(s, b->prop->val);
            break;
        case SB_WIDGET: walk_widget(s, b->widget); break;
        case SB_FOR:    walk_for(s, b->forb); break;
        case SB_GROUP: {
            Group *g = b->group;
            for (int k = 0; k < g->nprops; k++) { check_prop("group", g->props[k]); typecheck_prop(s, "group", g->props[k]); walk_expr(s, g->props[k]->val); }
            for (int k = 0; k < g->nmembers; k++) {
                if (g->fors && g->fors[k]) walk_for(s, g->fors[k]);
                else walk_widget(s, g->members[k]);
            }
            break;
        }
        case SB_REGION: {
            Region *rg = b->region;
            for (int k = 0; k < rg->nitems; k++) {
                SBody *rb = &rg->items[k];
                switch (rb->kind) {
                case SB_PROP:   check_prop("surface", rb->prop); typecheck_prop(s, "surface", rb->prop); walk_expr(s, rb->prop->val); break;
                case SB_WIDGET: walk_widget(s, rb->widget); break;
                case SB_FOR:    walk_for(s, rb->forb); break;
                case SB_REGION: case SB_GROUP: break;
                }
            }
            break;
        }
        }
    }
    /* Record deps in result. */
    int idx = s->r->nsurfaces++;
    s->r->surface_names[idx] = d->name;
    const char **deps = arena_alloc(s->a, sizeof(char*) * (size_t)(s->ndeps + 1));
    for (int i = 0; i < s->ndeps; i++) deps[i] = s->deps[i];
    deps[s->ndeps] = NULL;
    s->r->surface_deps[idx] = deps;
    /* Record spawned-by template args in first-seen order. */
    if (s->in_template) {
        int tidx = s->r->nspawned++;
        s->r->spawned_names[tidx] = d->name;
        const char **a = arena_alloc(s->a, sizeof(char*) * (size_t)(s->ntargs + 1));
        for (int i = 0; i < s->ntargs; i++) a[i] = s->targs[i];
        a[s->ntargs] = NULL;
        s->r->spawned_args[tidx] = a;
    }
    s->ntargs = 0;  /* reset per-surface */
}

/* ---------- top-level ---------- */
SemaResult *sema_check(Arena *a, Unit *u) {
    S s = { .a = a, .u = u };
    s.r = arena_alloc(a, sizeof *s.r);

    /* Pass 1: collect names; detect duplicate declarations. */
    int cap_src=0, cap_sur=0, cap_kon=0;
    for (int i = 0; i < u->n; i++) {
        Decl *d = u->decls[i];
        switch (d->kind) {
        case D_SOURCE:   cap_src++; break;
        case D_SURFACE:  cap_sur++; break;
        case D_COMPOUND: cap_sur++; break;
        case D_CONST:
        case D_MUT:      cap_kon++; break;
        default: break;
        }
    }
    s.s.src = arena_alloc(a, sizeof(Decl*) * (size_t)(cap_src ? cap_src : 1));
    s.s.sur = arena_alloc(a, sizeof(Decl*) * (size_t)(cap_sur ? cap_sur : 1));
    s.s.kon = arena_alloc(a, sizeof(Decl*) * (size_t)(cap_kon ? cap_kon : 1));
    for (int i = 0; i < u->n; i++) {
        Decl *d = u->decls[i];
        switch (d->kind) {
        case D_SOURCE:
            if (find_decl_in(s.s.src, s.s.nsrc, d->name, d->nlen))
                diag_error(d->loc, "duplicate source '%s'", d->name);
            s.s.src[s.s.nsrc++] = d;
            /* Validate RHS is a known built-in. */
            if (d->source.call && d->source.call->kind == EX_CALL) {
                const SrcDef *sd = find_src(d->source.call->call.name, d->source.call->call.nlen);
                if (!sd) {
                    const char *nm = d->source.call->call.name;
                    size_t nl = d->source.call->call.nlen;
                    diag_error(d->source.call->loc, "unknown built-in source '%.*s'", (int)nl, nm);
                    const char *best = NULL; int bd = 3;
                    for (size_t k = 0; k < sizeof SOURCES / sizeof SOURCES[0]; k++) {
                        int dist = edit_dist(nm, nl, SOURCES[k].name, strlen(SOURCES[k].name));
                        if (dist < bd) { bd = dist; best = SOURCES[k].name; }
                    }
                    if (best) diag_hint(d->source.call->loc, "did you mean '%s'?", best);
                }
                else {
                    set_flag(s.r, sd->flag);
                    check_source_args(sd, d->source.call);
                    if (sd->flag == F_TRAY) read_tray_icon_size(s.r, d->source.call);
                    if (!strcmp(sd->name, "notifications")) read_notif_history(d->source.call);
                }
            }
            break;
        case D_SURFACE:
        case D_COMPOUND:
            /* A menu carries a widget body but stays out of the surface
             * pipeline: menu.c owns its lifecycle, not surface_create. */
            if (d->is_menu) { s.r->has_menu = 1; break; }
            if (find_decl_in(s.s.sur, s.s.nsur, d->name, d->nlen))
                diag_error(d->loc, "duplicate surface '%s'", d->name);
            s.s.sur[s.s.nsur++] = d;
            if (d->kind == D_SURFACE) validate_scroll(d);
            if (d->kind == D_COMPOUND) s.r->has_bar = true;
            break;
        case D_CONST:
        case D_MUT:
            if (find_decl_in(s.s.kon, s.s.nkon, d->name, d->nlen))
                diag_error(d->loc, "duplicate '%s'", d->name);
            s.s.kon[s.s.nkon++] = d;
            break;
        case D_LOCK:
            if (s.s.lock)  diag_error(d->loc, "duplicate lock block");
            s.s.lock = d;  s.r->has_lock = 1;
            for (int j = 0; j < d->block.n; j++) check_prop("lock", d->block.props[j]);
            for (int j = 0; j < d->block.nels; j++) {
                LockElem *e = d->block.els[j];
                static const char *sch[] = { "lock_frame", "lock_text", "lock_ring" };
                for (int k = 0; k < e->n; k++) {
                    Prop *p = e->props[k];
                    check_prop(sch[e->kind], p);
                    if (e->kind != LK_RING || p->nlen != 8 ||
                        memcmp(p->name, "segments", 8) != 0) continue;
                    if (!p->val || p->val->kind != EX_INT) continue;
                    if (p->val->i < 1 || p->val->i > 360)
                        diag_error(p->val->loc,
                                   "lock ring segments=%lld out of range — 1..360",
                                   (long long)p->val->i);
                }
            }
            break;
        case D_GAMMA:
            if (s.s.gamma) diag_error(d->loc, "duplicate gamma block");
            s.s.gamma = d; s.r->has_gamma = 1;
            for (int j = 0; j < d->block.n; j++) check_prop("gamma", d->block.props[j]);
            break;
        case D_WALLPAPER: {
            if (s.s.wall) diag_error(d->loc, "duplicate wallpaper block");
            s.s.wall = d;  s.r->has_wallpaper = 1;
            /* A crossfade is the wall's own animation — it needs anim.o linked
             * even when no widget animates. fade_ms defaults to 300 (config.h),
             * so a wallpaper animates unless it explicitly opts out with 0. */
            int fade = 1;
            for (int i = 0; i < d->block.n; i++) {
                Prop *p = d->block.props[i];
                check_prop("wallpaper", p);
                if (p->nlen == 7 && memcmp(p->name, "fade_ms", 7) == 0
                    && p->val->kind == EX_INT && p->val->i == 0) fade = 0;
            }
            if (fade) s.r->has_anim = true;
            break;
        }
        case D_IDLE: {
            if (s.s.idle) diag_error(d->loc, "duplicate idle block");
            s.s.idle = d; s.r->has_idle = 1;
            for (int j = 0; j < d->block.n; j++) {
                Prop *p = d->block.props[j];
                check_prop("idle", p);
                if (p->nlen != 12 || memcmp(p->name, "before_sleep", 12) != 0) continue;
                int ok = p->val && (p->val->kind == EX_STRING ||
                        (p->val->kind == EX_IDENT && p->val->ident.n == 4 &&
                         memcmp(p->val->ident.s, "lock", 4) == 0));
                if (!ok)
                    diag_error(p->loc, "idle before_sleep must be the builtin `lock` or a shell command string");
            }
            for (int j = 0; j < d->block.nels; j++) {
                LockElem *e = d->block.els[j];
                if (!e->name) { diag_error(e->loc, "idle timeout needs a name"); continue; }
                for (int k = 0; k < j; k++) {
                    LockElem *o = d->block.els[k];
                    if (o->name && o->nlen == e->nlen && memcmp(o->name, e->name, e->nlen) == 0)
                        diag_error(e->loc, "duplicate idle timeout '%s'", e->name);
                }
                Prop *after = NULL;
                for (int k = 0; k < e->n; k++) {
                    Prop *p = e->props[k];
                    check_prop("idle_timeout", p);
                    if (p->nlen == 5 && memcmp(p->name, "after", 5) == 0) after = p;
                    else if (((p->nlen == 3 && !memcmp(p->name, "run", 3)) ||
                              (p->nlen == 6 && !memcmp(p->name, "resume", 6))) &&
                             p->val && p->val->kind != EX_STRING)
                        diag_error(p->loc, "idle timeout %s must be a shell command string", p->name);
                }
                if (!after)
                    diag_error(e->loc, "idle timeout '%s' needs `after = <duration>;`", e->name);
                else if (!after->val || after->val->kind != EX_INT || after->val->i <= 0)
                    diag_error(after->loc, "idle timeout after must be a positive duration (e.g. 300s)");
            }
            break;
        }
        case D_MEDIA:
            s.r->has_media = 1; s.r->has_pipewire = 1;   /* media keys read/write via pipewire.c */
            for (int j = 0; j < d->block.n; j++) check_prop("media", d->block.props[j]);
            break;
        case D_STYLE:     break;   /* stripped by style_apply */
        }
    }

    s.r->surface_names = arena_alloc(a, sizeof(char*) * (size_t)(s.s.nsur ? s.s.nsur : 1));
    s.r->surface_deps  = arena_alloc(a, sizeof(char**) * (size_t)(s.s.nsur ? s.s.nsur : 1));
    s.r->spawned_names = arena_alloc(a, sizeof(char*) * (size_t)(s.s.nsur ? s.s.nsur : 1));
    s.r->spawned_args  = arena_alloc(a, sizeof(char**) * (size_t)(s.s.nsur ? s.s.nsur : 1));

    /* Pass 2: walk const/mut initializers (no deps recorded — they're not per-surface). */
    for (int i = 0; i < s.s.nkon; i++) {
        walk_expr(&s, s.s.kon[i]->konst.val);
        typecheck_expr(&s, s.s.kon[i]->konst.val);
    }

    /* Pass 2b: source on_change() bodies. Deps recorded here belong to no
     * surface, so drop them before the per-surface pass snapshots its own. */
    for (int i = 0; i < s.s.nsrc; i++) {
        Decl *d = s.s.src[i];
        if (!d->source.on_change) continue;
        if (d->source.hkind != WB_ONCHANGE)
            diag_error(d->source.hloc, "source body allows only on_change()");
        walk_stmt(&s, d->source.on_change);
    }
    s.ndeps = 0;

    /* Pass 3: per-surface analysis. */
    for (int i = 0; i < s.s.nsur; i++) analyze_surface(&s, s.s.sur[i]);

    /* OSD/menu anchor to the focused monitor; wl_toplevel.c is the only
     * compositor-agnostic source of that (activated toplevel's output). */
    if (s.r->has_osd || s.r->has_menu) s.r->has_toplevel = 1;

    /* `tooltip =` with nothing to draw it would silently do nothing at
     * runtime — the engine only exists when the template is declared. */
    if (s.tip_prop && !s.r->has_tooltip)
        diag_error(s.tip_prop->loc,
                   "'tooltip' needs a `surface … { spawned_by = tooltip; … }` to draw it");

    free(s.deps);
    return s.r;
}

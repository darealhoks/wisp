#include "wispc.h"
#include <string.h>
#include <stdlib.h>

/* Built-in source schema: name → feature macro to enable + valid fields. */
typedef struct {
    const char *name;
    const char *primary;            /* default field (for bare-ident access) */
    const char *fields;             /* space-separated list of valid fields */
    /* which SemaResult flag(s) to set */
    int flag;                       /* see F_* below */
} SrcDef;

enum {
    F_NONE = 0,
    F_CLOCK, F_CPU, F_MEM, F_TEMP, F_BAT, F_WIFI, F_DISK, F_VPN,
    F_TAGS, F_EXEC, F_DBUS,
};

/* A row here without a driver in codegen_sources.c makes --check pass and
 * --emit die — add the row in the same commit as the driver, never before.
 * Wanted but undriven: mpris (dbus.c already speaks the wire), inotify (a
 * poll-free file source, unlike exec_line). */
static const SrcDef SOURCES[] = {
    {"clock",                "value",  "value", F_CLOCK },
    {"cpu",                  "pct",    "pct load1", F_CPU },
    {"mem",                  "pct",    "pct used_mb", F_MEM },
    {"temp",                 "c",      "c", F_TEMP },
    {"bat",                  "pct",    "pct charging", F_BAT },
    {"wifi",                 "ssid",   "ssid signal", F_WIFI },
    {"disk",                 "pct",    "pct free_gb", F_DISK },
    {"vpn",                  "state",  "state ok", F_VPN },
    {"tags",             "title",  "title list occ act urg", F_TAGS },
    {"gamma_warm",           "value",  "value", F_NONE },
    {"dnd",                  "value",  "value", F_NONE },
    {"ui_hidden",            "value",  "value", F_NONE },
    {"exec_line",            "value",  "value", F_EXEC },
    {"dbus_signal",          "value",  "value history", F_DBUS },
};

static const SrcDef *find_src(const char *name, size_t n) {
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

/* ---------- symbol table ---------- */
typedef struct {
    Decl **src;     int nsrc;
    Decl **sur;     int nsur;
    Decl **kon;     int nkon;    /* const + mut */
    Decl *lock, *gamma, *wall;
} Syms;

typedef struct ScopeEnt {
    const char *name; size_t nlen;
    struct ScopeEnt *next;
} ScopeEnt;

typedef struct {
    SemaResult *r;
    Syms s;
    Arena *a;
    Unit *u;
    /* current analysis scope */
    Decl *cur_surface;
    bool  in_template;       /* inside a spawned_by surface */
    ScopeEnt *locals;        /* for-vars, in scope */
    /* deps for current surface, dedup */
    const char **deps; int ndeps, capdeps;
    /* spawned-by template args for current template, dedup, first-seen order */
    const char **targs; int ntargs, captargs;
    /* set of source decl-names allowed (for resolution) */
} S;

static bool nameq(const char *a, size_t an, const char *b) {
    size_t bn = strlen(b);
    return an == bn && memcmp(a, b, an) == 0;
}

static Decl *find_decl_in(Decl **arr, int n, const char *name, size_t nlen) {
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
static bool is_local(S *s, const char *name, size_t n) {
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
    case F_WIFI: r->has_src_wifi = 1; break;
    case F_DISK: r->has_src_disk = 1; break;
    case F_VPN:  r->has_src_vpn = 1; break;
    case F_TAGS:  r->has_src_tags = 1; break;
    case F_EXEC: r->has_src_exec = 1; break;
    case F_DBUS: r->has_dbus = 1; break;
    default: break;
    }
}

/* ---------- expression walk ---------- */
static void walk_expr(S *s, Expr *e);

static void walk_call(S *s, Expr *e) {
    for (int i = 0; i < e->call.nargs; i++) walk_expr(s, e->call.args[i]);
}

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
        walk_call(s, e);
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
        /* Built-in enum identifiers (anchor/layer/align/keyboard values). */
        static const char *ENUMS[] = {
            "top","bottom","left","right","center",
            "background","overlay",
            "none","on_demand","exclusive",
            "vertical","horizontal",
            "start","end",                         /* alignment aliases */
            "bar","pill","circle","disc","knob",   /* slider thumb_shape */
            NULL
        };
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
        Decl *d = find_decl_in(s->s.src, s->s.nsrc, bn, bL);
        if (d) {
            /* Validate field against source schema. */
            if (d->source.call && d->source.call->kind == EX_CALL) {
                const SrcDef *sd = find_src(d->source.call->call.name, d->source.call->call.nlen);
                if (sd && !field_ok(sd, e->member.field, e->member.flen))
                    diag_error(e->loc, "source '%s' has no field '%.*s'", d->name, (int)e->member.flen, e->member.field);
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
        } else if (d->kind != D_MUT) diag_error(st->loc, "'%s' is const, not mut", st->set.name);
        walk_expr(s, st->set.val);
        return;
    }
    case ST_EMIT: {
        /* validate target surface exists (unless it's a reserved sink, e.g. menu_reply). */
        Decl *tgt = find_decl_in(s->s.sur, s->s.nsur, st->emit.name, st->emit.nlen);
        if (!tgt && !nameq(st->emit.name, st->emit.nlen, "menu_reply"))
            diag_error(st->loc, "emit target surface '%s' not declared", st->emit.name);
        for (int i = 0; i < st->emit.n; i++) walk_expr(s, st->emit.val[i]);
        return;
    }
    case ST_BLOCK:
        for (int i = 0; i < st->block.n; i++) walk_stmt(s, st->block.list[i]);
        return;
    case ST_ANIMATE: {
        Decl *d = find_decl_in(s->s.kon, s->s.nkon, st->anim.name, st->anim.nlen);
        if (!d) diag_error(st->loc, "undefined identifier '%s' in animate()", st->anim.name);
        else if (d->kind != D_MUT) diag_error(st->loc, "animate target '%s' is const, not mut", st->anim.name);
        walk_expr(s, st->anim.to);
        walk_expr(s, st->anim.duration);
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
    walk_expr(s, f->iter);
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
        case WB_ONPRESS:
        case WB_ONRELEASE:
        case WB_ONDRAG:
        case WB_ONCHANGE:
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
            if (strcmp(pn, "spawned_by") == 0) s->in_template = true;
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
            /* reveal_easing accepts a bare easing ident (Step 6.2). */
            if (b->prop->nlen == 13 && memcmp(b->prop->name, "reveal_easing", 13) == 0)
                break;
            walk_expr(s, b->prop->val);
            break;
        case SB_WIDGET: walk_widget(s, b->widget); break;
        case SB_FOR:    walk_for(s, b->forb); break;
        case SB_GROUP: {
            Group *g = b->group;
            for (int k = 0; k < g->nprops; k++) walk_expr(s, g->props[k]->val);
            for (int k = 0; k < g->nmembers; k++) walk_widget(s, g->members[k]);
            break;
        }
        case SB_REGION: {
            Region *rg = b->region;
            for (int k = 0; k < rg->nitems; k++) {
                SBody *rb = &rg->items[k];
                switch (rb->kind) {
                case SB_PROP:   walk_expr(s, rb->prop->val); break;
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
                if (!sd) diag_error(d->source.call->loc, "unknown built-in source '%.*s'",
                                    (int)d->source.call->call.nlen, d->source.call->call.name);
                else set_flag(s.r, sd->flag);
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
            if (d->kind == D_COMPOUND) s.r->has_bar = true;
            break;
        case D_CONST:
        case D_MUT:
            if (find_decl_in(s.s.kon, s.s.nkon, d->name, d->nlen))
                diag_error(d->loc, "duplicate '%s'", d->name);
            s.s.kon[s.s.nkon++] = d;
            break;
        case D_LOCK:      if (s.s.lock)  diag_error(d->loc, "duplicate lock block");      s.s.lock = d;  s.r->has_lock = 1;     break;
        case D_GAMMA:     if (s.s.gamma) diag_error(d->loc, "duplicate gamma block");     s.s.gamma = d; s.r->has_gamma = 1;    break;
        case D_WALLPAPER: if (s.s.wall)  diag_error(d->loc, "duplicate wallpaper block"); s.s.wall = d;  s.r->has_wallpaper = 1; break;
        case D_MEDIA:     s.r->has_media = 1; break;
        case D_STYLE:     break;   /* stripped by style_apply */
        }
    }

    s.r->surface_names = arena_alloc(a, sizeof(char*) * (size_t)(s.s.nsur ? s.s.nsur : 1));
    s.r->surface_deps  = arena_alloc(a, sizeof(char**) * (size_t)(s.s.nsur ? s.s.nsur : 1));
    s.r->spawned_names = arena_alloc(a, sizeof(char*) * (size_t)(s.s.nsur ? s.s.nsur : 1));
    s.r->spawned_args  = arena_alloc(a, sizeof(char**) * (size_t)(s.s.nsur ? s.s.nsur : 1));

    /* Pass 2: walk const/mut initializers (no deps recorded — they're not per-surface). */
    for (int i = 0; i < s.s.nkon; i++) walk_expr(&s, s.s.kon[i]->konst.val);

    /* Pass 3: per-surface analysis. */
    for (int i = 0; i < s.s.nsur; i++) analyze_surface(&s, s.s.sur[i]);

    free(s.deps);
    return s.r;
}

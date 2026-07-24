/* wispc — lightweight type checker (split from sema.c).
 *
 * Mirrors codegen_expr.c's lower() inference loosely: every value the DSL can
 * write has one of TY_{INT,FLOAT,BOOL,STR,COLOR,PIXMAP}. lower() never diagnoses
 * a type clash — it just emits C that gcc then rejects on the *generated* file.
 * This TU moves those clashes to `--check` with a `.wisp` location instead.
 *
 * Rule of thumb, in the same spirit as the property-name schema: a false
 * positive (rejecting a config that builds) is worse than a leak. Anything too
 * polymorphic to type confidently stays TY_UNK, which is compatible with
 * everything and never errors. */
#include "sema_internal.h"
#include <string.h>

const char *ty_name(Ty t) {
    switch (t) {
    case TY_INT:    return "int";
    case TY_FLOAT:  return "float";
    case TY_BOOL:   return "bool";
    case TY_STR:    return "string";
    case TY_COLOR:  return "color";
    case TY_PIXMAP: return "pixmap";
    case TY_ENUM:   return "enum value";
    case TY_UNK: default: return "?";
    }
}

/* Grouping for compatibility: int/float/bool interconvert freely (DSL law), so
 * they share group 0; everything else is its own island; UNK matches anything. */
static int tygroup(Ty t) {
    switch (t) {
    case TY_INT: case TY_FLOAT: case TY_BOOL: return 0;
    case TY_STR:    return 1;
    case TY_COLOR:  return 2;
    case TY_PIXMAP: return 3;
    case TY_ENUM:   return 4;
    default:        return -1;   /* UNK */
    }
}
static bool tynum(Ty t) { return tygroup(t) == 0; }

/* String-typed source fields — every other known field lowers to T_INT in
 * lower() (is_str ? T_STR : T_INT), so we only need to list the string ones. */
static const char *STR_FIELDS =
    " clock.value gamma_warm.value dnd.value ui_hidden.value"
    " exec_line.value inotify.value dbus_signal.value"
    " vpn.state net.ssid power_profile.profile bluez.device"
    " mpris.title mpris.artist mpris.status mpris.player"
    " toplevel.title tags.title ";

static bool word_in(const char *list, const char *w, size_t n) {
    for (const char *p = list; *p;) {
        while (*p == ' ') p++;
        const char *e = p; while (*e && *e != ' ') e++;
        if ((size_t)(e - p) == n && memcmp(p, w, n) == 0) return true;
        p = e;
    }
    return false;
}

/* Type of <source>.<field>. Unknown/for-only fields (list/history/items) are
 * caught by sema's field_ok / for-iter checks, not here — return TY_INT so a
 * misuse in a value position doesn't also spawn a bogus type error. */
static Ty src_field_ty(const char *sn, size_t snlen, const char *fld, size_t flen) {
    char key[96];
    if (snlen + flen + 2 >= sizeof key) return TY_UNK;
    size_t o = 0;
    key[o++] = ' ';
    memcpy(key + o, sn, snlen); o += snlen;
    key[o++] = '.';
    memcpy(key + o, fld, flen); o += flen;
    key[o] = 0;
    /* word_in wants the bare token; STR_FIELDS is space-delimited so build one. */
    return word_in(STR_FIELDS, key + 1, o - 1) ? TY_STR : TY_INT;
}

/* Enum identifiers accepted *somewhere* — used only to tag a bare ident TY_ENUM
 * when it resolves to no decl, so it errors in a numeric/color slot. Which enum
 * a prop actually accepts is enforced separately in check_enum_prop. */
static bool is_enum_ident(const char *n, size_t L) {
    static const char *E =
        " top bottom left right center background overlay none on_demand"
        " exclusive vertical horizontal start end bar pill circle disc knob"
        " fade dither wipe down up down_right down_left up_right up_left ";
    return word_in(E, n, L);
}

static int ty_depth;   /* guards const→const cycles while resolving a type */

Ty ty_of(S *s, Expr *e) {
    if (!e) return TY_UNK;
    switch (e->kind) {
    case EX_INT:    return TY_INT;
    case EX_FLOAT:  return TY_FLOAT;
    case EX_BOOL:   return TY_BOOL;
    case EX_COLOR:  return TY_COLOR;
    case EX_STRING:
    case EX_INTERP: return TY_STR;
    case EX_RANGE:  return ty_of(s, e->range.lo);   /* lowers to lo */
    case EX_DOLLAR: return TY_UNK;                  /* emit arg — polymorphic */
    case EX_CALL:   return TY_UNK;                  /* not a value (errored elsewhere) */
    case EX_UN:
        return e->un.op == OP_NOT ? TY_BOOL : ty_of(s, e->un.e);
    case EX_BIN: {
        switch (e->bin.op) {
        case OP_EQ: case OP_NEQ: case OP_LT: case OP_GT: case OP_LE: case OP_GE:
        case OP_AND: case OP_OR: return TY_BOOL;
        case OP_BITOR: case OP_BITAND: return TY_INT;
        default: break;
        }
        Ty l = ty_of(s, e->bin.l), r = ty_of(s, e->bin.r);
        return (l == TY_FLOAT || r == TY_FLOAT) ? TY_FLOAT : TY_INT;
    }
    case EX_TERN: {
        Ty t = ty_of(s, e->tern.t), el = ty_of(s, e->tern.e);
        if (t == el) return t;
        if (t == TY_FLOAT || el == TY_FLOAT) return TY_FLOAT;
        return t == TY_UNK ? el : t;
    }
    case EX_IDENT: {
        const char *n = e->ident.s; size_t L = e->ident.n;
        if (is_local(s, n, L)) return TY_UNK;
        Decl *d = find_decl_in(s->s.kon, s->s.nkon, n, L);
        if (d) {
            if (ty_depth > 32) return TY_UNK;
            ty_depth++;
            Ty t = ty_of(s, d->konst.val);
            ty_depth--;
            return t;
        }
        d = find_decl_in(s->s.src, s->s.nsrc, n, L);
        if (d && d->source.call && d->source.call->kind == EX_CALL) {
            const SrcDef *sd = find_src(d->source.call->call.name,
                                        d->source.call->call.nlen);
            if (sd) return src_field_ty(sd->name, strlen(sd->name),
                                        sd->primary, strlen(sd->primary));
        }
        if (find_decl_in(s->s.sur, s->s.nsur, n, L)) return TY_UNK;
        if (is_enum_ident(n, L)) return TY_ENUM;
        return TY_UNK;
    }
    case EX_MEMBER: {
        Expr *b = e->member.base;
        if (b->kind != EX_IDENT) return TY_UNK;
        const char *bn = b->ident.s; size_t bL = b->ident.n;
        const char *f = e->member.field; size_t fL = e->member.flen;
        if (is_local(s, bn, bL)) return TY_UNK;   /* loop-var field: polymorphic */
        if (bL == 4 && memcmp(bn, "anim", 4) == 0) return TY_INT;
        if (bL == 4 && memcmp(bn, "menu", 4) == 0)
            return (fL == 5 && memcmp(f, "count", 5) == 0) ? TY_INT : TY_STR;
        Decl *d = find_decl_in(s->s.src, s->s.nsrc, bn, bL);
        if (d && d->source.call && d->source.call->kind == EX_CALL) {
            const SrcDef *sd = find_src(d->source.call->call.name,
                                        d->source.call->call.nlen);
            if (sd) return src_field_ty(sd->name, strlen(sd->name), f, fL);
        }
        return TY_UNK;
    }
    }
    return TY_UNK;
}

static const char *op_word(Op o) {
    switch (o) {
    case OP_ADD: return "add"; case OP_SUB: return "subtract";
    case OP_MUL: return "multiply"; case OP_DIV: return "divide";
    case OP_MOD: return "take the remainder of";
    default: return "combine";
    }
}

void typecheck_expr(S *s, Expr *e) {
    if (!e) return;
    switch (e->kind) {
    case EX_INTERP:
        for (int i = 0; i < e->interp.nparts; i++)
            if (e->interp.parts[i].is_expr) typecheck_expr(s, e->interp.parts[i].expr);
        return;
    case EX_UN:  typecheck_expr(s, e->un.e); return;
    case EX_RANGE:
        typecheck_expr(s, e->range.lo); typecheck_expr(s, e->range.hi); return;
    case EX_BIN: {
        typecheck_expr(s, e->bin.l);
        typecheck_expr(s, e->bin.r);
        Ty l = ty_of(s, e->bin.l), r = ty_of(s, e->bin.r);
        switch (e->bin.op) {
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
            /* == / != on strings is fine (lower() → wisp_streq); arithmetic is
             * not. Only flag a non-numeric operand — UNK stays silent. */
            if (tygroup(l) > 0 || tygroup(r) > 0) {
                diag_error(e->loc, "cannot %s '%s' and '%s'",
                           op_word(e->bin.op), ty_name(l), ty_name(r));
                if (l == TY_STR || r == TY_STR)
                    diag_hint(e->loc, "build strings with interpolation: \"{a}{b}\"");
            }
            return;
        case OP_LT: case OP_GT: case OP_LE: case OP_GE:
            if (l == TY_STR || r == TY_STR)
                diag_error(e->loc, "cannot order '%s' and '%s' — use == / != for strings",
                           ty_name(l), ty_name(r));
            return;
        default: return;
        }
    }
    case EX_TERN: {
        typecheck_expr(s, e->tern.cond);
        typecheck_expr(s, e->tern.t);
        typecheck_expr(s, e->tern.e);
        Ty t = ty_of(s, e->tern.t), el = ty_of(s, e->tern.e);
        if (t != TY_UNK && el != TY_UNK && tygroup(t) != tygroup(el))
            diag_error(e->loc, "ternary arms have mismatched types '%s' and '%s'",
                       ty_name(t), ty_name(el));
        return;
    }
    default: return;
    }
}

/* ---------- property value types ---------- */
typedef enum { PT_ANY, PT_NUM, PT_COLOR } PropTy;

/* PT_NUM: props consumed by eval_int/eval_double (compile-time) or lowered into
 * a C int slot (width/height/body_lines) — a string/color/enum there is always
 * wrong. Runtime exprs over loop vars stay TY_UNK, so they pass. PT_COLOR: props
 * consumed by eval_color[_ctx] / color_ce. Everything else is PT_ANY (text and
 * friends accept any type — lower() coerces to string via snprintf). */
static PropTy prop_expected(const char *n, size_t L) {
    static const char *NUM =
        " width height pad pad_x pad_y x_offset y_offset border_width"
        " border_top border_bottom border_left border_right margin"
        " exclusive_zone font_size clip_top gap radius radius_tl radius_tr"
        " radius_bl radius_br radius_inner radius_outer thumb_size thumb_radius"
        " thumb_border_width track_radius value_gap value_scale value_max"
        " value_min shadow_x shadow_y shadow_blur shadow_spread body_lines"
        " reveal_on_hover reveal_gutter reveal_anim_ms row_h max_visible size"
        " anchor_gap fillet_r fillet_offset_y armpit_inner armpit_outer"
        " armpit_tl armpit_tr armpit_bl armpit_br enter_anim exit_anim"
        " separator_frac clock_size wrong_ms day_k night_k flat_k day_hour"
        " night_hour fade_ms dither_px wipe_soft prog_h icon_gap ";
    static const char *COLOR =
        " bg fg bg_bottom border press_bg shadow track_bg track_fg thumb_color"
        " thumb_border prog_fg prog_track armpit_color separator ring ring_bad"
        " dim caps scrim road ";
    if (word_in(NUM,   n, L)) return PT_NUM;
    if (word_in(COLOR, n, L)) return PT_COLOR;
    return PT_ANY;
}

/* ---------- enum props ---------- */
enum { E_LAYER, E_ANCHOR, E_ALIGN, E_KEYBOARD, E_AXIS, E_THUMB, E_VALIGN,
       E_EDGE, E_INPUT, E_TRANSITION, E_WIPEDIR, E_N };
static const char *ENUM_SETS[E_N] = {
    [E_LAYER]      = "background bottom top overlay",
    [E_ANCHOR]     = "top bottom left right",
    [E_ALIGN]      = "left right top bottom center start end",
    [E_KEYBOARD]   = "none on_demand exclusive",
    [E_AXIS]       = "vertical horizontal",
    [E_THUMB]      = "bar pill circle disc knob none",
    [E_VALIGN]     = "start center end top bottom left right",
    [E_EDGE]       = "top bottom left right",
    [E_INPUT]      = "none",
    [E_TRANSITION] = "fade dither wipe",
    [E_WIPEDIR]    = "right left down up down_right down_left up_right up_left",
};

static int enum_prop_set(const char *n, size_t L, int *is_flag) {
    *is_flag = 0;
    #define P(lit, id, flag) if (L == sizeof(lit)-1 && memcmp(n, lit, L) == 0) \
        { *is_flag = flag; return id; }
    P("layer", E_LAYER, 0);
    P("anchor", E_ANCHOR, 1);
    P("align", E_ALIGN, 0);
    P("keyboard", E_KEYBOARD, 0);
    P("axis", E_AXIS, 0);
    P("orientation", E_AXIS, 0);
    P("thumb_shape", E_THUMB, 0);
    P("value_align", E_VALIGN, 0);
    P("edge", E_EDGE, 0);
    P("input", E_INPUT, 0);
    P("transition", E_TRANSITION, 0);
    P("wipe_dir", E_WIPEDIR, 0);
    #undef P
    return -1;
}

/* Returns the first leaf that isn't a set member (or an int literal, always
 * tolerated: eval_layer/eval_anchor accept ints and the others ignore them). */
static Expr *enum_bad_leaf(int setid, int is_flag, Expr *v) {
    if (!v) return NULL;
    if (is_flag && v->kind == EX_BIN && v->bin.op == OP_BITOR) {
        Expr *b = enum_bad_leaf(setid, is_flag, v->bin.l);
        return b ? b : enum_bad_leaf(setid, is_flag, v->bin.r);
    }
    if (v->kind == EX_INT) return NULL;
    if (v->kind == EX_IDENT && word_in(ENUM_SETS[setid], v->ident.s, v->ident.n))
        return NULL;
    return v;
}

void typecheck_prop(S *s, const char *kind, Prop *p) {
    int is_flag;
    int es = enum_prop_set(p->name, p->nlen, &is_flag);
    if (es >= 0) {
        Expr *bad = enum_bad_leaf(es, is_flag, p->val);
        if (bad) {
            if (bad->kind == EX_IDENT)
                diag_error(bad->loc, "'%.*s' is not a valid %.*s value",
                           (int)bad->ident.n, bad->ident.s, (int)p->nlen, p->name);
            else
                diag_error(bad->loc, "%.*s expects one of its enum values",
                           (int)p->nlen, p->name);
            diag_hint(bad->loc, "%.*s accepts: %s", (int)p->nlen, p->name, ENUM_SETS[es]);
        }
        return;
    }
    typecheck_expr(s, p->val);
    PropTy pt = prop_expected(p->name, p->nlen);
    if (pt == PT_ANY) return;
    Ty t = ty_of(s, p->val);
    if (pt == PT_NUM && (t == TY_STR || t == TY_COLOR || t == TY_PIXMAP || t == TY_ENUM)) {
        diag_error(p->val->loc, "%s property '%.*s' expects a number, got '%s'",
                   kind, (int)p->nlen, p->name, ty_name(t));
    } else if (pt == PT_COLOR && (t == TY_STR || tynum(t) || t == TY_PIXMAP || t == TY_ENUM)) {
        diag_error(p->val->loc, "%s property '%.*s' expects a color, got '%s'",
                   kind, (int)p->nlen, p->name, ty_name(t));
        if (t == TY_STR)
            diag_hint(p->val->loc, "colors are #rrggbb / #aarrggbb literals or a const naming one");
    }
}

/* ---------- source call signatures ---------- */
static bool call_has_kw(Expr *c, const char *kw, size_t kl) {
    for (int i = 0; i < c->call.nargs; i++) {
        const char *kn = c->call.argnames ? c->call.argnames[i] : NULL;
        size_t n = c->call.anlen ? c->call.anlen[i] : 0;
        if (kn && n == kl && memcmp(kn, kw, kl) == 0) return true;
    }
    return false;
}

void check_source_args(const SrcDef *sd, Expr *c) {
    if (!sd || !c || c->kind != EX_CALL) return;
    const char *nm = sd->name;
    int na = c->call.nargs;
    /* Zero-arg sources: any argument is a mistake. (cpu/mem/temp are omitted —
     * as polled status kinds they accept a probe arg and every=.) */
    static const char *ZERO =
        " power_profile gamma_warm dnd ui_hidden mpris pipewire bluez ";
    if (word_in(ZERO, nm, strlen(nm))) {
        if (na != 0) diag_error(c->loc, "%s() takes no arguments", nm);
        return;
    }
    if (!strcmp(nm, "clock")) {
        if (na != 1 || c->call.args[0]->kind != EX_STRING)
            diag_error(c->loc, "clock() takes one string format arg, e.g. clock(\"%%H:%%M\")");
    } else if (!strcmp(nm, "dbus_signal")) {
        if (na < 2 || c->call.args[0]->kind != EX_STRING || c->call.args[1]->kind != EX_STRING)
            diag_error(c->loc, "dbus_signal() takes two string args (interface, member)");
    } else if (!strcmp(nm, "exec_line")) {
        if (na < 1 || c->call.args[0]->kind != EX_STRING)
            diag_error(c->loc, "exec_line() needs a string command as its first arg");
    } else if (!strcmp(nm, "inotify")) {
        if (!call_has_kw(c, "path", 4))
            diag_error(c->loc, "inotify() requires an absolute path=\"/…\"");
    } else if (!strcmp(nm, "toplevel")) {
        if (!call_has_kw(c, "app_id", 6))
            diag_error(c->loc, "toplevel() requires app_id=\"…\"");
    }
    /* temp/bat/net/disk/vpn/backlight (probe arg + every=), tags, tray: shapes
     * are permissive; codegen's collect_srcs remains the backstop for those. */
}

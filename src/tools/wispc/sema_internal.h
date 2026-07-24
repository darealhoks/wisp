/* Shared between sema.c (walk/scope/collection) and sema_types.c (type + enum +
 * source-arg checking). Not public — the world sees only sema_check(). */
#ifndef WISPC_SEMA_INTERNAL_H
#define WISPC_SEMA_INTERNAL_H

#include "wispc.h"

/* ---------- source registry (defined in sema.c) ---------- */
enum {
    F_NONE = 0,
    F_CLOCK, F_CPU, F_MEM, F_TEMP, F_BAT, F_NET, F_DISK, F_VPN,
    F_TAGS, F_EXEC, F_DBUS, F_MPRIS, F_TRAY, F_PIPEWIRE, F_TOPLEVEL,
    F_BACKLIGHT, F_POWER, F_BLUEZ,
};

typedef struct {
    const char *name;
    const char *primary;            /* default field (for bare-ident access) */
    const char *fields;             /* space-separated list of valid fields */
    int flag;                       /* F_* */
} SrcDef;

const SrcDef *find_src(const char *name, size_t n);

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
    Decl *cur_surface;
    bool  in_template;
    bool  in_for_iter;       /* walking a `for … in ITER` head — list fields ok */
    ScopeEnt *locals;
    const char **deps; int ndeps, capdeps;
    const char **targs; int ntargs, captargs;
} S;

Decl *find_decl_in(Decl **arr, int n, const char *name, size_t nlen);
bool  is_local(S *s, const char *name, size_t n);

/* ---------- type checking (sema_types.c) ---------- */
typedef enum {
    TY_UNK, TY_INT, TY_FLOAT, TY_BOOL, TY_STR, TY_COLOR, TY_PIXMAP, TY_ENUM,
} Ty;

const char *ty_name(Ty t);
Ty   ty_of(S *s, Expr *e);            /* pure inference, no diagnostics */
void typecheck_expr(S *s, Expr *e);   /* report arith/ordering/ternary mismatches */
/* Validate one prop's value against its expected type (enum / number / color).
 * Runs typecheck_expr internally; `kind` is the container ("widget"/"surface"/…). */
void typecheck_prop(S *s, const char *kind, Prop *p);
/* Validate `source x = foo(args)` arg count/shape against the source signature. */
void check_source_args(const SrcDef *sd, Expr *call);

#endif

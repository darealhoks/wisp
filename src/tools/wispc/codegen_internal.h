/* Internal shared declarations for the wispc codegen, split across
 * codegen_util.c, codegen_sources.c, codegen_expr.c, codegen_items.c,
 * codegen_surface.c and codegen.c. Not part of the public wispc.h. */
#ifndef WISPC_CODEGEN_INTERNAL_H
#define WISPC_CODEGEN_INTERNAL_H

#include "wispc.h"
#include <stdio.h>

typedef struct CGCtx CGCtx;

/* ---------- codegen_util.c ---------- */

typedef enum { ALIGN_START = 0, ALIGN_END = 1, ALIGN_CENTER = 2 } Align;

const char *sname(const char *s, size_t n);
char       *strndup0(const char *s, size_t n);
int         eval_anchor(Expr *e);
int         eval_layer(Expr *e);
int         eval_int(Expr *e, int dflt);
int         eval_int_range(Expr *e, int *lo, int *hi, int dflt);
uint32_t    eval_color(Expr *e, uint32_t dflt);
uint32_t    eval_color_ctx(CGCtx *ctx, Expr *e, uint32_t dflt);
Align       eval_align(Expr *e);
int         surface_is_vertical(Decl *sur);
Expr       *surface_prop(Decl *sur, const char *name);
Expr       *widget_prop(Widget *w, const char *name);
WBody      *widget_onclick(Widget *w);
WBody      *widget_handler(Widget *w, WBKind k);
int         widget_is_slider(Widget *w);
int         transition_dur(Widget *wd, const char *which);
const char *transition_easing_id(Widget *wd);
const char *surface_easing_id(Decl *sur, const char *prop_name);
int         item_has_any_transition(Widget *wd);
int         widget_enter_ms(Widget *wd);
int         widget_exit_ms (Widget *wd);
int         widget_has_vis_anim(Widget *wd);
const char *widget_easing_id(Widget *wd, const char *prop_name);
int         widget_is_vertical(Widget *w);
double      eval_double(Expr *e, double dflt);
int         widget_flag(Widget *w, const char *name);
int         widget_thumb_shape(Widget *w);
int         widget_value_align(Widget *w);

/* ---------- codegen_sources.c ---------- */

typedef enum {
    DRV_CLOCK,
    DRV_STATUS,
    DRV_TAGS,
    DRV_EXEC,
    DRV_DBUS,
    DRV_WISP,   /* in-process daemon state; no fd, pinged by wispgen_wisp_state_changed() */
} DrvKind;

typedef struct {
    const char *name;
    DrvKind drv;
    struct { const char *field; const char *c_expr; int is_string; } fields[8];
} SrcDrv;

typedef struct {
    Decl   *decl;
    const SrcDrv *drv;
    const char *fmt; size_t flen;
    int interval_ms;
    int refresh_ms;
    const char *arg2; size_t a2len;
} SrcInst;

const SrcDrv *find_drv(const char *name, size_t n);
const char   *drv_field_expr(const SrcDrv *d, const char *f, size_t n, int *is_str);
int           collect_srcs(Unit *u, SrcInst *out, int max);
SrcInst      *find_inst(SrcInst *s, int n, const char *name, size_t L);
int           has_status_src(SrcInst *s, int n);
int           has_tags(SrcInst *s, int n);
int           has_dbus_src(SrcInst *s, int n);
void          emit_sources(FILE *o, SrcInst *srcs, int nsrc);
void          emit_bindings(FILE *o, SrcInst *srcs, int nsrc, SemaResult *r,
                            CGCtx *ctx);

/* ---------- codegen_expr.c ---------- */

typedef enum { T_INT, T_FLOAT, T_BOOL, T_STR, T_COLOR, T_UNK } CT;

typedef struct {
    char text[1024];
    CT type;
} CE;

typedef enum {
    LB_TAG_IDX,
    LB_CLICK_PARAM,
    LB_DBUS_HIST_IT,
    /* $-name → C-expression binding, used inside a spawned_by template's
     * generated render to resolve `$summary`/`$icon`/`$pct`/... to the
     * current slab's field (e.g. `w->s.osd.items[__sl].summary`). Pushed by
     * emit_spawned_surface around per-slab widget emission. */
    LB_DOLLAR_BIND,
} LBKind;

typedef struct {
    const char *name; size_t nlen;
    LBKind kind;
    const char *c_expr;
    const char *src_name;
} Local;

struct CGCtx {
    SrcInst *srcs; int nsrc;
    Decl   **konst; int nkonst;
    Local locals[8]; int nlocals;
    const char *widget_var;
    FILE *prelude;
    char *prelude_buf; size_t prelude_sz;
    int   buf_seq;
    int failed;
    SemaResult *r;
};

void   cgctx_open_prelude(CGCtx *c);
void   cgctx_flush_prelude(CGCtx *c, FILE *out, const char *indent);
void   push_local(CGCtx *c, const char *name, size_t n, LBKind k,
                  const char *expr, const char *src_name);
void   pop_local(CGCtx *c);
Local *find_local(CGCtx *c, const char *name, size_t n);
Decl  *find_konst(CGCtx *c, const char *name, size_t n);
char  *expand_widget(const char *tmpl, const char *wvar);
CE     coerce_to_str(CGCtx *c, CE e, Loc loc);
CE     coerce_to_int(CGCtx *c, CE e);
CE     lower(CGCtx *c, Expr *e);
CE     lower_member(CGCtx *c, Expr *e);
CE     lower_ident(CGCtx *c, Expr *e);
const char *op_C(Op o);

/* ---------- codegen_items.c ---------- */

typedef struct {
    Widget *w;
    bool   is_for_cell;
    int    cell_idx;
    const char *for_var; size_t for_var_n;
    const char *for_src;
    bool   is_runtime_for_cell;
    const char *runtime_for_src;
    int    runtime_for_cap;
    int    st_base;
    int    slider_idx;
    int    handler_idx;
    int    group_id;        /* -1 = not in a group; else group index */
    bool   group_first;     /* first member of its group (draws container) */
    Group *grp;             /* container props, set on group_first only */
} BarItem;

void assign_handler_idx(BarItem *items, int nitems);
int  collect_bar_items(SBody *body, int nbody, BarItem *out, int max,
                       CGCtx *ctx, int *err);
void emit_item_measure(FILE *o, BarItem *it, CGCtx *ctx, int vertical,
                       const char *surf_nm, int item_idx);
void emit_item_draw(FILE *o, BarItem *it, CGCtx *ctx, int vertical, const char *nm);
/* Draw one group (container + members) starting at items[first]; returns the
 * count of member items consumed. Horizontal surfaces only. */
int  emit_group_draw(FILE *o, BarItem *items, int first, int nitems,
                     CGCtx *ctx, const char *nm);
void emit_stmt(FILE *o, CGCtx *ctx, Stmt *st, const char *indent,
               SemaResult *r);
int  emit_surface_click_dispatch(FILE *o, BarItem *items, int nitems,
                                 CGCtx *ctx, SemaResult *r, const char *nm);

/* ---------- codegen_surface.c ---------- */

int  emit_generated_surface(FILE *o, Decl *sur, CGCtx *ctx, const char *nm);
int  emit_generated_compound(FILE *o, Decl *cmp, CGCtx *ctx, const char *nm);
int  emit_spawned_osd_skeleton(FILE *o, Decl *sur, CGCtx *ctx, const char *nm);
int  emit_surfaces(FILE *o, Unit *u, CGCtx *ctx);

#endif

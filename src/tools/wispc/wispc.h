/* wispc — compile shell.wisp to C. Single shared header. */
#ifndef WISPC_H
#define WISPC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* ---------- arena ---------- */
typedef struct Arena Arena;
Arena *arena_new(void);
void  *arena_alloc(Arena *a, size_t n);
char  *arena_strn(Arena *a, const char *s, size_t n);
char  *arena_str(Arena *a, const char *s);
void   arena_free(Arena *a);

/* ---------- diag ---------- */
typedef struct { const char *file; int line, col; } Loc;
void diag_error(Loc l, const char *fmt, ...) __attribute__((format(printf,2,3)));
void diag_note (Loc l, const char *fmt, ...) __attribute__((format(printf,2,3)));
void diag_hint (Loc l, const char *fmt, ...) __attribute__((format(printf,2,3)));
void diag_add_source(const char *file, const char *buf);
int  diag_count(void);

/* ---------- tokens ---------- */
typedef enum {
    TK_EOF = 0,
    TK_IDENT, TK_INT, TK_FLOAT, TK_STRING, TK_COLOR,
    TK_LBRACE, TK_RBRACE, TK_LPAREN, TK_RPAREN, TK_LBRACK, TK_RBRACK,
    TK_SEMI, TK_COMMA, TK_DOT, TK_DOTDOT, TK_DOLLAR, TK_QMARK, TK_COLON,
    TK_HASH,         /* '#' when not starting a color literal — id selector */
    TK_ASSIGN, TK_EQ, TK_NEQ, TK_LT, TK_GT, TK_LE, TK_GE,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_AND, TK_OR, TK_NOT, TK_PIPE, TK_AMP,
    TK_KW_SOURCE, TK_KW_SURFACE, TK_KW_WIDGET, TK_KW_CONST, TK_KW_MUT,
    TK_KW_LOCK, TK_KW_GAMMA, TK_KW_WALLPAPER, TK_KW_MEDIA,
    TK_KW_COMPOUND, TK_KW_REGION, TK_KW_GROUP,
    TK_KW_FOR, TK_KW_IN, TK_KW_CELL,
    TK_KW_TRUE, TK_KW_FALSE, TK_KW_ON_CLICK, TK_KW_ON_SCROLL,
    TK_KW_ON_PRESS, TK_KW_ON_RELEASE, TK_KW_ON_DRAG, TK_KW_ON_CHANGE,
    TK_KW_ON_RCLICK, TK_KW_ON_MCLICK,
    TK_KW_EXEC, TK_KW_EMIT, TK_KW_SET, TK_KW_ANIMATE,
} TokKind;

typedef struct {
    TokKind kind;
    Loc loc;
    const char *s;   /* points into source buffer; not NUL-terminated */
    size_t len;
    int64_t  i;      /* INT, COLOR (ARGB u32 stored in i) */
    double   f;      /* FLOAT */
} Tok;

typedef struct {
    const char *src;
    const char *p;
    const char *file;
    int line, col;
    Tok cur, peek;
    int have_peek;
} Lexer;

void lex_init(Lexer *L, const char *file, const char *src);
void lex_next(Lexer *L);            /* advances cur */
Tok  lex_peek(Lexer *L);            /* lookahead 1 */

/* ---------- AST ---------- */
typedef struct Expr Expr;
typedef struct Stmt Stmt;
typedef struct Prop Prop;
typedef struct Widget Widget;
typedef struct ForBlock ForBlock;
typedef struct Decl Decl;

typedef enum {
    EX_INT, EX_FLOAT, EX_STRING, EX_BOOL, EX_COLOR,
    EX_IDENT, EX_MEMBER, EX_DOLLAR,
    EX_CALL,
    EX_BIN, EX_UN, EX_TERN,
    EX_INTERP,            /* string with embedded {expr} parts */
    EX_RANGE,             /* `lo..hi` — currently only int endpoints; used by
                             reveal-animated props like `fillet_tl = 16..0`. */
} ExKind;

typedef enum {
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_EQ, OP_NEQ, OP_LT, OP_GT, OP_LE, OP_GE,
    OP_AND, OP_OR, OP_NOT, OP_NEG,
    OP_BITOR, OP_BITAND,  /* used in flag exprs like top|left */
} Op;

typedef struct InterpPart {
    bool is_expr;
    const char *lit; size_t llen;  /* literal slice */
    Expr *expr;                    /* when is_expr */
} InterpPart;

struct Expr {
    ExKind kind;
    Loc loc;
    union {
        int64_t i;
        double  f;
        bool    b;
        struct { const char *s; size_t n; } str;
        uint32_t color;            /* ARGB */
        struct { const char *s; size_t n; } ident;
        struct { Expr *base; const char *field; size_t flen; } member;
        struct { const char *s; size_t n; } dollar;
        struct { const char *name; size_t nlen; Expr **args; const char **argnames; size_t *anlen; int nargs; } call;
        struct { Op op; Expr *l, *r; } bin;
        struct { Op op; Expr *e; }    un;
        struct { Expr *cond, *t, *e; } tern;
        struct { InterpPart *parts; int nparts; } interp;
        struct { Expr *lo, *hi; } range;
    };
};

typedef enum {
    ST_EXEC,   /* exec(string-or-interp) */
    ST_EMIT,   /* emit(name, kw=val, ...) */
    ST_SET,    /* set(ident, expr) */
    ST_BLOCK,  /* { s; s; } */
    ST_ANIMATE,/* animate(target, to, duration_ms, easing) */
} StKind;

struct Stmt {
    StKind kind;
    Loc loc;
    union {
        struct { Expr *arg; } exec;
        struct { const char *name; size_t nlen; const char **kw; size_t *kwlen; Expr **val; int n; } emit;
        struct { const char *name; size_t nlen; Expr *val; } set;
        struct { Stmt **list; int n; } block;
        struct { const char *name; size_t nlen; Expr *to; Expr *duration; Expr *easing; } anim;
    };
};

/* A Prop is name = expr; — used everywhere. */
struct Prop {
    Loc loc;
    const char *name; size_t nlen;
    Expr *val;
};

/* Widget body items: properties, on_click handlers, nested for. */
typedef enum {
    WB_PROP, WB_ONCLICK, WB_FOR,
    WB_ONPRESS, WB_ONRELEASE, WB_ONDRAG, WB_ONCHANGE, WB_ONRCLICK, WB_ONMCLICK,
} WBKind;
typedef struct WBody {
    WBKind kind;
    union {
        Prop *prop;
        struct { const char *param; size_t plen; Stmt *body; Loc loc;
                 const char *param2; size_t plen2; } click;
        ForBlock *forb;
    };
} WBody;

struct Widget {
    Loc loc;
    const char *name; size_t nlen;
    WBody *items; int nitems;
    bool is_cell;                  /* `cell { ... }` inside a for */
    const char **classes; int nclasses;   /* `widget wifi.pill.warn` */
};

struct ForBlock {
    Loc loc;
    const char *var; size_t vlen;
    Expr *iter;                    /* e.g. tags.list, $items */
    Widget **cells;                /* expected: cell { ... } */
    int ncells;
};

/* A group: a container (bg/border/radius) wrapping N member widgets laid out
 * contiguously as one flex slot — waybar's `group/...`. Container props live in
 * `props`; members are plain widgets rendered borderless inside. */
typedef struct Group {
    Loc loc;
    const char *name; size_t nlen;
    Prop **props; int nprops;
    Widget **members; int nmembers;
    /* Parallel to members: non-NULL where the member came from a `for` block
     * (members[k] is then that block's cell, repeated per iteration at draw). */
    ForBlock **fors;
    const char **classes; int nclasses;
} Group;

/* Surface body items: properties, widgets, top-level for blocks. */
typedef enum { SB_PROP, SB_WIDGET, SB_FOR, SB_REGION, SB_GROUP } SBKind;
typedef struct Region Region;
typedef struct SBody {
    SBKind kind;
    union {
        Prop *prop;
        Widget *widget;
        ForBlock *forb;
        Region *region;
        Group *group;
    };
} SBody;

/* A compound region: edge (LS_ANCHOR_* bit), size (px), and a body that
 * accepts the same items a surface accepts (widget/for/prop), parsed via
 * the surface-body parser. */
struct Region {
    Loc loc;
    const char *name; size_t nlen;
    int edge;           /* 1=top 2=bottom 4=left 8=right; -1 unresolved */
    int size;           /* px along edge-normal axis */
    SBody *items; int nitems;
};

typedef enum {
    D_SOURCE, D_SURFACE, D_CONST, D_MUT, D_LOCK, D_GAMMA, D_WALLPAPER,
    D_MEDIA, D_COMPOUND, D_STYLE,
} DKind;

/* One row of a user-declared `menu NAME { item { … } }` block. */
typedef struct {
    uint32_t icon;                 /* codepoint, 0 = none */
    const char *label; size_t llen;
    const char *exec;  size_t elen;
} MenuItem;

/* ---------- style rules (CSS-ish cascade, resolved in style.c before sema) ----
 * One compound selector: optional type keyword, optional id, N classes.
 * A Sel is a descendant chain; parts[n-1] is the subject. */
typedef struct {
    const char *type;              /* "surface"/"group"/"widget"/"cell"/"region", or NULL */
    const char *id;                /* node name, or NULL */
    const char **classes; int nclasses;
    const char *pseudo;            /* "active"/"urgent"/"pressed"/"hover", or NULL */
} SimpleSel;

typedef struct { SimpleSel *parts; int nparts; int spec; } Sel;

typedef struct StyleRule {
    Loc loc;
    Sel **sels; int nsels;
    Prop **props; int nprops;
} StyleRule;

struct Decl {
    DKind kind;
    Loc loc;
    const char *name; size_t nlen;
    /* `menu NAME {}` is a D_SURFACE that also carries a static item table.
     * Outside the union: a menu has both a widget body and its rows. */
    bool is_menu; int memoji;
    MenuItem *mitems; int nmitems;
    union {
        struct { Expr *call; } source;          /* RHS is a call expr */
        struct { SBody *items; int n; } surface;
        struct { Expr *val; } konst;            /* const/mut share */
        struct { Prop **props; int n; } block;  /* lock/gamma/wallpaper */
        StyleRule *style;
    };
};

typedef struct {
    Decl **decls;
    int n;
    Arena *arena;
    const char *file;
} Unit;

/* ---------- parser ---------- */
Unit *parse_file(Arena *a, const char *file, const char *src);

/* ---------- style cascade ----------
 * Resolves D_STYLE rules against the AST and splices the winning props into
 * each node's own prop list, then drops the rules from the unit. Everything
 * downstream never learns selectors exist. */
void style_apply(Arena *a, Unit *u);

/* ---------- dump ---------- */
void dump_unit(FILE *out, Unit *u);

/* ---------- sema ---------- */
typedef struct SemaResult {
    /* per-surface dependency sets, feature flags etc. */
    int nsurfaces;
    const char **surface_names;
    const char ***surface_deps;   /* deps[i] = NULL-terminated source names */
    /* spawned_by templates: name + first-seen $-argument list (NULL-term). */
    int nspawned;
    const char **spawned_names;
    const char ***spawned_args;
    /* feature set */
    bool has_dbus, has_mpris, has_tray, has_osd, has_menu, has_hud, has_bar, has_lock, has_gamma, has_wallpaper, has_media, has_anim, has_pipewire, has_toplevel;
    bool has_src_cpu, has_src_mem, has_src_temp, has_src_bat, has_src_net, has_src_disk, has_src_vpn;
    bool has_src_exec, has_src_tags, has_src_backlight, has_power, has_bluez;
    bool net_rates_used;           /* a config reads net.rx_kbps/tx_kbps */
    int  tray_icon_px;             /* tray(icon_size=N); 0 = runtime default */
} SemaResult;

SemaResult *sema_check(Arena *a, Unit *u);

/* ---------- codegen ---------- */
/* Emits gen_main.c, gen_sources.c, gen_surfaces.c, gen_bindings.c into dir.
 * Supports the DSL slice needed by configs/minimal.wisp + configs/bar.wisp:
 *   sources:  clock, cpu, mem, temp, bat, disk, vpn, net, tags
 *   surfaces: single anchored "bar" with multi-widget left/right flex pack,
 *             icon+text widgets, ternary/member/interp expressions,
 *             `for tag in tags.list { cell { … } }`, visible=expr guard,
 *             on_click(param)=exec("…{param}…") handlers. */
int codegen_emit(const char *dir, Unit *u, SemaResult *r);
extern int cg_line_map;   /* --no-line-map clears; see codegen_util.c */

#endif

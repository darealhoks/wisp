#include "wispc.h"
#include <string.h>
#include <stdlib.h>

typedef struct {
    Lexer L;
    Arena *a;
    const char *file;
} P;

/* ---------- helpers ---------- */

static Tok cur(P *p) { return p->L.cur; }
static int at(P *p, TokKind k) { return cur(p).kind == k; }
static int eat(P *p, TokKind k) { if (at(p, k)) { lex_next(&p->L); return 1; } return 0; }

static void expect(P *p, TokKind k, const char *what) {
    if (!eat(p, k)) {
        diag_error(cur(p).loc, "expected %s", what);
    }
}

/* Close a `{`-block. On EOF, point the caret at the *opener* — a beginner who
 * dropped a `}` needs to see which block never closed, not the end of file. */
static void expect_rbrace(P *p, Loc open) {
    if (eat(p, TK_RBRACE)) return;
    if (at(p, TK_EOF))
        diag_error(open, "unclosed '{' — reached end of file with no matching '}'");
    else
        diag_error(cur(p).loc, "expected '}' to close this block");
}

/* Levenshtein for did-you-mean on mistyped declaration keywords. */
static int lev(const char *a, size_t an, const char *b, size_t bn) {
    if (an > 24 || bn > 24) return 99;
    int prev[25], cur[25];
    for (size_t j = 0; j <= bn; j++) prev[j] = (int)j;
    for (size_t i = 1; i <= an; i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= bn; j++) {
            int c = a[i-1] == b[j-1] ? 0 : 1, m = prev[j] + 1;
            if (cur[j-1] + 1 < m) m = cur[j-1] + 1;
            if (prev[j-1] + c < m) m = prev[j-1] + c;
            cur[j] = m;
        }
        for (size_t j = 0; j <= bn; j++) prev[j] = cur[j];
    }
    return prev[bn];
}

/* Nearest declaration keyword within one edit — tight so a valid selector id
 * that merely resembles a keyword never triggers a false rewrite. */
static const char *kw_suggest(const char *s, size_t n) {
    static const char *K[] = { "source","surface","compound","widget","group",
        "cell","region","const","mut","lock","gamma","wallpaper","media","menu", NULL };
    for (int i = 0; K[i]; i++)
        if (lev(s, n, K[i], strlen(K[i])) <= 1) return K[i];
    return NULL;
}

/* Skip to the end of a malformed top-level declaration (its closing '}' or ';')
 * so one typo yields one error, not a cascade. */
static void skip_decl(P *p) {
    int depth = 0;
    for (;;) {
        Tok t = cur(p);
        if (t.kind == TK_EOF) return;
        if (t.kind == TK_LBRACE) { depth++; lex_next(&p->L); }
        else if (t.kind == TK_RBRACE) { lex_next(&p->L); if (depth <= 1) return; depth--; }
        else if (t.kind == TK_SEMI && depth == 0) { lex_next(&p->L); return; }
        else lex_next(&p->L);
    }
}

#define NEW(p, T) ((T*)arena_alloc((p)->a, sizeof(T)))
#define NEW_ARR(p, T, n) ((T*)arena_alloc((p)->a, sizeof(T)*(size_t)(n)))

/* Dynamic list pattern: collect into a temp malloc buffer, then arena-copy. */
typedef struct { void **buf; int n, cap; } VList;
static void vl_push(VList *v, void *x) {
    if (v->n == v->cap) { v->cap = v->cap ? v->cap*2 : 4; v->buf = realloc(v->buf, sizeof(void*)*v->cap); }
    v->buf[v->n++] = x;
}
static void **vl_freeze(P *p, VList *v, int *out_n) {
    *out_n = v->n;
    if (!v->n) { free(v->buf); v->buf = NULL; return NULL; }
    void **r = NEW_ARR(p, void*, v->n);
    memcpy(r, v->buf, sizeof(void*)*v->n);
    free(v->buf); v->buf = NULL;
    return r;
}

/* ---------- expressions ---------- */
/* Precedence climbing.
 *  ternary:   cond ? a : b           (lowest, right-assoc)
 *  or:        ||
 *  and:       &&
 *  bitor:     |        (used for flag exprs: top | left)
 *  bitand:    &
 *  eq:        ==  !=
 *  rel:       <  >  <=  >=
 *  add:       +  -
 *  mul:       *  /  %
 *  unary:     !  -
 *  primary
 */

static Expr *parse_expr(P *p);

/* Parse an interpolated string. The lexer already gave us the raw slice;
 * we split on {...} here. */
static Expr *parse_interp_or_string(P *p) {
    Tok t = cur(p); lex_next(&p->L);
    /* Scan for any '{' that isn't preceded by '\\'. */
    const char *s = t.s; size_t n = t.len;
    int has_interp = 0;
    for (size_t i = 0; i + 1 < n; i++) {
        if (s[i] == '\\') { i++; continue; }
        if (s[i] == '{') { has_interp = 1; break; }
    }
    Expr *e = NEW(p, Expr);
    e->loc = t.loc;
    if (!has_interp) {
        e->kind = EX_STRING;
        e->str.s = arena_strn(p->a, s, n);
        e->str.n = n;
        return e;
    }
    e->kind = EX_INTERP;
    VList parts = {0};
    size_t i = 0, lit_start = 0;
    while (i < n) {
        if (s[i] == '\\' && i+1 < n) { i += 2; continue; }
        if (s[i] == '{') {
            if (i > lit_start) {
                InterpPart *pp = NEW(p, InterpPart);
                pp->is_expr = false;
                pp->lit = arena_strn(p->a, s + lit_start, i - lit_start);
                pp->llen = i - lit_start;
                vl_push(&parts, pp);
            }
            /* Find matching '}'. Parse the inner as an expression by spinning
             * up a sub-lexer over a copy of the slice. */
            size_t j = i + 1, depth = 1;
            while (j < n && depth) {
                if (s[j] == '{') depth++;
                else if (s[j] == '}') depth--;
                if (depth) j++;
            }
            if (j >= n) { diag_error(t.loc, "unterminated {...} in string"); break; }
            char *inner = arena_strn(p->a, s + i + 1, j - i - 1);
            Lexer save = p->L;
            lex_init(&p->L, p->file, inner);
            /* Adjust line:col reasonably — keep parent string's loc. */
            p->L.line = t.loc.line; p->L.col = t.loc.col;
            Expr *sub = parse_expr(p);
            p->L = save;
            InterpPart *pp = NEW(p, InterpPart);
            pp->is_expr = true; pp->expr = sub;
            vl_push(&parts, pp);
            i = j + 1; lit_start = i;
        } else i++;
    }
    if (lit_start < n) {
        InterpPart *pp = NEW(p, InterpPart);
        pp->is_expr = false;
        pp->lit = arena_strn(p->a, s + lit_start, n - lit_start);
        pp->llen = n - lit_start;
        vl_push(&parts, pp);
    }
    int np = 0;
    InterpPart **raw = (InterpPart**)vl_freeze(p, &parts, &np);
    e->interp.parts = NEW_ARR(p, InterpPart, np);
    for (int k = 0; k < np; k++) e->interp.parts[k] = *raw[k];
    e->interp.nparts = np;
    return e;
}

static Expr *parse_primary(P *p) {
    Tok t = cur(p);
    Expr *e;
    switch (t.kind) {
    case TK_INT:
        lex_next(&p->L);
        e = NEW(p, Expr); e->kind = EX_INT; e->loc = t.loc; e->i = t.i; return e;
    case TK_FLOAT:
        lex_next(&p->L);
        e = NEW(p, Expr); e->kind = EX_FLOAT; e->loc = t.loc; e->f = t.f; return e;
    case TK_COLOR:
        lex_next(&p->L);
        e = NEW(p, Expr); e->kind = EX_COLOR; e->loc = t.loc; e->color = (uint32_t)t.i; return e;
    case TK_KW_TRUE:
        lex_next(&p->L);
        e = NEW(p, Expr); e->kind = EX_BOOL; e->loc = t.loc; e->b = true; return e;
    case TK_KW_FALSE:
        lex_next(&p->L);
        e = NEW(p, Expr); e->kind = EX_BOOL; e->loc = t.loc; e->b = false; return e;
    case TK_STRING:
        return parse_interp_or_string(p);
    case TK_LPAREN: {
        lex_next(&p->L);
        Expr *inner = parse_expr(p);
        expect(p, TK_RPAREN, "')'");
        return inner;
    }
    case TK_DOLLAR: {
        lex_next(&p->L);
        Tok id = cur(p);
        if (id.kind != TK_IDENT) { diag_error(id.loc, "expected identifier after '$'"); return NEW(p, Expr); }
        lex_next(&p->L);
        e = NEW(p, Expr); e->kind = EX_DOLLAR; e->loc = t.loc;
        e->dollar.s = arena_strn(p->a, id.s, id.len); e->dollar.n = id.len;
        return e;
    }
    case TK_IDENT: {
        lex_next(&p->L);
        /* call? ident.member? bare ident? */
        if (at(p, TK_LPAREN)) {
            lex_next(&p->L);
            e = NEW(p, Expr); e->kind = EX_CALL; e->loc = t.loc;
            e->call.name = arena_strn(p->a, t.s, t.len);
            e->call.nlen = t.len;
            VList args = {0}, names = {0}; VList lens = {0};
            if (!at(p, TK_RPAREN)) for (;;) {
                /* kw=val ? */
                if (cur(p).kind == TK_IDENT && lex_peek(&p->L).kind == TK_ASSIGN) {
                    Tok kn = cur(p); lex_next(&p->L); lex_next(&p->L);
                    vl_push(&names, arena_strn(p->a, kn.s, kn.len));
                    size_t *lp = NEW(p, size_t); *lp = kn.len;
                    vl_push(&lens, lp);
                } else {
                    vl_push(&names, NULL);
                    size_t *lp = NEW(p, size_t); *lp = 0;
                    vl_push(&lens, lp);
                }
                Expr *a = parse_expr(p);
                vl_push(&args, a);
                if (!eat(p, TK_COMMA)) break;
            }
            expect(p, TK_RPAREN, "')' in call");
            int n; Expr **ea = (Expr**)vl_freeze(p, &args, &n);
            int nn; char **na = (char**)vl_freeze(p, &names, &nn);
            int nl; size_t **la = (size_t**)vl_freeze(p, &lens, &nl);
            e->call.args = ea; e->call.nargs = n;
            e->call.argnames = (const char **)na;
            e->call.anlen = NEW_ARR(p, size_t, n);
            for (int k = 0; k < n; k++) e->call.anlen[k] = *la[k];
            return e;
        }
        e = NEW(p, Expr); e->kind = EX_IDENT; e->loc = t.loc;
        e->ident.s = arena_strn(p->a, t.s, t.len); e->ident.n = t.len;
        /* member chain */
        while (eat(p, TK_DOT)) {
            Tok m = cur(p);
            if (m.kind != TK_IDENT) { diag_error(m.loc, "expected field name after '.'"); break; }
            lex_next(&p->L);
            Expr *outer = NEW(p, Expr);
            outer->kind = EX_MEMBER; outer->loc = e->loc;
            outer->member.base = e;
            outer->member.field = arena_strn(p->a, m.s, m.len);
            outer->member.flen = m.len;
            e = outer;
        }
        return e;
    }
    default:
        diag_error(t.loc, "expected a value here (number, string, color, name, or '(' expr ')')");
        lex_next(&p->L);
        e = NEW(p, Expr); e->kind = EX_INT; e->loc = t.loc; e->i = 0; return e;
    }
}

static Expr *parse_unary_core(P *p) {
    Tok t = cur(p);
    if (t.kind == TK_NOT || t.kind == TK_MINUS) {
        lex_next(&p->L);
        Expr *e = NEW(p, Expr); e->kind = EX_UN; e->loc = t.loc;
        e->un.op = (t.kind == TK_NOT) ? OP_NOT : OP_NEG;
        e->un.e = parse_unary_core(p);
        return e;
    }
    return parse_primary(p);
}

/* `lo .. hi` — used by reveal-animated props (fillet_tl = 16..0). Bound
 * tighter than any binary op so it falls out before precedence climbing. */
static Expr *parse_unary(P *p) {
    Expr *lo = parse_unary_core(p);
    if (cur(p).kind != TK_DOTDOT) return lo;
    Loc loc = cur(p).loc;
    lex_next(&p->L);
    Expr *hi = parse_unary_core(p);
    Expr *r = NEW(p, Expr); r->kind = EX_RANGE; r->loc = loc;
    r->range.lo = lo; r->range.hi = hi;
    return r;
}

static Expr *mk_bin(P *p, Op op, Expr *l, Expr *r, Loc loc) {
    Expr *e = NEW(p, Expr); e->kind = EX_BIN; e->loc = loc;
    e->bin.op = op; e->bin.l = l; e->bin.r = r;
    return e;
}

static Expr *parse_mul(P *p) {
    Expr *l = parse_unary(p);
    for (;;) {
        Tok t = cur(p);
        Op op;
        if      (t.kind == TK_STAR)    op = OP_MUL;
        else if (t.kind == TK_SLASH)   op = OP_DIV;
        else if (t.kind == TK_PERCENT) op = OP_MOD;
        else break;
        lex_next(&p->L);
        l = mk_bin(p, op, l, parse_unary(p), t.loc);
    }
    return l;
}
static Expr *parse_add(P *p) {
    Expr *l = parse_mul(p);
    for (;;) {
        Tok t = cur(p);
        Op op;
        if      (t.kind == TK_PLUS)  op = OP_ADD;
        else if (t.kind == TK_MINUS) op = OP_SUB;
        else break;
        lex_next(&p->L);
        l = mk_bin(p, op, l, parse_mul(p), t.loc);
    }
    return l;
}
static Expr *parse_rel(P *p) {
    Expr *l = parse_add(p);
    for (;;) {
        Tok t = cur(p);
        Op op;
        if      (t.kind == TK_LT) op = OP_LT;
        else if (t.kind == TK_GT) op = OP_GT;
        else if (t.kind == TK_LE) op = OP_LE;
        else if (t.kind == TK_GE) op = OP_GE;
        else break;
        lex_next(&p->L);
        l = mk_bin(p, op, l, parse_add(p), t.loc);
    }
    return l;
}
static Expr *parse_eq(P *p) {
    Expr *l = parse_rel(p);
    for (;;) {
        Tok t = cur(p);
        Op op;
        if      (t.kind == TK_EQ)  op = OP_EQ;
        else if (t.kind == TK_NEQ) op = OP_NEQ;
        else break;
        lex_next(&p->L);
        l = mk_bin(p, op, l, parse_rel(p), t.loc);
    }
    return l;
}
static Expr *parse_bitand(P *p) {
    Expr *l = parse_eq(p);
    while (at(p, TK_AMP)) { Tok t=cur(p); lex_next(&p->L); l = mk_bin(p, OP_BITAND, l, parse_eq(p), t.loc); }
    return l;
}
static Expr *parse_bitor(P *p) {
    Expr *l = parse_bitand(p);
    while (at(p, TK_PIPE)) { Tok t=cur(p); lex_next(&p->L); l = mk_bin(p, OP_BITOR, l, parse_bitand(p), t.loc); }
    return l;
}
static Expr *parse_and(P *p) {
    Expr *l = parse_bitor(p);
    while (at(p, TK_AND)) { Tok t=cur(p); lex_next(&p->L); l = mk_bin(p, OP_AND, l, parse_bitor(p), t.loc); }
    return l;
}
static Expr *parse_or(P *p) {
    Expr *l = parse_and(p);
    while (at(p, TK_OR)) { Tok t=cur(p); lex_next(&p->L); l = mk_bin(p, OP_OR, l, parse_and(p), t.loc); }
    return l;
}
static Expr *parse_expr(P *p) {
    Expr *cond = parse_or(p);
    if (eat(p, TK_QMARK)) {
        Expr *t = parse_expr(p);
        expect(p, TK_COLON, "':' in ternary");
        Expr *e = parse_expr(p);
        Expr *r = NEW(p, Expr);
        r->kind = EX_TERN; r->loc = cond->loc;
        r->tern.cond = cond; r->tern.t = t; r->tern.e = e;
        return r;
    }
    return cond;
}

/* ---------- statements (event handler bodies) ---------- */

static Stmt *parse_stmt(P *p);

static Stmt *parse_stmt(P *p) {
    Tok t = cur(p);
    Stmt *s = NEW(p, Stmt); s->loc = t.loc;
    if (t.kind == TK_LBRACE) {
        Loc open = cur(p).loc;
        lex_next(&p->L);
        VList list = {0};
        while (!at(p, TK_RBRACE) && !at(p, TK_EOF)) {
            vl_push(&list, parse_stmt(p));
            eat(p, TK_SEMI);
        }
        expect_rbrace(p, open);
        s->kind = ST_BLOCK;
        int n; s->block.list = (Stmt**)vl_freeze(p, &list, &n); s->block.n = n;
        return s;
    }
    if (t.kind == TK_KW_EXEC) {
        lex_next(&p->L);
        expect(p, TK_LPAREN, "'(' after exec");
        s->kind = ST_EXEC; s->exec.arg = parse_expr(p);
        expect(p, TK_RPAREN, "')'");
        return s;
    }
    if (t.kind == TK_KW_SET) {
        lex_next(&p->L);
        expect(p, TK_LPAREN, "'(' after set");
        Tok n = cur(p);
        if (n.kind != TK_IDENT) diag_error(n.loc, "expected identifier in set()");
        else lex_next(&p->L);
        expect(p, TK_COMMA, "',' in set()");
        s->kind = ST_SET;
        s->set.name = arena_strn(p->a, n.s, n.len); s->set.nlen = n.len;
        s->set.val = parse_expr(p);
        expect(p, TK_RPAREN, "')'");
        return s;
    }
    if (t.kind == TK_KW_ANIMATE) {
        lex_next(&p->L);
        expect(p, TK_LPAREN, "'(' after animate");
        Tok n = cur(p);
        if (n.kind != TK_IDENT) diag_error(n.loc, "expected mut name in animate()");
        else lex_next(&p->L);
        expect(p, TK_COMMA, "',' in animate()");
        s->kind = ST_ANIMATE;
        s->anim.name = arena_strn(p->a, n.s, n.len); s->anim.nlen = n.len;
        s->anim.to = parse_expr(p);
        expect(p, TK_COMMA, "',' in animate()");
        s->anim.duration = parse_expr(p);
        expect(p, TK_COMMA, "',' in animate()");
        s->anim.easing = parse_expr(p);
        expect(p, TK_RPAREN, "')'");
        return s;
    }
    if (t.kind == TK_KW_EMIT) {
        lex_next(&p->L);
        expect(p, TK_LPAREN, "'(' after emit");
        Tok n = cur(p);
        if (n.kind != TK_IDENT) diag_error(n.loc, "expected surface name in emit()");
        else lex_next(&p->L);
        s->kind = ST_EMIT;
        s->emit.name = arena_strn(p->a, n.s, n.len); s->emit.nlen = n.len;
        VList kw = {0}, vals = {0}, lens = {0};
        while (eat(p, TK_COMMA)) {
            Tok k = cur(p);
            if (k.kind != TK_IDENT) { diag_error(k.loc, "expected kwarg name"); break; }
            lex_next(&p->L);
            expect(p, TK_ASSIGN, "'=' in emit kwarg");
            vl_push(&kw, arena_strn(p->a, k.s, k.len));
            size_t *lp = NEW(p, size_t); *lp = k.len;
            vl_push(&lens, lp);
            vl_push(&vals, parse_expr(p));
        }
        expect(p, TK_RPAREN, "')'");
        int nn; s->emit.kw = (const char**)vl_freeze(p, &kw, &nn);
        int nv; s->emit.val = (Expr**)vl_freeze(p, &vals, &nv);
        int nl; size_t **la = (size_t**)vl_freeze(p, &lens, &nl);
        s->emit.kwlen = NEW_ARR(p, size_t, nn);
        for (int i = 0; i < nn; i++) s->emit.kwlen[i] = *la[i];
        s->emit.n = nn;
        return s;
    }
    diag_error(t.loc, "expected a handler statement: exec(...), emit(...), set(...), animate(...), or a { } block");
    lex_next(&p->L);
    s->kind = ST_EXEC; s->exec.arg = NULL;
    return s;
}

/* ---------- declarations / blocks ---------- */

static Prop *parse_prop(P *p) {
    Tok n = cur(p);
    if (n.kind != TK_IDENT) {
        diag_error(n.loc, "expected property name");
        lex_next(&p->L);
        return NEW(p, Prop);
    }
    lex_next(&p->L);
    Prop *pr = NEW(p, Prop);
    pr->loc = n.loc;
    pr->name = arena_strn(p->a, n.s, n.len);
    pr->nlen = n.len;
    /* Marker prop: bare `ident;` (no `=`) — useful for `slider;` etc. */
    if (at(p, TK_SEMI)) {
        pr->val = NULL;
        lex_next(&p->L);
        return pr;
    }
    if (at(p, TK_EQ)) {   /* `x == v;` — comparison written where assignment goes */
        diag_error(cur(p).loc, "use '=' to set a property ('==' is the comparison operator)");
        lex_next(&p->L);
    } else {
        expect(p, TK_ASSIGN, "'='");
    }
    pr->val = parse_expr(p);
    expect(p, TK_SEMI, "';'");
    return pr;
}

static Widget *parse_widget_or_cell(P *p, bool is_cell);
static ForBlock *parse_for(P *p);

/* `.foo.bar` suffix on a node name — style classes. Only dots *adjacent* to
 * the preceding token attach: `bar .pill` is a descendant selector, not a
 * class. `end` is one past the last char of that token. */
/* Returns the source end of the last token consumed (or `end` if none), so a
 * caller can keep checking adjacency past the classes. */
static const char *parse_classes(P *p, const char *end, const char ***out, int *nout) {
    VList cl = {0};
    while (at(p, TK_DOT) && (!end || cur(p).s == end)) {
        lex_next(&p->L);
        Tok c = cur(p);
        if (c.kind != TK_IDENT) { diag_error(c.loc, "expected class name after '.'"); break; }
        lex_next(&p->L);
        vl_push(&cl, arena_strn(p->a, c.s, c.len));
        end = c.s + c.len;
    }
    *out = (const char **)vl_freeze(p, &cl, nout);
    return end;
}

static Widget *parse_widget_or_cell(P *p, bool is_cell) {
    Tok kw = cur(p); lex_next(&p->L);
    Widget *w = NEW(p, Widget);
    w->loc = kw.loc;
    w->is_cell = is_cell;
    const char *end = kw.s + kw.len;
    if (is_cell) {
        w->name = "cell"; w->nlen = 4;
    } else {
        Tok n = cur(p);
        if (n.kind != TK_IDENT) diag_error(n.loc, "expected widget name");
        else { lex_next(&p->L); w->name = arena_strn(p->a, n.s, n.len); w->nlen = n.len; end = n.s + n.len; }
    }
    parse_classes(p, end, &w->classes, &w->nclasses);
    Loc wopen = cur(p).loc;
    expect(p, TK_LBRACE, "'{'");
    VList items = {0};
    while (!at(p, TK_RBRACE) && !at(p, TK_EOF)) {
        WBody *b = NEW(p, WBody);
        Tok t = cur(p);
        if (t.kind == TK_KW_ON_CLICK   || t.kind == TK_KW_ON_SCROLL ||
            t.kind == TK_KW_ON_PRESS   || t.kind == TK_KW_ON_RELEASE ||
            t.kind == TK_KW_ON_DRAG    || t.kind == TK_KW_ON_CHANGE ||
            t.kind == TK_KW_ON_RCLICK  || t.kind == TK_KW_ON_MCLICK) {
            lex_next(&p->L);
            expect(p, TK_LPAREN, "'('");
            switch (t.kind) {
            case TK_KW_ON_PRESS:   b->kind = WB_ONPRESS;   break;
            case TK_KW_ON_RELEASE: b->kind = WB_ONRELEASE; break;
            case TK_KW_ON_DRAG:    b->kind = WB_ONDRAG;    break;
            case TK_KW_ON_CHANGE:  b->kind = WB_ONCHANGE;  break;
            case TK_KW_ON_RCLICK:  b->kind = WB_ONRCLICK;  break;
            case TK_KW_ON_MCLICK:  b->kind = WB_ONMCLICK;  break;
            default:               b->kind = WB_ONCLICK;   break;
            }
            b->click.loc = t.loc;
            b->click.param = NULL; b->click.plen = 0;
            b->click.param2 = NULL; b->click.plen2 = 0;
            if (at(p, TK_IDENT)) {
                Tok pn = cur(p); lex_next(&p->L);
                b->click.param = arena_strn(p->a, pn.s, pn.len);
                b->click.plen  = pn.len;
                if (eat(p, TK_COMMA) && at(p, TK_IDENT)) {
                    Tok pn2 = cur(p); lex_next(&p->L);
                    b->click.param2 = arena_strn(p->a, pn2.s, pn2.len);
                    b->click.plen2  = pn2.len;
                }
            }
            expect(p, TK_RPAREN, "')'");
            expect(p, TK_ASSIGN, "'='");
            b->click.body = parse_stmt(p);
            expect(p, TK_SEMI, "';'");
        } else if (t.kind == TK_KW_FOR) {
            b->kind = WB_FOR;
            b->forb = parse_for(p);
        } else if (t.kind == TK_IDENT) {
            b->kind = WB_PROP;
            b->prop = parse_prop(p);
        } else {
            diag_error(t.loc, "expected a widget property, an event handler (on_click(...) etc.), or `for`");
            lex_next(&p->L);
            continue;
        }
        vl_push(&items, b);
    }
    expect_rbrace(p, wopen);
    int n;
    WBody **arr = (WBody**)vl_freeze(p, &items, &n);
    w->items = NEW_ARR(p, WBody, n);
    for (int i = 0; i < n; i++) w->items[i] = *arr[i];
    w->nitems = n;
    return w;
}

static ForBlock *parse_for(P *p) {
    Tok kw = cur(p); lex_next(&p->L);     /* 'for' */
    ForBlock *f = NEW(p, ForBlock);
    f->loc = kw.loc;
    Tok v = cur(p);
    if (v.kind != TK_IDENT) diag_error(v.loc, "expected loop variable");
    else { lex_next(&p->L); f->var = arena_strn(p->a, v.s, v.len); f->vlen = v.len; }
    if (!eat(p, TK_KW_IN)) diag_error(cur(p).loc, "expected 'in'");
    f->iter = parse_expr(p);
    Loc fopen = cur(p).loc;
    expect(p, TK_LBRACE, "'{'");
    VList cells = {0};
    while (!at(p, TK_RBRACE) && !at(p, TK_EOF)) {
        if (!at(p, TK_KW_CELL)) {
            diag_error(cur(p).loc, "expected 'cell' inside for");
            lex_next(&p->L); continue;
        }
        vl_push(&cells, parse_widget_or_cell(p, true));
    }
    expect_rbrace(p, fopen);
    int n;
    Widget **arr = (Widget**)vl_freeze(p, &cells, &n);
    f->cells = NEW_ARR(p, Widget*, n);
    for (int i = 0; i < n; i++) f->cells[i] = arr[i];
    f->ncells = n;
    return f;
}

static Decl *parse_source(P *p) {
    Tok kw = cur(p); lex_next(&p->L);
    Decl *d = NEW(p, Decl);
    d->kind = D_SOURCE; d->loc = kw.loc;
    Tok n = cur(p);
    if (n.kind != TK_IDENT) diag_error(n.loc, "expected source name");
    else { lex_next(&p->L); d->name = arena_strn(p->a, n.s, n.len); d->nlen = n.len; }
    expect(p, TK_ASSIGN, "'='");
    d->source.call = parse_expr(p);
    expect(p, TK_SEMI, "';'");
    if (d->source.call && d->source.call->kind != EX_CALL)
        diag_error(d->source.call->loc, "source RHS must be a call");
    return d;
}

static Decl *parse_const_or_mut(P *p, DKind dk) {
    Tok kw = cur(p); lex_next(&p->L);
    Decl *d = NEW(p, Decl);
    d->kind = dk; d->loc = kw.loc;
    Tok n = cur(p);
    if (n.kind != TK_IDENT) diag_error(n.loc, "expected name");
    else { lex_next(&p->L); d->name = arena_strn(p->a, n.s, n.len); d->nlen = n.len; }
    expect(p, TK_ASSIGN, "'='");
    d->konst.val = parse_expr(p);
    expect(p, TK_SEMI, "';'");
    return d;
}

/* Parse the body of a `region <name> { ... }` block — same items as a
 * surface body except `region` is not nested (no compound-in-compound). */
static Region *parse_region(P *p) {
    Tok kw = cur(p); lex_next(&p->L);
    Region *r = NEW(p, Region);
    r->loc = kw.loc; r->edge = -1; r->size = 0;
    Tok n = cur(p);
    if (n.kind != TK_IDENT) diag_error(n.loc, "expected region name");
    else { lex_next(&p->L); r->name = arena_strn(p->a, n.s, n.len); r->nlen = n.len; }
    Loc ropen = cur(p).loc;
    expect(p, TK_LBRACE, "'{'");
    VList items = {0};
    while (!at(p, TK_RBRACE) && !at(p, TK_EOF)) {
        SBody *b = NEW(p, SBody);
        Tok t = cur(p);
        if (t.kind == TK_KW_WIDGET) {
            b->kind = SB_WIDGET; b->widget = parse_widget_or_cell(p, false);
        } else if (t.kind == TK_KW_FOR) {
            b->kind = SB_FOR; b->forb = parse_for(p);
        } else if (t.kind == TK_IDENT) {
            b->kind = SB_PROP; b->prop = parse_prop(p);
        } else {
            diag_error(t.loc, "expected a property, `widget`, or `for` in region body");
            lex_next(&p->L); continue;
        }
        vl_push(&items, b);
    }
    expect_rbrace(p, ropen);
    int nn;
    SBody **arr = (SBody**)vl_freeze(p, &items, &nn);
    r->items = NEW_ARR(p, SBody, nn);
    for (int i = 0; i < nn; i++) r->items[i] = *arr[i];
    r->nitems = nn;
    return r;
}

/* group NAME { (prop | widget)* } — container props + member widgets. */
static Group *parse_group(P *p) {
    Tok kw = cur(p); lex_next(&p->L);     /* 'group' */
    Group *g = NEW(p, Group);
    g->loc = kw.loc;
    Tok n = cur(p);
    if (n.kind != TK_IDENT) diag_error(n.loc, "expected group name");
    else { lex_next(&p->L); g->name = arena_strn(p->a, n.s, n.len); g->nlen = n.len; }
    parse_classes(p, n.s + n.len, &g->classes, &g->nclasses);
    Loc gopen = cur(p).loc;
    expect(p, TK_LBRACE, "'{'");
    VList props = {0}, members = {0}, fors = {0};
    while (!at(p, TK_RBRACE) && !at(p, TK_EOF)) {
        Tok t = cur(p);
        if (t.kind == TK_KW_WIDGET || t.kind == TK_KW_CELL) {
            vl_push(&members, parse_widget_or_cell(p, t.kind == TK_KW_CELL));
            vl_push(&fors, NULL);
        } else if (t.kind == TK_KW_FOR) {
            /* A for-block member is one cell repeated at runtime — it becomes a
             * single member slot that draws N times inside the container. */
            ForBlock *f = parse_for(p);
            if (f->ncells != 1) diag_error(f->loc, "for inside a group needs exactly one cell");
            else { vl_push(&members, f->cells[0]); vl_push(&fors, f); }
        } else if (t.kind == TK_IDENT) {
            vl_push(&props, parse_prop(p));
        } else {
            diag_error(t.loc, "expected a group property, `widget`, `cell`, or `for`");
            lex_next(&p->L); continue;
        }
    }
    expect_rbrace(p, gopen);
    int np; Prop **pa = (Prop**)vl_freeze(p, &props, &np);
    g->props = NEW_ARR(p, Prop*, np);
    for (int i = 0; i < np; i++) g->props[i] = pa[i];
    g->nprops = np;
    int nm; Widget **ma = (Widget**)vl_freeze(p, &members, &nm);
    g->members = NEW_ARR(p, Widget*, nm);
    for (int i = 0; i < nm; i++) g->members[i] = ma[i];
    g->nmembers = nm;
    int nf; ForBlock **fa = (ForBlock**)vl_freeze(p, &fors, &nf);
    g->fors = NEW_ARR(p, ForBlock*, nm ? nm : 1);
    for (int i = 0; i < nm; i++) g->fors[i] = i < nf ? fa[i] : NULL;
    return g;
}

/* `item { icon = 0xf011; label = "…"; exec = "…"; }` — sugar inside a menu
 * decl. Lowers to the static string table in gen_menus.h, not to widgets. */
static void parse_menu_item(P *p, VList *rows) {
    Tok kw = cur(p); lex_next(&p->L);
    expect(p, TK_LBRACE, "'{' after item");
    MenuItem *it = NEW(p, MenuItem);
    *it = (MenuItem){0};
    while (!at(p, TK_RBRACE) && !at(p, TK_EOF)) {
        Tok k = cur(p);
        /* `exec` is a keyword token elsewhere in the grammar. */
        if (k.kind != TK_IDENT && k.kind != TK_KW_EXEC) { diag_error(k.loc, "expected item property"); lex_next(&p->L); continue; }
        lex_next(&p->L); expect(p, TK_ASSIGN, "'='");
        Tok v = cur(p); lex_next(&p->L);
        if (k.len == 4 && memcmp(k.s, "icon", 4) == 0) {
            if (v.kind != TK_INT) diag_error(v.loc, "icon takes a codepoint literal");
            else it->icon = (uint32_t)v.i;
        } else if (k.len == 5 && memcmp(k.s, "label", 5) == 0) {
            if (v.kind != TK_STRING) diag_error(v.loc, "label takes a string");
            else { it->label = arena_strn(p->a, v.s, v.len); it->llen = v.len; }
        } else if (k.len == 4 && memcmp(k.s, "exec", 4) == 0) {
            if (v.kind != TK_STRING) diag_error(v.loc, "exec takes a string");
            else { it->exec = arena_strn(p->a, v.s, v.len); it->elen = v.len; }
        } else {
            diag_error(k.loc, "unknown item property");
        }
        eat(p, TK_SEMI);
    }
    expect(p, TK_RBRACE, "'}'");
    if (!it->label || !it->exec) diag_error(kw.loc, "item needs label and exec");
    vl_push(rows, it);
}

static Decl *parse_surface_or_compound(P *p, DKind dk, bool menu_decl) {
    Tok kw = cur(p); lex_next(&p->L);
    Decl *d = NEW(p, Decl);
    d->kind = dk; d->loc = kw.loc;
    d->is_menu = menu_decl;
    Tok n = cur(p);
    if (n.kind != TK_IDENT) diag_error(n.loc, "expected surface name");
    else { lex_next(&p->L); d->name = arena_strn(p->a, n.s, n.len); d->nlen = n.len; }
    Loc sopen = cur(p).loc;
    expect(p, TK_LBRACE, "'{'");
    VList items = {0}, rows = {0};
    while (!at(p, TK_RBRACE) && !at(p, TK_EOF)) {
        SBody *b = NEW(p, SBody);
        Tok t = cur(p);
        if (menu_decl && t.kind == TK_IDENT && t.len == 4 && memcmp(t.s, "item", 4) == 0
            && lex_peek(&p->L).kind == TK_LBRACE) {
            parse_menu_item(p, &rows);
            continue;
        }
        if (menu_decl && t.kind == TK_IDENT && t.len == 6 && memcmp(t.s, "preset", 6) == 0) {
            lex_next(&p->L); expect(p, TK_ASSIGN, "'='");
            Tok v = cur(p);
            if (v.kind != TK_IDENT || v.len != 5 || memcmp(v.s, "emoji", 5) != 0)
                diag_error(v.loc, "only 'emoji' preset exists");
            else d->memoji = 1;
            lex_next(&p->L); eat(p, TK_SEMI);
            continue;
        }
        if (t.kind == TK_KW_WIDGET) {
            b->kind = SB_WIDGET;
            b->widget = parse_widget_or_cell(p, false);
        } else if (t.kind == TK_KW_FOR) {
            b->kind = SB_FOR;
            b->forb = parse_for(p);
        } else if (t.kind == TK_KW_REGION) {
            if (dk != D_COMPOUND) {
                diag_error(t.loc, "'region' only allowed inside compound");
                lex_next(&p->L); continue;
            }
            b->kind = SB_REGION;
            b->region = parse_region(p);
        } else if (t.kind == TK_KW_GROUP) {
            b->kind = SB_GROUP;
            b->group = parse_group(p);
        } else if (t.kind == TK_IDENT) {
            b->kind = SB_PROP;
            b->prop = parse_prop(p);
        } else {
            diag_error(t.loc, "expected a property, `widget`, `group`, `for`, or `region` in surface body");
            lex_next(&p->L); continue;
        }
        vl_push(&items, b);
    }
    expect_rbrace(p, sopen);
    int nn;
    SBody **arr = (SBody**)vl_freeze(p, &items, &nn);
    d->surface.items = NEW_ARR(p, SBody, nn);
    for (int i = 0; i < nn; i++) d->surface.items[i] = *arr[i];
    d->surface.n = nn;
    int nr; MenuItem **ra = (MenuItem**)vl_freeze(p, &rows, &nr);
    d->mitems = NEW_ARR(p, MenuItem, nr ? nr : 1);
    for (int i = 0; i < nr; i++) d->mitems[i] = *ra[i];
    d->nmitems = nr;
    return d;
}

static Decl *parse_block_decl(P *p, DKind dk, const char *name) {
    (void)name;
    Tok kw = cur(p); lex_next(&p->L);
    Decl *d = NEW(p, Decl);
    d->kind = dk; d->loc = kw.loc;
    d->name = NULL; d->nlen = 0;
    Loc bopen = cur(p).loc;
    expect(p, TK_LBRACE, "'{'");
    VList props = {0};
    while (!at(p, TK_RBRACE) && !at(p, TK_EOF)) {
        if (!at(p, TK_IDENT)) { diag_error(cur(p).loc, "expected a property name"); lex_next(&p->L); continue; }
        vl_push(&props, parse_prop(p));
    }
    expect_rbrace(p, bopen);
    int n;
    Prop **arr = (Prop**)vl_freeze(p, &props, &n);
    d->block.props = NEW_ARR(p, Prop*, n);
    for (int i = 0; i < n; i++) d->block.props[i] = arr[i];
    d->block.n = n;
    return d;
}

/* ---------- style rules ----------
 * `sel[, sel]* { prop = val; ... }` where sel is a descendant chain of
 * `[type][#id|id][.class]*`. A bare identifier is an id (node name); type
 * selectors are the node keywords (widget/group/cell/surface/region). */

static const char *sel_type_kw(TokKind k) {
    switch (k) {
    case TK_KW_WIDGET:  return "widget";
    case TK_KW_GROUP:   return "group";
    case TK_KW_CELL:    return "cell";
    case TK_KW_SURFACE: return "surface";
    case TK_KW_REGION:  return "region";
    default:            return NULL;
    }
}

static int at_simple_sel(P *p) {
    return at(p, TK_IDENT) || at(p, TK_HASH) || at(p, TK_DOT) || sel_type_kw(cur(p).kind);
}

static SimpleSel parse_simple_sel(P *p) {
    SimpleSel s = {0};
    const char *end = NULL;
    const char *ty = sel_type_kw(cur(p).kind);
    if (ty) { s.type = ty; end = cur(p).s + cur(p).len; lex_next(&p->L); }
    if (at(p, TK_HASH)) { end = cur(p).s; lex_next(&p->L); }  /* tok.s is past the '#' */
    if (at(p, TK_IDENT) && (!end || cur(p).s == end)) {
        Tok n = cur(p); lex_next(&p->L);
        s.id = arena_strn(p->a, n.s, n.len);
        end = n.s + n.len;
    }
    end = parse_classes(p, end, &s.classes, &s.nclasses);
    if (at(p, TK_COLON) && (!end || cur(p).s == end)) {
        lex_next(&p->L);
        Tok n = cur(p);
        if (n.kind != TK_IDENT) diag_error(n.loc, "expected pseudo-class name after ':'");
        else { s.pseudo = arena_strn(p->a, n.s, n.len); lex_next(&p->L); }
    }
    if (!s.type && !s.id && !s.nclasses && !s.pseudo) {
        diag_error(cur(p).loc, "expected selector");
        lex_next(&p->L);                 /* never spin on an unconsumed token */
    }
    return s;
}

static Sel *parse_sel(P *p) {
    Sel *sel = NEW(p, Sel);
    VList parts = {0};
    do {
        SimpleSel *s = NEW(p, SimpleSel);
        *s = parse_simple_sel(p);
        sel->spec += (s->id ? 100 : 0) + 10 * (s->nclasses + (s->pseudo ? 1 : 0)) + (s->type ? 1 : 0);
        vl_push(&parts, s);
    } while (at_simple_sel(p));
    int n; SimpleSel **arr = (SimpleSel**)vl_freeze(p, &parts, &n);
    sel->parts = NEW_ARR(p, SimpleSel, n);
    for (int i = 0; i < n; i++) sel->parts[i] = *arr[i];
    sel->nparts = n;
    for (int i = 0; i < n - 1; i++)
        if (sel->parts[i].pseudo)
            diag_error(p->L.cur.loc, "pseudo-class is only allowed on the last part of a selector");
    return sel;
}

static Decl *parse_style_rule(P *p) {
    Decl *d = NEW(p, Decl);
    d->kind = D_STYLE; d->loc = cur(p).loc;
    StyleRule *r = NEW(p, StyleRule);
    r->loc = d->loc;
    d->style = r;
    VList sels = {0};
    do { vl_push(&sels, parse_sel(p)); } while (eat(p, TK_COMMA));
    int ns; Sel **sa = (Sel**)vl_freeze(p, &sels, &ns);
    r->sels = NEW_ARR(p, Sel*, ns);
    for (int i = 0; i < ns; i++) r->sels[i] = sa[i];
    r->nsels = ns;
    Loc styopen = cur(p).loc;
    expect(p, TK_LBRACE, "'{' in style rule");
    VList props = {0};
    while (!at(p, TK_RBRACE) && !at(p, TK_EOF)) {
        if (!at(p, TK_IDENT)) { diag_error(cur(p).loc, "expected a property in style rule"); lex_next(&p->L); continue; }
        vl_push(&props, parse_prop(p));
    }
    expect_rbrace(p, styopen);
    int np; Prop **pa = (Prop**)vl_freeze(p, &props, &np);
    r->props = NEW_ARR(p, Prop*, np);
    for (int i = 0; i < np; i++) r->props[i] = pa[i];
    r->nprops = np;
    return d;
}

Unit *parse_file(Arena *a, const char *file, const char *src) {
    P p = {.a = a, .file = file };
    lex_init(&p.L, file, src);

    Unit *u = NEW(&p, Unit);
    u->arena = a; u->file = file;
    VList decls = {0};
    while (!at(&p, TK_EOF)) {
        Tok t = cur(&p);
        Decl *d = NULL;
        switch (t.kind) {
        case TK_KW_SOURCE:    d = parse_source(&p); break;
        /* `surface NAME {` is a decl; bare `surface {`/`surface.cls {` is a
         * style rule on every surface. Same tokens can't mean both. */
        case TK_KW_SURFACE:
            d = (lex_peek(&p.L).kind == TK_IDENT)
                ? parse_surface_or_compound(&p, D_SURFACE, false) : parse_style_rule(&p);
            break;
        case TK_KW_WIDGET: case TK_KW_GROUP: case TK_KW_CELL: case TK_KW_REGION:
        case TK_IDENT: case TK_HASH: case TK_DOT:
            if (t.kind == TK_IDENT && !(t.len == 4 && memcmp(t.s, "menu", 4) == 0)) {
                Tok pk = lex_peek(&p.L);
                /* `foo = ...;` at top level — a property with no block around it. */
                if (pk.kind == TK_ASSIGN) {
                    diag_error(t.loc, "property '%.*s' is not inside any block", (int)t.len, t.s);
                    diag_hint(t.loc, "properties live inside a surface/widget/group/... block, not at top level");
                    skip_decl(&p); continue;
                }
                /* `srface bar { ... }` — a mistyped declaration keyword. A real
                 * two-part style selector never leads with a near-keyword id. */
                const char *sug;
                if (pk.kind == TK_IDENT && (sug = kw_suggest(t.s, t.len))) {
                    diag_error(t.loc, "unknown declaration keyword '%.*s'", (int)t.len, t.s);
                    diag_hint(t.loc, "did you mean '%s'?", sug);
                    skip_decl(&p); continue;
                }
            }
            d = (t.kind == TK_IDENT && t.len == 4 && memcmp(t.s, "menu", 4) == 0 &&
                 lex_peek(&p.L).kind == TK_IDENT)
                ? parse_surface_or_compound(&p, D_SURFACE, true) : parse_style_rule(&p);
            break;
        case TK_KW_COMPOUND:  d = parse_surface_or_compound(&p, D_COMPOUND, false); break;
        case TK_KW_CONST:     d = parse_const_or_mut(&p, D_CONST); break;
        case TK_KW_MUT:       d = parse_const_or_mut(&p, D_MUT); break;
        case TK_KW_LOCK:      d = parse_block_decl(&p, D_LOCK,      "lock"); break;
        case TK_KW_GAMMA:     d = parse_block_decl(&p, D_GAMMA,     "gamma"); break;
        case TK_KW_WALLPAPER: d = parse_block_decl(&p, D_WALLPAPER, "wallpaper"); break;
        case TK_KW_MEDIA:     d = parse_block_decl(&p, D_MEDIA,     "media"); break;
        default:
            diag_error(t.loc, "expected a top-level declaration "
                       "(source, const, mut, surface, compound, group, widget, "
                       "lock, gamma, wallpaper, media) or a style rule");
            lex_next(&p.L);
            continue;
        }
        if (d) vl_push(&decls, d);
    }
    int n;
    Decl **arr = (Decl**)vl_freeze(&p, &decls, &n);
    u->decls = NEW_ARR(&p, Decl*, n);
    for (int i = 0; i < n; i++) u->decls[i] = arr[i];
    u->n = n;
    return u;
}

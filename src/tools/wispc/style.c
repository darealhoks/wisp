/* Style cascade — resolves top-level selector rules against the AST and
 * splices the winning props into each node's own prop list, then drops the
 * rules. Runs between parse and sema; nothing downstream sees selectors.
 * ponytail: O(rules × nodes) with strcmp — a bar has tens of each. */
#include "wispc.h"
#include <string.h>
#include <stdlib.h>

typedef enum { N_SURFACE, N_REGION, N_GROUP, N_WIDGET, N_CELL } NKind;

typedef struct Node {
    NKind kind;
    const char *id;                /* node name ("cell" for cells) */
    const char **classes; int nclasses;
    struct Node *parent;
    ForBlock *loop;                /* for-cells only: the enclosing `for` */
    void *owner;                   /* Decl* / Region* / Group* / Widget* */
} Node;

typedef struct { Node **v; int n, cap; } Nodes;

/* One prop candidate for a node, keyed by prop name. */
typedef struct { Prop *p; int spec; Loc rule; } Cand;

static const char *KIND_NAME[] = { "surface", "region", "group", "widget", "cell" };

static void nodes_push(Nodes *ns, Node *n) {
    if (ns->n == ns->cap) { ns->cap = ns->cap ? ns->cap * 2 : 16; ns->v = realloc(ns->v, sizeof(Node*) * (size_t)ns->cap); }
    ns->v[ns->n++] = n;
}

static Node *mk(Arena *a, Nodes *ns, NKind k, const char *id, const char **cl, int ncl, Node *parent, void *owner) {
    Node *n = arena_alloc(a, sizeof *n);
    n->kind = k; n->id = id; n->classes = cl; n->nclasses = ncl;
    n->parent = parent; n->loop = NULL; n->owner = owner;
    nodes_push(ns, n);
    return n;
}

/* ---------- collect ---------- */

static void collect_widget(Arena *a, Nodes *ns, Widget *w, Node *parent, ForBlock *loop) {
    Node *n = mk(a, ns, w->is_cell ? N_CELL : N_WIDGET, w->name, w->classes, w->nclasses, parent, w);
    n->loop = loop;
    for (int i = 0; i < w->nitems; i++)
        if (w->items[i].kind == WB_FOR)
            for (int c = 0; c < w->items[i].forb->ncells; c++)
                collect_widget(a, ns, w->items[i].forb->cells[c], n, w->items[i].forb);
}

static void collect_sbody(Arena *a, Nodes *ns, SBody *items, int nitems, Node *parent) {
    for (int i = 0; i < nitems; i++) {
        SBody *b = &items[i];
        switch (b->kind) {
        case SB_PROP: break;
        case SB_WIDGET: collect_widget(a, ns, b->widget, parent, NULL); break;
        case SB_FOR:
            for (int c = 0; c < b->forb->ncells; c++) collect_widget(a, ns, b->forb->cells[c], parent, b->forb);
            break;
        case SB_REGION: {
            Node *n = mk(a, ns, N_REGION, b->region->name, NULL, 0, parent, b->region);
            collect_sbody(a, ns, b->region->items, b->region->nitems, n);
            break;
        }
        case SB_GROUP: {
            Group *g = b->group;
            Node *n = mk(a, ns, N_GROUP, g->name, g->classes, g->nclasses, parent, g);
            for (int m = 0; m < g->nmembers; m++) collect_widget(a, ns, g->members[m], n, NULL);
            break;
        }
        }
    }
}

/* ---------- match ---------- */

static bool has_class(Node *n, const char *c) {
    for (int i = 0; i < n->nclasses; i++) if (!strcmp(n->classes[i], c)) return true;
    return false;
}

static bool match_simple(Node *n, SimpleSel *s) {
    if (s->type && strcmp(s->type, KIND_NAME[n->kind])) return false;
    if (s->id && (!n->id || strcmp(s->id, n->id))) return false;
    for (int i = 0; i < s->nclasses; i++) if (!has_class(n, s->classes[i])) return false;
    return true;
}

/* Descendant chain, matched right-to-left; parts[0..pi] must match ancestors. */
static bool match_chain(Node *n, Sel *sel, int pi) {
    if (pi < 0) return true;
    for (Node *p = n; p; p = p->parent)
        if (match_simple(p, &sel->parts[pi]) && match_chain(p->parent, sel, pi - 1)) return true;
    return false;
}

/* `pseudo` selects which slice of the cascade we are resolving: NULL = the base
 * pass, a name = the overlay pass for that state. */
static bool match_sel(Node *n, Sel *sel, const char *pseudo) {
    const char *sp = sel->parts[sel->nparts - 1].pseudo;
    if (!!sp != !!pseudo || (sp && strcmp(sp, pseudo))) return false;
    return match_simple(n, &sel->parts[sel->nparts - 1]) && match_chain(n->parent, sel, sel->nparts - 2);
}

/* ---------- splice ---------- */

/* Props declared inline in the node body are specificity ∞ — they win, and a
 * cascade prop of the same name is simply dropped. Also the lookup a pseudo
 * overlay uses to find the base value it wraps in a ternary. */
static Prop *find_prop(Node *n, const char *name) {
    switch (n->kind) {
    case N_WIDGET: case N_CELL: {
        Widget *w = n->owner;
        for (int i = 0; i < w->nitems; i++)
            if (w->items[i].kind == WB_PROP && !strcmp(w->items[i].prop->name, name)) return w->items[i].prop;
        return NULL;
    }
    case N_GROUP: {
        Group *g = n->owner;
        for (int i = 0; i < g->nprops; i++) if (!strcmp(g->props[i]->name, name)) return g->props[i];
        return NULL;
    }
    case N_SURFACE: case N_REGION: {
        SBody *it; int nit;
        if (n->kind == N_SURFACE) { Decl *d = n->owner; it = d->surface.items; nit = d->surface.n; }
        else { Region *r = n->owner; it = r->items; nit = r->nitems; }
        for (int i = 0; i < nit; i++)
            if (it[i].kind == SB_PROP && !strcmp(it[i].prop->name, name)) return it[i].prop;
        return NULL;
    }
    }
    return NULL;
}

static void add_props(Arena *a, Node *n, Prop **add, int nadd) {
    if (!nadd) return;
    switch (n->kind) {
    case N_WIDGET: case N_CELL: {
        Widget *w = n->owner;
        WBody *ni = arena_alloc(a, sizeof(WBody) * (size_t)(w->nitems + nadd));
        memcpy(ni, w->items, sizeof(WBody) * (size_t)w->nitems);
        for (int i = 0; i < nadd; i++) { ni[w->nitems + i].kind = WB_PROP; ni[w->nitems + i].prop = add[i]; }
        w->items = ni; w->nitems += nadd;
        break;
    }
    case N_GROUP: {
        Group *g = n->owner;
        Prop **np = arena_alloc(a, sizeof(Prop*) * (size_t)(g->nprops + nadd));
        memcpy(np, g->props, sizeof(Prop*) * (size_t)g->nprops);
        memcpy(np + g->nprops, add, sizeof(Prop*) * (size_t)nadd);
        g->props = np; g->nprops += nadd;
        break;
    }
    case N_SURFACE: case N_REGION: {
        SBody **slot; int *cnt;
        Decl *d = n->owner; Region *r = n->owner;
        if (n->kind == N_SURFACE) { slot = &d->surface.items; cnt = &d->surface.n; }
        else                      { slot = &r->items;         cnt = &r->nitems; }
        SBody *ni = arena_alloc(a, sizeof(SBody) * (size_t)(*cnt + nadd));
        memcpy(ni, *slot, sizeof(SBody) * (size_t)*cnt);
        for (int i = 0; i < nadd; i++) { ni[*cnt + i].kind = SB_PROP; ni[*cnt + i].prop = add[i]; }
        *slot = ni; *cnt += nadd;
        break;
    }
    }
}

/* ---------- pseudo-class overlays ---------- */

/* State pseudo-classes, applied in this order — a later one wraps the earlier
 * ternary, so `:urgent` wins over `:active` when both hold. */
static const char *PSEUDOS[] = { "active", "urgent", "pressed" };

static Expr *mk_expr(Arena *a, ExKind k, Loc loc) {
    Expr *e = arena_alloc(a, sizeof *e);
    memset(e, 0, sizeof *e);
    e->kind = k; e->loc = loc;
    return e;
}

/* `LOOPVAR.field` — the condition a for-cell pseudo lowers to. */
static Expr *loop_field(Arena *a, Node *n, const char *field, Loc loc) {
    Expr *base = mk_expr(a, EX_IDENT, loc);
    base->ident.s = n->loop->var; base->ident.n = n->loop->vlen;
    Expr *m = mk_expr(a, EX_MEMBER, loc);
    m->member.base = base; m->member.field = field; m->member.flen = strlen(field);
    return m;
}

/* Splice one resolved pseudo prop into the node, wrapping whatever value the
 * prop already has. */
static void apply_pseudo(Arena *a, Node *n, const char *pseudo, Prop *p) {
    if (!strcmp(pseudo, "pressed")) {
        /* The runtime already tracks the pressed widget, but only as a bg swap. */
        if (strcmp(p->name, "bg")) {
            diag_error(p->loc, "':pressed' can only set 'bg' (lowers to press_bg), not '%s'", p->name);
            return;
        }
        if (find_prop(n, "press_bg")) return;    /* inline press_bg wins */
        Prop *np = arena_alloc(a, sizeof *np);
        *np = *p; np->name = "press_bg"; np->nlen = 8;
        add_props(a, n, &np, 1);
        return;
    }
    if (n->kind != N_CELL || !n->loop) {
        diag_error(p->loc, "':%s' is only supported on `for` cells (it reads the loop item's field)", pseudo);
        return;
    }
    Prop *base = find_prop(n, p->name);
    if (!base) {
        diag_error(p->loc, "':%s' sets '%s' but the node has no base value for it", pseudo, p->name);
        return;
    }
    Expr *t = mk_expr(a, EX_TERN, p->loc);
    t->tern.cond = loop_field(a, n, pseudo, p->loc);
    t->tern.t = p->val;
    t->tern.e = base->val;
    base->val = t;                               /* base prop is this node's own */
}

/* ---------- driver ---------- */

/* Resolve one cascade slice (base pass when `pseudo` is NULL) into `cand`.
 * Returns the candidate count. */
static int resolve(Unit *u, Node *n, const char *pseudo, Cand **cand, int *ccap) {
    int ncand = 0;
    for (int di = 0; di < u->n; di++) {
        if (u->decls[di]->kind != D_STYLE) continue;
        StyleRule *r = u->decls[di]->style;
        int spec = -1;
        for (int s = 0; s < r->nsels; s++)
            if (match_sel(n, r->sels[s], pseudo) && r->sels[s]->spec > spec) spec = r->sels[s]->spec;
        if (spec < 0) continue;
        for (int pi = 0; pi < r->nprops; pi++) {
            Prop *p = r->props[pi];
            int k = 0;
            for (; k < ncand; k++) if (!strcmp((*cand)[k].p->name, p->name)) break;
            if (k == ncand) {
                if (ncand == *ccap) { *ccap = *ccap ? *ccap * 2 : 16; *cand = realloc(*cand, sizeof(Cand) * (size_t)*ccap); }
                (*cand)[ncand++] = (Cand){ p, spec, r->loc };
            } else if (spec > (*cand)[k].spec) {
                (*cand)[k] = (Cand){ p, spec, r->loc };
            } else if (spec == (*cand)[k].spec) {
                diag_error(p->loc, "conflicting style rules set '%s' on %s '%s' at equal specificity",
                           p->name, KIND_NAME[n->kind], n->id ? n->id : "?");
                diag_note((*cand)[k].rule, "other rule here");
            }
        }
    }
    return ncand;
}

void style_apply(Arena *a, Unit *u) {
    Nodes ns = {0};
    for (int i = 0; i < u->n; i++) {
        Decl *d = u->decls[i];
        if (d->kind != D_SURFACE && d->kind != D_COMPOUND) continue;
        Node *n = mk(a, &ns, N_SURFACE, d->name, NULL, 0, NULL, d);
        collect_sbody(a, &ns, d->surface.items, d->surface.n, n);
    }

    Cand *cand = NULL; int ccap = 0;
    Prop **add = NULL; int addcap = 0;

    for (int i = 0; i < ns.n; i++) {
        Node *n = ns.v[i];
        int ncand = resolve(u, n, NULL, &cand, &ccap);
        int nadd = 0;
        for (int k = 0; k < ncand; k++) {
            if (find_prop(n, cand[k].p->name)) continue;
            if (nadd == addcap) { addcap = addcap ? addcap * 2 : 16; add = realloc(add, sizeof(Prop*) * (size_t)addcap); }
            add[nadd++] = cand[k].p;
        }
        add_props(a, n, add, nadd);

        /* Overlays run after the base splice so find_prop() sees the base value. */
        for (size_t ps = 0; ps < sizeof PSEUDOS / sizeof *PSEUDOS; ps++) {
            ncand = resolve(u, n, PSEUDOS[ps], &cand, &ccap);
            for (int k = 0; k < ncand; k++) apply_pseudo(a, n, PSEUDOS[ps], cand[k].p);
        }
        if (resolve(u, n, "hover", &cand, &ccap))
            diag_error(cand[0].rule, "':hover' is not supported — the runtime tracks hover per surface, not per widget");
    }

    free(cand); free(add); free(ns.v);

    /* Drop the rules — sema and codegen never see them. */
    int w = 0;
    for (int i = 0; i < u->n; i++) if (u->decls[i]->kind != D_STYLE) u->decls[w++] = u->decls[i];
    u->n = w;
}

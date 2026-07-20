#include "wispc.h"
#include <stdio.h>
#include <string.h>

static void pad(FILE *o, int n) { while (n-- > 0) fputc(' ', o); }

static const char *op_name(Op op) {
    switch (op) {
    case OP_ADD: return "+"; case OP_SUB: return "-"; case OP_MUL: return "*";
    case OP_DIV: return "/"; case OP_MOD: return "%";
    case OP_EQ: return "=="; case OP_NEQ: return "!=";
    case OP_LT: return "<"; case OP_GT: return ">"; case OP_LE: return "<="; case OP_GE: return ">=";
    case OP_AND: return "&&"; case OP_OR: return "||";
    case OP_NOT: return "!"; case OP_NEG: return "-(u)";
    case OP_BITOR: return "|"; case OP_BITAND: return "&";
    }
    return "?";
}

static void dump_expr(FILE *o, Expr *e, int ind) {
    if (!e) { pad(o, ind); fputs("(null)\n", o); return; }
    pad(o, ind);
    switch (e->kind) {
    case EX_INT:    fprintf(o, "INT %lld\n", (long long)e->i); break;
    case EX_FLOAT:  fprintf(o, "FLOAT %g\n", e->f); break;
    case EX_BOOL:   fprintf(o, "BOOL %s\n", e->b ? "true" : "false"); break;
    case EX_STRING: fprintf(o, "STR %.*s\n", (int)e->str.n, e->str.s); break;
    case EX_COLOR:  fprintf(o, "COLOR 0x%08x\n", e->color); break;
    case EX_IDENT:  fprintf(o, "IDENT %.*s\n", (int)e->ident.n, e->ident.s); break;
    case EX_DOLLAR: fprintf(o, "$%.*s\n", (int)e->dollar.n, e->dollar.s); break;
    case EX_MEMBER:
        fprintf(o, "MEMBER .%.*s\n", (int)e->member.flen, e->member.field);
        dump_expr(o, e->member.base, ind + 2);
        break;
    case EX_CALL:
        fprintf(o, "CALL %.*s(%d args)\n", (int)e->call.nlen, e->call.name, e->call.nargs);
        for (int i = 0; i < e->call.nargs; i++) {
            if (e->call.argnames && e->call.argnames[i]) {
                pad(o, ind+2); fprintf(o, "kw=%s:\n", e->call.argnames[i]);
            }
            dump_expr(o, e->call.args[i], ind + 4);
        }
        break;
    case EX_BIN:
        fprintf(o, "BIN %s\n", op_name(e->bin.op));
        dump_expr(o, e->bin.l, ind + 2);
        dump_expr(o, e->bin.r, ind + 2);
        break;
    case EX_UN:
        fprintf(o, "UN %s\n", op_name(e->un.op));
        dump_expr(o, e->un.e, ind + 2);
        break;
    case EX_TERN:
        fputs("TERN\n", o);
        dump_expr(o, e->tern.cond, ind + 2);
        dump_expr(o, e->tern.t, ind + 2);
        dump_expr(o, e->tern.e, ind + 2);
        break;
    case EX_RANGE:
        fputs("RANGE\n", o);
        pad(o, ind+2); fputs("lo:\n", o); dump_expr(o, e->range.lo, ind + 4);
        pad(o, ind+2); fputs("hi:\n", o); dump_expr(o, e->range.hi, ind + 4);
        break;
    case EX_INTERP:
        fprintf(o, "INTERP (%d parts)\n", e->interp.nparts);
        for (int i = 0; i < e->interp.nparts; i++) {
            InterpPart *p = &e->interp.parts[i];
            if (p->is_expr) {
                pad(o, ind+2); fputs("{expr}:\n", o);
                dump_expr(o, p->expr, ind + 4);
            } else {
                pad(o, ind+2); fprintf(o, "lit %.*s\n", (int)p->llen, p->lit);
            }
        }
        break;
    }
}

static void dump_stmt(FILE *o, Stmt *s, int ind) {
    if (!s) return;
    pad(o, ind);
    switch (s->kind) {
    case ST_EXEC:  fputs("EXEC\n", o); dump_expr(o, s->exec.arg, ind + 2); break;
    case ST_SET:   fprintf(o, "SET %s\n", s->set.name); dump_expr(o, s->set.val, ind + 2); break;
    case ST_EMIT:
        fprintf(o, "EMIT %s (%d kwargs)\n", s->emit.name, s->emit.n);
        for (int i = 0; i < s->emit.n; i++) {
            pad(o, ind+2); fprintf(o, "%s =\n", s->emit.kw[i]);
            dump_expr(o, s->emit.val[i], ind + 4);
        }
        break;
    case ST_BLOCK:
        fputs("BLOCK\n", o);
        for (int i = 0; i < s->block.n; i++) dump_stmt(o, s->block.list[i], ind + 2);
        break;
    case ST_ANIMATE:
        fprintf(o, "ANIMATE %s\n", s->anim.name);
        dump_expr(o, s->anim.to, ind + 2);
        dump_expr(o, s->anim.duration, ind + 2);
        dump_expr(o, s->anim.easing, ind + 2);
        break;
    }
}

static void dump_prop(FILE *o, Prop *p, int ind) {
    pad(o, ind); fprintf(o, "prop %s =\n", p->name);
    dump_expr(o, p->val, ind + 2);
}

static void dump_widget(FILE *o, Widget *w, int ind);

static void dump_for(FILE *o, ForBlock *f, int ind) {
    pad(o, ind); fprintf(o, "for %.*s in\n", (int)f->vlen, f->var);
    dump_expr(o, f->iter, ind + 2);
    for (int i = 0; i < f->ncells; i++) dump_widget(o, f->cells[i], ind + 2);
}

static void dump_widget(FILE *o, Widget *w, int ind) {
    pad(o, ind); fprintf(o, "%s %s {\n", w->is_cell ? "cell" : "widget", w->is_cell ? "" : w->name);
    for (int i = 0; i < w->nitems; i++) {
        WBody *b = &w->items[i];
        switch (b->kind) {
        case WB_PROP:    dump_prop(o, b->prop, ind + 2); break;
        case WB_ONCLICK:
        case WB_ONPRESS:
        case WB_ONRELEASE:
        case WB_ONDRAG:
        case WB_ONCHANGE:
            pad(o, ind + 2); fprintf(o, "on_*(%s%s):\n",
                b->click.plen ? "" : "", b->click.plen ? b->click.param : "");
            dump_stmt(o, b->click.body, ind + 4);
            break;
        case WB_FOR:     dump_for(o, b->forb, ind + 2); break;
        }
    }
    pad(o, ind); fputs("}\n", o);
}

void dump_unit(FILE *o, Unit *u) {
    fprintf(o, "Unit %s — %d decls\n", u->file, u->n);
    for (int i = 0; i < u->n; i++) {
        Decl *d = u->decls[i];
        switch (d->kind) {
        case D_SOURCE:
            fprintf(o, "source %s =\n", d->name);
            dump_expr(o, d->source.call, 2);
            break;
        case D_SURFACE:
            fprintf(o, "surface %s {\n", d->name);
            for (int j = 0; j < d->surface.n; j++) {
                SBody *b = &d->surface.items[j];
                switch (b->kind) {
                case SB_PROP:   dump_prop(o, b->prop, 2); break;
                case SB_WIDGET: dump_widget(o, b->widget, 2); break;
                case SB_FOR:    dump_for(o, b->forb, 2); break;
                case SB_REGION: fprintf(o, "  region %s {...}\n", b->region->name); break;
                case SB_GROUP:
                    fprintf(o, "  group %s {\n", b->group->name);
                    for (int k = 0; k < b->group->nprops; k++) dump_prop(o, b->group->props[k], 4);
                    for (int k = 0; k < b->group->nmembers; k++) dump_widget(o, b->group->members[k], 4);
                    fputs("  }\n", o);
                    break;
                }
            }
            fputs("}\n", o);
            break;
        case D_COMPOUND:
            fprintf(o, "compound %s {...}\n", d->name);
            break;
        case D_CONST: fprintf(o, "const %s =\n", d->name); dump_expr(o, d->konst.val, 2); break;
        case D_MUT:   fprintf(o, "mut %s =\n",   d->name); dump_expr(o, d->konst.val, 2); break;
        case D_LOCK:   fputs("lock {\n", o);      for (int j=0;j<d->block.n;j++) dump_prop(o, d->block.props[j], 2); fputs("}\n", o); break;
        case D_GAMMA:  fputs("gamma {\n", o);     for (int j=0;j<d->block.n;j++) dump_prop(o, d->block.props[j], 2); fputs("}\n", o); break;
        case D_WALLPAPER:fputs("wallpaper {\n",o);for (int j=0;j<d->block.n;j++) dump_prop(o, d->block.props[j], 2); fputs("}\n", o); break;
        case D_MEDIA:  fputs("media {\n", o);     for (int j=0;j<d->block.n;j++) dump_prop(o, d->block.props[j], 2); fputs("}\n", o); break;
        default: break;
        }
    }
}

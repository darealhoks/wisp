/* wispc codegen — expression lowerer (split from codegen.c). */
#include "codegen_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================ */
/* Expression lowerer                                            */
/* ============================================================ */

/* CT / CE / LBKind / Local / CGCtx are declared in codegen_internal.h. */

void cgctx_open_prelude(CGCtx *c) {
    if (c->prelude) return;
    c->prelude_buf = NULL; c->prelude_sz = 0;
    c->prelude = open_memstream(&c->prelude_buf, &c->prelude_sz);
}
void cgctx_flush_prelude(CGCtx *c, FILE *out, const char *indent) {
    if (!c->prelude) return;
    fclose(c->prelude); c->prelude = NULL;
    if (c->prelude_buf && c->prelude_sz) {
        /* prefix each line with indent */
        char *p = c->prelude_buf, *end = c->prelude_buf + c->prelude_sz;
        while (p < end) {
            char *nl = memchr(p, '\n', end - p);
            size_t L = nl ? (size_t)(nl - p) : (size_t)(end - p);
            fputs(indent, out); fwrite(p, 1, L, out); fputc('\n', out);
            if (!nl) break;
            p = nl + 1;
            if (p >= end) break;
        }
    }
    free(c->prelude_buf); c->prelude_buf = NULL; c->prelude_sz = 0;
}

void push_local(CGCtx *c, const char *name, size_t n, LBKind k,
                       const char *expr, const char *src_name) {
    /* Silent truncation here loses a $-binding and only shows up as a bogus
     * "no binding" error at the use site — fail loudly instead. */
    if (c->nlocals >= (int)(sizeof c->locals / sizeof c->locals[0])) {
        fprintf(stderr, "wispc: too many locals in scope (pushing '%.*s')\n", (int)n, name);
        exit(1);
    }
    c->locals[c->nlocals++] = (Local){ name, n, k, expr, src_name };
}
void pop_local(CGCtx *c) { if (c->nlocals > 0) c->nlocals--; }
Local *find_local(CGCtx *c, const char *name, size_t n) {
    for (int i = c->nlocals - 1; i >= 0; i--)
        if (c->locals[i].nlen == n && memcmp(c->locals[i].name, name, n) == 0)
            return &c->locals[i];
    return NULL;
}
Decl *find_konst(CGCtx *c, const char *name, size_t n) {
    for (int i = 0; i < c->nkonst; i++)
        if (c->konst[i]->nlen == n && memcmp(c->konst[i]->name, name, n) == 0)
            return c->konst[i];
    return NULL;
}

CE lower(CGCtx *c, Expr *e);

/* Substitute "$W" with the widget var in a template; returns malloc'd string. */
char *expand_widget(const char *tmpl, const char *wvar) {
    if (!wvar) wvar = "w";
    size_t L = strlen(tmpl), out_cap = L * 2 + 16, out_n = 0;
    char *out = malloc(out_cap);
    for (size_t i = 0; i < L; i++) {
        if (tmpl[i] == '$' && i + 1 < L && tmpl[i+1] == 'W') {
            size_t wl = strlen(wvar);
            while (out_n + wl + 1 > out_cap) { out_cap *= 2; out = realloc(out, out_cap); }
            memcpy(out + out_n, wvar, wl); out_n += wl; i++;
        } else {
            if (out_n + 2 > out_cap) { out_cap *= 2; out = realloc(out, out_cap); }
            out[out_n++] = tmpl[i];
        }
    }
    out[out_n] = 0; return out;
}

/* Promote a CE to a string. For numbers, snprintf to a temp buf (prelude). */
CE coerce_to_str(CGCtx *c, CE e, Loc loc) {
    if (e.type == T_STR) return e;
    if (e.type == T_UNK) return e;
    cgctx_open_prelude(c);
    int seq = c->buf_seq++;
    fprintf(c->prelude, "static char tbuf%d[32]; ", seq);
    if (e.type == T_INT || e.type == T_BOOL)
        fprintf(c->prelude, "snprintf(tbuf%d, 32, \"%%d\", (int)(%s));", seq, e.text);
    else if (e.type == T_FLOAT)
        fprintf(c->prelude, "snprintf(tbuf%d, 32, \"%%g\", (double)(%s));", seq, e.text);
    else if (e.type == T_COLOR)
        fprintf(c->prelude, "snprintf(tbuf%d, 32, \"#%%08x\", (unsigned)(%s));", seq, e.text);
    else { (void)loc; }
    CE r = { .type = T_STR };
    snprintf(r.text, sizeof r.text, "tbuf%d", seq);
    return r;
}

CE lower_member(CGCtx *c, Expr *e) {
    Expr *b = e->member.base;
    const char *fld = e->member.field; size_t flen = e->member.flen;
    if (b->kind != EX_IDENT) {
        diag_error(e->loc, "codegen: member access on non-ident not supported");
        c->failed = 1;
        CE z = { .text = "0", .type = T_UNK }; return z;
    }
    /* local (for-loop var) member? */
    Local *L = find_local(c, b->ident.s, b->ident.n);
    if (L && L->kind == LB_DBUS_HIST_IT) {
        CE r = { .type = T_UNK };
        const char *src = L->src_name;
        const char *it  = L->c_expr;
        if (flen == 7 && memcmp(fld, "summary", 7) == 0) {
            snprintf(r.text, sizeof r.text, "src_%s_hist[%s].summary", src, it); r.type = T_STR;
        } else if (flen == 4 && memcmp(fld, "body", 4) == 0) {
            snprintf(r.text, sizeof r.text, "src_%s_hist[%s].body", src, it); r.type = T_STR;
        } else if (flen == 3 && memcmp(fld, "url", 3) == 0) {
            snprintf(r.text, sizeof r.text, "src_%s_hist[%s].url", src, it); r.type = T_STR;
        } else if (flen == 6 && memcmp(fld, "urgent", 6) == 0) {
            snprintf(r.text, sizeof r.text, "((int)src_%s_hist[%s].urgent >= 2)", src, it); r.type = T_BOOL;
        } else {
            diag_error(e->loc, "codegen: dbus history entry has no field '%.*s'", (int)flen, fld);
            c->failed = 1;
        }
        return r;
    }
    if (L && L->kind == LB_TRAY_IT) {
        const char *it = L->c_expr;
        CE r = { .type = T_UNK };
        if (flen == 4 && memcmp(fld, "icon", 4) == 0) {
            snprintf(r.text, sizeof r.text, "tray_icon(%s)", it);
            r.type = T_PIXMAP;
            /* Collapse the reserved square when the item has no pixmap — an
             * icon-less item would otherwise render as a hole in the row. */
            static char pms[64];   /* consumed by the caller's fprintf at once */
            snprintf(pms, sizeof pms, "(tray_icon(%s) ? TRAY_ICON_PX : 0)", it);
            r.pm_size = pms;
        } else if (flen == 8 && memcmp(fld, "has_icon", 8) == 0) {
            snprintf(r.text, sizeof r.text, "(tray_icon(%s) != 0)", it); r.type = T_BOOL;
        } else if (flen == 5 && memcmp(fld, "title", 5) == 0) {
            snprintf(r.text, sizeof r.text, "tray_title(%s)", it); r.type = T_STR;
        } else if (flen == 2 && memcmp(fld, "id", 2) == 0) {
            snprintf(r.text, sizeof r.text, "tray_id(%s)", it); r.type = T_STR;
        } else if (flen == 6 && memcmp(fld, "status", 6) == 0) {
            snprintf(r.text, sizeof r.text, "tray_status(%s)", it); r.type = T_STR;
        } else if (flen == 5 && memcmp(fld, "index", 5) == 0) {
            snprintf(r.text, sizeof r.text, "(%s)", it); r.type = T_INT;
        } else if (flen == 9 && memcmp(fld, "menu_open", 9) == 0) {
            snprintf(r.text, sizeof r.text, "tray_menu_is_open(%s)", it); r.type = T_BOOL;
        } else {
            diag_error(e->loc, "codegen: tray item has no field '%.*s'", (int)flen, fld);
            c->failed = 1;
        }
        return r;
    }
    if (L && L->kind == LB_MENU_SELF) {
        const char *wv = c->widget_var ? c->widget_var : "w";
        CE r = { .type = T_UNK };
        if (flen == 5 && memcmp(fld, "query", 5) == 0) {
            snprintf(r.text, sizeof r.text, "%s->s.menu.query", wv);
            r.type = T_STR;
        } else if (flen == 6 && memcmp(fld, "prompt", 6) == 0) {
            snprintf(r.text, sizeof r.text,
                     "(%s->s.menu.prompt[0] ? %s->s.menu.prompt : %s)",
                     wv, wv, L->c_expr);
            r.type = T_STR;
        } else if (flen == 5 && memcmp(fld, "count", 5) == 0) {
            snprintf(r.text, sizeof r.text, "%s->s.menu.n_filtered", wv);
            r.type = T_INT;
        } else {
            diag_error(e->loc, "codegen: menu has no field '%.*s'", (int)flen, fld);
            c->failed = 1;
        }
        return r;
    }
    if (L && L->kind == LB_MENU_ROW) {
        const char *wv = c->widget_var ? c->widget_var : "w";
        const char *r0 = L->c_expr;
        CE r = { .type = T_UNK };
        if (flen == 5 && memcmp(fld, "label", 5) == 0) {
            snprintf(r.text, sizeof r.text, "%s->s.menu.items[%s->s.menu.filtered[%s]]", wv, wv, r0);
            r.type = T_STR;
        } else if (flen == 4 && memcmp(fld, "icon", 4) == 0) {
            snprintf(r.text, sizeof r.text,
                     "(%s->s.menu.icons ? %s->s.menu.icons[%s->s.menu.filtered[%s]] : 0)",
                     wv, wv, wv, r0);
            r.type = T_PIXMAP;
            /* Reserved even when this row has no icon, so labels stay aligned. */
            r.pm_size = "(w->s.menu.icons ? w->s.menu.icon_px : 0)";
        } else if (flen == 8 && memcmp(fld, "selected", 8) == 0) {
            snprintf(r.text, sizeof r.text, "(%s == %s->s.menu.sel)", r0, wv);
            r.type = T_BOOL;
        } else if (flen == 5 && memcmp(fld, "index", 5) == 0) {
            snprintf(r.text, sizeof r.text, "(%s)", r0);
            r.type = T_INT;
        } else {
            diag_error(e->loc, "codegen: menu row has no field '%.*s'", (int)flen, fld);
            c->failed = 1;
        }
        return r;
    }
    if (L && L->kind == LB_TAG_IDX) {
        SrcInst *si = find_inst(c->srcs, c->nsrc, L->src_name, strlen(L->src_name));
        const char *wv = c->widget_var ? c->widget_var : "w";
        CE r = { .type = T_UNK };
        if (flen == 5 && memcmp(fld, "label", 5) == 0) {
            snprintf(r.text, sizeof r.text, "TAG_LABEL[%s]", L->c_expr);
            r.type = T_STR;
        } else if (flen == 5 && memcmp(fld, "index", 5) == 0) {
            snprintf(r.text, sizeof r.text, "(%s + 1)", L->c_expr);
            r.type = T_INT;
        } else if (flen == 6 && memcmp(fld, "output", 6) == 0) {
            /* Slot index of the output this bar sits on — lets on_click exec
             * target the clicked monitor, not the kbd-focused one. */
            snprintf(r.text, sizeof r.text, "(int)((%s)->output - outputs)", wv);
            r.type = T_INT;
        } else if (flen == 6 && memcmp(fld, "active", 6) == 0) {
            snprintf(r.text, sizeof r.text, "(((%s)->s.bar.active_mask >> (%s)) & 1u)", wv, L->c_expr);
            r.type = T_BOOL;
        } else if (flen == 6 && memcmp(fld, "urgent", 6) == 0) {
            snprintf(r.text, sizeof r.text, "(((%s)->s.bar.urgent_mask >> (%s)) & 1u)", wv, L->c_expr);
            r.type = T_BOOL;
        } else if (flen == 6 && memcmp(fld, "pinned", 6) == 0) {
            snprintf(r.text, sizeof r.text, "((TAG_PINNED >> (%s)) & 1u)", L->c_expr);
            r.type = T_BOOL;
        } else if (flen == 8 && memcmp(fld, "occupied", 8) == 0) {
            snprintf(r.text, sizeof r.text, "(((%s)->s.bar.tag_mask >> (%s)) & 1u)", wv, L->c_expr);
            r.type = T_BOOL;
        } else {
            diag_error(e->loc, "codegen: tag has no field '%.*s'", (int)flen, fld);
            c->failed = 1;
        }
        (void)si; return r;
    }
    /* source.field? */
    SrcInst *si = find_inst(c->srcs, c->nsrc, b->ident.s, b->ident.n);
    if (si) {
        /* DRV_EXEC: only .value is valid, lowers to src_<n>_line. */
        if (si->drv->drv == DRV_EXEC) {
            if (flen != 5 || memcmp(fld, "value", 5) != 0) {
                diag_error(e->loc, "codegen: exec_line has no field '%.*s'", (int)flen, fld);
                c->failed = 1;
                CE z = { .text = "0", .type = T_UNK }; return z;
            }
            CE r;
            snprintf(r.text, sizeof r.text, "src_%s_line", sname(si->decl->name, si->decl->nlen));
            r.type = T_STR;
            return r;
        }
        /* DRV_INOTIFY: only .value is valid, lowers to src_<n>_value. */
        if (si->drv->drv == DRV_INOTIFY) {
            if (flen != 5 || memcmp(fld, "value", 5) != 0) {
                diag_error(e->loc, "codegen: inotify has no field '%.*s'", (int)flen, fld);
                c->failed = 1;
                CE z = { .text = "0", .type = T_UNK }; return z;
            }
            CE r;
            snprintf(r.text, sizeof r.text, "src_%s_value", sname(si->decl->name, si->decl->nlen));
            r.type = T_STR;
            return r;
        }
        /* DRV_DBUS: .value lowers to src_<n>_value. .history is post-v0. */
        if (si->drv->drv == DRV_DBUS) {
            if (flen == 7 && memcmp(fld, "history", 7) == 0) {
                diag_error(e->loc, "codegen: dbus_signal.history (list semantics) is post-v0");
                c->failed = 1;
                CE z = { .text = "0", .type = T_UNK }; return z;
            }
            if (flen != 5 || memcmp(fld, "value", 5) != 0) {
                diag_error(e->loc, "codegen: dbus_signal has no field '%.*s'", (int)flen, fld);
                c->failed = 1;
                CE z = { .text = "0", .type = T_UNK }; return z;
            }
            CE r;
            snprintf(r.text, sizeof r.text, "src_%s_value", sname(si->decl->name, si->decl->nlen));
            r.type = T_STR;
            return r;
        }
        /* toplevel: fields carry the declaration's match-table index. */
        if (si->drv->drv == DRV_WISP && !strcmp(si->drv->name, "toplevel")) {
            int idx = tl_match_index(c->srcs, c->nsrc, si);
            CE r = { .type = T_UNK };
            if (flen == 6 && memcmp(fld, "exists", 6) == 0) {
                snprintf(r.text, sizeof r.text, "tl_exists(%d)", idx); r.type = T_BOOL;
            } else if (flen == 5 && memcmp(fld, "count", 5) == 0) {
                snprintf(r.text, sizeof r.text, "tl_count(%d)", idx); r.type = T_INT;
            } else if (flen == 5 && memcmp(fld, "title", 5) == 0) {
                snprintf(r.text, sizeof r.text, "tl_title(%d)", idx); r.type = T_STR;
            } else {
                diag_error(e->loc, "codegen: toplevel has no field '%.*s'", (int)flen, fld);
                c->failed = 1;
            }
            return r;
        }
        int is_str = 0;
        const char *tmpl = drv_field_expr(si->drv, fld, flen, &is_str);
        if (!tmpl) {
            diag_error(e->loc, "codegen: source '%.*s' has no field '%.*s'",
                       (int)b->ident.n, b->ident.s, (int)flen, fld);
            c->failed = 1;
            CE z = { .text = "0", .type = T_UNK }; return z;
        }
        char *expanded = expand_widget(tmpl, c->widget_var);
        CE r;
        snprintf(r.text, sizeof r.text, "%s", expanded);
        free(expanded);
        r.type = is_str ? T_STR : T_INT;
        return r;
    }
    /* `anim.emerged_h` / `anim.emerged_w` — the body's emerged extent past the
     * gutter along the slide axis, in pixels. Mirrors the codegen-internal
     * __sur_f_emerged formula so DSL can drive e.g. `radius = anim.emerged_h`.
     * Returns 0 for non-HUD widgets so the expression is safe to evaluate
     * outside a sliding context. */
    if (b->ident.n == 4 && memcmp(b->ident.s, "anim", 4) == 0) {
        const char *wv = c->widget_var ? c->widget_var : "w";
        const char *dim = NULL;
        if (flen == 9 && memcmp(fld, "emerged_h", 9) == 0) dim = "h";
        else if (flen == 9 && memcmp(fld, "emerged_w", 9) == 0) dim = "w";
        if (dim) {
            CE r = { .type = T_INT };
            snprintf(r.text, sizeof r.text,
                "(((%s)->kind == W_HUD && (int)(%s)->%s - (int)(%s)->s.hud.trigger_size - (int)(%s)->s.hud.cur_off > 0) ? "
                "((int)(%s)->%s - (int)(%s)->s.hud.trigger_size - (int)(%s)->s.hud.cur_off) : 0)",
                wv, wv, dim, wv, wv, wv, dim, wv, wv);
            return r;
        }
        diag_error(e->loc, "codegen: anim has no field '%.*s' (expected emerged_h or emerged_w)", (int)flen, fld);
        c->failed = 1;
        CE z = { .text = "0", .type = T_UNK }; return z;
    }
    diag_error(e->loc, "codegen: unknown identifier '%.*s' in member access",
               (int)b->ident.n, b->ident.s);
    c->failed = 1;
    CE z = { .text = "0", .type = T_UNK }; return z;
}

CE lower_ident(CGCtx *c, Expr *e) {
    const char *n = e->ident.s; size_t L = e->ident.n;
    Local *lc = find_local(c, n, L);
    if (lc) {
        CE r;
        snprintf(r.text, sizeof r.text, "%s", lc->c_expr);
        r.type = (lc->kind == LB_CLICK_PARAM) ? T_STR : T_INT;
        return r;
    }
    Decl *k = find_konst(c, n, L);
    if (k) {
        if (k->kind == D_MUT) {
            /* mut reads compile to the static variable. Type is inferred
             * from the initializer's lowered CE.type. */
            CE init = lower(c, k->konst.val);
            CE r;
            snprintf(r.text, sizeof r.text, "mut_%s", sname(n, L));
            r.type = init.type;
            return r;
        }
        return lower(c, k->konst.val);  /* D_CONST inlines */
    }
    SrcInst *si = find_inst(c->srcs, c->nsrc, n, L);
    if (si) {
        CE r;
        if (si->drv->drv == DRV_CLOCK || si->drv->drv == DRV_INOTIFY) {
            snprintf(r.text, sizeof r.text, "src_%s_value", sname(n, L));
            r.type = T_STR;
        } else if (si->drv->drv == DRV_EXEC) {
            snprintf(r.text, sizeof r.text, "src_%s_line", sname(n, L));
            r.type = T_STR;
        } else if (si->drv->drv == DRV_DBUS) {
            snprintf(r.text, sizeof r.text, "src_%s_value", sname(n, L));
            r.type = T_STR;
        } else {
            /* fallback to primary field */
            const char *prim = si->drv->fields[0].field;
            int is_str = si->drv->fields[0].is_string;
            char *expanded = expand_widget(si->drv->fields[0].c_expr, c->widget_var);
            (void)prim;
            snprintf(r.text, sizeof r.text, "%s", expanded);
            free(expanded);
            r.type = is_str ? T_STR : T_INT;
        }
        return r;
    }
    /* Built-in identifier enums shouldn't reach here in a value position;
     * if they do (e.g. surface body misuse) fall through with a literal 0. */
    diag_error(e->loc, "codegen: unresolved identifier '%.*s'", (int)L, n);
    c->failed = 1;
    CE z = { .text = "0", .type = T_UNK }; return z;
}

const char *op_C(Op o) {
    switch (o) {
    case OP_ADD: return "+"; case OP_SUB: return "-";
    case OP_MUL: return "*"; case OP_DIV: return "/"; case OP_MOD: return "%";
    case OP_EQ:  return "=="; case OP_NEQ: return "!=";
    case OP_LT:  return "<";  case OP_GT:  return ">";
    case OP_LE:  return "<="; case OP_GE:  return ">=";
    case OP_AND: return "&&"; case OP_OR:  return "||";
    case OP_BITOR: return "|"; case OP_BITAND: return "&";
    default: return "?";
    }
}

CE lower(CGCtx *c, Expr *e) {
    CE r = { .text = "0", .type = T_UNK };
    if (!e) return r;
    switch (e->kind) {
    case EX_INT:    snprintf(r.text, sizeof r.text, "%lld", (long long)e->i); r.type = T_INT;   return r;
    case EX_RANGE:  /* `lo..hi` in expression context lowers to its lo endpoint. */
                    return lower(c, e->range.lo);
    case EX_FLOAT:  snprintf(r.text, sizeof r.text, "%g",   e->f);            r.type = T_FLOAT; return r;
    case EX_BOOL:   snprintf(r.text, sizeof r.text, "%d",   e->b ? 1 : 0);    r.type = T_BOOL;  return r;
    case EX_COLOR:  snprintf(r.text, sizeof r.text, "0x%08xu", e->color);     r.type = T_COLOR; return r;
    case EX_STRING: {
        /* Emit as a string literal embedded inline. */
        FILE *m = open_memstream(&(char*){NULL}, &(size_t){0});
        (void)m;
        char *s = strndup0(e->str.s, e->str.n);
        size_t need = e->str.n * 4 + 8;
        if (need > sizeof r.text) need = sizeof r.text;
        /* manually escape into r.text */
        size_t o = 0; r.text[o++] = '"';
        for (size_t i = 0; i < e->str.n && o + 5 < sizeof r.text; i++) {
            unsigned char ch = (unsigned char)s[i];
            if (ch == '\\' || ch == '"') { r.text[o++] = '\\'; r.text[o++] = ch; }
            else if (ch == '\n')         { r.text[o++] = '\\'; r.text[o++] = 'n'; }
            else if (ch < 32)            { o += snprintf(r.text + o, sizeof r.text - o, "\\x%02x", ch); }
            else                          { r.text[o++] = ch; }
        }
        r.text[o++] = '"'; r.text[o] = 0;
        free(s); r.type = T_STR; return r;
    }
    case EX_IDENT: return lower_ident(c, e);
    case EX_MEMBER:return lower_member(c, e);
    case EX_DOLLAR: {
        /* Resolve a `$name` template arg by looking for an LB_DOLLAR_BIND
         * local with matching name. Bindings are pushed by
         * emit_spawned_surface around the per-slab widget emission so each
         * iteration sees the right C expression for that slab's field. */
        for (int i = c->nlocals - 1; i >= 0; i--) {
            Local *l = &c->locals[i];
            if (l->kind != LB_DOLLAR_BIND) continue;
            if (l->nlen != e->ident.n) continue;
            if (memcmp(l->name, e->ident.s, e->ident.n) != 0) continue;
            snprintf(r.text, sizeof r.text, "(%s)", l->c_expr);
            /* Type inferred from binding: codegen_surface.c picks T_STR for
             * char-array fields and T_INT otherwise via the src_name slot. */
            r.type = (l->src_name && l->src_name[0] == 's') ? T_STR : T_INT;
            return r;
        }
        diag_error(e->loc, "codegen: '$%.*s' has no binding in this context",
                   (int)e->ident.n, e->ident.s);
        c->failed = 1; return r;
    }
    case EX_CALL:
        diag_error(e->loc, "codegen: call expressions in values not supported");
        c->failed = 1; return r;
    case EX_UN: {
        CE x = lower(c, e->un.e);
        if (e->un.op == OP_NOT) { snprintf(r.text, sizeof r.text, "(!(%s))", x.text); r.type = T_BOOL; }
        else                    { snprintf(r.text, sizeof r.text, "(-(%s))", x.text); r.type = x.type; }
        return r;
    }
    case EX_BIN: {
        CE l = lower(c, e->bin.l), rr = lower(c, e->bin.r);
        /* String == / != lowers to strcmp; raw `==` on `char*` would be a
         * pointer compare and never match. Treat NULL safely. */
        if ((e->bin.op == OP_EQ || e->bin.op == OP_NEQ)
            && (l.type == T_STR || rr.type == T_STR)) {
            CE ls = (l.type  == T_STR) ? l  : coerce_to_str(c, l,  e->bin.l->loc);
            CE rs = (rr.type == T_STR) ? rr : coerce_to_str(c, rr, e->bin.r->loc);
            snprintf(r.text, sizeof r.text, "(%swisp_streq((%s),(%s)))",
                     (e->bin.op == OP_EQ) ? "" : "!", ls.text, rs.text);
            r.type = T_BOOL;
            return r;
        }
        snprintf(r.text, sizeof r.text, "(%s %s %s)", l.text, op_C(e->bin.op), rr.text);
        switch (e->bin.op) {
        case OP_EQ: case OP_NEQ: case OP_LT: case OP_GT:
        case OP_LE: case OP_GE: case OP_AND: case OP_OR: r.type = T_BOOL; break;
        default: r.type = (l.type == T_FLOAT || rr.type == T_FLOAT) ? T_FLOAT : T_INT; break;
        }
        return r;
    }
    case EX_TERN: {
        CE cnd = lower(c, e->tern.cond);
        CE t = lower(c, e->tern.t), el = lower(c, e->tern.e);
        snprintf(r.text, sizeof r.text, "((%s) ? (%s) : (%s))", cnd.text, t.text, el.text);
        r.type = (t.type == el.type) ? t.type :
                 ((t.type == T_FLOAT || el.type == T_FLOAT) ? T_FLOAT : t.type);
        return r;
    }
    case EX_INTERP: {
        /* Build a snprintf into a static char buf in the prelude. */
        cgctx_open_prelude(c);
        int seq = c->buf_seq++;
        /* Format string is built into `fmt` first, not straight into the
         * prelude: the buffer must be sized from the *lowered* part types (a
         * %s can be a 256-char notification body, a %d never exceeds 11), and
         * lowering a nested interp writes its own decl to the prelude — mid
         * fprintf that would land inside our format string literal. */
        char fmt[1024]; size_t fn = 0;
        size_t budget = 16;
        #define FMTC(ch) do { if (fn < sizeof fmt - 1) fmt[fn++] = (ch); } while (0)
        #define FMTS(s)  do { for (const char *_p = (s); *_p; _p++) FMTC(*_p); } while (0)
        CE args[16]; int nargs = 0;
        for (int i = 0; i < e->interp.nparts; i++) {
            InterpPart *p = &e->interp.parts[i];
            if (!p->is_expr) {
                budget += p->llen;
                for (size_t k = 0; k < p->llen; k++) {
                    unsigned char ch = (unsigned char)p->lit[k];
                    if (ch == '%') { FMTC('%'); FMTC('%'); }
                    else if (ch == '\\' || ch == '"') { FMTC('\\'); FMTC(ch); }
                    else if (ch == '\n') FMTS("\\n");
                    else if (ch < 32) { char t[8]; snprintf(t, sizeof t, "\\x%02x", ch); FMTS(t); }
                    else FMTC(ch);
                }
                continue;
            }
            CE ce = lower(c, p->expr);
            if (nargs >= 16) { diag_error(e->loc, "codegen: too many interp args"); c->failed = 1; break; }
            args[nargs++] = ce;
            switch (ce.type) {
            case T_INT: case T_BOOL: FMTS("%d");    budget += 12;  break;
            case T_FLOAT:            FMTS("%g");    budget += 24;  break;
            case T_COLOR:            FMTS("#%08x"); budget += 10;  break;
            /* A string part is the only unbounded one — an OSD body wraps to
             * OSD_MAX_BODY_LINES × OSD_BODY_MAX. Budget for a real one. */
            case T_STR: default:     FMTS("%s");    budget += 288; break;
            }
        }
        fmt[fn] = 0;
        #undef FMTC
        #undef FMTS
        int bufsize = (int)budget;
        if (bufsize < 64) bufsize = 64;
        if (bufsize > 2048) bufsize = 2048;
        fprintf(c->prelude, "static char ibuf%d[%d]; ", seq, bufsize);
        fprintf(c->prelude, "snprintf(ibuf%d, %d, \"%s\"", seq, bufsize, fmt);
        for (int i = 0; i < nargs; i++) {
            fputs(", ", c->prelude);
            if (args[i].type == T_INT || args[i].type == T_BOOL)
                fprintf(c->prelude, "(int)(%s)", args[i].text);
            else if (args[i].type == T_FLOAT)
                fprintf(c->prelude, "(double)(%s)", args[i].text);
            else if (args[i].type == T_COLOR)
                fprintf(c->prelude, "(unsigned)(%s)", args[i].text);
            else
                fprintf(c->prelude, "(%s)", args[i].text);
        }
        fputs(");", c->prelude);
        snprintf(r.text, sizeof r.text, "ibuf%d", seq);
        r.type = T_STR;
        return r;
    }
    }
    return r;
}

/* Coerce to int (for visible-guard, width, pad, etc.) */
CE coerce_to_int(CGCtx *c, CE e) {
    if (e.type == T_INT || e.type == T_BOOL) return e;
    if (e.type == T_STR) { /* truthy = non-empty non-null */
        CE r;
        snprintf(r.text, sizeof r.text, "((%s) && (%s)[0])", e.text, e.text);
        r.type = T_INT; return r;
    }
    CE r = e; r.type = T_INT; return r;
    (void)c;
}

/* Recursive statement emitter used from event handlers (on_click bodies).
 * Handles ST_EXEC / ST_SET / ST_EMIT / ST_BLOCK. */
void emit_stmt(FILE *o, CGCtx *ctx, Stmt *st, const char *indent,
                      SemaResult *r) {
    if (!st) return;
    switch (st->kind) {
    case ST_BLOCK:
        for (int i = 0; i < st->block.n; i++)
            emit_stmt(o, ctx, st->block.list[i], indent, r);
        return;
    case ST_EXEC: {
        CE ce = lower(ctx, st->exec.arg);
        if (ce.type != T_STR) ce = coerce_to_str(ctx, ce, st->loc);
        cgctx_flush_prelude(ctx, o, indent);
        fprintf(o, "%sexec_cmd(%s);\n", indent, ce.text);
        return;
    }
    case ST_SET: {
        const char *nm = sname(st->set.name, st->set.nlen);
        Decl *k = find_konst(ctx, st->set.name, st->set.nlen);
        /* Allow `set <exec_line_source> = <expr>` to override the source's
         * line buffer optimistically. Used so a toggle button can paint the
         * post-click state in the same frame, instead of waiting for the
         * spawned probe to re-poll over a pipe. */
        SrcInst *si = find_inst(ctx->srcs, ctx->nsrc, st->set.name, st->set.nlen);
        CE val = lower(ctx, st->set.val);
        cgctx_flush_prelude(ctx, o, indent);
        if (si && si->drv->drv == DRV_EXEC) {
            if (val.type != T_STR) val = coerce_to_str(ctx, val, st->loc);
            cgctx_flush_prelude(ctx, o, indent);
            fprintf(o, "%ssnprintf(src_%s_line, sizeof src_%s_line, \"%%s\", %s);\n",
                    indent, nm, nm, val.text);
        } else if (k && k->konst.val && k->konst.val->kind == EX_STRING) {
            /* String mut: copy with bounded snprintf. */
            if (val.type != T_STR) val = coerce_to_str(ctx, val, st->loc);
            cgctx_flush_prelude(ctx, o, indent);
            fprintf(o, "%ssnprintf(mut_%s, sizeof mut_%s, \"%%s\", %s);\n",
                    indent, nm, nm, val.text);
        } else {
            fprintf(o, "%smut_%s = (%s);\n", indent, nm, val.text);
        }
        /* Mark every surface that reads this mut/source dirty. sema records
         * deps uniformly for both source and mut names. */
        if (r) {
            for (int i = 0; i < r->nsurfaces; i++) {
                const char **deps = r->surface_deps[i];
                for (int j = 0; deps && deps[j]; j++) {
                    if (strcmp(deps[j], nm) == 0) {
                        fprintf(o, "%sdirty_%s = 1;\n", indent, r->surface_names[i]);
                        break;
                    }
                }
            }
        }
        return;
    }
    case ST_ANIMATE: {
        const char *nm = sname(st->anim.name, st->anim.nlen);
        Decl *k = find_konst(ctx, st->anim.name, st->anim.nlen);
        const char *easing_id = "EASE_LINEAR";
        const char *bez_arg = "NULL";
        char bez_buf[160] = "";
        if (st->anim.easing) {
            Expr *e = st->anim.easing;
            if (e->kind == EX_IDENT) {
                const char *en = e->ident.s; size_t eL = e->ident.n;
                if      (eL == 6 && !memcmp(en, "linear",      6)) easing_id = "EASE_LINEAR";
                else if (eL == 7 && !memcmp(en, "ease_in",     7)) easing_id = "EASE_IN";
                else if (eL == 8 && !memcmp(en, "ease_out",    8)) easing_id = "EASE_OUT";
                else if (eL == 11 && !memcmp(en, "ease_in_out",11)) easing_id = "EASE_IN_OUT";
                else diag_error(e->loc, "unknown easing '%.*s'", (int)eL, en);
            } else if (e->kind == EX_CALL && e->call.nlen == 13 &&
                       !memcmp(e->call.name, "cubic_bezier", 12)) {
                /* fall-through below */
            } else if (e->kind == EX_CALL && e->call.nlen == 12 &&
                       !memcmp(e->call.name, "cubic_bezier", 12) && e->call.nargs == 4) {
                easing_id = "EASE_CUBIC_BEZIER";
                CE a = lower(ctx, e->call.args[0]);
                CE b = lower(ctx, e->call.args[1]);
                CE c = lower(ctx, e->call.args[2]);
                CE d = lower(ctx, e->call.args[3]);
                snprintf(bez_buf, sizeof bez_buf,
                         "(double[]){(double)(%s),(double)(%s),(double)(%s),(double)(%s)}",
                         a.text, b.text, c.text, d.text);
                bez_arg = bez_buf;
            } else {
                diag_error(e->loc, "easing must be ident or cubic_bezier(a,b,c,d)");
            }
        }
        CE to = lower(ctx, st->anim.to);
        CE dur = lower(ctx, st->anim.duration);
        cgctx_flush_prelude(ctx, o, indent);
        if (!k || k->kind != D_MUT) {
            fprintf(o, "%s/* animate: target '%s' not a mut */\n", indent, nm);
            return;
        }
        CE init = lower(ctx, k->konst.val);
        const char *fn = "anim_start_num";
        const char *type_id = "ANIM_T_INT";
        if (init.type == T_FLOAT) type_id = "ANIM_T_FLOAT";
        else if (init.type == T_COLOR) { fn = "anim_start_color"; type_id = NULL; }
        else if (init.type == T_STR) {
            diag_error(st->loc, "cannot animate a string mut");
            return;
        }
        /* Owner widget: `w` is in scope inside <surf>_on_click handlers.
         * Outside click context the codegen-emitted ctx doesn't bind `w` yet
         * — for now pass NULL (acceptable: bar_redraw_all() on the next status
         * tick will catch up). */
        const char *owner = "w";
        if (type_id) {
            fprintf(o, "%s%s(&mut_%s, %s, (double)(mut_%s), (double)(%s), (int)(%s), %s, %s, %s, NULL, NULL);\n",
                    indent, fn, nm, type_id, nm, to.text, dur.text, easing_id, bez_arg, owner);
        } else {
            fprintf(o, "%s%s(&mut_%s, mut_%s, (uint32_t)(%s), (int)(%s), %s, %s, %s, NULL, NULL);\n",
                    indent, fn, nm, nm, to.text, dur.text, easing_id, bez_arg, owner);
        }
        /* Mark surfaces that read this mut dirty so any side-effects (visible/
         * static layout) recompute on next tick. */
        if (r) {
            for (int i = 0; i < r->nsurfaces; i++) {
                const char **deps = r->surface_deps[i];
                for (int j = 0; deps && deps[j]; j++) {
                    if (strcmp(deps[j], nm) == 0) {
                        fprintf(o, "%sdirty_%s = 1;\n", indent, r->surface_names[i]);
                        break;
                    }
                }
            }
        }
        return;
    }
    case ST_EMIT: {
        const char *nm = sname(st->emit.name, st->emit.nlen);
        /* Lower each positional value into a comma-separated arg list. */
        char args[1024]; size_t off = 0; args[0] = 0;
        for (int i = 0; i < st->emit.n; i++) {
            CE ce = lower(ctx, st->emit.val[i]);
            if (off && off + 2 < sizeof args) { args[off++] = ','; args[off++] = ' '; args[off] = 0; }
            int wrote = snprintf(args + off, sizeof args - off, "(%s)", ce.text);
            if (wrote > 0) off += (size_t)wrote;
        }
        cgctx_flush_prelude(ctx, o, indent);
        /* Forward decl + call. Only spawn_osd() is emitted (gen_spawn.c); any
         * other template deliberately fails at link until a generic slot
         * allocator exists. */
        fprintf(o, "%sextern void spawn_%s();\n", indent, nm);
        fprintf(o, "%sspawn_%s(%s);\n", indent, nm, args);
        return;
    }
    }
}

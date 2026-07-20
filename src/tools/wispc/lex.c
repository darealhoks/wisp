#include "wispc.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

static const struct { const char *kw; TokKind k; } KWS[] = {
    {"source",   TK_KW_SOURCE},
    {"surface",  TK_KW_SURFACE},
    {"widget",   TK_KW_WIDGET},
    {"const",    TK_KW_CONST},
    {"mut",      TK_KW_MUT},
    {"lock",     TK_KW_LOCK},
    {"gamma",    TK_KW_GAMMA},
    {"wallpaper",TK_KW_WALLPAPER},
    {"media",    TK_KW_MEDIA},
    {"compound", TK_KW_COMPOUND},
    {"region",   TK_KW_REGION},
    {"group",    TK_KW_GROUP},
    {"for",      TK_KW_FOR},
    {"in",       TK_KW_IN},
    {"cell",     TK_KW_CELL},
    {"true",     TK_KW_TRUE},
    {"false",    TK_KW_FALSE},
    {"on_click", TK_KW_ON_CLICK},
    {"on_scroll",TK_KW_ON_SCROLL},
    {"on_press", TK_KW_ON_PRESS},
    {"on_release",TK_KW_ON_RELEASE},
    {"on_drag",  TK_KW_ON_DRAG},
    {"on_change",TK_KW_ON_CHANGE},
    {"exec",     TK_KW_EXEC},
    {"emit",     TK_KW_EMIT},
    {"set",      TK_KW_SET},
    {"animate",  TK_KW_ANIMATE},
};

static void advance(Lexer *L, int n) {
    while (n--) {
        if (*L->p == '\n') { L->line++; L->col = 1; }
        else L->col++;
        L->p++;
    }
}

static int is_id_start(int c) { return c == '_' || isalpha(c); }
static int is_id_cont (int c) { return c == '_' || isalnum(c); }

static void skip_ws(Lexer *L) {
    for (;;) {
        while (*L->p && isspace((unsigned char)*L->p)) advance(L, 1);
        if (L->p[0] == '/' && L->p[1] == '/') {
            while (*L->p && *L->p != '\n') advance(L, 1);
        } else if (L->p[0] == '/' && L->p[1] == '*') {
            advance(L, 2);
            while (*L->p && !(L->p[0] == '*' && L->p[1] == '/')) advance(L, 1);
            if (*L->p) advance(L, 2);
        } else break;
    }
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void scan_color(Lexer *L, Tok *t) {
    /* '#' already consumed (no — we consume it here) */
    Loc loc = { L->file, L->line, L->col };
    advance(L, 1);                              /* skip '#' */
    const char *start = L->p;
    int n = 0;
    while (hexval((unsigned char)*L->p) >= 0) { advance(L, 1); n++; }
    if (n != 6 && n != 8) {
        diag_error(loc, "color literal must be 6 or 8 hex digits (got %d)", n);
    }
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 4) | (uint32_t)hexval((unsigned char)start[i]);
    if (n == 6) v |= 0xFF000000u;               /* default opaque */
    t->kind = TK_COLOR;
    t->loc  = loc;
    t->s    = start; t->len = n;
    t->i    = (int64_t)(uint64_t)v;
}

static void scan_number(Lexer *L, Tok *t) {
    Loc loc = { L->file, L->line, L->col };
    const char *start = L->p;
    int is_float = 0, is_hex = 0;
    if (L->p[0] == '0' && (L->p[1] == 'x' || L->p[1] == 'X')) {
        advance(L, 2); is_hex = 1;
        while (hexval((unsigned char)*L->p) >= 0) advance(L, 1);
    } else {
        while (isdigit((unsigned char)*L->p)) advance(L, 1);
        /* Don't grab the first dot of `..` — leave both for TK_DOTDOT so
         * `16..0` lexes as INT(16) DOTDOT INT(0), not FLOAT(16.) DOT INT(0). */
        if (*L->p == '.' && L->p[1] != '.') {
            is_float = 1; advance(L, 1);
            while (isdigit((unsigned char)*L->p)) advance(L, 1);
        }
    }
    size_t len = (size_t)(L->p - start);
    char *tmp = (char *)malloc(len + 1);
    memcpy(tmp, start, len); tmp[len] = 0;
    t->loc = loc; t->s = start; t->len = len;
    if (is_float) { t->kind = TK_FLOAT; t->f = strtod(tmp, NULL); }
    else { t->kind = TK_INT; t->i = is_hex ? (int64_t)strtoll(tmp, NULL, 16) : (int64_t)strtoll(tmp, NULL, 10); }
    free(tmp);
    /* Optional time suffix `ms` or `s` directly attached to an int literal
     * (no whitespace). Result is normalised to milliseconds. Float literals
     * don't get a suffix — keep that path simple. */
    if (!is_float) {
        if (L->p[0] == 'm' && L->p[1] == 's' && !is_id_cont((unsigned char)L->p[2])) {
            advance(L, 2);
        } else if (L->p[0] == 's' && !is_id_cont((unsigned char)L->p[1])) {
            advance(L, 1);
            t->i *= 1000;
        }
    }
}

static void scan_string(Lexer *L, Tok *t) {
    Loc loc = { L->file, L->line, L->col };
    advance(L, 1);                              /* opening quote */
    const char *start = L->p;
    while (*L->p && *L->p != '"') {
        if (*L->p == '\\' && L->p[1]) advance(L, 2);
        else advance(L, 1);
    }
    size_t len = (size_t)(L->p - start);
    if (*L->p != '"') diag_error(loc, "unterminated string");
    else advance(L, 1);
    t->kind = TK_STRING;
    t->loc  = loc;
    t->s    = start; t->len = len;
}

static void scan_ident(Lexer *L, Tok *t) {
    Loc loc = { L->file, L->line, L->col };
    const char *start = L->p;
    while (is_id_cont((unsigned char)*L->p)) advance(L, 1);
    size_t len = (size_t)(L->p - start);
    t->loc = loc; t->s = start; t->len = len; t->kind = TK_IDENT;
    for (size_t i = 0; i < sizeof KWS / sizeof KWS[0]; i++) {
        if (strlen(KWS[i].kw) == len && memcmp(KWS[i].kw, start, len) == 0) {
            t->kind = KWS[i].k; return;
        }
    }
}

static void scan_one(Lexer *L, Tok *t) {
    skip_ws(L);
    Loc loc = { L->file, L->line, L->col };
    int c = (unsigned char)*L->p;
    if (!c) { t->kind = TK_EOF; t->loc = loc; return; }
    if (c == '#') {
        /* `#rrggbb[aa]` is a color; anything else after '#' is a style-rule id
         * selector (`#audio`). Disambiguate by scanning the whole ident run. */
        size_t n = 0; int hex = 1;
        while (is_id_cont((unsigned char)L->p[1 + n])) {
            if (hexval((unsigned char)L->p[1 + n]) < 0) hex = 0;
            n++;
        }
        if (hex && (n == 6 || n == 8)) { scan_color(L, t); return; }
        advance(L, 1); t->loc = loc; t->s = L->p; t->len = 1; t->kind = TK_HASH; return;
    }
    if (c == '"') { scan_string(L, t); return; }
    if (isdigit(c)) { scan_number(L, t); return; }
    if (is_id_start(c)) { scan_ident(L, t); return; }

    t->loc = loc; t->s = L->p; t->len = 1;
    switch (c) {
    case '{': advance(L,1); t->kind = TK_LBRACE; return;
    case '}': advance(L,1); t->kind = TK_RBRACE; return;
    case '(': advance(L,1); t->kind = TK_LPAREN; return;
    case ')': advance(L,1); t->kind = TK_RPAREN; return;
    case '[': advance(L,1); t->kind = TK_LBRACK; return;
    case ']': advance(L,1); t->kind = TK_RBRACK; return;
    case ';': advance(L,1); t->kind = TK_SEMI;   return;
    case ',': advance(L,1); t->kind = TK_COMMA;  return;
    case '.':
        if (L->p[1] == '.') { advance(L,2); t->len = 2; t->kind = TK_DOTDOT; }
        else                { advance(L,1); t->kind = TK_DOT; }
        return;
    case '$': advance(L,1); t->kind = TK_DOLLAR; return;
    case '?': advance(L,1); t->kind = TK_QMARK;  return;
    case ':': advance(L,1); t->kind = TK_COLON;  return;
    case '+': advance(L,1); t->kind = TK_PLUS;   return;
    case '-': advance(L,1); t->kind = TK_MINUS;  return;
    case '*': advance(L,1); t->kind = TK_STAR;   return;
    case '/': advance(L,1); t->kind = TK_SLASH;  return;
    case '%': advance(L,1); t->kind = TK_PERCENT;return;
    case '=':
        if (L->p[1] == '=') { advance(L,2); t->len=2; t->kind = TK_EQ; }
        else { advance(L,1); t->kind = TK_ASSIGN; }
        return;
    case '!':
        if (L->p[1] == '=') { advance(L,2); t->len=2; t->kind = TK_NEQ; }
        else { advance(L,1); t->kind = TK_NOT; }
        return;
    case '<':
        if (L->p[1] == '=') { advance(L,2); t->len=2; t->kind = TK_LE; }
        else { advance(L,1); t->kind = TK_LT; }
        return;
    case '>':
        if (L->p[1] == '=') { advance(L,2); t->len=2; t->kind = TK_GE; }
        else { advance(L,1); t->kind = TK_GT; }
        return;
    case '&':
        if (L->p[1] == '&') { advance(L,2); t->len=2; t->kind = TK_AND; }
        else { advance(L,1); t->kind = TK_AMP; }
        return;
    case '|':
        if (L->p[1] == '|') { advance(L,2); t->len=2; t->kind = TK_OR; }
        else { advance(L,1); t->kind = TK_PIPE; }
        return;
    }
    diag_error(loc, "unexpected character '%c' (0x%02x)", c, c);
    advance(L, 1);
    t->kind = TK_EOF;
}

void lex_init(Lexer *L, const char *file, const char *src) {
    L->src = src; L->p = src; L->file = file;
    L->line = 1; L->col = 1; L->have_peek = 0;
    scan_one(L, &L->cur);
}

void lex_next(Lexer *L) {
    if (L->have_peek) { L->cur = L->peek; L->have_peek = 0; return; }
    scan_one(L, &L->cur);
}

Tok lex_peek(Lexer *L) {
    if (!L->have_peek) { scan_one(L, &L->peek); L->have_peek = 1; }
    return L->peek;
}

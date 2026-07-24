#include "wispc.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int err_count = 0;

/* file -> source buffer, so a diagnostic can print the offending line. The
 * lexer owns the buffers; we only borrow pointers (valid for the whole run). */
static struct { const char *file; const char *buf; } sources[16];
static int nsources = 0;

void diag_add_source(const char *file, const char *buf) {
    if (nsources < (int)(sizeof sources / sizeof sources[0])) {
        sources[nsources].file = file;
        sources[nsources].buf = buf;
        nsources++;
    }
}

static const char *source_for(const char *file) {
    if (!file) return NULL;
    for (int i = 0; i < nsources; i++)
        if (sources[i].file == file || strcmp(sources[i].file, file) == 0)
            return sources[i].buf;
    return NULL;
}

/* ANSI only on a tty; the vscode extension parses the plain stream. */
static int use_color(void) {
    static int c = -1;
    if (c < 0) c = isatty(STDERR_FILENO);
    return c;
}
#define C_RESET "\033[0m"
#define C_BOLD  "\033[1m"
#define C_RED   "\033[1;31m"
#define C_CYAN  "\033[36m"
#define C_GREEN "\033[32m"

/* Print the offending source line + a caret at col. Tabs in the source are
 * echoed verbatim so the caret (built from the same tab/space run) lines up
 * once the terminal expands them. Long lines are clipped around the caret. */
static void print_snippet(Loc l, const char *sev_color) {
    const char *buf = source_for(l.file);
    if (!buf || l.line < 1 || l.col < 1) return;

    const char *p = buf;
    for (int ln = 1; ln < l.line && *p; p++)
        if (*p == '\n') ln++;
    const char *end = p;
    while (*end && *end != '\n') end++;

    int len = (int)(end - p);
    int col = l.col - 1;              /* 0-based caret position */
    int start = 0;
    const int MAXW = 100;
    if (len > MAXW) {                 /* window around the caret */
        if (col > MAXW - 20) start = col - (MAXW - 20);
        if (start + MAXW > len) start = len - MAXW;
        if (start < 0) start = 0;
    }
    int show = len - start;
    if (show > MAXW) show = MAXW;

    fputs("  ", stderr);
    if (start > 0) fputs("...", stderr);
    fwrite(p + start, 1, (size_t)show, stderr);
    fputc('\n', stderr);

    fputs("  ", stderr);
    if (start > 0) fputs("   ", stderr);
    if (use_color()) fputs(sev_color, stderr);
    for (int i = start; i < col && i < len; i++)
        fputc(p[i] == '\t' ? '\t' : ' ', stderr);
    fputc('^', stderr);
    if (use_color()) fputs(C_RESET, stderr);
    fputc('\n', stderr);
}

static void head(const char *kind, const char *kind_color, Loc l) {
    const char *file = l.file ? l.file : "<?>";
    if (use_color())
        fprintf(stderr, C_BOLD "%s:%d:%d:" C_RESET " %s%s:" C_RESET " ",
                file, l.line, l.col, kind_color, kind);
    else
        fprintf(stderr, "%s:%d:%d: %s: ", file, l.line, l.col, kind);
}

void diag_error(Loc l, const char *fmt, ...) {
    err_count++;
    /* Hard stop: a parser that fails to consume a token would otherwise spin
     * forever spewing the same error. 50 is well past useful output. */
    if (err_count > 50) {
        fprintf(stderr, "too many errors, giving up\n");
        exit(1);
    }
    head("error", C_RED, l);
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    print_snippet(l, C_RED);
}

void diag_note(Loc l, const char *fmt, ...) {
    head("note", C_CYAN, l);
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    print_snippet(l, C_CYAN);
}

/* Teaching-style suggestion: an indented `help:` line, no snippet. */
void diag_hint(Loc l, const char *fmt, ...) {
    (void)l;
    int c = use_color();
    fputs(c ? "  " C_GREEN "help: " : "  help: ", stderr);
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (c) fputs(C_RESET, stderr);
    fputc('\n', stderr);
}

int diag_count(void) { return err_count; }

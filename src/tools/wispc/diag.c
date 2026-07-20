#include "wispc.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static int err_count = 0;

static void vprn(const char *kind, Loc l, const char *fmt, va_list ap) {
    fprintf(stderr, "%s:%d:%d: %s: ", l.file ? l.file : "<?>", l.line, l.col, kind);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void diag_error(Loc l, const char *fmt, ...) {
    err_count++;
    /* Hard stop: a parser that fails to consume a token would otherwise spin
     * forever spewing the same error. 50 is well past useful output. */
    if (err_count > 50) {
        fprintf(stderr, "too many errors, giving up\n");
        exit(1);
    }
    va_list ap; va_start(ap, fmt);
    vprn("error", l, fmt, ap);
    va_end(ap);
}

void diag_note(Loc l, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vprn("note", l, fmt, ap);
    va_end(ap);
}

int diag_count(void) { return err_count; }

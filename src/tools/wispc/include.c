/* `include "path.wisp";` — resolve, read, and splice at most once.
 * The parser splices the included file's declarations into the same Unit, so
 * everything downstream (sema, style, codegen) is unaware includes exist; only
 * the per-token Loc.file changes, which is what keeps diagnostics honest. */
#include "wispc.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define INC_MAX_DEPTH 8
#define INC_MAX_FILES 64

/* realpath of every file already spliced into this unit, root included.
 * include is include-once (C's `#pragma once`): a second include of the same
 * file is a silent skip, which makes the diamond work and cycles benign.
 * Per-compilation only — nothing is cached across builds, so a symlinked
 * theme.wisp still re-resolves every compile. */
static char *seen[INC_MAX_FILES];
static int nseen = 0;
static int depth = 0;

/* Every file we read, in include order. Kept because --font-sizes scans source
 * text lexically for icon codepoints, and the Makefile needs the resolved
 * target list to know when a build is stale. Never freed: tokens point into
 * these buffers for the whole run. */
static struct { const char *file, *src; } opened[INC_MAX_FILES];
static int nopened = 0;

const char *include_src (int i) { return i < nopened ? opened[i].src  : NULL; }
const char *include_file(int i) { return i < nopened ? opened[i].file : NULL; }

/* Join `path` against the directory of `from` (absolute paths pass through). */
static void join_dir(char *out, size_t cap, const char *from, const char *path) {
    if (path[0] == '/') { snprintf(out, cap, "%s", path); return; }
    const char *slash = strrchr(from, '/');
    if (!slash) { snprintf(out, cap, "%s", path); return; }
    snprintf(out, cap, "%.*s/%s", (int)(slash - from), from, path);
}

static char *read_all(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    buf[n] = 0;
    fclose(f);
    return buf;
}

/* Returns the resolved filename (owned here, valid for the run) and stores the
 * file's text in *out_src. NULL means "don't parse": either an already-reported
 * error or an already-included file. The caller pairs a non-NULL return with
 * include_close(). Resolution happens fresh on every compile — a symlinked
 * theme.wisp must follow its current target. */
char *include_open(Loc site, const char *from, const char *path, char **out_src) {
    if (depth > INC_MAX_DEPTH) {
        diag_error(site, "include nesting too deep (max %d)", INC_MAX_DEPTH);
        return NULL;
    }
    char joined[PATH_MAX];
    join_dir(joined, sizeof joined, from, path);

    char resolved[PATH_MAX];
    if (!realpath(joined, resolved)) {
        diag_error(site, "cannot include '%s': %s", joined, strerror(errno));
        return NULL;
    }
    for (int i = 0; i < nseen; i++)
        if (strcmp(seen[i], resolved) == 0) return NULL;   /* include-once */
    if (nseen == INC_MAX_FILES) {
        diag_error(site, "too many included files (max %d)", INC_MAX_FILES);
        return NULL;
    }

    char *src = read_all(joined);
    if (!src) {
        diag_error(site, "cannot read '%s': %s", joined, strerror(errno));
        return NULL;
    }
    seen[nseen++] = strdup(resolved);
    char *shown = strdup(joined);
    depth++;
    diag_add_source(shown, src);
    opened[nopened].file = seen[nseen - 1];
    opened[nopened].src  = src;
    nopened++;
    *out_src = src;
    return shown;
}

void include_close(void) {
    if (depth > 0) depth--;
}

/* The root file seeds the seen set, so an include back to it is skipped too. */
void include_root(const char *file) {
    char resolved[PATH_MAX];
    if (!realpath(file, resolved)) return;
    seen[nseen++] = strdup(resolved);
    depth = 1;
}

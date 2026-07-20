#include "wispc.h"
#include <stdlib.h>
#include <string.h>

typedef struct Chunk { struct Chunk *next; size_t used, cap; char buf[]; } Chunk;
struct Arena { Chunk *head; };

#define CHUNK_MIN (64 * 1024)

Arena *arena_new(void) {
    Arena *a = calloc(1, sizeof *a);
    return a;
}

static Chunk *new_chunk(size_t need) {
    size_t cap = need + sizeof(Chunk);
    if (cap < CHUNK_MIN) cap = CHUNK_MIN;
    Chunk *c = malloc(cap);
    c->next = NULL; c->used = 0; c->cap = cap - sizeof(Chunk);
    return c;
}

void *arena_alloc(Arena *a, size_t n) {
    n = (n + 7) & ~(size_t)7;
    if (!a->head || a->head->used + n > a->head->cap) {
        Chunk *c = new_chunk(n);
        c->next = a->head;
        a->head = c;
    }
    void *p = a->head->buf + a->head->used;
    a->head->used += n;
    memset(p, 0, n);
    return p;
}

char *arena_strn(Arena *a, const char *s, size_t n) {
    char *p = arena_alloc(a, n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *arena_str(Arena *a, const char *s) { return arena_strn(a, s, strlen(s)); }

void arena_free(Arena *a) {
    if (!a) return;
    Chunk *c = a->head;
    while (c) { Chunk *n = c->next; free(c); c = n; }
    free(a);
}

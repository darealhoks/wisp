/* D-Bus wire primitives shared by both bus connections: marshalling (W/R),
 * signature-driven skip, SASL EXTERNAL auth, address parsing, and message
 * building. No fd or bus state here — dbus.c (session) and power.c (system)
 * each own their socket and dispatch; this TU is what lets power.c exist
 * without dragging in the session transport + notification server. */

#define _GNU_SOURCE
#include "wisp.h"
#include "dbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int dbus_parse_bus_addr(const char *spec, char *out, size_t cap) {
    /* Accept "unix:path=...", "unix:abstract=...", semicolon-separated lists.
       Pick the first unix:path/abstract entry. */
    const char *p = spec;
    while (*p) {
        const char *end = strchr(p, ';'); if (!end) end = p + strlen(p);
        if (!strncmp(p, "unix:", 5)) {
            const char *kv = p + 5;
            const char *eq;
            if ((eq = strstr(kv, "path=")) && eq < end) {
                eq += 5;
                const char *valend = eq;
                while (valend < end && *valend != ',') valend++;
                size_t n = valend - eq;
                if (n >= cap) return -1;
                memcpy(out, eq, n); out[n] = 0; return 0;
            }
            if ((eq = strstr(kv, "abstract=")) && eq < end) {
                eq += 9;
                const char *valend = eq;
                while (valend < end && *valend != ',') valend++;
                size_t n = valend - eq;
                if (n + 1 >= cap) return -1;
                out[0] = '\0';  /* leading nul → abstract namespace */
                memcpy(out + 1, eq, n); out[n + 1] = 0;
                return (int)(n + 1);
            }
        }
        if (!*end) break;
        p = end + 1;
    }
    return -1;
}

int dbus_sasl_auth(int fd) {
    char buf[256]; int n;
    /* Required first byte for AF_UNIX D-Bus. */
    if (send(fd, "\0", 1, MSG_NOSIGNAL) != 1) return -1;

    /* AUTH EXTERNAL <hex uid> */
    uint32_t uid = (uint32_t)geteuid();
    char ascii[16]; snprintf(ascii, sizeof ascii, "%u", uid);
    char hex[64]; int hp = 0;
    for (int i = 0; ascii[i]; i++) {
        static const char H[] = "0123456789abcdef";
        hex[hp++] = H[(ascii[i] >> 4) & 0xf];
        hex[hp++] = H[ascii[i] & 0xf];
    }
    hex[hp] = 0;
    n = snprintf(buf, sizeof buf, "AUTH EXTERNAL %s\r\n", hex);
    if (send(fd, buf, n, MSG_NOSIGNAL) != n) return -1;

    /* Read "OK <guid>\r\n" (or REJECTED). */
    int got = 0;
    for (;;) {
        ssize_t k = recv(fd, buf + got, sizeof buf - 1 - got, 0);
        if (k <= 0) return -1;
        got += k; buf[got] = 0;
        if (memmem(buf, got, "\r\n", 2)) break;
        if (got >= (int)sizeof buf - 1) return -1;
    }
    if (strncmp(buf, "OK ", 3) != 0) return -1;

    n = snprintf(buf, sizeof buf, "BEGIN\r\n");
    if (send(fd, buf, n, MSG_NOSIGNAL) != n) return -1;
    return 0;
}

/* ================================================================== */
/* Marshalling helpers                                                  */
/* ================================================================== */

void wensure(W *w, int n) {
    if (w->pos + n <= w->cap) return;
    int nc = w->cap; while (nc < w->pos + n) nc = nc ? nc * 2 : 256;
    w->b = realloc(w->b, nc);
    w->cap = nc;
}
void walign(W *w, int a) {
    int p = (w->pos + a - 1) & ~(a - 1);
    wensure(w, p - w->pos);
    while (w->pos < p) w->b[w->pos++] = 0;
}
void wbyte(W *w, uint8_t v)  { wensure(w, 1); w->b[w->pos++] = v; }
void wu32 (W *w, uint32_t v) { walign(w, 4); wensure(w, 4); memcpy(w->b + w->pos, &v, 4); w->pos += 4; }
void wstr (W *w, const char *s) {
    if (!s) s = "";
    uint32_t l = (uint32_t)strlen(s);
    wu32(w, l);
    wensure(w, l + 1);
    memcpy(w->b + w->pos, s, l + 1);
    w->pos += l + 1;
}
void wsig (W *w, const char *s) {
    uint8_t l = (uint8_t)strlen(s);
    wbyte(w, l);
    wensure(w, l + 1);
    memcpy(w->b + w->pos, s, l + 1);
    w->pos += l + 1;
}

void ralign(R *r, int a) {
    int p = (r->pos + a - 1) & ~(a - 1);
    if (p > r->len) { r->ok = 0; return; }
    r->pos = p;
}
uint8_t  rbyte(R *r)  { if (r->pos + 1 > r->len) { r->ok = 0; return 0; } return r->b[r->pos++]; }
uint16_t ru16 (R *r)  { ralign(r, 2); if (!r->ok || r->pos + 2 > r->len) { r->ok = 0; return 0; }
                         uint16_t v; memcpy(&v, r->b + r->pos, 2); r->pos += 2; return v; }
uint32_t ru32 (R *r)  { ralign(r, 4); if (!r->ok || r->pos + 4 > r->len) { r->ok = 0; return 0; }
                         uint32_t v; memcpy(&v, r->b + r->pos, 4); r->pos += 4; return v; }
int32_t  ri32 (R *r)  { return (int32_t)ru32(r); }
const char *rstr(R *r) {
    uint32_t l = ru32(r);
    /* 64-bit compare: (int)l would go negative for l > INT_MAX and bypass the
     * bound, letting a non-terminated pointer escape to snprintf("%s") below. */
    if (!r->ok || (int64_t)r->pos + (int64_t)l + 1 > (int64_t)r->len) { r->ok = 0; return ""; }
    const char *s = (const char *)(r->b + r->pos);
    /* D-Bus mandates the NUL; a hostile sender can omit it, leaving "%s"/strcmp
     * consumers scanning past the message into the heap. */
    if (s[l] != 0) { r->ok = 0; return ""; }
    r->pos += l + 1;
    return s;
}
const char *rsig(R *r) {
    uint8_t l = rbyte(r);
    if (!r->ok || r->pos + l + 1 > r->len) { r->ok = 0; return ""; }
    const char *s = (const char *)(r->b + r->pos);
    if (s[l] != 0) { r->ok = 0; return ""; }
    r->pos += l + 1;
    return s;
}

/* Skip a value of the given single-type signature character. Walks containers
 * by recursing on the nested signature. Used to consume header fields and
 * variant payloads we don't care about. Returns 0 on success. */
static int skip_single(R *r, char c, const char **sigp, int depth) {
    /* Variants embed a fresh signature, so nesting isn't bounded by the 255-byte
     * outer signature; cap depth like the reference impl to stop stack blowup. */
    if (depth > 64) { r->ok = 0; return -1; }
    switch (c) {
    case 'y': rbyte(r); return r->ok ? 0 : -1;
    case 'b': case 'u': case 'i': ru32(r); return r->ok ? 0 : -1;
    case 'n': case 'q': ru16(r); return r->ok ? 0 : -1;
    case 'x': case 't': case 'd': ralign(r, 8); r->pos += 8; return r->pos <= r->len ? 0 : -1;
    case 'h': ru32(r); return r->ok ? 0 : -1;
    case 's': case 'o': rstr(r); return r->ok ? 0 : -1;
    case 'g': rsig(r); return r->ok ? 0 : -1;
    case 'a': {
        uint32_t len = ru32(r);
        if (!r->ok) return -1;
        char etype = **sigp;
        if (!etype) { r->ok = 0; return -1; }  /* malformed sig "a" with no element type */
        (*sigp)++;
        if (etype == '{' || etype == '(') {
            /* skip nested compound element signature in sig string */
            int depth = 1;
            while (depth && **sigp) {
                if (**sigp == '{' || **sigp == '(') depth++;
                else if (**sigp == '}' || **sigp == ')') depth--;
                (*sigp)++;
            }
        }
        ralign(r, etype == '(' || etype == '{' || etype == 'x' || etype == 't' || etype == 'd' ? 8 : 4);
        int64_t end = (int64_t)r->pos + (int64_t)len;
        if (end > r->len) { r->ok = 0; return -1; }
        r->pos = (int)end;
        return 0;
    }
    case '(': case '{': {
        ralign(r, 8);
        while (**sigp && **sigp != ')' && **sigp != '}') {
            char nc = **sigp; (*sigp)++;
            if (skip_single(r, nc, sigp, depth + 1) < 0) return -1;
        }
        if (**sigp) (*sigp)++;
        return 0;
    }
    case 'v': {
        const char *vs = rsig(r);
        if (!r->ok) return -1;
        return skip_val(r, &vs, depth + 1);
    }
    default: r->ok = 0; return -1;
    }
}
int skip_val(R *r, const char **sigp, int depth) {
    while (**sigp) {
        char c = **sigp; (*sigp)++;
        if (skip_single(r, c, sigp, depth) < 0) return -1;
    }
    return 0;
}

/* ================================================================== */
/* Message header                                                       */
/* ================================================================== */

static void w_header_field_str(W *w, uint8_t code, char sigc, const char *val) {
    walign(w, 8);            /* struct alignment */
    wbyte(w, code);
    char sig[2] = { sigc, 0 };
    wsig(w, sig);
    if (sigc == 'g') {
        wsig(w, val);
    } else {
        wstr(w, val);
    }
}
static void w_header_field_u32(W *w, uint8_t code, uint32_t val) {
    walign(w, 8);
    wbyte(w, code);
    wsig(w, "u");
    wu32(w, val);
}

/* Marshal a complete message (header + body) into `h`. The caller owns the
 * serial counter and the socket write. */
void dbus_msg_build(W *h, const Msg *m, uint32_t serial) {
    wbyte(h, 'l');                    /* little-endian */
    wbyte(h, m->type);
    wbyte(h, m->flags);
    wbyte(h, 1);                      /* protocol version */
    wu32(h, (uint32_t)m->body_len);
    wu32(h, serial);
    /* fields array — length placeholder, then entries */
    int arr_len_pos = h->pos;
    wu32(h, 0);                       /* placeholder */
    int arr_start = h->pos;
    if (m->path)         w_header_field_str(h, HF_PATH, 'o', m->path);
    if (m->interface)    w_header_field_str(h, HF_INTERFACE, 's', m->interface);
    if (m->member)       w_header_field_str(h, HF_MEMBER, 's', m->member);
    if (m->error_name)   w_header_field_str(h, HF_ERROR_NAME, 's', m->error_name);
    if (m->reply_serial) w_header_field_u32(h, HF_REPLY_SERIAL, m->reply_serial);
    if (m->destination)  w_header_field_str(h, HF_DESTINATION, 's', m->destination);
    if (m->signature && m->signature[0])
        w_header_field_str(h, HF_SIGNATURE, 'g', m->signature);
    uint32_t alen = (uint32_t)(h->pos - arr_start);
    memcpy(h->b + arr_len_pos, &alen, 4);
    walign(h, 8);                     /* pad header to body alignment */

    if (m->body_len) {
        wensure(h, m->body_len);
        memcpy(h->b + h->pos, m->body, m->body_len);
        h->pos += m->body_len;
    }
}

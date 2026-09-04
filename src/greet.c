/* greetd greeter client — the login surface, where gtkgreet used to run.
 *
 * wisp owns none of the privileged half: greetd runs PAM as root, handles the
 * VT and execs the session. This file speaks greetd's *client* protocol on
 * $GREETD_SOCK — native-endian uint32 length prefix + one flat JSON object,
 * both directions — and draws the declared `spawned_by = greet` surface.
 *
 * Unlike polkit's per-auth prompt, the surface IS the session: created once at
 * startup, torn down only by exit(0) after start_session succeeds (greetd
 * takes the VT from there). Zero timers; the socket is the only event source.
 *
 * The JSON reader is a bounded scanner for these flat objects only — no
 * general parser, and every value is truncated into a fixed field. */

#define _GNU_SOURCE
#include "wisp.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* All GREET_* come from gen_overrides.h, which the Makefile force-includes;
 * the fallbacks only keep this file standalone-compilable. Names must match
 * codegen.c's greet override block. */
#ifndef GREET_W
#define GREET_W 420
#endif
#ifndef GREET_H
#define GREET_H 180
#endif
#ifndef GREET_LAYER
#define GREET_LAYER 3
#endif
#ifndef GREET_KBD
#define GREET_KBD (1)
#endif
#ifndef GREET_ANCHOR
#define GREET_ANCHOR 0
#endif
#ifndef GREET_SHADOW_PAD_L
#define GREET_SHADOW_PAD_L 0
#endif
#ifndef GREET_SHADOW_PAD_R
#define GREET_SHADOW_PAD_R 0
#endif
#ifndef GREET_SHADOW_PAD_T
#define GREET_SHADOW_PAD_T 0
#endif
#ifndef GREET_SHADOW_PAD_B
#define GREET_SHADOW_PAD_B 0
#endif
#ifndef GREET_USER
#define GREET_USER ""
#endif
#ifndef GREET_SESSIONS
#define GREET_SESSIONS "/etc/greetd/environments"
#endif

/* Linux input keycodes (evdev), same subset polkit.c uses. */
#define KEY_ESC    1
#define KEY_TAB   15
#define KEY_BS    14
#define KEY_ENTER 28
#define KEY_LSHIFT 42
#define KEY_RSHIFT 54
#define KEY_LEFT  105
#define KEY_RIGHT 106

#define GREET_SESSIONS_CAP 8
#define SESSION_MAX 256

extern void render_greet(Widget *w);

int greet_fd = -1;

static struct {
    char sess[GREET_SESSIONS_CAP][SESSION_MAX];
    int  nsess, sel;

    int  awaiting;              /* a prompt is on screen, waiting for Enter */
    int  echo;                  /* visible prompt: show the text, not dots */
    char pw[512];               /* never reachable from the DSL */
    int  pw_len;

    uint8_t rx[4096];
    int     rx_len;
} G;

/* ============================================================ */
/* surface                                                       */
/* ============================================================ */

static Widget *greet_widget(void) {
    for (int i = 0; i < MAX_WIDGETS; i++)
        if (widgets[i].kind == W_GREET) return &widgets[i];
    return NULL;
}

void greet_render(Widget *w) { render_greet(w); }

static void ui_repaint(void) {
    Widget *w = greet_widget();
    if (w && w->configured) render_greet(w);
}

void greet_on_caps_changed(void) {
    Widget *w = greet_widget();
    if (!w || w->s.greet.caps == xkb_caps_on) return;   /* repaint on change only */
    w->s.greet.caps = xkb_caps_on;
    ui_repaint();
}

/* every caller feeds a bigger buffer than the field, so gcc 16 makes
 * snprintf's truncation a -Wformat-truncation error */
static void set_field(char *dst, size_t cap, const char *src) {
    size_t n = strnlen(src, cap - 1);
    while (n > 0 && (src[n] & 0xc0) == 0x80) n--;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void sync_session(void) {
    Widget *w = greet_widget();
    if (!w) return;
    set_field(w->s.greet.session, sizeof w->s.greet.session,
              G.nsess ? G.sess[G.sel] : "");
}

/* Visible (echo-on) prompt: the answer is not a secret, so it is shown
 * verbatim, truncated back to a codepoint boundary. */
static void echo_visible(Widget *w) {
    int n = G.pw_len;
    if (n > (int)sizeof w->s.greet.input - 1) n = (int)sizeof w->s.greet.input - 1;
    while (n > 0 && (G.pw[n] & 0xc0) == 0x80) n--;
    memcpy(w->s.greet.input, G.pw, (size_t)n);
    w->s.greet.input[n] = 0;
}

static void clear_entry(void) {
    Widget *w = greet_widget();
    explicit_bzero(G.pw, sizeof G.pw);
    G.pw_len = 0;
    if (w) { w->s.greet.dots[0] = 0; w->s.greet.input[0] = 0; }
}

/* ============================================================ */
/* JSON: bounded scanner over one flat object                    */
/* ============================================================ */

/* Copies the string value of "key" into out. Walks the object key by key
 * rather than searching for the literal, so a matching substring inside
 * another value cannot be mistaken for a key. Returns 1 on a string hit. */
static int json_skip_str(const char *s, int len, int *i, char *out, size_t cap) {
    if (*i >= len || s[*i] != '"') return 0;
    (*i)++;
    size_t o = 0;
    while (*i < len && s[*i] != '"') {
        char c = s[(*i)++];
        if (c != '\\') { if (out && o + 1 < cap) out[o++] = c; continue; }
        if (*i >= len) return 0;
        char e = s[(*i)++];
        char d = 0;
        switch (e) {
        case 'n': d = '\n'; break;
        case 't': d = '\t'; break;
        case 'r': d = '\r'; break;
        case 'b': d = '\b'; break;
        case 'f': d = '\f'; break;
        case 'u': {
            /* \uXXXX is replaced, not decoded: greetd's prompts are ASCII in
             * practice and a surrogate pair decoder buys nothing here. */
            if (*i + 4 > len) return 0;
            *i += 4;
            d = '?';
            break;
        }
        default: d = e; break;      /* covers \" \\ \/ */
        }
        if (out && o + 1 < cap) out[o++] = d;
    }
    if (*i >= len) return 0;
    (*i)++;                          /* closing quote */
    if (out) out[o] = 0;
    return 1;
}

static void json_skip_val(const char *s, int len, int *i) {
    if (*i < len && s[*i] == '"') { json_skip_str(s, len, i, NULL, 0); return; }
    int depth = 0;
    while (*i < len) {
        char c = s[*i];
        if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') {
            if (depth == 0) return;
            depth--;
        } else if ((c == ',' ) && depth == 0) return;
        else if (c == '"') { json_skip_str(s, len, i, NULL, 0); continue; }
        (*i)++;
    }
}

static int json_get(const char *s, int len, const char *key, char *out, size_t cap) {
    int i = 0;
    while (i < len && s[i] != '{') i++;
    if (i >= len) return 0;
    i++;
    char k[64];
    while (i < len) {
        while (i < len && (s[i] == ' ' || s[i] == ',' || s[i] == '\n' || s[i] == '\t')) i++;
        if (i >= len || s[i] == '}') return 0;
        if (!json_skip_str(s, len, &i, k, sizeof k)) return 0;
        while (i < len && (s[i] == ' ' || s[i] == ':')) i++;
        if (strcmp(k, key) == 0) {
            if (i < len && s[i] == '"') return json_skip_str(s, len, &i, out, cap);
            return 0;                /* null / number / object: not a string */
        }
        json_skip_val(s, len, &i);
    }
    return 0;
}

/* JSON string escape for what wisp sends (a password may hold " or \). */
static int jesc(char *out, size_t cap, const char *in) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        const char *rep = NULL;
        char buf[8];
        if (*p == '"')       rep = "\\\"";
        else if (*p == '\\') rep = "\\\\";
        else if (*p < 0x20) { snprintf(buf, sizeof buf, "\\u%04x", *p); rep = buf; }
        size_t n = rep ? strlen(rep) : 1;
        if (o + n + 1 > cap) return -1;
        if (rep) memcpy(out + o, rep, n);
        else     out[o] = (char)*p;
        o += n;
    }
    out[o] = 0;
    return (int)o;
}

/* ============================================================ */
/* wire                                                          */
/* ============================================================ */

static void greet_down(const char *why) {
    if (greet_fd >= 0) { epoll_del_fd(greet_fd); close(greet_fd); greet_fd = -1; }
    die("greet: %s", why);
}

static void send_json(const char *json) {
    if (greet_fd < 0) return;
    uint32_t n = (uint32_t)strlen(json);
    uint8_t hdr[4];
    memcpy(hdr, &n, 4);
    struct iovec v[2] = { { hdr, 4 }, { (void *)json, n } };
    struct msghdr m = { .msg_iov = v, .msg_iovlen = 2 };
    if (sendmsg(greet_fd, &m, MSG_NOSIGNAL) != (ssize_t)(n + 4))
        greet_down("cannot write to greetd");
}

static void create_session(void) {
    Widget *w = greet_widget();
    char esc[128], buf[256];
    if (jesc(esc, sizeof esc, w ? w->s.greet.user : GREET_USER) < 0) return;
    snprintf(buf, sizeof buf, "{\"type\":\"create_session\",\"username\":\"%s\"}", esc);
    if (w) w->s.greet.busy = 1;
    send_json(buf);
}

static void post_response(const char *text) {
    char buf[1200];
    if (!text) {
        snprintf(buf, sizeof buf, "{\"type\":\"post_auth_message_response\",\"response\":null}");
    } else {
        char esc[1024];
        if (jesc(esc, sizeof esc, text) < 0) return;
        snprintf(buf, sizeof buf,
                 "{\"type\":\"post_auth_message_response\",\"response\":\"%s\"}", esc);
    }
    Widget *w = greet_widget();
    if (w) w->s.greet.busy = 1;
    send_json(buf);
    explicit_bzero(buf, sizeof buf);
}

static void cancel_session(void) {
    send_json("{\"type\":\"cancel_session\"}");
}

static void start_session(void) {
    char esc[SESSION_MAX * 2], buf[SESSION_MAX * 2 + 64];
    if (jesc(esc, sizeof esc, G.nsess ? G.sess[G.sel] : "") < 0) return;
    snprintf(buf, sizeof buf,
             "{\"type\":\"start_session\",\"cmd\":[\"%s\"],\"env\":[]}", esc);
    send_json(buf);
}

/* ============================================================ */
/* protocol state machine                                        */
/* ============================================================ */

static int starting;     /* start_session sent: the next success ends us */
static int cancelling;   /* cancel_session sent: the next success reopens one */

static void on_message(const char *s, int len) {
    Widget *w = greet_widget();
    char type[32] = "";
    if (!json_get(s, len, "type", type, sizeof type)) return;
    if (w) w->s.greet.busy = 0;

    if (strcmp(type, "success") == 0) {
        if (cancelling) { cancelling = 0; create_session(); return; }
        if (starting) exit(0);   /* greetd owns the VT from here */
        starting = 1;
        start_session();
        return;
    }
    if (strcmp(type, "error") == 0) {
        char kind[32] = "", desc[256] = "";
        json_get(s, len, "error_type", kind, sizeof kind);
        json_get(s, len, "description", desc, sizeof desc);
        if (w) {
            set_field(w->s.greet.error, sizeof w->s.greet.error, desc);
            w->s.greet.failed = 1;
            w->s.greet.prompt[0] = 0;
        }
        clear_entry();
        G.awaiting = 0;
        starting = 0;
        /* greetd keeps the failed session configured; create_session would
         * answer "already configuring a session" until it is cancelled */
        if (cancelling) { cancelling = 0; create_session(); }
        else { cancelling = 1; cancel_session(); }
        ui_repaint();
        return;
    }
    if (strcmp(type, "auth_message") != 0) return;

    char amt[16] = "", text[256] = "";
    json_get(s, len, "auth_message_type", amt, sizeof amt);
    json_get(s, len, "auth_message", text, sizeof text);
    if (strcmp(amt, "info") == 0 || strcmp(amt, "error") == 0) {
        if (w) set_field(w->s.greet.error, sizeof w->s.greet.error, text);
        post_response(NULL);         /* info/error are acknowledged, not answered */
        ui_repaint();
        return;
    }
    G.echo = strcmp(amt, "visible") == 0;
    G.awaiting = 1;
    clear_entry();
    if (w) set_field(w->s.greet.prompt, sizeof w->s.greet.prompt, text);
    ui_repaint();
}

/* ============================================================ */
/* keys                                                          */
/* ============================================================ */

static void cycle_session(int d) {
    if (G.nsess < 2) return;
    G.sel = (G.sel + d + G.nsess) % G.nsess;
    sync_session();
    ui_repaint();
}

/* Clicked session row — the generated hit table hands over the same index the
 * `for` drew, so no geometry is recomputed here. */
void greet_select(int i) {
    if (i < 0 || i >= G.nsess || i == G.sel) return;
    G.sel = i;
    sync_session();
    ui_repaint();
}

static void submit(void) {
    if (!G.awaiting) return;
    G.awaiting = 0;
    post_response(G.pw);
    clear_entry();
    Widget *w = greet_widget();
    if (w) { w->s.greet.prompt[0] = 0; w->s.greet.error[0] = 0; }
    ui_repaint();
}

/* Only text edits repeat: a held Enter would resubmit, a held Right/Tab would
 * spin the session list. */
int greet_key_repeats(uint32_t key) {
    if (key == KEY_BS) return 1;
    uint32_t cp = xkb_xlat(key, xkb_shift_on);
    return cp >= 0x20 && cp != 0x7f;
}

int greet_on_key(Widget *w, uint32_t key, uint32_t state) {
    if (w->kind != W_GREET) return 0;
    if (state != 1) return 1;
    if (key == KEY_LSHIFT || key == KEY_RSHIFT) return 1;
    if (key == KEY_LEFT)  { cycle_session(-1); return 1; }
    if (key == KEY_RIGHT || key == KEY_TAB) { cycle_session(1); return 1; }
    if (key == KEY_ESC)  { clear_entry(); ui_repaint(); return 1; }
    if (key == KEY_ENTER) { submit(); return 1; }
    if (!G.awaiting) return 1;
    if (key == KEY_BS) {
        if (G.pw_len > 0) {
            int nl = utf8_back(G.pw, G.pw_len);
            explicit_bzero(G.pw + nl, (size_t)(G.pw_len - nl));
            G.pw_len = nl;
            if (G.echo) echo_visible(w);
            else {
                int dl = (int)strlen(w->s.greet.dots);
                if (dl > 0) w->s.greet.dots[dl - 1] = 0;
            }
            ui_repaint();
        }
        return 1;
    }
    uint32_t cp = xkb_xlat(key, xkb_shift_on);
    if (!cp || cp < 0x20 || cp == 0x7f) return 1;
    char enc[4];
    int n = utf8_encode(cp, enc);
    if (n <= 0) return 1;
    if (G.pw_len + n >= (int)sizeof G.pw - 1) return 1;
    memcpy(G.pw + G.pw_len, enc, (size_t)n);
    G.pw_len += n;
    G.pw[G.pw_len] = 0;
    if (G.echo) {
        echo_visible(w);
    } else {
        /* One '*' per codepoint — the raw secret is never reachable from the
         * DSL. Past the field's width the mask stops growing. */
        int dl = (int)strlen(w->s.greet.dots);
        if (dl + 1 < (int)sizeof w->s.greet.dots) {
            w->s.greet.dots[dl] = '*';
            w->s.greet.dots[dl + 1] = 0;
        }
    }
    ui_repaint();
    return 1;
}

/* ============================================================ */
/* dispatch                                                      */
/* ============================================================ */

int greet_owns_fd(int fd) { return fd >= 0 && fd == greet_fd; }

void greet_dispatch(int fd) {
    if (fd != greet_fd) return;
    for (;;) {
        ssize_t n = recv(greet_fd, G.rx + G.rx_len,
                         sizeof G.rx - (size_t)G.rx_len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            greet_down("socket error");
        }
        if (n == 0) greet_down("greetd closed the socket");
        G.rx_len += (int)n;
        for (;;) {
            if (G.rx_len < 4) break;
            uint32_t blen;
            memcpy(&blen, G.rx, 4);
            /* A frame that cannot fit is a peer we don't understand; there is
             * no resync point in a length-prefixed stream. */
            if (blen > sizeof G.rx - 4) greet_down("oversized frame from greetd");
            if ((uint32_t)G.rx_len < blen + 4) break;
            on_message((const char *)G.rx + 4, (int)blen);
            G.rx_len -= (int)blen + 4;
            memmove(G.rx, G.rx + 4 + blen, (size_t)G.rx_len);
        }
        if (G.rx_len == (int)sizeof G.rx) greet_down("greetd frame overflow");
    }
}

/* ============================================================ */
/* startup                                                       */
/* ============================================================ */

/* One command line per entry, blanks and #-comments skipped. */
static void load_sessions(void) {
    FILE *f = fopen(GREET_SESSIONS, "re");
    if (!f) { msg("greet: no sessions file %s", GREET_SESSIONS); return; }
    char line[SESSION_MAX];
    while (G.nsess < GREET_SESSIONS_CAP && fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        size_t l = strlen(p);
        while (l && (p[l-1] == '\n' || p[l-1] == '\r' || p[l-1] == ' ')) p[--l] = 0;
        if (!l || *p == '#') continue;
        snprintf(G.sess[G.nsess++], SESSION_MAX, "%s", p);
    }
    fclose(f);
}

void greet_init(void) {
    const char *sock = getenv("GREETD_SOCK");
    /* A greet surface outside greetd is a config error, not a degraded mode. */
    if (!sock || !*sock) die("greet: $GREETD_SOCK unset — not running under greetd");

    load_sessions();

    Output *o = focused_output;
    if (!o)
        for (int i = 0; i < MAX_OUTPUTS; i++)
            if (outputs[i].active) { o = &outputs[i]; break; }
    if (!o) die("greet: no active output");
    Widget *w = widget_alloc(W_GREET);
    if (!w) die("greet: no widget slot");
    memset(&w->s.greet, 0, sizeof w->s.greet);
    set_field(w->s.greet.user, sizeof w->s.greet.user, GREET_USER);
    sync_session();
    widget_setup_surface(w, GREET_LAYER, "wisp-greet", o);
    widget_set_size(w, GREET_W + GREET_SHADOW_PAD_L + GREET_SHADOW_PAD_R,
                       GREET_H + GREET_SHADOW_PAD_T + GREET_SHADOW_PAD_B);
    /* GREET_ANCHOR is 0 when the config omits `anchor`, which is what makes
     * layer-shell centre both axes. */
    widget_set_anchor(w, GREET_ANCHOR);
    widget_set_exclusive_zone(w, -1);
    widget_set_kbd_interactive(w, GREET_KBD);
    wl_req(w->surface, SURFACE_REQ_COMMIT, NULL, 0, -1);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) die("greet: socket: %s", strerror(errno));
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (snprintf(a.sun_path, sizeof a.sun_path, "%s", sock) >= (int)sizeof a.sun_path)
        die("greet: $GREETD_SOCK path too long");
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0)
        die("greet: connect %s: %s", sock, strerror(errno));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    greet_fd = fd;
    create_session();
}

/* DSL for-list accessors (`for s in greet.sessions`). */
int greet_session_count(void) { return G.nsess; }

const char *greet_session_name(int i) {
    return (i >= 0 && i < G.nsess) ? G.sess[i] : "";
}

int greet_session_is_selected(int i) { return i == G.sel; }

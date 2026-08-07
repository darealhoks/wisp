/* polkit authentication agent (org.freedesktop.PolicyKit1.AuthenticationAgent,
 * SYSTEM bus) — the prompt that appears when an app asks for auth_admin.
 *
 * wisp never links or calls PAM: /usr/lib/polkit-1/polkit-agent-helper-1 is
 * setuid root, runs the whole PAM stack and reports the verdict to polkitd
 * itself. This file only registers as the session's agent, draws the declared
 * prompt surface, and pumps text between the user and the helper's stdio.
 *
 * Two things here exist nowhere else in the codebase:
 *  - a DEFERRED reply. BeginAuthentication is answered when auth ends, which
 *    can be a minute later, so its serial + a copy of its sender are stashed.
 *  - a second fd that comes and goes (the helper's stdout), hence the
 *    owns_fd/dispatch pair rather than a single static fd in the main loop.
 *
 * Connect/SASL/framing are the bluez.c skeleton. No reconnect on bus loss —
 * same policy as power.c/bluez.c: a dead system bus is a reboot. */

#define _GNU_SOURCE
#include "wisp.h"
#include "dbus.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define PK_DEST   "org.freedesktop.PolicyKit1"
#define PK_AUTH   "/org/freedesktop/PolicyKit1/Authority"
#define PK_IFACE  "org.freedesktop.PolicyKit1.Authority"
#define PK_AGENT  "/org/freedesktop/PolicyKit1/AuthenticationAgent"
#define PK_AIFACE "org.freedesktop.PolicyKit1.AuthenticationAgent"
#define PK_HELPER "/usr/lib/polkit-1/polkit-agent-helper-1"

#define PK_TRIES  3

/* Declared surface geometry (`spawned_by = polkit` template). Defaults keep
 * this TU compiling standalone before the template lands. */
/* All PK_* come from gen_overrides.h, which the Makefile force-includes; the
 * fallbacks only keep this file standalone-compilable. Names must match
 * codegen.c's polkit override block. */
#ifndef PK_W
#define PK_W 420
#endif
#ifndef PK_H
#define PK_H 180
#endif
#ifndef PK_LAYER
#define PK_LAYER 3
#endif
#ifndef PK_KBD
#define PK_KBD (1)
#endif
#ifndef PK_ANCHOR
#define PK_ANCHOR 0
#endif
#ifndef PK_SHADOW_PAD_L
#define PK_SHADOW_PAD_L 0
#endif
#ifndef PK_SHADOW_PAD_R
#define PK_SHADOW_PAD_R 0
#endif
#ifndef PK_SHADOW_PAD_T
#define PK_SHADOW_PAD_T 0
#endif
#ifndef PK_SHADOW_PAD_B
#define PK_SHADOW_PAD_B 0
#endif

/* Linux input keycodes (evdev), same subset menu.c uses. */
#define KEY_ESC    1
#define KEY_BS    14
#define KEY_ENTER 28
#define KEY_LSHIFT 42
#define KEY_RSHIFT 54

extern void render_polkit(Widget *w);

int pk_fd = -1;

static uint32_t pk_serial = 1;

/* One authentication at a time. polkitd may ask for a second while one is in
 * flight; that one is refused immediately (see begin_auth) rather than queued.
 * ponytail: serialised, not a table — two simultaneous admin prompts is not a
 * workflow; grow a cookie-keyed table if it ever happens for real. */
static struct {
    int      live;
    char     cookie[256];
    char     sender[64];      /* copy: the receive buffer is transient */
    uint32_t serial;          /* the deferred BeginAuthentication */
    char     user[64];
    int      attempt;

    pid_t    pid;
    int      in_fd, out_fd;   /* helper stdin (we write), stdout (in epoll) */
    char     line[1024];
    int      line_len;

    int      awaiting;        /* a PAM prompt is on screen, waiting for Enter */
    int      echo;            /* PAM_PROMPT_ECHO_ON: show the text, not dots */
    char     pw[512];         /* helper fgets()es PAM_MAX_RESP_SIZE == 512 */
    int      pw_len;
} A;

/* ============================================================ */
/* bus plumbing                                                  */
/* ============================================================ */

static uint32_t pk_send(const Msg *m) {
    if (pk_fd < 0) return 0;
    uint32_t ser = pk_serial++;
    if (!pk_serial) pk_serial = 1;
    W h = {0};
    dbus_msg_build(&h, m, ser);
    int rc = (int)send(pk_fd, h.b, h.pos, MSG_NOSIGNAL);
    free(h.b);
    return rc == h.pos ? ser : 0;
}

static void pk_reply_empty(uint32_t serial, const char *sender) {
    Msg m = { .type = DBUS_TYPE_METHOD_RETURN, .reply_serial = serial,
              .destination = sender };
    pk_send(&m);
}

static void pk_reply_error(uint32_t serial, const char *sender,
                           const char *name, const char *text) {
    W b = {0};
    wstr(&b, text);
    Msg m = { .type = DBUS_TYPE_ERROR, .reply_serial = serial,
              .destination = sender, .error_name = name,
              .signature = "s", .body = b.b, .body_len = b.pos };
    pk_send(&m);
    free(b.b);
}

/* Body of RegisterAuthenticationAgent: (sa{sv})ss. There is no container
 * helper in dbus_wire.c — array lengths are backpatched, tray.c:459 style. */
static void register_agent(void) {
    const char *sid = getenv("XDG_SESSION_ID");
    if (!sid || !*sid) {
        msg("polkit: no $XDG_SESSION_ID — not registering as auth agent");
        return;
    }
    W b = {0};
    walign(&b, 8);                     /* the (sa{sv}) struct */
    wstr(&b, "unix-session");
    /* align before recording lp: wu32 aligns to 4 itself, so on an unaligned
     * pos the length field would land past lp and the backpatch would miss it */
    walign(&b, 4);
    int lp = b.pos;
    wu32(&b, 0);
    walign(&b, 8);                     /* dict_entry alignment */
    int start = b.pos;
    wstr(&b, "session-id");
    wsig(&b, "s");
    wstr(&b, sid);
    uint32_t alen = (uint32_t)(b.pos - start);
    memcpy(b.b + lp, &alen, 4);
    wstr(&b, "en_US.UTF-8");
    wstr(&b, PK_AGENT);
    Msg m = { .type = DBUS_TYPE_METHOD_CALL, .path = PK_AUTH,
              .interface = PK_IFACE, .member = "RegisterAuthenticationAgent",
              .destination = PK_DEST,
              .signature = "(sa{sv})ss", .body = b.b, .body_len = b.pos };
    pk_send(&m);
    free(b.b);
}

/* ============================================================ */
/* prompt surface                                                */
/* ============================================================ */

static Widget *pk_widget(void) {
    for (int i = 0; i < MAX_WIDGETS; i++)
        if (widgets[i].kind == W_POLKIT) return &widgets[i];
    return NULL;
}

void polkit_render(Widget *w) { render_polkit(w); }

static void ui_repaint(void) {
    Widget *w = pk_widget();
    if (w && w->configured) render_polkit(w);
}

static void ui_show(void) {
    if (pk_widget()) { ui_repaint(); return; }
    Output *o = focused_output;
    if (!o)
        for (int i = 0; i < MAX_OUTPUTS; i++)
            if (outputs[i].active) { o = &outputs[i]; break; }
    Widget *w = widget_alloc(W_POLKIT);
    if (!w) { msg("polkit: no widget slot for the prompt"); return; }
    w->s.polkit.message[0] = w->s.polkit.prompt[0] = 0;
    w->s.polkit.dots[0] = w->s.polkit.error[0] = 0;
    w->s.polkit.user[0] = 0;
    w->s.polkit.failed = 0;
    widget_setup_surface(w, PK_LAYER, "wisp-polkit", o);
    widget_set_size(w, PK_W + PK_SHADOW_PAD_L + PK_SHADOW_PAD_R,
                       PK_H + PK_SHADOW_PAD_T + PK_SHADOW_PAD_B);
    /* PK_ANCHOR is 0 when the config omits `anchor`, which is what makes
     * layer-shell centre both axes. */
    widget_set_anchor(w, PK_ANCHOR);
    widget_set_exclusive_zone(w, -1);
    widget_set_kbd_interactive(w, PK_KBD);
    wl_req(w->surface, SURFACE_REQ_COMMIT, NULL, 0, -1);
}

/* Full teardown, not a hide: nothing stays mapped and no pool stays alive
 * between authentications. */
static void ui_hide(void) {
    Widget *w = pk_widget();
    if (w) widget_destroy(w);
}

static void set_field(char *dst, size_t cap, const char *src) {
    snprintf(dst, cap, "%s", src);
}

/* PAM_PROMPT_ECHO_ON: the answer is not a secret, so it is shown verbatim.
 * Truncated to the declared field, backing off to a codepoint boundary. */
static void echo_visible(Widget *w) {
    int n = A.pw_len;
    if (n > (int)sizeof w->s.polkit.dots - 1) n = (int)sizeof w->s.polkit.dots - 1;
    while (n > 0 && (A.pw[n] & 0xc0) == 0x80) n--;
    memcpy(w->s.polkit.dots, A.pw, (size_t)n);
    w->s.polkit.dots[n] = 0;
}

/* ============================================================ */
/* helper process                                                */
/* ============================================================ */

static void helper_kill(void) {
    if (A.out_fd >= 0) { epoll_del_fd(A.out_fd); close(A.out_fd); A.out_fd = -1; }
    if (A.in_fd  >= 0) { close(A.in_fd);  A.in_fd  = -1; }
    /* main() sets SIGCHLD to SIG_IGN, so the kernel reaps it for us. */
    if (A.pid > 0) kill(A.pid, SIGTERM);
    A.pid = 0;
    A.line_len = 0;
    A.awaiting = 0;
    explicit_bzero(A.pw, sizeof A.pw);
    A.pw_len = 0;
}

/* argc == 2: the cookie goes on stdin, never on argv — argv is the legacy
 * CVE-2015-4625 path and would expose it in `ps`. */
static int helper_spawn(void) {
    int ip[2], op[2];
    if (pipe(ip) < 0) return -1;
    if (pipe(op) < 0) { close(ip[0]); close(ip[1]); return -1; }
    pid_t p = fork();
    if (p == 0) {
        signal(SIGCHLD, SIG_DFL);
        dup2(ip[0], 0);
        dup2(op[1], 1);
        int devnull = open("/dev/null", O_RDWR);
        /* The helper's diagnostics go to stderr; a pipe nobody drains would
         * wedge it once the buffer fills. */
        if (devnull >= 0) { dup2(devnull, 2); close(devnull); }
        close(ip[0]); close(ip[1]); close(op[0]); close(op[1]);
        execl(PK_HELPER, PK_HELPER, A.user, (char *)NULL);
        _exit(127);
    }
    close(ip[0]); close(op[1]);
    if (p < 0) { close(ip[1]); close(op[0]); return -1; }
    A.pid = p;
    A.in_fd = ip[1];
    A.out_fd = op[0];
    fcntl(A.out_fd, F_SETFL, fcntl(A.out_fd, F_GETFL) | O_NONBLOCK);
    epoll_add_fd(A.out_fd);
    /* First thing on the helper's stdin is the cookie, one line. */
    char c[sizeof A.cookie + 2];
    int n = snprintf(c, sizeof c, "%s\n", A.cookie);
    if (write(A.in_fd, c, (size_t)n) != n) { helper_kill(); return -1; }
    return 0;
}

/* ============================================================ */
/* authentication lifecycle                                      */
/* ============================================================ */

static void auth_finish(const char *err_name, const char *err_text) {
    if (!A.live) return;
    uint32_t serial = A.serial;
    char sender[64];
    memcpy(sender, A.sender, sizeof sender);
    helper_kill();
    ui_hide();
    explicit_bzero(&A, sizeof A);
    A.in_fd = A.out_fd = -1;
    if (err_name) pk_reply_error(serial, sender, err_name, err_text);
    else          pk_reply_empty(serial, sender);
}

static void auth_cancel(void) {
    auth_finish("org.freedesktop.PolicyKit1.Error.Cancelled",
                "wisp: authentication dismissed");
}

/* One attempt failed. Up to PK_TRIES total, each a fresh helper spawn. */
static void auth_retry(void) {
    helper_kill();
    if (++A.attempt >= PK_TRIES) { auth_finish(NULL, NULL); return; }
    Widget *w = pk_widget();
    if (w) {
        w->s.polkit.failed = 1;
        w->s.polkit.prompt[0] = 0;
        w->s.polkit.dots[0] = 0;
    }
    if (helper_spawn() < 0) { auth_finish(NULL, NULL); return; }
    ui_repaint();
}

/* ============================================================ */
/* helper stdout: "<TOKEN> <payload>\n", payload g_strescape'd                */
/* ============================================================ */

static void unescape(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; *s && o + 1 < cap; s++) {
        if (*s != '\\') { out[o++] = *s; continue; }
        s++;
        switch (*s) {
        case 'n':  out[o++] = '\n'; break;
        case 't':  out[o++] = '\t'; break;
        case 'r':  out[o++] = '\r'; break;
        case 'b':  out[o++] = '\b'; break;
        case 'f':  out[o++] = '\f'; break;
        case '\\': out[o++] = '\\'; break;
        case '"':  out[o++] = '"';  break;
        case 0:    s--; break;      /* trailing backslash: stop at the NUL */
        default:
            if (*s >= '0' && *s <= '7') {
                int v = 0, k = 0;
                while (k < 3 && *s >= '0' && *s <= '7') { v = v * 8 + (*s - '0'); s++; k++; }
                s--;
                out[o++] = (char)v;
            } else {
                out[o++] = *s;
            }
        }
    }
    out[o] = 0;
}

static void helper_line(char *line) {
    char *sp = strchr(line, ' ');
    const char *payload = "";
    if (sp) { *sp = 0; payload = sp + 1; }
    Widget *w = pk_widget();
    if (!strcmp(line, "SUCCESS")) { auth_finish(NULL, NULL); return; }
    if (!strcmp(line, "FAILURE")) { auth_retry(); return; }
    if (!w) return;
    char txt[256];
    unescape(payload, txt, sizeof txt);
    if (!strcmp(line, "PAM_PROMPT_ECHO_OFF") || !strcmp(line, "PAM_PROMPT_ECHO_ON")) {
        A.echo = !strcmp(line, "PAM_PROMPT_ECHO_ON");
        A.awaiting = 1;
        explicit_bzero(A.pw, sizeof A.pw);
        A.pw_len = 0;
        w->s.polkit.dots[0] = 0;
        set_field(w->s.polkit.prompt, sizeof w->s.polkit.prompt, txt);
    } else if (!strcmp(line, "PAM_ERROR_MSG") || !strcmp(line, "PAM_TEXT_INFO")) {
        set_field(w->s.polkit.error, sizeof w->s.polkit.error, txt);
    } else {
        return;                      /* unknown token: ignore, keep pumping */
    }
    ui_repaint();
}

static void helper_dispatch(void) {
    for (;;) {
        ssize_t n = read(A.out_fd, A.line + A.line_len,
                         sizeof A.line - 1 - (size_t)A.line_len);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            auth_retry(); return;
        }
        /* EOF without a terminal line: treat as a failed attempt. */
        if (n == 0) { auth_retry(); return; }
        A.line_len += (int)n;
        for (;;) {
            char *nl = memchr(A.line, '\n', (size_t)A.line_len);
            if (!nl) break;
            *nl = 0;
            int used = (int)(nl - A.line) + 1;
            char one[sizeof A.line];
            memcpy(one, A.line, (size_t)used);
            A.line_len -= used;
            memmove(A.line, A.line + used, (size_t)A.line_len);
            helper_line(one);
            if (!A.live) return;      /* SUCCESS/give-up tore everything down */
        }
        /* A line longer than the buffer is not something the helper emits. */
        if (A.line_len >= (int)sizeof A.line - 1) { auth_retry(); return; }
    }
}

/* ============================================================ */
/* BeginAuthentication / CancelAuthentication                    */
/* ============================================================ */

/* a{sv} of one identity; returns 1 if it carried a "uid". */
static int scan_identity_dict(R *r, uint32_t *uid) {
    uint32_t alen = ru32(r);
    if (!r->ok) return 0;
    ralign(r, 8);
    int64_t end = (int64_t)r->pos + (int64_t)alen;
    if (end > (int64_t)r->len) { r->ok = 0; return 0; }
    int got = 0;
    while (r->pos < (int)end && r->ok) {
        ralign(r, 8);
        const char *k  = rstr(r);
        const char *vs = rsig(r);
        if (!r->ok) return 0;
        if (!strcmp(k, "uid") && vs[0] == 'u') {
            uint32_t v = ru32(r);
            if (r->ok) { *uid = v; got = 1; }
        } else {
            skip_val(r, &vs, 0);
        }
    }
    if (r->ok) r->pos = (int)end;
    return got;
}

/* a(sa{sv}) — pick the identity matching our own uid, else the first. */
static int scan_identities(R *r, uint32_t *out_uid) {
    uint32_t alen = ru32(r);
    if (!r->ok) return 0;
    ralign(r, 8);
    int64_t end = (int64_t)r->pos + (int64_t)alen;
    if (end > (int64_t)r->len) { r->ok = 0; return 0; }
    uint32_t self = (uint32_t)getuid();
    int found = 0;
    while (r->pos < (int)end && r->ok) {
        ralign(r, 8);
        const char *kind = rstr(r);
        if (!r->ok) return 0;
        int is_user = !strcmp(kind, "unix-user");
        uint32_t uid = 0;
        int got = scan_identity_dict(r, &uid);
        if (!r->ok) return 0;
        if (!is_user || !got) continue;
        if (!found) { *out_uid = uid; found = 1; }
        if (uid == self) { *out_uid = uid; break; }
    }
    if (r->ok) r->pos = (int)end;
    return found;
}

static void begin_auth(R *r, uint32_t serial, const char *sender) {
    if (A.live) {
        /* ponytail: one prompt at a time; the second requester is told the
         * user dismissed it rather than being queued. */
        pk_reply_error(serial, sender,
                       "org.freedesktop.PolicyKit1.Error.Cancelled",
                       "wisp: another authentication is already in progress");
        return;
    }
    rstr(r);                                     /* action_id */
    const char *message = rstr(r);
    char msgbuf[256];
    snprintf(msgbuf, sizeof msgbuf, "%s", r->ok ? message : "");
    rstr(r);                                     /* icon_name */
    uint32_t dlen = ru32(r);                     /* a{ss} details: skipped */
    if (r->ok) {
        ralign(r, 8);
        int64_t dend = (int64_t)r->pos + (int64_t)dlen;
        if (dend > (int64_t)r->len) r->ok = 0; else r->pos = (int)dend;
    }
    const char *cookie = rstr(r);
    char cookiebuf[256];
    snprintf(cookiebuf, sizeof cookiebuf, "%s", r->ok ? cookie : "");
    uint32_t uid = 0;
    int have_uid = scan_identities(r, &uid);
    if (!r->ok || !cookiebuf[0] || !have_uid) {
        pk_reply_error(serial, sender, "org.freedesktop.DBus.Error.InvalidArgs",
                       "wisp: malformed BeginAuthentication");
        return;
    }
    struct passwd *pw = getpwuid((uid_t)uid);
    if (!pw || !pw->pw_name) {
        pk_reply_error(serial, sender, "org.freedesktop.PolicyKit1.Error.Failed",
                       "wisp: no such user");
        return;
    }

    explicit_bzero(&A, sizeof A);
    A.in_fd = A.out_fd = -1;
    A.live = 1;
    A.serial = serial;
    snprintf(A.sender, sizeof A.sender, "%s", sender);
    snprintf(A.cookie, sizeof A.cookie, "%s", cookiebuf);
    snprintf(A.user, sizeof A.user, "%s", pw->pw_name);

    ui_show();
    Widget *w = pk_widget();
    if (w) {
        set_field(w->s.polkit.message, sizeof w->s.polkit.message, msgbuf);
        set_field(w->s.polkit.user, sizeof w->s.polkit.user, A.user);
    }
    if (helper_spawn() < 0) {
        auth_finish("org.freedesktop.PolicyKit1.Error.Failed",
                    "wisp: cannot run " PK_HELPER);
        return;
    }
    ui_repaint();
}

static void cancel_auth(R *r, uint32_t serial, const char *sender) {
    const char *cookie = rstr(r);
    if (r->ok && A.live && !strcmp(cookie, A.cookie)) auth_cancel();
    pk_reply_empty(serial, sender);
}

/* ============================================================ */
/* keys                                                          */
/* ============================================================ */

static void send_response(void) {
    if (!A.awaiting || A.in_fd < 0) return;
    /* A newline inside the response would split into two PAM answers; the
     * append path rejects control codepoints, so this is belt-and-braces. */
    if (memchr(A.pw, '\n', (size_t)A.pw_len)) { explicit_bzero(A.pw, sizeof A.pw); A.pw_len = 0; return; }
    A.pw[A.pw_len] = '\n';
    ssize_t rc = write(A.in_fd, A.pw, (size_t)A.pw_len + 1);
    explicit_bzero(A.pw, sizeof A.pw);
    A.pw_len = 0;
    A.awaiting = 0;
    Widget *w = pk_widget();
    if (w) { w->s.polkit.dots[0] = 0; w->s.polkit.prompt[0] = 0; }
    if (rc < 0) { auth_retry(); return; }
    ui_repaint();
}

int polkit_on_key(Widget *w, uint32_t key, uint32_t state) {
    if (w->kind != W_POLKIT) return 0;
    if (state != 1) return 1;
    if (key == KEY_LSHIFT || key == KEY_RSHIFT) return 1;
    if (key == KEY_ESC) { auth_cancel(); return 1; }
    if (key == KEY_ENTER) { send_response(); return 1; }
    if (!A.awaiting) return 1;
    if (key == KEY_BS) {
        if (A.pw_len > 0) {
            int nl = utf8_back(A.pw, A.pw_len);
            explicit_bzero(A.pw + nl, (size_t)(A.pw_len - nl));
            A.pw_len = nl;
            int dl = (int)strlen(w->s.polkit.dots);
            if (A.echo) echo_visible(w);
            else if (dl > 0) w->s.polkit.dots[dl - 1] = 0;
            ui_repaint();
        }
        return 1;
    }
    uint32_t cp = xkb_xlat(key, xkb_shift_on);
    if (!cp || cp < 0x20 || cp == 0x7f) return 1;
    char enc[4];
    int n = utf8_encode(cp, enc);
    if (n <= 0) return 1;
    /* 511 usable bytes + the '\n' send_response appends. */
    if (A.pw_len + n >= (int)sizeof A.pw - 1) return 1;
    memcpy(A.pw + A.pw_len, enc, (size_t)n);
    A.pw_len += n;
    A.pw[A.pw_len] = 0;
    if (A.echo) {
        echo_visible(w);
    } else {
        /* One '*' per codepoint — the raw password is never reachable from
         * the DSL. Past the field's width the mask simply stops growing. */
        int dl = (int)strlen(w->s.polkit.dots);
        if (dl + 1 < (int)sizeof w->s.polkit.dots) {
            w->s.polkit.dots[dl] = '*';
            w->s.polkit.dots[dl + 1] = 0;
        }
    }
    ui_repaint();
    return 1;
}

/* ============================================================ */
/* bus dispatch                                                  */
/* ============================================================ */

static void bus_down(void) {
    if (A.live)
        auth_finish("org.freedesktop.PolicyKit1.Error.Failed", "wisp: system bus lost");
    if (pk_fd >= 0) { epoll_del_fd(pk_fd); close(pk_fd); }
    pk_fd = -1;
}

static void bus_dispatch_one(const uint8_t *b, int len) {
    R r = { .b = b, .len = len, .pos = 0, .ok = 1 };
    rbyte(&r); uint8_t type = rbyte(&r); uint8_t flags = rbyte(&r); rbyte(&r);
    ru32(&r);                              /* body length */
    uint32_t serial = ru32(&r);
    uint32_t flen = ru32(&r);
    if (!r.ok) return;
    int fend = r.pos + (int)flen;
    if (fend > r.len) return;
    char member[64] = "", iface[96] = "", sender[64] = "";
    while (r.pos < fend) {
        ralign(&r, 8);
        if (!r.ok) return;
        uint8_t code = rbyte(&r);
        const char *sig = rsig(&r);
        if (!r.ok || !sig[0]) break;
        switch (code) {
        case HF_INTERFACE: snprintf(iface,  sizeof iface,  "%s", rstr(&r)); break;
        case HF_MEMBER:    snprintf(member, sizeof member, "%s", rstr(&r)); break;
        case HF_SENDER:    snprintf(sender, sizeof sender, "%s", rstr(&r)); break;
        default: { const char *s = sig; skip_val(&r, &s, 0); break; }
        }
        if (!r.ok) return;
    }
    r.pos = fend;
    ralign(&r, 8);

    if (type == DBUS_TYPE_ERROR && !A.live) {
        msg("polkit: polkitd refused the agent registration");
        return;
    }
    if (type != DBUS_TYPE_METHOD_CALL) return;
    if (!strcmp(iface, PK_AIFACE) && !strcmp(member, "BeginAuthentication")) {
        begin_auth(&r, serial, sender);   /* deferred: replies when auth ends */
        return;
    }
    if (!strcmp(iface, PK_AIFACE) && !strcmp(member, "CancelAuthentication")) {
        cancel_auth(&r, serial, sender);
        return;
    }
    /* polkitd blocks on its calls: never leave one unanswered. */
    if (!(flags & 1))
        pk_reply_error(serial, sender, "org.freedesktop.DBus.Error.UnknownMethod",
                       "wisp: no such method");
}

static void bus_dispatch(void) {
    /* polkit messages are small; a frame that overruns the buffer is a peer we
     * don't understand — drop the connection rather than resync. */
    static uint8_t buf[8192];
    static int have;
    for (;;) {
        ssize_t n = recv(pk_fd, buf + have, sizeof buf - (size_t)have, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            bus_down(); return;
        }
        if (n == 0) { bus_down(); return; }
        have += (int)n;
        for (;;) {
            if (have < 16) break;
            uint32_t body_len, fields_len;
            memcpy(&body_len, buf + 4, 4);
            memcpy(&fields_len, buf + 12, 4);
            uint32_t hdr = (16 + fields_len + 7u) & ~7u;
            uint64_t total = (uint64_t)hdr + body_len;
            if (total > sizeof buf) { bus_down(); return; }
            if ((uint64_t)have < total) break;
            bus_dispatch_one(buf, (int)total);
            if (pk_fd < 0) return;
            memmove(buf, buf + total, (size_t)have - total);
            have -= (int)total;
        }
        if (have == (int)sizeof buf) { bus_down(); return; }
    }
}

/* ============================================================ */
/* entry points                                                  */
/* ============================================================ */

int polkit_owns_fd(int fd) {
    return fd >= 0 && (fd == pk_fd || fd == A.out_fd);
}

void polkit_dispatch(int fd) {
    if (fd == pk_fd) { bus_dispatch(); return; }
    if (fd == A.out_fd && A.out_fd >= 0) helper_dispatch();
}

void polkit_init(void) {
    A.in_fd = A.out_fd = -1;
    const char *addr = getenv("DBUS_SYSTEM_BUS_ADDRESS");
    if (!addr) addr = "unix:path=/run/dbus/system_bus_socket";
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)] = "";
    int abs_len = dbus_parse_bus_addr(addr, path, sizeof path);
    if (abs_len < 0) { msg("polkit: bad system bus addr %s", addr); return; }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    int slen;
    if (abs_len > 0) {
        memcpy(a.sun_path, path, (size_t)abs_len);
        slen = (int)offsetof(struct sockaddr_un, sun_path) + abs_len;
    } else {
        snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
        slen = sizeof a;
    }
    if (connect(fd, (struct sockaddr *)&a, slen) < 0 || dbus_sasl_auth(fd) < 0) {
        msg("polkit: system bus unavailable — no authentication agent");
        close(fd); return;
    }
    pk_fd = fd;
    Msg hello = { .type = DBUS_TYPE_METHOD_CALL,
                  .path = "/org/freedesktop/DBus",
                  .interface = "org.freedesktop.DBus",
                  .member = "Hello",
                  .destination = "org.freedesktop.DBus" };
    pk_send(&hello);
    register_agent();
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

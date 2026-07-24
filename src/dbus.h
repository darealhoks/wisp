/* Shared wire-level guts of the dbus file family (dbus.c transport +
 * notify.c Notifications server; mpris/tray consumers plug in the same way).
 * Not part of the public runtime API — that stays in wisp.h. */
#ifndef WISP_DBUS_H
#define WISP_DBUS_H

#include <stdint.h>

#define DBUS_TYPE_METHOD_CALL   1
#define DBUS_TYPE_METHOD_RETURN 2
#define DBUS_TYPE_ERROR         3
#define DBUS_TYPE_SIGNAL        4

#define HF_PATH         1
#define HF_INTERFACE    2
#define HF_MEMBER       3
#define HF_ERROR_NAME   4
#define HF_REPLY_SERIAL 5
#define HF_DESTINATION  6
#define HF_SENDER       7
#define HF_SIGNATURE    8

/* Append-only write buffer (heap-backed; caller frees b). */
typedef struct {
    uint8_t *b;
    int      cap;
    int      pos;
} W;

void wensure(W *w, int n);
void walign(W *w, int a);
void wbyte(W *w, uint8_t v);
void wu32(W *w, uint32_t v);
void wstr(W *w, const char *s);
void wsig(W *w, const char *s);

/* Read view (read-only); every reader bounds-checks and latches !ok. */
typedef struct {
    const uint8_t *b;
    int            len;
    int            pos;
    int            ok;
} R;

void        ralign(R *r, int a);
uint8_t     rbyte(R *r);
uint16_t    ru16(R *r);
uint32_t    ru32(R *r);
int32_t     ri32(R *r);
const char *rstr(R *r);
const char *rsig(R *r);
int         skip_val(R *r, const char **sigp, int depth);

/* Build and send a complete D-Bus message.
 *   type   : DBUS_TYPE_*
 *   serial : 0 → auto-assign
 *   flags  : 0, or 1 = no-reply expected
 *   For each field, only non-NULL strings (and non-zero reply_serial) emit. */
typedef struct {
    uint8_t     type;
    uint8_t     flags;
    uint32_t    serial;        /* 0 = auto */
    const char *path;
    const char *interface;
    const char *member;
    const char *error_name;
    uint32_t    reply_serial;
    const char *destination;
    const char *signature;     /* if NULL but body_len>0, asserts */
    const uint8_t *body;
    int            body_len;
} Msg;

uint32_t send_msg(const Msg *m);

/* dbus_wire.c — connection-agnostic guts shared with power.c's system-bus
 * client: full message marshal (caller owns serial + socket), SASL EXTERNAL,
 * and bus-address parsing (returns >0 abstract-namespace length, 0 for a
 * filesystem path, -1 on parse failure). */
void dbus_msg_build(W *h, const Msg *m, uint32_t serial);
int  dbus_sasl_auth(int fd);
int  dbus_parse_bus_addr(const char *spec, char *out, size_t cap);

/* Async method call: sends `m` and routes its METHOD_RETURN/ERROR to `cb`.
 * `r` is positioned at the reply body, `sig` is its signature, `is_err` is 1
 * for an ERROR (body is then the error message, if any). `sender` is the
 * replier's unique name — the only way to learn which peer owns a well-known
 * name we called. Returns the serial, or 0 if the call could not be
 * sent/tracked (cb then never fires). */
typedef void (*dbus_reply_cb)(const char *sender, R *r, const char *sig,
                              int is_err, void *ud);
uint32_t dbus_call(const Msg *m, dbus_reply_cb cb, void *ud);

/* notify.c: method calls with interface org.freedesktop.Notifications,
 * routed here by dbus.c's dispatch. r is positioned at the body. */
void notify_method_call(R *r, const char *member, uint32_t serial,
                        const char *sender);

/* Empty METHOD_RETURN / error reply to `serial`. Every method call we do not
 * answer stalls a blocking caller for its full timeout, so dispatch answers
 * unconditionally — see dispatch_one. */
void dbus_reply_empty(uint32_t serial, const char *sender);
void dbus_reply_error(uint32_t serial, const char *sender,
                      const char *name, const char *msg);

/* tray.c: method calls on org.kde.StatusNotifierWatcher plus the Properties
 * and Introspectable interfaces (we're the only object serving them, so they
 * all land here). Returns non-zero if the call was answered.
 * Also a bus-up hook: the item list is per-connection state. */
int  tray_method_call(R *r, const char *iface, const char *member,
                      const char *path, uint32_t serial, const char *sender);
void tray_on_bus_up(void);

#endif

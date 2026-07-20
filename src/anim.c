/* anim.c — global tween scheduler. Frame-paced via a 60 Hz timerfd that we
 * only arm while at least one anim is active; when the active count drops to
 * 0 the timerfd is disarmed so the daemon's idle-CPU invariant is preserved.
 *
 * The plan's strict "compositor frame-callback drives every tick" path is an
 * optimization deferred to a later step (the per-widget frame-callback
 * machinery already exists in widget.c but isn't wired into a generic
 * scheduler). A 16 ms timerfd matches dwl's typical refresh rate closely
 * enough for the verify-gate (idle CPU = 0 when no tween is live). */

#include "wisp.h"

#include <math.h>
#include <string.h>
#include <unistd.h>
#include <sys/timerfd.h>

Anim anims[ANIM_MAX];
int  anim_active_count = 0;

static int     anim_tfd = -1;
static int     anim_armed = 0;

static void anim_tfd_arm(void) {
    if (anim_tfd < 0) {
        anim_tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (anim_tfd < 0) return;
        epoll_add_fd(anim_tfd);
    }
    if (anim_armed) return;
    struct itimerspec ts = {
        .it_value    = { .tv_sec = 0, .tv_nsec = 16 * 1000 * 1000 },
        .it_interval = { .tv_sec = 0, .tv_nsec = 16 * 1000 * 1000 },
    };
    timerfd_settime(anim_tfd, 0, &ts, NULL);
    anim_armed = 1;
}

static void anim_tfd_disarm(void) {
    if (anim_tfd < 0 || !anim_armed) return;
    struct itimerspec ts = {{0,0},{0,0}};
    timerfd_settime(anim_tfd, 0, &ts, NULL);
    anim_armed = 0;
}

int anim_fd(void) { return anim_tfd; }

int anim_any_active(void) { return anim_active_count; }

void anim_cancel_for(void *target) {
    for (int i = 0; i < ANIM_MAX; i++) {
        if (anims[i].active && anims[i].target == target) {
            anims[i].active = 0;
            if (anim_active_count > 0) anim_active_count--;
        }
    }
    if (anim_active_count == 0) anim_tfd_disarm();
}

/* Cancel every tween owned by `w` (codegen item tweens pass the Widget as
 * owner). Called from widget_destroy so a dying slot leaves no tween firing
 * bar_render() on a freed/reused widget. on_done is intentionally skipped —
 * the owner is gone. */
void anim_cancel_owner(Widget *w) {
    for (int i = 0; i < ANIM_MAX; i++) {
        if (anims[i].active && anims[i].owner == w) {
            anims[i].active = 0;
            if (anim_active_count > 0) anim_active_count--;
        }
    }
    if (anim_active_count == 0) anim_tfd_disarm();
}

static int alloc_slot(void) {
    for (int i = 0; i < ANIM_MAX; i++) if (!anims[i].active) return i;
    return -1;
}

uint32_t anim_start_num(void *target, AnimType type, double from, double to,
                        int duration_ms, Easing e, const double bez[4],
                        Widget *owner, AnimDone on_done, void *user) {
    anim_cancel_for(target);
    int i = alloc_slot();
    if (i < 0) return 0;
    Anim *a = &anims[i];
    memset(a, 0, sizeof *a);
    a->active = 1;
    a->target = target;
    a->type = type;
    a->from = from;
    a->to = to;
    a->start_ms = now_ms();
    a->duration_ms = duration_ms > 0 ? duration_ms : 1;
    a->easing = e;
    if (bez) memcpy(a->bez, bez, sizeof a->bez);
    a->owner = owner;
    a->on_done = on_done;
    a->user = user;
    anim_active_count++;
    anim_tfd_arm();
    return (uint32_t)(i + 1);
}

uint32_t anim_start_color(uint32_t *target, uint32_t from, uint32_t to,
                          int duration_ms, Easing e, const double bez[4],
                          Widget *owner, AnimDone on_done, void *user) {
    anim_cancel_for(target);
    int i = alloc_slot();
    if (i < 0) return 0;
    Anim *a = &anims[i];
    memset(a, 0, sizeof *a);
    a->active = 1;
    a->target = target;
    a->type = ANIM_T_COLOR;
    a->from_c = from;
    a->to_c = to;
    a->start_ms = now_ms();
    a->duration_ms = duration_ms > 0 ? duration_ms : 1;
    a->easing = e;
    if (bez) memcpy(a->bez, bez, sizeof a->bez);
    a->owner = owner;
    a->on_done = on_done;
    a->user = user;
    anim_active_count++;
    anim_tfd_arm();
    return (uint32_t)(i + 1);
}

/* Cubic Bezier with control points (ax,ay) and (cx,cy) — endpoints at (0,0)
 * and (1,1). Given x = t (linear time), find the parametric s such that
 * Bx(s) = x, then return By(s). Newton iteration, ~6 steps is plenty. */
static double bez_eval(double x, double ax, double ay, double cx, double cy) {
    double s = x;
    for (int i = 0; i < 6; i++) {
        double bx = 3*(1-s)*(1-s)*s*ax + 3*(1-s)*s*s*cx + s*s*s;
        double dbx = 3*(1-s)*(1-s)*(ax) + 6*(1-s)*s*(cx-ax) + 3*s*s*(1-cx);
        double err = bx - x;
        if (fabs(err) < 1e-5 || dbx == 0) break;
        s -= err / dbx;
        if (s < 0) s = 0; else if (s > 1) s = 1;
    }
    return 3*(1-s)*(1-s)*s*ay + 3*(1-s)*s*s*cy + s*s*s;
}

double anim_ease(int easing, double t) {
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    Easing e = (Easing)easing;
    switch (e) {
    case EASE_LINEAR:   return t;
    case EASE_IN:       return t * t * t;
    case EASE_OUT: {
        double u = 1 - t;
        return 1 - u * u * u;
    }
    case EASE_IN_OUT:
        if (t < 0.5) return 4 * t * t * t;
        else { double u = -2 * t + 2; return 1 - (u*u*u) / 2; }
    case EASE_CUBIC_BEZIER:
        return t;  /* Bezier needs the [4]-coeff array; not used for hud reveal */
    }
    return t;
}

static double ease(Easing e, double t, const double bez[4]) {
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    switch (e) {
    case EASE_LINEAR:   return t;
    case EASE_IN:       return t * t * t;
    case EASE_OUT: {
        double u = 1 - t;
        return 1 - u * u * u;
    }
    case EASE_IN_OUT:
        if (t < 0.5) return 4 * t * t * t;
        else { double u = -2 * t + 2; return 1 - (u*u*u) / 2; }
    case EASE_CUBIC_BEZIER:
        return bez_eval(t, bez[0], bez[1], bez[2], bez[3]);
    }
    return t;
}

static uint32_t lerp_color(uint32_t a, uint32_t b, double u) {
    int ca[4], cb[4], cr[4];
    for (int i = 0; i < 4; i++) { ca[i] = (a >> (i*8)) & 0xff; cb[i] = (b >> (i*8)) & 0xff; }
    for (int i = 0; i < 4; i++) {
        double v = ca[i] + (cb[i] - ca[i]) * u;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        cr[i] = (int)(v + 0.5);
    }
    return ((uint32_t)cr[0]) | ((uint32_t)cr[1] << 8) |
           ((uint32_t)cr[2] << 16) | ((uint32_t)cr[3] << 24);
}

void anim_tick(int64_t now) {
    int repainted[MAX_WIDGETS] = {0};
    for (int i = 0; i < ANIM_MAX; i++) {
        Anim *a = &anims[i];
        if (!a->active) continue;
        double t = (double)(now - a->start_ms) / (double)a->duration_ms;
        int done = 0;
        if (t >= 1.0) { t = 1.0; done = 1; }
        double u = ease(a->easing, t, a->bez);
        switch (a->type) {
        case ANIM_T_INT: {
            double v = a->from + (a->to - a->from) * u;
            *(int*)a->target = (int)(v + (v >= 0 ? 0.5 : -0.5));
            break;
        }
        case ANIM_T_FLOAT:
            *(double*)a->target = a->from + (a->to - a->from) * u;
            break;
        case ANIM_T_COLOR:
            *(uint32_t*)a->target = done ? a->to_c : lerp_color(a->from_c, a->to_c, u);
            break;
        }
        if (a->owner) {
            int idx = (int)(a->owner - widgets);
            if (idx >= 0 && idx < MAX_WIDGETS && !repainted[idx]) {
                repainted[idx] = 1;
                bar_render(a->owner);
            }
        }
        if (done) {
            AnimDone cb = a->on_done; void *u2 = a->user;
            a->active = 0;
            if (anim_active_count > 0) anim_active_count--;
            if (cb) cb(u2);
        }
    }
    if (anim_active_count == 0) anim_tfd_disarm();
}

/* Drain the timerfd; main loop calls this when EPOLLIN fires on anim_fd(). */
void anim_on_tfd(void) {
    if (anim_tfd < 0) return;
    uint64_t exp;
    (void)!read(anim_tfd, &exp, sizeof exp);
    anim_tick(now_ms());
}

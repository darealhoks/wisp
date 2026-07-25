/* Night-mode color temperature via zwlr_gamma_control_v1. Replaces the
 * external wlsunset process: schedule lives here, HUD toggle drives it via
 * `wispctl gamma flat|off|auto|...`.
 *
 * Hard-step at GAMMA_NIGHT_HOUR (warm) and GAMMA_DAY_HOUR (cool), softened by
 * a GAMMA_FADE_MIN crossfade centered on each edge (0 = hard step). Manual
 * mode changes (HUD button, wispctl) step instantly unless GAMMA_TRANSITION_MS
 * asks for a tween.
 *
 * Blackbody → sRGB approximation is Tanner Helland's polynomial. Accurate
 * enough for warming-the-screen-at-night; not a colorimetric tool. */
#include "wisp.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static GammaMode mode = GM_AUTO;
static int       last_minute = -1;

/* Compute linear-RGB coefficients [0,1] for blackbody temperature in K. */
static void temp_to_rgb(int kelvin, double *r, double *g, double *b) {
    if (kelvin <  1000) kelvin =  1000;
    if (kelvin > 40000) kelvin = 40000;
    double t = kelvin / 100.0;
    double dr, dg, db;
    if (t <= 66.0) {
        dr = 255.0;
        dg = 99.4708025861 * log(t) - 161.1195681661;
        if (t <= 19.0) db = 0;
        else db = 138.5177312231 * log(t - 10.0) - 305.0447927307;
    } else {
        dr = 329.698727446  * pow(t - 60.0, -0.1332047592);
        dg = 288.1221695283 * pow(t - 60.0, -0.0755148492);
        db = 255.0;
    }
    if (dr < 0) dr = 0; else if (dr > 255) dr = 255;
    if (dg < 0) dg = 0; else if (dg > 255) dg = 255;
    if (db < 0) db = 0; else if (db > 255) db = 255;
    *r = dr / 255.0; *g = dg / 255.0; *b = db / 255.0;
}

static int schedule_target_k(void) {
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    int mins = lt.tm_hour * 60 + lt.tm_min;
    int day_mins   = GAMMA_DAY_HOUR   * 60;
    int night_mins = GAMMA_NIGHT_HOUR * 60;
    /* Linear fade centered on each transition; 0 disables it (hard step). */
    int fade = GAMMA_FADE_MIN;
    if (fade > 0) {
        if (mins >= day_mins - fade/2 && mins < day_mins + fade/2) {
            /* dawn: night → day */
            double a = (double)(mins - (day_mins - fade/2)) / fade;
            return (int)(GAMMA_NIGHT_K + (GAMMA_DAY_K - GAMMA_NIGHT_K) * a + 0.5);
        }
        if (mins >= night_mins - fade/2 && mins < night_mins + fade/2) {
            /* dusk: day → night */
            double a = (double)(mins - (night_mins - fade/2)) / fade;
            return (int)(GAMMA_DAY_K + (GAMMA_NIGHT_K - GAMMA_DAY_K) * a + 0.5);
        }
    }
    if (mins >= day_mins && mins < night_mins) return GAMMA_DAY_K;
    return GAMMA_NIGHT_K;
}

static int mode_target_k(void) {
    switch (mode) {
    case GM_DAY:   return GAMMA_DAY_K;
    case GM_NIGHT: return GAMMA_NIGHT_K;
    case GM_FLAT:  return GAMMA_FLAT_K;
    case GM_OFF:   return 0;
    case GM_AUTO:
    default:       return schedule_target_k();
    }
}

/* Write a fresh ramp to a new memfd and ship it to one output's control. */
static void apply_kelvin_one(Output *o, int kelvin) {
    if (!o || !o->gamma_ctrl || !o->gamma_size) return;
    if (kelvin == o->last_applied_k) return;

    int fd = syscall(SYS_memfd_create, "wisp-gamma", 0u);
    if (fd < 0) { msg("gamma: memfd_create: %s", strerror(errno)); return; }

    size_t bytes = (size_t)o->gamma_size * 3 * sizeof(uint16_t);
    if (ftruncate(fd, bytes) < 0) { close(fd); return; }
    uint16_t *map = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); return; }

    double r = 1, g = 1, b = 1;
    if (kelvin > 0) temp_to_rgb(kelvin, &r, &g, &b);
    /* kelvin == 0 (OFF) → identity ramp (r=g=b=1) → passthrough */

    uint16_t *Rch = map;
    uint16_t *Gch = map + o->gamma_size;
    uint16_t *Bch = map + 2 * o->gamma_size;
    double denom = o->gamma_size > 1 ? (double)(o->gamma_size - 1) : 1.0;
    for (uint32_t i = 0; i < o->gamma_size; i++) {
        double v = (double)i / denom * 65535.0;
        Rch[i] = (uint16_t)(v * r + 0.5);
        Gch[i] = (uint16_t)(v * g + 0.5);
        Bch[i] = (uint16_t)(v * b + 0.5);
    }
    munmap(map, bytes);

    wl_req(o->gamma_ctrl, GAMMA_CTRL_REQ_SET_GAMMA, NULL, 0, fd);
    close(fd);
    o->last_applied_k = kelvin;
}

static void apply_kelvin_all(int kelvin) {
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (outputs[i].active) apply_kelvin_one(&outputs[i], kelvin);
}

void gamma_bind_output(Output *o) {
    if (!o || !id_gamma_mgr || o->gamma_ctrl || o->gamma_failed) return;
    /* Reload path: the control is exclusive per output, so reuse the pre-exec
     * process's instead of release+rebind (which blips the screen to neutral).
     * The old ramp stays applied; no size event will come, but gamma_size is
     * handed over and the auto tick re-applies within a second. */
    o->gamma_ctrl = wl_take_adopted_gamma(o->name, &o->gamma_size);
    if (o->gamma_ctrl) return;
    o->gamma_ctrl = wl_new_id();
    uint32_t a[2] = { o->gamma_ctrl, o->wl_output };
    wl_req(id_gamma_mgr, GAMMA_MGR_REQ_GET_GAMMA_CONTROL, a, 2, -1);
}

void gamma_init(void) {
    if (!id_gamma_mgr) {
        msg("gamma: zwlr_gamma_control_manager_v1 not advertised — skipping");
        return;
    }
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (outputs[i].active) gamma_bind_output(&outputs[i]);
}

void gamma_on_size(Output *o, uint32_t size) {
    if (!o) return;
    o->gamma_size = size;
    o->last_applied_k = 0;
    apply_kelvin_one(o, mode_target_k());
}

void gamma_on_failed(Output *o) {
    if (!o) return;
    msg("gamma: control failed for output (another client holds it?) — disabling");
    o->gamma_failed = 1;
    o->gamma_ctrl = 0;
    o->gamma_size = 0;
}

/* Weak: only defined when a .wisp declares a gamma_warm()/dnd() source. */
extern void wispgen_wisp_state_changed(void) __attribute__((weak));

#if GAMMA_TRANSITION_MS > 0
static double tween_k;      /* animated Kelvin while a manual switch runs */
static int    tween_to;     /* real target (may be 0 = OFF/identity) */
static int    tweening;

/* OFF is kelvin 0 (identity ramp), which isn't on the blackbody line — tween
 * through DAY_K instead so the fade doesn't dive into deep red on its way. */
static int tween_k_of(int k) { return k ? k : GAMMA_DAY_K; }

static void tween_frame(void *u) {
    (void)u;
    apply_kelvin_all((int)(tween_k + 0.5));
}

static void tween_end(void *u) {
    (void)u;
    tweening = 0;
    apply_kelvin_all(tween_to);
}

/* Is anything on screen yet? OFF applies kelvin 0, so last_applied_k alone
 * can't tell "passthrough" from "never bound". */
static int have_ramp(void) {
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (outputs[i].active && outputs[i].gamma_size)
            return 1;
    return 0;
}

static int current_k(void) {
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (outputs[i].active && outputs[i].gamma_size)
            return outputs[i].last_applied_k;
    return 0;
}
#endif

void gamma_set_mode(GammaMode m) {
    mode = m;
    last_minute = -1;        /* force re-eval on next tick if AUTO */
    int target = mode_target_k();
#if GAMMA_TRANSITION_MS > 0
    int from = current_k();
    if (have_ramp() && tween_k_of(from) != tween_k_of(target)) {
        tween_to = target;
        tweening = 1;
        uint32_t id = anim_start_num(&tween_k, ANIM_T_FLOAT,
                                     tween_k_of(from), tween_k_of(target),
                                     GAMMA_TRANSITION_MS, EASE_OUT, NULL,
                                     NULL, tween_end, NULL, 1, 0);
        if (id) { anim_on_frame(id, tween_frame, NULL); goto done; }
        tweening = 0;        /* no free slot — fall through to the hard step */
    }
#endif
    apply_kelvin_all(target);
#if GAMMA_TRANSITION_MS > 0
done:;
#endif
    if (wispgen_wisp_state_changed) wispgen_wisp_state_changed();
}

void gamma_tick(int tick_n) {
    (void)tick_n;
    if (mode != GM_AUTO) return;
#if GAMMA_TRANSITION_MS > 0
    if (tweening) return;    /* don't fight the manual fade into AUTO */
#endif
    time_t t = time(NULL);
    struct tm lt; localtime_r(&t, &lt);
    if (lt.tm_min == last_minute) return;
    last_minute = lt.tm_min;
    apply_kelvin_all(schedule_target_k());
    /* AUTO may have crossed a day/night edge; dirty-flags make this free. */
    if (wispgen_wisp_state_changed) wispgen_wisp_state_changed();
}

int gamma_is_warm(void) {
    if (mode == GM_OFF || mode == GM_DAY) return 0;
    if (mode == GM_NIGHT || mode == GM_FLAT) return 1;
    /* AUTO: warm whenever current target < day temperature. */
    return schedule_target_k() < GAMMA_DAY_K;
}

const char *gamma_mode_str(void) {
    switch (mode) {
    case GM_DAY:   return "day";
    case GM_NIGHT: return "night";
    case GM_FLAT:  return "flat";
    case GM_OFF:   return "off";
    case GM_AUTO:
    default:       return gamma_is_warm() ? "auto-night" : "auto-day";
    }
}

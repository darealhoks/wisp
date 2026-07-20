/* media.c — wispctl volume / mic / backlight. Replaces dwl-osd shell.
 *
 *   volume up|down|mute → wpctl @DEFAULT_AUDIO_SINK@   + OSD slot 1
 *   mic mute            → wpctl @DEFAULT_AUDIO_SOURCE@ + OSD slot 2
 *   backlight up|down   → /sys/class/backlight/<dev>/brightness + OSD slot 3
 *
 * wpctl is part of wireplumber (already mandatory). Backlight is a direct
 * sysfs write; brightnessctl ships a udev rule that makes that file group-
 * writable by `video`, which dwlarp users already need to be in. */

#include "wisp.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SLOT_VOL 1
#define SLOT_MIC 2
#define SLOT_BRI 3

#define ICON_VOL    0xf028
#define ICON_VOL_X  0xf026
#define ICON_MIC    0xf130
#define ICON_MIC_X  0xf131
#define ICON_SUN    0xf185

#define VOL_STEP 5
#define VOL_CAP  150
#define BRI_STEP 5

static int popen_read(const char *cmd, char *out, int cap) {
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    int n = (int)fread(out, 1, cap - 1, f);
    out[n > 0 ? n : 0] = 0;
    /* main() sets SIGCHLD=SIG_IGN, so pclose()'s waitpid() returns -1/ECHILD
     * even on success — trust bytes read, not the exit status. */
    pclose(f);
    return n > 0 ? 0 : -1;
}

/* Parses `wpctl get-volume <target>` output:
 *   "Volume: 0.42 [MUTED]\n" */
static int wp_get(const char *target, int *pct, int *muted) {
    char cmd[160]; snprintf(cmd, sizeof cmd, "wpctl get-volume %s 2>/dev/null", target);
    char out[160]; if (popen_read(cmd, out, sizeof out) < 0) return -1;
    float v = 0;
    if (sscanf(out, "Volume: %f", &v) != 1) return -1;
    *pct   = (int)(v * 100 + 0.5f);
    *muted = strstr(out, "MUTED") != NULL;
    return 0;
}
static void wp_set_vol(const char *target, int pct) {
    char cmd[160]; snprintf(cmd, sizeof cmd, "wpctl set-volume %s %d%% 2>/dev/null", target, pct);
    int rc = system(cmd); (void)rc;
}
static void wp_set_mute(const char *target, const char *arg) {
    char cmd[160]; snprintf(cmd, sizeof cmd, "wpctl set-mute %s %s 2>/dev/null", target, arg);
    int rc = system(cmd); (void)rc;
}

/* Held-key repeats fire faster than wpctl can fork+exec+read, so caching the
 * volume state across calls cuts the per-press cost from two wpctl spawns to
 * one. Cache invalidates after VOL_CACHE_MS so external changes (mixers,
 * other clients) reconcile within a beat of going idle. Mute path still
 * re-reads — a missed external mute toggle is worse than a missed level. */
#define VOL_CACHE_MS 1500
static int     vol_cache_have = 0;
static int     vol_cache_pct, vol_cache_muted;
static int64_t vol_cache_ms;

void media_volume(const char *arg) {
    const char *T = "@DEFAULT_AUDIO_SINK@";
    int pct = 0, muted = 0;
    int64_t now = now_ms();
    int is_mute = !strcmp(arg, "mute");
    int fresh = vol_cache_have && !is_mute && (now - vol_cache_ms) < VOL_CACHE_MS;
    if (fresh) {
        pct = vol_cache_pct; muted = vol_cache_muted;
    } else if (wp_get(T, &pct, &muted) < 0) return;

    if (is_mute) {
        wp_set_mute(T, "toggle");
        muted = !muted;
    } else {
        int dir = !strcmp(arg, "up") ? 1 : !strcmp(arg, "down") ? -1 : 0;
        if (!dir) return;
        int n = pct + dir * VOL_STEP;
        if (n < 0) n = 0;
        if (n > VOL_CAP) n = VOL_CAP;
        if (muted) { wp_set_mute(T, "0"); muted = 0; }
        wp_set_vol(T, n);
        pct = n;
    }
    vol_cache_have = 1;
    vol_cache_pct = pct; vol_cache_muted = muted; vol_cache_ms = now;
    osd_post(SLOT_VOL, muted ? "Volume muted" : "Volume", "",
             muted ? ICON_VOL_X : ICON_VOL, pct, 1, muted, OSD_TIMEOUT_OSD);
}

void media_mic(const char *arg) {
    const char *T = "@DEFAULT_AUDIO_SOURCE@";
    if (strcmp(arg, "mute") != 0) return;
    int pct = 0, muted = 0;
    wp_get(T, &pct, &muted);
    wp_set_mute(T, "toggle");
    muted = !muted;
    osd_post(SLOT_MIC, muted ? "Microphone muted" : "Microphone", "",
             muted ? ICON_MIC_X : ICON_MIC, -1, 1, muted, OSD_TIMEOUT_OSD);
}

/* Cache the first /sys/class/backlight/<dev> path. Single-display assumption
 * matches wisp's single-output binding elsewhere. */
static const char *bl_dev(void) {
    static char dev[320];  /* /sys/class/backlight/ (21) + NAME_MAX (255) + slack */
    if (dev[0]) return dev;
    DIR *d = opendir("/sys/class/backlight");
    if (!d) return NULL;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(dev, sizeof dev, "/sys/class/backlight/%s", e->d_name);
        break;
    }
    closedir(d);
    return dev[0] ? dev : NULL;
}

static int read_int_file(const char *path) {
    int fd = open(path, O_RDONLY); if (fd < 0) return -1;
    char b[32]; int n = (int)read(fd, b, sizeof b - 1); close(fd);
    if (n <= 0) return -1;
    b[n] = 0; return atoi(b);
}
static int write_int_file(const char *path, int v) {
    int fd = open(path, O_WRONLY); if (fd < 0) return -1;
    char b[32]; int n = snprintf(b, sizeof b, "%d", v);
    int r = (int)write(fd, b, n); close(fd);
    return r < 0 ? -1 : 0;
}

void media_backlight(const char *arg) {
    int dir = !strcmp(arg, "up") ? 1 : !strcmp(arg, "down") ? -1 : 0;
    if (!dir) return;
    const char *dev = bl_dev(); if (!dev) return;
    char cur_p[360], max_p[360];
    snprintf(cur_p, sizeof cur_p, "%s/brightness", dev);
    snprintf(max_p, sizeof max_p, "%s/max_brightness", dev);
    int cur = read_int_file(cur_p), max = read_int_file(max_p);
    if (cur < 0 || max <= 0) return;
    int step = max * BRI_STEP / 100; if (step < 1) step = 1;
    int n = cur + dir * step;
    if (n < 0)   n = 0;
    if (n > max) n = max;
    write_int_file(cur_p, n);
    osd_post(SLOT_BRI, "Brightness", "", ICON_SUN, n * 100 / max, 1, 0, OSD_TIMEOUT_OSD);
}

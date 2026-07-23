/* media.c — volume / mic / backlight media keys. Replaces dwl-osd shell.
 *
 *   volume up|down|mute → pipewire() sink   + OSD slot 1
 *   mic mute            → pipewire() source + OSD slot 2
 *   backlight up|down   → /sys/class/backlight/<dev>/brightness + OSD slot 3
 *
 * Audio state is read/written live through the native-protocol pipewire client
 * (src/pipewire.c) — no wpctl fork, no poll, no cache: pw_vol_pct() et al. are
 * plain reads of the event-driven subscription. Backlight is a direct sysfs
 * write; brightnessctl ships a udev rule that makes that file group-writable by
 * `video`, which dwlarp users already need to be in. */

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

void media_volume(const char *arg) {
    if (!pw_ok()) return;              /* no default sink resolved → keys dead, loudly */
    int pct = pw_vol_pct(), muted = pw_vol_muted();
    if (!strcmp(arg, "mute")) {
        pw_set_mute(-1);
        muted = !muted;
    } else {
        int dir = !strcmp(arg, "up") ? 1 : !strcmp(arg, "down") ? -1 : 0;
        if (!dir) return;
        int n = pct + dir * VOL_STEP;
        if (n < 0) n = 0;
        if (n > VOL_CAP) n = VOL_CAP;
        pw_set_volume(n);              /* unmutes as a side effect */
        pct = n; muted = 0;
    }
    osd_post(SLOT_VOL, muted ? "Volume muted" : "Volume", "",
             muted ? ICON_VOL_X : ICON_VOL, pct, 1, muted, OSD_TIMEOUT_OSD);
}

void media_mic(const char *arg) {
    if (strcmp(arg, "mute") != 0) return;
    if (!pw_ok()) return;
    int muted = !pw_mic_muted();
    pw_set_mic_mute(-1);
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

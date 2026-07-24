/* System metric sampling (mirrors dwlb-status semantics, no libpulse). */
#include "wisp.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

Status status;

static char bat_dev[256];
static char cpu_temp_path[320];
static char net_iface[64];         /* "" = any iface (wireless: first entry) */
static char backlight_dev[256];    /* "" = first entry in /sys/class/backlight */
static char disk_path[256] = "/";
static long long prev_busy, prev_total;

void status_set_arg(const char *kind, const char *val) {
    if (!val || !val[0]) return;
    if (!strcmp(kind, "temp")) {
        /* absolute path used as-is; else a /sys/class/thermal zone name */
        if (val[0] == '/') snprintf(cpu_temp_path, sizeof cpu_temp_path, "%s", val);
        else snprintf(cpu_temp_path, sizeof cpu_temp_path, "/sys/class/thermal/%s/temp", val);
    } else if (!strcmp(kind, "bat"))  snprintf(bat_dev, sizeof bat_dev, "%s", val);
    else if (!strcmp(kind, "net"))    snprintf(net_iface, sizeof net_iface, "%s", val);
    else if (!strcmp(kind, "backlight")) snprintf(backlight_dev, sizeof backlight_dev, "%s", val);
    else if (!strcmp(kind, "disk"))   snprintf(disk_path, sizeof disk_path, "%s", val);
}

/* Low-battery edge-trigger latches: bit set = a fired notification has not yet
 * been cleared by charging/recovery. Prevents per-sample spam while keeping
 * the alert sticky enough to notice. */
static int bat_warn_fired;
static int bat_crit_fired;
/* Persistent notification ids so subsequent warn/crit posts replace in place
 * instead of stacking new slabs every battery sample. */
#define BAT_OSD_ID 0xb47b47fu

static int read_int_file(const char *p) {
    FILE *f = fopen(p, "r"); if (!f) return -1;
    int v = -1; if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f); return v;
}

static void sample_cpu(void) {
    FILE *f = fopen("/proc/stat", "r"); if (!f) return;
    long long u, n, s, idle, iow=0, irq=0, sirq=0, st=0, gu=0, gn=0;
    int got = fscanf(f, "cpu %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld",
                     &u,&n,&s,&idle,&iow,&irq,&sirq,&st,&gu,&gn);
    fclose(f);
    if (got < 4) return;
    long long busy  = u + n + s;
    long long total = busy + idle + iow + irq + sirq + st + gu + gn;
    long long db = busy - prev_busy, dt = total - prev_total;
    prev_busy = busy; prev_total = total;
    status.cpu_t10 = dt > 0 ? (int)(db * 1000 / dt) : 0;
}

static void detect_cpu_temp(void) {
    static int probed;
    if (cpu_temp_path[0] || probed) return;
    probed = 1;  /* one-shot: don't reopen /sys/class/hwmon if first probe failed */
    DIR *d = opendir("/sys/class/hwmon"); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char np[320], name[64] = "";
        snprintf(np, sizeof np, "/sys/class/hwmon/%s/name", e->d_name);
        FILE *f = fopen(np, "r"); if (!f) continue;
        if (!fgets(name, sizeof name, f)) { fclose(f); continue; }
        fclose(f);
        name[strcspn(name, "\n")] = 0;
        if (strcmp(name, "coretemp") != 0 && strcmp(name, "k10temp") != 0) continue;
        for (int i = 1; i <= 8; i++) {
            char lp[320], lab[64] = "";
            snprintf(lp, sizeof lp, "/sys/class/hwmon/%s/temp%d_label", e->d_name, i);
            FILE *lf = fopen(lp, "r"); if (!lf) continue;
            if (fgets(lab, sizeof lab, lf)) lab[strcspn(lab, "\n")] = 0;
            fclose(lf);
            if (strncmp(lab, "Package", 7) == 0 || strncmp(lab, "Tctl", 4) == 0) {
                snprintf(cpu_temp_path, sizeof cpu_temp_path,
                         "/sys/class/hwmon/%s/temp%d_input", e->d_name, i);
                break;
            }
        }
        if (!cpu_temp_path[0])
            snprintf(cpu_temp_path, sizeof cpu_temp_path,
                     "/sys/class/hwmon/%s/temp1_input", e->d_name);
        break;
    }
    closedir(d);
}

static void sample_cpu_temp(void) {
    detect_cpu_temp();
    if (!cpu_temp_path[0]) return;
    int milli = read_int_file(cpu_temp_path);
    if (milli < 0) return;
    status.cpu_temp = (milli + 500) / 1000;
}

static void sample_mem(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) { status.mem_used_kb = -1; return; }
    long total = -1, avail = -1;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "MemTotal:", 9))         sscanf(line + 9,  "%ld", &total);
        else if (!strncmp(line, "MemAvailable:", 13)) sscanf(line + 13, "%ld", &avail);
        if (total >= 0 && avail >= 0) break;
    }
    fclose(f);
    if (total < 0 || avail < 0) { status.mem_used_kb = -1; return; }
    long used = total - avail;
    status.mem_used_kb = used > 0 ? (int)used : 0;
}

static void sample_disk(void) {
    struct statvfs s;
    if (statvfs(disk_path, &s) || s.f_blocks == 0) return;
    unsigned long long total = (unsigned long long)s.f_blocks * s.f_frsize;
    unsigned long long used  = (unsigned long long)(s.f_blocks - s.f_bfree) * s.f_frsize;
    /* swap-file subtraction only makes sense for the fs the swapfile lives on */
    FILE *sw = strcmp(disk_path, "/") ? NULL : fopen("/proc/swaps", "re");
    if (sw) {
        char line[512];
        (void)!fgets(line, sizeof line, sw);
        while (fgets(line, sizeof line, sw)) {
            char path[256], type[32];
            unsigned long long size_kb;
            if (sscanf(line, "%255s %31s %llu", path, type, &size_kb) == 3
                && strcmp(type, "file") == 0) {
                unsigned long long b = size_kb * 1024ULL;
                if (b < used) used -= b; else used = 0;
            }
        }
        fclose(sw);
    }
    status.disk_pct = total ? (int)((used * 100) / total) : 0;
}

static void detect_bat(void) {
    if (bat_dev[0]) return;
    DIR *d = opendir("/sys/class/power_supply"); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "BAT", 3) == 0) {
            snprintf(bat_dev, sizeof bat_dev, "%s", e->d_name); break;
        }
    }
    closedir(d);
}

static void sample_bat(void) {
    detect_bat();
    if (!bat_dev[0]) { status.bat_pct = -1; return; }
    char path[384];
    snprintf(path, sizeof path, "/sys/class/power_supply/%s/capacity", bat_dev);
    int pct = read_int_file(path);
    if (pct < 0) { status.bat_pct = -1; return; }
    status.bat_pct = pct;
    snprintf(path, sizeof path, "/sys/class/power_supply/%s/status", bat_dev);
    FILE *f = fopen(path, "r");
    status.bat_charging = 0;
    if (f) {
        char st[32] = "";
        if (fgets(st, sizeof st, f))
            status.bat_charging = (strstr(st, "Charging") || strstr(st, "Full")) ? 1 : 0;
        fclose(f);
    }

    /* Charging or recovered above the warn threshold → reset latches so the
     * next discharge cycle re-arms the alert. */
    if (status.bat_charging || pct > BAT_WARN_PCT) {
#ifdef WISP_HAS_OSD
        if (bat_warn_fired || bat_crit_fired) osd_close(BAT_OSD_ID);
#endif
        bat_warn_fired = bat_crit_fired = 0;
        return;
    }

#ifdef WISP_HAS_OSD
    char body[64];
    if (pct <= BAT_CRIT_PCT && !bat_crit_fired) {
        snprintf(body, sizeof body, "%d%% remaining — plug in now", pct);
        osd_post(BAT_OSD_ID, "Battery critical", body, 0xf244, -1, 2, 1, 0);
        bat_crit_fired = bat_warn_fired = 1;
    } else if (pct <= BAT_WARN_PCT && !bat_warn_fired) {
        snprintf(body, sizeof body, "%d%% remaining", pct);
        osd_post(BAT_OSD_ID, "Battery low", body, 0xf243, -1, 1, 2, 0);
        bat_warn_fired = 1;
    }
#endif
}

void status_sample_vpn(void) {
    FILE *f = fopen("/run/mullvad.handshake", "r");
    if (!f) {
        status.vpn_state = access("/sys/class/net/mullvad", 0) == 0 ? 1 : 0;
        return;
    }
    long long hs = 0;
    if (fscanf(f, "%lld", &hs) != 1) hs = 0;
    fclose(f);
    if (hs <= 0) { status.vpn_state = 0; return; }
    long long now = (long long)time(NULL);
    status.vpn_state = (now - hs <= VPN_STALE_S) ? 1 : 2;
}

void status_sample_net(void) {
    FILE *f = fopen("/proc/net/wireless", "r");
    status.wifi_level = -1;
    if (f) {
        char buf[256]; int line = 0;
        while (fgets(buf, sizeof buf, f)) {
            if (++line < 3) continue;
            char *p = strchr(buf, ':'); if (!p) continue;
            if (net_iface[0]) {
                char *t = buf; while (*t == ' ') t++;
                if ((size_t)(p - t) != strlen(net_iface) ||
                    strncmp(t, net_iface, (size_t)(p - t))) continue;
            }
            int st_, link;
            if (sscanf(p+1, " %d %d", &st_, &link) != 2) continue;
            (void)st_;
            if (link <= 0)        status.wifi_level = -1;
            else if (link >= 55)  status.wifi_level = 3;
            else if (link >= 40)  status.wifi_level = 2;
            else if (link >= 25)  status.wifi_level = 1;
            else                  status.wifi_level = 0;
            break;
        }
        fclose(f);
    }
    /* up = a default route exists (Destination 00000000). Wired-only boxes
     * have wifi_level -1 but net_up 1 — a widget shows the ethernet glyph. */
    status.net_up = 0;
    f = fopen("/proc/net/route", "r");
    if (!f) return;
    char line[256];
    (void)!fgets(line, sizeof line, f);            /* header */
    while (fgets(line, sizeof line, f)) {
        char ifn[64]; unsigned long dest;
        if (sscanf(line, "%63s %lx", ifn, &dest) != 2 || dest != 0) continue;
        if (net_iface[0] && strcmp(ifn, net_iface)) continue;
        status.net_up = 1;
        break;
    }
    fclose(f);
}

#ifdef WISP_HAS_NET_RATES
/* rx/tx KB/s since the previous call, summed over matching non-lo ifaces.
 * Divides by real elapsed time so an every=-slowed tick still reads right. */
static void sample_net_rates(void) {
    static unsigned long long prev_rx, prev_tx;
    static long long prev_ms;
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return;
    char line[512];
    unsigned long long rx = 0, tx = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = strchr(line, ':'); if (!p) continue;
        char *t = line; while (*t == ' ') t++;
        *p = 0;
        if (!strcmp(t, "lo")) continue;
        if (net_iface[0] && strcmp(t, net_iface)) continue;
        unsigned long long r, s;
        /* rx bytes is field 1 after ':'; tx bytes is field 9. */
        if (sscanf(p + 1, " %llu %*u %*u %*u %*u %*u %*u %*u %llu", &r, &s) != 2)
            continue;
        rx += r; tx += s;
    }
    fclose(f);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long now = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (prev_ms && now > prev_ms && rx >= prev_rx && tx >= prev_tx) {
        long long dt = now - prev_ms;
        status.net_rx_kbps = (int)((rx - prev_rx) * 1000 / (unsigned long long)dt / 1024);
        status.net_tx_kbps = (int)((tx - prev_tx) * 1000 / (unsigned long long)dt / 1024);
    }
    prev_rx = rx; prev_tx = tx; prev_ms = now;
}
#endif

static void detect_backlight(void) {
    static int probed;
    if (backlight_dev[0] || probed) return;
    probed = 1;
    DIR *d = opendir("/sys/class/backlight"); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(backlight_dev, sizeof backlight_dev, "%s", e->d_name);
        break;
    }
    closedir(d);
}

void status_sample_backlight(void) {
    detect_backlight();
    if (!backlight_dev[0]) { status.backlight_pct = -1; return; }
    char path[320];
    snprintf(path, sizeof path, "/sys/class/backlight/%s/brightness", backlight_dev);
    int cur = read_int_file(path);
    snprintf(path, sizeof path, "/sys/class/backlight/%s/max_brightness", backlight_dev);
    int max = read_int_file(path);
    status.backlight_pct = (cur < 0 || max <= 0) ? -1 : (cur * 100 + max / 2) / max;
}

void status_init(void) {
    status.cpu_temp = -1;
    status.bat_pct  = -1;
    status.mem_used_kb = -1;
    status.wifi_level = -1;
    status.backlight_pct = -1;
}

void status_sample_all(void) {
    sample_cpu(); sample_cpu_temp(); sample_mem(); sample_disk();
    sample_bat(); status_sample_vpn(); status_sample_net();
    status_sample_backlight();
#ifdef WISP_HAS_NET_RATES
    sample_net_rates();                    /* prime the delta baseline */
#endif
}

void status_sample_bat(void) { sample_bat(); }
void status_sample_disk(void) { sample_disk(); }

/* vpn/net-link/bat/disk/backlight deliberately absent: rtnetlink drives vpn +
 * net up/ssid, the slow signal timer refreshes wifi signal, bat and backlight
 * ride kernel uevents (bat also a slow fallback timer), disk its own slow
 * timer. Only cpu/mem/temp — and net rates when a config reads them — ride
 * this shared tick (1 Hz by default; `every=` slows it). */
void status_tick(int tick_n) {
    sample_cpu(); sample_mem();
    if (tick_n % STATUS_CADENCE_TEMP == 0) sample_cpu_temp();
#ifdef WISP_HAS_NET_RATES
    sample_net_rates();
#endif
}

/* Apps — built-in launcher index behind `wispctl apps`. Scans XDG desktop
 * entries into a tiny TSV cache (~/.cache/wisp-apps.tsv) keyed by the summed
 * mtimes of the applications/ dirs. Open path: load cache → menu is up
 * immediately; only if a dir mtime changed do we rescan, after the surface
 * is already committed (the scan overlaps the compositor map round-trip, so
 * it reads as instant). Usage counts live in the cache and drive most-used
 * sorting (MENU_SORT_FREQ; `sort = "alphabetical"` in the DSL flips it).
 * All state is heap-allocated per invocation and freed when the menu closes
 * — the launcher costs zero RSS while idle. */

#include "wisp.h"
#include "image.h"

#ifdef WISP_HAS_MENU

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define APP_CAP   MAX_ITEMS   /* menu ceiling; ponytail: >256 visible apps get truncated */
#define EXEC_MAX  256         /* flatpak Exec lines overflow ITEM_MAX (160) */
#define ICON_MAX  128         /* Icon= value: theme name or absolute path */

typedef struct {
    char name[ITEM_MAX];
    char exec[EXEC_MAX];
    char icon[ICON_MAX];
    int  count;
} App;

/* Truncating copy; snprintf "%s" would trip -Wformat-truncation. */
static void scpy(char *dst, size_t sz, const char *src) {
    size_t l = strnlen(src, sz - 1);
    memcpy(dst, src, l); dst[l] = 0;
}

static App *apps;
static int *ranks;            /* usage counts in menu item order, for search tiebreak */
static uint32_t **icons;      /* decoded premultiplied icons, menu item order */
static int  icon_sz;
static int  n_apps;
static long disk_stamp;

/* XDG data dirs, highest precedence first. */
static int data_dirs(char out[][256], int cap) {
    char buf[1024];
    int n = 0;
    const char *dh = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (dh)
        snprintf(out[n++], 256, "%.240s", dh);
    else if (home)
        snprintf(out[n++], 256, "%.220s/.local/share", home);
    const char *dd = getenv("XDG_DATA_DIRS");
    snprintf(buf, sizeof buf, "%s", dd && dd[0] ? dd : "/usr/local/share:/usr/share");
    for (char *p = buf, *tok; n < cap && (tok = strsep(&p, ":")); )
        if (tok[0]) snprintf(out[n++], 256, "%.240s", tok);
    return n;
}

static int app_dirs(char out[][256], int cap) {
    int n = data_dirs(out, cap);
    for (int i = 0; i < n; i++) {
        size_t l = strlen(out[i]);
        snprintf(out[i] + l, 256 - l, "/applications");
    }
    return n;
}

static long stamp_dirs(void) {
    char dirs[8][256];
    int nd = app_dirs(dirs, 8);
    long s = 0;
    struct stat st;
    for (int i = 0; i < nd; i++)
        if (stat(dirs[i], &st) == 0) s += (long)st.st_mtime + st.st_size;
    return s;
}

/* Parse one .desktop file's [Desktop Entry] section. Returns 1 if it is a
 * visible launchable application. */
static int parse_desktop(const char *path, App *a) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512], exec[EXEC_MAX] = "";
    int in_entry = 0, is_app = 0, hidden = 0, terminal = 0;
    a->name[0] = 0;
    a->icon[0] = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '[') {
            if (in_entry) break;   /* next section: done */
            in_entry = strncmp(line, "[Desktop Entry]", 15) == 0;
            continue;
        }
        if (!in_entry) continue;
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        if      (!strncmp(line, "Name=", 5) && !a->name[0])
            scpy(a->name, sizeof a->name, line + 5);
        else if (!strncmp(line, "Exec=", 5))
            scpy(exec, sizeof exec, line + 5);
        else if (!strncmp(line, "Icon=", 5) && !a->icon[0])
            scpy(a->icon, sizeof a->icon, line + 5);
        else if (!strncmp(line, "Type=", 5))
            is_app = strcmp(line + 5, "Application") == 0;
        else if (!strncmp(line, "NoDisplay=true", 14) || !strncmp(line, "Hidden=true", 11))
            hidden = 1;
        else if (!strncmp(line, "Terminal=true", 13))
            terminal = 1;
    }
    fclose(f);
    if (!is_app || hidden || !a->name[0] || !exec[0]) return 0;
    /* strip %f/%F/%u/%U/… field codes; "%%" → "%" */
    char clean[EXEC_MAX - 16];   /* leave room for the "foot -e " prefix */
    int o = 0;
    for (const char *p = exec; *p && o < EXEC_MAX - 1; p++) {
        if (*p == '%') {
            if (p[1] == '%') { clean[o++] = '%'; p++; }
            else if (p[1])   p++;
            continue;
        }
        clean[o++] = *p;
    }
    while (o > 0 && clean[o-1] == ' ') o--;
    clean[o] = 0;
    if (terminal) {
        /* ponytail: terminal hardcoded to foot — it's this rice's terminal */
        memcpy(a->exec, "foot -e ", 8);
        scpy(a->exec + 8, sizeof a->exec - 8, clean);
    } else
        scpy(a->exec, sizeof a->exec, clean);
    a->count = 0;
    return 1;
}

/* Full disk scan. Dedup by Name, earlier (higher-precedence) dirs win;
 * usage counts are carried over from whatever was in apps[] before. */
static void scan(void) {
    App *old = apps;
    int n_old = n_apps;
    App *fresh = calloc(APP_CAP, sizeof *fresh);
    if (!fresh) return;
    int n = 0;
    char dirs[8][256];
    int nd = app_dirs(dirs, 8);
    for (int d = 0; d < nd; d++) {
        DIR *dp = opendir(dirs[d]);
        if (!dp) continue;
        struct dirent *e;
        while (n < APP_CAP && (e = readdir(dp))) {
            size_t l = strlen(e->d_name);
            if (l < 9 || strcmp(e->d_name + l - 8, ".desktop")) continue;
            char path[600];
            if (snprintf(path, sizeof path, "%.256s/%.256s", dirs[d], e->d_name)
                >= (int)sizeof path) continue;
            if (!parse_desktop(path, &fresh[n])) continue;
            int dup = 0;
            for (int i = 0; i < n && !dup; i++)
                dup = strcmp(fresh[i].name, fresh[n].name) == 0;
            if (!dup) n++;
        }
        closedir(dp);
    }
    for (int i = 0; i < n && old; i++)
        for (int j = 0; j < n_old; j++)
            if (!strcmp(fresh[i].name, old[j].name)) { fresh[i].count = old[j].count; break; }
    free(old);
    apps = fresh;
    n_apps = n;
}

static void cache_path(char *out, size_t sz) {
    const char *c = getenv("XDG_CACHE_HOME");
    if (c) snprintf(out, sz, "%s/wisp-apps.tsv", c);
    else   snprintf(out, sz, "%s/.cache/wisp-apps.tsv", getenv("HOME") ? getenv("HOME") : ".");
}

static int load_cache(void) {
    char path[512], line[600];
    cache_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    long stamp = 0;
    int ver = 0;
    if (!fgets(line, sizeof line, f) || sscanf(line, "wispapps%d\t%ld", &ver, &stamp) != 2
        || ver < 1 || ver > 2) {
        fclose(f); return 0;
    }
    int n = 0;
    while (n < APP_CAP && fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        char *t1 = strchr(line, '\t');       if (!t1) continue;
        char *t2 = strchr(t1 + 1, '\t');     if (!t2) continue;
        char *t3 = ver >= 2 ? strchr(t2 + 1, '\t') : NULL;
        if (ver >= 2 && !t3) continue;
        *t1 = *t2 = 0;
        if (t3) *t3 = 0;
        apps[n].count = atoi(line);
        scpy(apps[n].name, ITEM_MAX, t1 + 1);
        if (t3) {   /* v2: count name icon exec */
            scpy(apps[n].icon, ICON_MAX, t2 + 1);
            scpy(apps[n].exec, EXEC_MAX, t3 + 1);
        } else {    /* v1: count name exec */
            apps[n].icon[0] = 0;
            scpy(apps[n].exec, EXEC_MAX, t2 + 1);
        }
        n++;
    }
    fclose(f);
    if (!n) return 0;
    n_apps = n;
    disk_stamp = stamp;
    return 1;
}

static void save_cache(void) {
    char path[512], tmp[520];
    cache_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "wispapps2\t%ld\n", disk_stamp);
    for (int i = 0; i < n_apps; i++)
        fprintf(f, "%d\t%s\t%s\t%s\n", apps[i].count, apps[i].name,
                apps[i].icon, apps[i].exec);
    fclose(f);
    rename(tmp, path);
}

/* Resolve an Icon= value to a PNG path. Absolute paths pass through; names
 * are looked up in hicolor apps dirs (preferred sizes first) and pixmaps.
 * ponytail: no theme-index parsing, no SVG — hicolor+pixmaps PNGs cover the
 * installed apps here; extend to the full icon-theme spec if misses annoy. */
static int resolve_icon(const char *name, char *out, size_t sz) {
    if (!name[0]) return 0;
    struct stat st;
    if (name[0] == '/') {
        if (stat(name, &st) == 0) { scpy(out, sz, name); return 1; }
        return 0;
    }
    char dirs[8][256];
    int nd = data_dirs(dirs, 8);
    static const int sizes[] = { 48, 64, 32, 128, 256 };
    for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++)
        for (int d = 0; d < nd; d++) {
            snprintf(out, sz, "%.240s/icons/hicolor/%dx%d/apps/%.100s.png",
                     dirs[d], sizes[si], sizes[si], name);
            if (stat(out, &st) == 0) return 1;
        }
    snprintf(out, sz, "/usr/share/pixmaps/%.100s.png", name);
    return stat(out, &st) == 0;
}

/* Box-filter downscale RGBA8 → premultiplied ARGB ds×ds. Averaging happens
 * premultiplied, which is the correct filter for translucent edges. */
static uint32_t *icon_scale(const uint8_t *rgba, int sw, int sh, int ds) {
    uint32_t *out = malloc((size_t)ds * ds * 4);
    if (!out) return NULL;
    for (int j = 0; j < ds; j++) {
        int y0 = j * sh / ds, y1 = (j + 1) * sh / ds; if (y1 <= y0) y1 = y0 + 1;
        for (int i = 0; i < ds; i++) {
            int x0 = i * sw / ds, x1 = (i + 1) * sw / ds; if (x1 <= x0) x1 = x0 + 1;
            uint32_t a = 0, r = 0, g = 0, b = 0, cnt = (uint32_t)((x1 - x0) * (y1 - y0));
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    const uint8_t *px = rgba + 4 * ((size_t)y * sw + x);
                    uint32_t pa = px[3];
                    a += pa;
                    r += px[0] * pa / 255;
                    g += px[1] * pa / 255;
                    b += px[2] * pa / 255;
                }
            out[j * ds + i] = (a / cnt) << 24 | (r / cnt) << 16
                            | (g / cnt) << 8 | (b / cnt);
        }
    }
    return out;
}

static void icons_free(void) {
    if (!icons) return;
    for (int i = 0; i < APP_CAP; i++) free(icons[i]);
    free(icons);
    icons = NULL;
}

/* Decode all app icons in current apps[] order. */
static void load_icons(void) {
    icons_free();
    icons = calloc(APP_CAP, sizeof *icons);
    if (!icons) return;
    icon_sz = menu_icon_px();
    char path[512];
    for (int i = 0; i < n_apps; i++) {
        if (!resolve_icon(apps[i].icon, path, sizeof path)) continue;
        int w, h;
        uint8_t *px = image_load(path, &w, &h);
        if (!px) continue;
        icons[i] = icon_scale(px, w, h, icon_sz);
        image_free(px);
    }
}

static int cmp_app(const void *pa, const void *pb) {
    const App *a = pa, *b = pb;
    if (MENU_SORT_FREQ && a->count != b->count) return b->count - a->count;
    return strcasecmp(a->name, b->name);
}

static void apps_free(void) {
    free(apps);  apps  = NULL;  n_apps = 0;
    free(ranks); ranks = NULL;
    icons_free();
}

static void on_pick(int idx) {
    if (idx >= 0 && idx < n_apps) {
        apps[idx].count++;
        save_cache();
        spawn_detached(apps[idx].exec);
    }
    apps_free();
}

/* Sort apps and push (items, ranks) into the given live menu (or create it). */
static Widget *apps_push(Widget *w) {
    qsort(apps, (size_t)n_apps, sizeof *apps, cmp_app);
    char (*items)[ITEM_MAX] = calloc((size_t)n_apps, ITEM_MAX);
    if (!items) return w;
    for (int i = 0; i < n_apps; i++) {
        memcpy(items[i], apps[i].name, ITEM_MAX);
        ranks[i] = apps[i].count;
    }
    if (w) menu_update_items(w, items, n_apps);
    else   w = menu_create(NULL, items, n_apps, -1);
    free(items);
    if (w) {
        menu_set_ranks(w, ranks);
        load_icons();
        menu_set_icons(w, icons, icon_sz);
    }
    return w;
}

Widget *apps_open(void) {
    apps_free();
    apps  = calloc(APP_CAP, sizeof *apps);
    ranks = calloc(APP_CAP, sizeof *ranks);
    if (!apps || !ranks) { apps_free(); return NULL; }
    long now_stamp = stamp_dirs();
    int cached = load_cache();
    if (!cached) { scan(); disk_stamp = now_stamp; save_cache(); }
    Widget *w = apps_push(NULL);
    if (!w) { apps_free(); return NULL; }
    menu_set_pick_hook(on_pick);
    if (cached && disk_stamp != now_stamp) {
        /* Stale cache: the menu surface is already committed above, so this
         * synchronous rescan hides behind the map round-trip. */
        scan();
        disk_stamp = now_stamp;
        save_cache();
        apps_push(w);
    }
    return w;
}

#endif /* WISP_HAS_MENU */

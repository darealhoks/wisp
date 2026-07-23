/* PNG decode + cover-fit blit shared by wall.c and lock.c. See image.h.
 * Extracted from wall.c so the lock can reuse the exact same loader without
 * dragging in the wallpaper widget. */

#include "image.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* stb_image config: PNG-only, in-memory only, no string error tables. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_FAILURE_STRINGS
#include <stb/stb_image.h>

static char *expand_home(const char *p) {
    if (p[0] != '~' || p[1] != '/') return strdup(p);
    const char *home = getenv("HOME"); if (!home) home = "";
    size_t hl = strlen(home), pl = strlen(p);
    char *r = malloc(hl + pl);
    memcpy(r, home, hl); memcpy(r + hl, p + 1, pl - 1 + 1);
    return r;
}

static uint8_t *slurp(const char *path, int *len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0 || st.st_size > (1 << 26)) {
        close(fd); return NULL;  /* >64MB PNG is almost certainly a mistake */
    }
    uint8_t *buf = malloc(st.st_size);
    if (!buf) { close(fd); return NULL; }
    int got = 0;
    while (got < st.st_size) {
        ssize_t r = read(fd, buf + got, st.st_size - got);
        if (r <= 0) { free(buf); close(fd); return NULL; }
        got += r;
    }
    close(fd);
    *len = (int)st.st_size;
    return buf;
}

uint8_t *image_load(const char *path, int *w, int *h) {
    char *real = expand_home(path);
    int flen = 0;
    uint8_t *fbuf = slurp(real, &flen);
    free(real);
    if (!fbuf) return NULL;
    uint8_t *px = image_decode_png(fbuf, flen, w, h);
    free(fbuf);
    return px;
}

/* Resolve a freedesktop icon *name* to a PNG path. Absolute paths pass
 * through; names are looked up under `extra` (an app-supplied theme dir, may
 * be NULL) and then the XDG data dirs' hicolor apps/ dirs and /usr/share/pixmaps.
 * ponytail: no theme-index parsing, no SVG — hicolor+pixmaps PNGs cover the
 * installed apps here; extend to the full icon-theme spec if misses annoy. */
int image_find_icon(const char *name, const char *extra, char *out, size_t sz) {
    if (!name || !name[0]) return 0;
    struct stat st;
    if (name[0] == '/') {
        if (stat(name, &st) < 0) return 0;
        snprintf(out, sz, "%s", name);
        return 1;
    }
    static const int sizes[] = { 48, 64, 32, 128, 256, 24, 16 };
    if (extra && extra[0]) {
        snprintf(out, sz, "%.200s/%.100s.png", extra, name);
        if (stat(out, &st) == 0) return 1;
        for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
            snprintf(out, sz, "%.160s/hicolor/%dx%d/apps/%.100s.png",
                     extra, sizes[si], sizes[si], name);
            if (stat(out, &st) == 0) return 1;
        }
    }
    char dirs[8][256];
    int nd = 0;
    char buf[1024];
    const char *dh = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (dh)        snprintf(dirs[nd++], 256, "%.240s", dh);
    else if (home) snprintf(dirs[nd++], 256, "%.220s/.local/share", home);
    const char *dd = getenv("XDG_DATA_DIRS");
    snprintf(buf, sizeof buf, "%s", dd && dd[0] ? dd : "/usr/local/share:/usr/share");
    for (char *p = buf, *tok; nd < 8 && (tok = strsep(&p, ":")); )
        if (tok[0]) snprintf(dirs[nd++], 256, "%.240s", tok);
    for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++)
        for (int d = 0; d < nd; d++) {
            snprintf(out, sz, "%.160s/icons/hicolor/%dx%d/apps/%.100s.png",
                     dirs[d], sizes[si], sizes[si], name);
            if (stat(out, &st) == 0) return 1;
        }
    snprintf(out, sz, "/usr/share/pixmaps/%.100s.png", name);
    return stat(out, &st) == 0;
}

uint8_t *image_decode_png(const uint8_t *buf, int len, int *w, int *h) {
    int c = 0;
    return stbi_load_from_memory(buf, len, w, h, &c, 4);
}

void image_free(uint8_t *px) { stbi_image_free(px); }

/* Is `path` a readable PNG? (signature only — stb is built STBI_ONLY_PNG, so
 * anything else fails the real decode later; this makes `wispctl wall` err
 * synchronously instead of "ok" followed by a silent no-op). */
int image_is_png(const char *path) {
    static const uint8_t sig[8] = {0x89,'P','N','G','\r','\n',0x1a,'\n'};
    char *real = expand_home(path);
    int fd = open(real, O_RDONLY | O_CLOEXEC);
    free(real);
    if (fd < 0) return 0;
    uint8_t hdr[8];
    int ok = read(fd, hdr, 8) == 8 && !memcmp(hdr, sig, 8);
    close(fd);
    return ok;
}

int image_mtime(const char *path, int64_t *mtime) {
    char *real = expand_home(path);
    struct stat st;
    int r = stat(real, &st);
    free(real);
    if (r < 0) return -1;
    *mtime = (int64_t)st.st_mtime;
    return 0;
}

/* --- lock background disk cache (see image.h) --------------------------- */

#define BGCACHE_MAGIC 0x47424c57u   /* 'WLBG' */
#define BGCACHE_VER   2u
typedef struct {
    uint32_t magic, ver;
    int32_t  w, h;
    int64_t  mtime;
} BgHdr;

static int bgcache_path(char *out, size_t n, int W, int H) {
    const char *base = getenv("XDG_CACHE_HOME");
    char fall[512];
    if (!base || !base[0]) {
        const char *home = getenv("HOME");
        if (!home || !home[0]) return -1;
        snprintf(fall, sizeof fall, "%s/.cache", home);
        base = fall;
    }
    char dir[640];
    snprintf(dir, sizeof dir, "%s/wisp", base);
    mkdir(dir, 0700);                 /* best-effort; EEXIST is fine */
    snprintf(out, n, "%s/lockbg-%dx%d.bin", dir, W, H);
    return 0;
}

/* Read the header only; 1 if the cached copy matches (W,H,mtime). */
static int bgcache_hdr_ok(const char *path, int W, int H, int64_t mt) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    BgHdr h;
    int ok = read(fd, &h, sizeof h) == (ssize_t)sizeof h &&
             h.magic == BGCACHE_MAGIC && h.ver == BGCACHE_VER &&
             h.w == W && h.h == H && h.mtime == mt;
    close(fd);
    return ok;
}

uint32_t *image_bgcache_load(const char *img_path, int W, int H) {
    int64_t mt;
    if (image_mtime(img_path, &mt) < 0) return NULL;
    char path[768];
    if (bgcache_path(path, sizeof path, W, H) < 0) return NULL;
    if (!bgcache_hdr_ok(path, W, H, mt)) return NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    size_t bytes = (size_t)W * H * 4;
    uint32_t *bg = malloc(bytes);
    if (bg) {
        if (lseek(fd, sizeof(BgHdr), SEEK_SET) < 0) { free(bg); bg = NULL; }
        size_t got = 0;
        while (bg && got < bytes) {
            ssize_t r = read(fd, (char *)bg + got, bytes - got);
            if (r <= 0) { free(bg); bg = NULL; break; }
            got += r;
        }
    }
    close(fd);
    return bg;
}

void image_bgcache_store(const char *img_path, int W, int H, const uint32_t *px) {
    int64_t mt;
    if (image_mtime(img_path, &mt) < 0) return;
    char path[768];
    if (bgcache_path(path, sizeof path, W, H) < 0) return;
    if (bgcache_hdr_ok(path, W, H, mt)) return;   /* already current */
    char tmp[800];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return;
    BgHdr h = { BGCACHE_MAGIC, BGCACHE_VER, W, H, mt };
    size_t bytes = (size_t)W * H * 4;
    int ok = write(fd, &h, sizeof h) == (ssize_t)sizeof h;
    size_t put = 0;
    while (ok && put < bytes) {
        ssize_t r = write(fd, (const char *)px + put, bytes - put);
        if (r <= 0) { ok = 0; break; }
        put += r;
    }
    close(fd);
    if (ok) rename(tmp, path); else unlink(tmp);   /* atomic swap on success */
}

/* Bilinear cover-fit. src is RGBA (stbi); dst is ARGB8888 little-endian
 * (== 0xAARRGGBB). Fixed-point Q16 sample step for the inner loop;
 * float-only on the per-row setup. When base_a/grad_a are non-zero, the dim
 * toward `tint` is folded into the same pass so the lock needs no second
 * full-buffer scrim sweep (the dominant secondary cost on big monitors). */
void image_blit_cover_dim(uint32_t *dst, int dw, int dh,
                          const uint8_t *src, int sw, int sh,
                          uint32_t tint, int base_a, int grad_a) {
    double fx = (double)dw / sw, fy = (double)dh / sh;
    double scale = fx > fy ? fx : fy;
    double crop_x = (sw * scale - dw) / 2.0;
    double crop_y = (sh * scale - dh) / 2.0;

    int tr = (tint >> 16) & 0xff, tg = (tint >> 8) & 0xff, tb = tint & 0xff;

    /* Q16 source step per output pixel = 1/scale * 65536. */
    uint32_t step = (uint32_t)((65536.0 / scale) + 0.5);
    uint32_t base_x = (uint32_t)((crop_x / scale) * 65536.0 + 0.5);

    for (int oy = 0; oy < dh; oy++) {
        uint32_t syq = (uint32_t)(((oy + crop_y) / scale) * 65536.0 + 0.5);
        int iy = syq >> 16;
        uint32_t ty = syq & 0xffff;
        if (iy >= sh - 1) { iy = sh - 2; ty = 0xffff; }
        const uint8_t *r0 = src + (size_t)iy * sw * 4;
        const uint8_t *r1 = r0 + (size_t)sw * 4;

        /* Per-row scrim alpha: base everywhere + smoothstep ramp over the
         * bottom half. Computed once per row, not per pixel. */
        int a = base_a;
        if (grad_a) {
            double f = dh > 1 ? (double)oy / (dh - 1) : 0.0;
            double t = f < 0.5 ? 0.0 : (f - 0.5) / 0.5;
            t = t * t * (3.0 - 2.0 * t);
            a += (int)(grad_a * t);
        }
        if (a > 255) a = 255;
        int ia = 255 - a;

        uint32_t sxq = base_x;
        for (int ox = 0; ox < dw; ox++, sxq += step) {
            int ix = sxq >> 16;
            uint32_t tx = sxq & 0xffff;
            if (ix >= sw - 1) { ix = sw - 2; tx = 0xffff; }
            const uint8_t *p00 = r0 + ix * 4;
            const uint8_t *p10 = p00 + 4;
            const uint8_t *p01 = r1 + ix * 4;
            const uint8_t *p11 = p01 + 4;
            /* Bilinear weights in Q16. uint64 because (0x10000 * 0x10000)
             * overflows uint32 — happens whenever tx or ty is 0, which is
             * every pixel at scale=1.0. */
            uint64_t wa = 0x10000u - tx, b_ = 0x10000u - ty;
            uint32_t w00 = (uint32_t)((wa      * b_) >> 16);
            uint32_t w10 = (uint32_t)(((uint64_t)tx * b_) >> 16);
            uint32_t w01 = (uint32_t)((wa      * (uint64_t)ty) >> 16);
            uint32_t w11 = (uint32_t)(((uint64_t)tx * ty) >> 16);
            uint32_t r = (p00[0]*w00 + p10[0]*w10 + p01[0]*w01 + p11[0]*w11) >> 16;
            uint32_t g = (p00[1]*w00 + p10[1]*w10 + p01[1]*w01 + p11[1]*w11) >> 16;
            uint32_t b = (p00[2]*w00 + p10[2]*w10 + p01[2]*w01 + p11[2]*w11) >> 16;
            if (a) {
                r = (r * ia + tr * a) / 255;
                g = (g * ia + tg * a) / 255;
                b = (b * ia + tb * a) / 255;
            }
            dst[oy * dw + ox] = 0xff000000u | (r << 16) | (g << 8) | b;
        }
    }
}

void image_blit_cover(uint32_t *dst, int dw, int dh,
                      const uint8_t *src, int sw, int sh) {
    image_blit_cover_dim(dst, dw, dh, src, sw, sh, 0, 0, 0);
}

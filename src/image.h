/* Shared PNG decode + cover-fit blit. Used by the wallpaper (wall.c) and the
 * session lock (lock.c), which renders the same wallpaper behind the prompt.
 * Self-contained: stb_image is compiled into image.c only, so neither the
 * daemon nor the lock binary grows a libpng dependency. */
#pragma once

#include <stdint.h>

/* Decode a PNG file at `path` (a leading "~/" is expanded against $HOME) into
 * a freshly malloc'd RGBA8 buffer. Returns the pixels and sets *w,*h, or NULL
 * on any failure (missing file, >64 MB, decode error). Free with image_free. */
uint8_t *image_load(const char *path, int *w, int *h);
void     image_free(uint8_t *px);

/* Last-modified time of `path` (leading "~/" expanded), seconds since epoch,
 * for cache-invalidation keys. Returns 0 on success, -1 if the file is gone. */
int      image_mtime(const char *path, int64_t *mtime);

/* Bilinear cover-fit: scale `src` (RGBA8, sw*sh) to fill dst (ARGB8888,
 * dw*dh) at max scale with a centred crop. Output pixels are opaque. */
void image_blit_cover(uint32_t *dst, int dw, int dh,
                      const uint8_t *src, int sw, int sh);

/* Disk cache of a finished W*H cover-fit of `img_path`, keyed by size + the
 * image's mtime, at $XDG_CACHE_HOME/wisp/lockbg-WxH.bin. The daemon seeds it
 * from wall.c's render so wisp-lock's first load skips the decode + scale.
 * Load returns a malloc'd ARGB buffer or NULL; store is a no-op when the
 * cached copy is already current. */
uint32_t *image_bgcache_load(const char *img_path, int W, int H);
void      image_bgcache_store(const char *img_path, int W, int H,
                              const uint32_t *px);

/* Same cover-fit, but dim each pixel toward `tint` (0xRRGGBB) as it's written,
 * folding a scrim into the single scaling pass. `base_a` (0..255) is applied
 * everywhere; `grad_a` is added on top, ramped 0→1 by smoothstep over the
 * bottom half of the image. base_a==grad_a==0 reduces to image_blit_cover. */
void image_blit_cover_dim(uint32_t *dst, int dw, int dh,
                          const uint8_t *src, int sw, int sh,
                          uint32_t tint, int base_a, int grad_a);

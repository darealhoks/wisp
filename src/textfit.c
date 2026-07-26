/* Fitting text to a pixel budget: ellipsis truncation and word wrap. Both the
 * generated widget path (`elide;` / `wrap;`) and osd.c's body layout call in
 * here — the wrap used to be private to osd.c. */
#include "wisp.h"
#include <string.h>

/* Byte count of the longest prefix of `s` that fits in `budget` px. Cuts on a
 * codepoint boundary — a split UTF-8 sequence renders as replacement junk. */
static int fit_prefix(const Font *f, const char *s, int budget) {
    int n = 0, w = 0;
    while (s[n]) {
        uint32_t cp; int k = utf8_decode(s + n, &cp);
        if (!k) break;
        const Glyph *g = font_find(f, cp);
        int gw = g ? g->adv : f->px_size / 2;
        if (w + gw > budget) break;
        n += k; w += gw;
    }
    return n;
}

void draw_text_elided(uint32_t *px, int sw, int sh, int x, int y,
                      const Font *f, const char *s, int max_w, uint32_t fg) {
    if (max_w <= 0) return;
    if (text_width(f, s) <= max_w) { draw_text(px, sw, sh, x, y, f, s, fg); return; }
    int budget = max_w - text_width(f, "\xe2\x80\xa6");
    if (budget < 0) budget = 0;
    int n = fit_prefix(f, s, budget);
    char buf[288];
    if (n > (int)sizeof buf - 4) n = (int)sizeof buf - 4;
    memcpy(buf, s, n);
    memcpy(buf + n, "\xe2\x80\xa6", 3);
    buf[n + 3] = 0;
    draw_text(px, sw, sh, x, y, f, buf, fg);
}

/* Append one wrapped line to `out`; returns 0 when the buffer is full. */
static int push_line(char *out, int out_sz, int *n, int nl, const char *s, int len) {
    if (*n + nl + len + 1 > out_sz) return 0;
    if (nl) out[(*n)++] = '\n';
    memcpy(out + *n, s, len);
    *n += len;
    out[*n] = 0;
    return 1;
}

/* Ellipsise the last line already in `out` so it fits `max_w`. */
static void ellipsise_tail(const Font *f, char *out, int *n, int out_sz, int max_w) {
    char *last = out + *n;
    while (last > out && last[-1] != '\n') last--;
    int ell_w = text_width(f, "\xe2\x80\xa6");
    while (text_width(f, last) + ell_w > max_w) {
        int len = (int)strlen(last);
        if (!len) break;
        int back = 1;
        while (back < len && (last[len - back] & 0xc0) == 0x80) back++;
        last[len - back] = 0;
    }
    int ll = (int)strlen(last);
    *n = (int)(last - out) + ll;
    if (*n + 4 > out_sz) return;
    memcpy(out + *n, "\xe2\x80\xa6", 3);
    *n += 3;
    out[*n] = 0;
}

int text_wrap(const Font *f, const char *s, int max_w, int max_lines,
              char *out, int out_sz) {
    int line = 0, n = 0;
    out[0] = 0;
    if (max_w < 1 || max_lines < 1 || out_sz < 8) return 0;
    int sp_w = text_width(f, " ");
    char cur[256]; int cur_len = 0, cur_w = 0;
    cur[0] = 0;

    while (*s && line < max_lines) {
        /* '\n' is a HARD break: flush even an empty accumulator, so blank lines
         * in line-structured input (a todo list, a header/separator block)
         * survive instead of being reflowed away. */
        if (*s == '\n') {
            s++;
            if (!push_line(out, out_sz, &n, line, cur, cur_len)) break;
            line++; cur[0] = 0; cur_len = 0; cur_w = 0;
            continue;
        }
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '\n') continue;
        const char *we = s;
        while (*we && *we != ' ' && *we != '\n') we++;
        int wl = (int)(we - s);
        if (wl > (int)sizeof cur - 1) wl = (int)sizeof cur - 1;
        char word[256];
        memcpy(word, s, wl); word[wl] = 0;
        int ww = text_width(f, word);
        int add_sp = cur_len > 0 ? sp_w : 0;
        /* Break on byte capacity too, not just pixel width: zero-advance glyphs
         * (combining marks, U+200B) fit unbounded bytes under max_w and would
         * overflow cur. (cur_len>0 ⇒ a flush frees the space, so no infinite loop.) */
        int byte_full = cur_len > 0 &&
            cur_len + (add_sp ? 1 : 0) + wl > (int)sizeof cur - 1;
        if (cur_w + add_sp + ww > max_w || byte_full) {
            if (cur_len == 0) {
                /* A single word wider than the column: hard-cut it. */
                int cut = fit_prefix(f, word, max_w);
                if (cut == 0) cut = 1;
                if (!push_line(out, out_sz, &n, line, word, cut)) break;
                line++;
                s += cut;
                continue;
            }
            if (!push_line(out, out_sz, &n, line, cur, cur_len)) break;
            line++; cur[0] = 0; cur_len = 0; cur_w = 0;
            continue;
        }
        if (add_sp) { cur[cur_len++] = ' '; cur[cur_len] = 0; cur_w += sp_w; }
        memcpy(cur + cur_len, word, wl); cur_len += wl; cur[cur_len] = 0;
        cur_w += ww;
        s = we;
    }
    if (cur_len > 0 && line < max_lines && push_line(out, out_sz, &n, line, cur, cur_len))
        line++;
    /* Leftover input means we ran out of lines (or buffer): mark the cut. */
    if (*s && line > 0) ellipsise_tail(f, out, &n, out_sz, max_w);
    return line;
}

const char *text_wrapped(const Font *f, const char *s, int max_w, int max_lines,
                         int *nlines) {
    /* ponytail: one shared scratch, valid until the next call — the generated
     * measure/draw passes consume it immediately. 16 KB caps a wrapped block —
     * 4 KB silently ellipsised a body_lines=200 panel long before its ceiling;
     * give it its own buffer if a config ever needs more. */
    static char buf[16384];
    int n = text_wrap(f, s, max_w, max_lines, buf, sizeof buf);
    if (nlines) *nlines = n;
    return buf;
}

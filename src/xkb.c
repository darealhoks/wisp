/* Tiny xkb keymap parser. wl_keyboard.keymap delivers a text blob (format=1,
 * xkb_v1); we walk just enough of it to extract `key <NAME> { [ sym1, sym2 ] }`
 * for group 1 and resolve each symbol name to a Unicode codepoint. No
 * libxkbcommon; the table is rebuilt on every keymap event.
 *
 * Caveats — knowingly skipped:
 *   - Only group 1. Multi-group layouts (us+cz toggled by Alt+Shift) lose
 *     the alternate group; sufficient for password/menu typing.
 *   - No key types: caps-lock is treated as "shift" only on keys whose hi
 *     keysym is uppercase(lo) (alphabetic-ish). Symbols are unaffected.
 *   - Modifier bitmasks use the fixed X11 layout (shift=1, lock=2, ctrl=4,
 *     mod1=8...); we don't parse `modifier_map`. Universally true for any
 *     xkb keymap built from evdev. */

#include "wisp.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

XkbKey xkb_keys[256];
int    xkb_loaded;
int    xkb_caps_on;
int    xkb_shift_on;

/* Named-keysym → codepoint table. Covers ASCII punctuation, latin-1, and the
 * latin-2 letters that appear on Czech/Slovak/Polish/Hungarian layouts (the
 * maintainer's locale). Anything else (Print, F-keys, dead_*) returns 0 and is
 * either handled by evdev keycode in the caller (BackSpace/Enter/Esc/Tab) or
 * ignored. Linear scan — only consulted at keymap-load time. */
static const struct { const char *n; uint32_t cp; } NAMED[] = {
    {"space",       ' '},  {"exclam",      '!'},  {"quotedbl",    '"'},
    {"numbersign",  '#'},  {"dollar",      '$'},  {"percent",     '%'},
    {"ampersand",   '&'},  {"apostrophe",  '\''}, {"parenleft",   '('},
    {"parenright",  ')'},  {"asterisk",    '*'},  {"plus",        '+'},
    {"comma",       ','},  {"minus",       '-'},  {"period",      '.'},
    {"slash",       '/'},  {"colon",       ':'},  {"semicolon",   ';'},
    {"less",        '<'},  {"equal",       '='},  {"greater",     '>'},
    {"question",    '?'},  {"at",          '@'},  {"bracketleft", '['},
    {"backslash",   '\\'}, {"bracketright",']'},  {"asciicircum", '^'},
    {"underscore",  '_'},  {"grave",       '`'},  {"braceleft",   '{'},
    {"bar",         '|'},  {"braceright",  '}'},  {"asciitilde",  '~'},
    /* Latin-1 commonly seen on EU layouts */
    {"nobreakspace",0xa0}, {"exclamdown",  0xa1}, {"cent",        0xa2},
    {"sterling",    0xa3}, {"currency",    0xa4}, {"yen",         0xa5},
    {"brokenbar",   0xa6}, {"section",     0xa7}, {"diaeresis",   0xa8},
    {"copyright",   0xa9}, {"ordfeminine", 0xaa}, {"guillemotleft",0xab},
    {"notsign",     0xac}, {"hyphen",      0xad}, {"registered",  0xae},
    {"macron",      0xaf}, {"degree",      0xb0}, {"plusminus",   0xb1},
    {"twosuperior", 0xb2}, {"threesuperior",0xb3},{"acute",       0xb4},
    {"mu",          0xb5}, {"paragraph",   0xb6}, {"periodcentered",0xb7},
    {"cedilla",     0xb8}, {"onesuperior", 0xb9}, {"masculine",   0xba},
    {"guillemotright",0xbb},{"onequarter", 0xbc}, {"onehalf",     0xbd},
    {"threequarters",0xbe},{"questiondown",0xbf}, {"multiply",    0xd7},
    {"ssharp",      0xdf}, {"division",    0xf7},
    {"Agrave",      0xc0}, {"Aacute",      0xc1}, {"Acircumflex", 0xc2},
    {"Atilde",      0xc3}, {"Adiaeresis",  0xc4}, {"Aring",       0xc5},
    {"AE",          0xc6}, {"Ccedilla",    0xc7}, {"Egrave",      0xc8},
    {"Eacute",      0xc9}, {"Ecircumflex", 0xca}, {"Ediaeresis",  0xcb},
    {"Igrave",      0xcc}, {"Iacute",      0xcd}, {"Icircumflex", 0xce},
    {"Idiaeresis",  0xcf}, {"ETH",         0xd0}, {"Ntilde",      0xd1},
    {"Ograve",      0xd2}, {"Oacute",      0xd3}, {"Ocircumflex", 0xd4},
    {"Otilde",      0xd5}, {"Odiaeresis",  0xd6}, {"Oslash",      0xd8},
    {"Ugrave",      0xd9}, {"Uacute",      0xda}, {"Ucircumflex", 0xdb},
    {"Udiaeresis",  0xdc}, {"Yacute",      0xdd}, {"THORN",       0xde},
    {"agrave",      0xe0}, {"aacute",      0xe1}, {"acircumflex", 0xe2},
    {"atilde",      0xe3}, {"adiaeresis",  0xe4}, {"aring",       0xe5},
    {"ae",          0xe6}, {"ccedilla",    0xe7}, {"egrave",      0xe8},
    {"eacute",      0xe9}, {"ecircumflex", 0xea}, {"ediaeresis",  0xeb},
    {"igrave",      0xec}, {"iacute",      0xed}, {"icircumflex", 0xee},
    {"idiaeresis",  0xef}, {"eth",         0xf0}, {"ntilde",      0xf1},
    {"ograve",      0xf2}, {"oacute",      0xf3}, {"ocircumflex", 0xf4},
    {"otilde",      0xf5}, {"odiaeresis",  0xf6}, {"oslash",      0xf8},
    {"ugrave",      0xf9}, {"uacute",      0xfa}, {"ucircumflex", 0xfb},
    {"udiaeresis",  0xfc}, {"yacute",      0xfd}, {"thorn",       0xfe},
    {"ydiaeresis",  0xff},
    /* Latin-2 / Central European letters (Czech, Slovak, Polish, Hungarian) */
    {"Aogonek",     0x104},{"aogonek",     0x105},
    {"Cacute",      0x106},{"cacute",      0x107},
    {"Ccaron",      0x10c},{"ccaron",      0x10d},
    {"Dstroke",     0x110},{"dstroke",     0x111},
    {"Dcaron",      0x10e},{"dcaron",      0x10f},
    {"Eogonek",     0x118},{"eogonek",     0x119},
    {"Ecaron",      0x11a},{"ecaron",      0x11b},
    {"Lcaron",      0x13d},{"lcaron",      0x13e},
    {"Lstroke",     0x141},{"lstroke",     0x142},
    {"Nacute",      0x143},{"nacute",      0x144},
    {"Ncaron",      0x147},{"ncaron",      0x148},
    {"Oacute",      0xd3}, {"oacute",      0xf3},
    {"Odoubleacute",0x150},{"odoubleacute",0x151},
    {"Racute",      0x154},{"racute",      0x155},
    {"Rcaron",      0x158},{"rcaron",      0x159},
    {"Sacute",      0x15a},{"sacute",      0x15b},
    {"Scaron",      0x160},{"scaron",      0x161},
    {"Scedilla",    0x15e},{"scedilla",    0x15f},
    {"Tcaron",      0x164},{"tcaron",      0x165},
    {"Tcedilla",    0x162},{"tcedilla",    0x163},
    {"Udoubleacute",0x170},{"udoubleacute",0x171},
    {"Uring",       0x16e},{"uring",       0x16f},
    {"Ydiaeresis",  0x178},
    {"Zacute",      0x179},{"zacute",      0x17a},
    {"Zcaron",      0x17d},{"zcaron",      0x17e},
    {"Zabovedot",   0x17b},{"zabovedot",   0x17c},
    /* Euro on AltGr+5 / AltGr+E layouts */
    {"EuroSign",    0x20ac},
};

static uint32_t name_to_cp(const char *n, int nl) {
    if (nl <= 0) return 0;
    /* Single ASCII letter/digit: keysym XK_a..XK_z, XK_A..XK_Z, XK_0..XK_9
     * have name == the character. */
    if (nl == 1) {
        unsigned char c = n[0];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) return c;
    }
    /* "Unnnn" / "U+nnnn" — Unicode keysym (XKB lets the layout write these). */
    if (n[0] == 'U' && nl >= 2) {
        const char *p = n + 1; int pl = nl - 1;
        if (*p == '+') { p++; pl--; }
        char buf[16]; if (pl > 15) pl = 15;
        memcpy(buf, p, pl); buf[pl] = 0;
        char *end; unsigned long v = strtoul(buf, &end, 16);
        if (end != buf && *end == 0 && v > 0 && v < 0x110000) return (uint32_t)v;
    }
    /* "0xNN" — bare hex keysym; <0x100 is Latin-1, ≥0x01000000 is Unicode. */
    if (n[0] == '0' && nl >= 3 && (n[1] == 'x' || n[1] == 'X')) {
        char buf[16]; int pl = nl > 15 ? 15 : nl;
        memcpy(buf, n, pl); buf[pl] = 0;
        char *end; unsigned long v = strtoul(buf + 2, &end, 16);
        if (end != buf + 2 && *end == 0) {
            if (v >= 0x01000000) v -= 0x01000000;  /* xkb-unicode encoding */
            /* X11 keysyms 0xfd00-0xffff are function/modifier/nav keys
             * (Super_L=0xffeb, Return=0xff0d, …), not characters. */
            if (v >= 0xfd00 && v <= 0xffff) return 0;
            if (v > 0 && v < 0x110000) return (uint32_t)v;
        }
    }
    for (size_t i = 0; i < sizeof NAMED / sizeof *NAMED; i++) {
        const char *m = NAMED[i].n;
        if ((int)strlen(m) == nl && memcmp(m, n, nl) == 0) return NAMED[i].cp;
    }
    return 0;
}

/* Per-name → evdev keycode table built from xkb_keycodes. */
#define MAX_KEYNAMES 320
typedef struct { char name[12]; int evdev; } KName;
static KName knames[MAX_KEYNAMES];
static int   n_knames;

static int find_evdev(const char *n, int nl) {
    if (nl <= 0 || nl >= (int)sizeof(knames[0].name)) return -1;
    for (int i = 0; i < n_knames; i++) {
        if ((int)strlen(knames[i].name) == nl && memcmp(knames[i].name, n, nl) == 0)
            return knames[i].evdev;
    }
    return -1;
}

/* Cursor utilities. */
static const char *skip_ws(const char *p, const char *end) {
    while (p < end && (isspace((unsigned char)*p) || *p == ',')) p++;
    return p;
}
static const char *find_lit(const char *p, const char *end, const char *lit) {
    int ll = (int)strlen(lit);
    for (; p + ll <= end; p++) if (memcmp(p, lit, ll) == 0) return p;
    return NULL;
}

/* Parse the `xkb_keycodes` block: collect `<NAME> = N;` entries. */
static void parse_keycodes(const char *p, const char *end) {
    n_knames = 0;
    while (p < end && n_knames < MAX_KEYNAMES) {
        while (p < end && *p != '<' && *p != '}') p++;
        if (p >= end || *p == '}') return;
        p++;  /* past '<' */
        const char *nb = p;
        while (p < end && *p != '>') p++;
        if (p >= end) return;
        int nl = (int)(p - nb);
        p++;  /* past '>' */
        while (p < end && *p != '=' && *p != ';' && *p != '\n') p++;
        if (p >= end || *p != '=') continue;
        p++;
        while (p < end && isspace((unsigned char)*p)) p++;
        /* Bounded decimal parse: p..end is not NUL-terminated, so strtol could
         * read past the mapping when a digit run abuts EOF. */
        const char *ds = p;
        long v = 0;
        while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; if (v > 1 << 20) break; }
        if (p == ds) continue;
        if (v < 8 || v >= 8 + 256 || nl <= 0 || nl >= (int)sizeof(knames[0].name))
            continue;
        memcpy(knames[n_knames].name, nb, nl);
        knames[n_knames].name[nl] = 0;
        knames[n_knames].evdev = (int)v - 8;
        n_knames++;
    }
}

/* Parse one `key <NAME> { ... [ sym, sym, ... ] ... };` entry starting at p
 * (just past `key`). Advances p past the closing `};` and returns the new p. */
static const char *parse_key_entry(const char *p, const char *end) {
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end || *p != '<') return p;
    p++;
    const char *nb = p;
    while (p < end && *p != '>') p++;
    if (p >= end) return p;
    int nl = (int)(p - nb);
    p++;
    /* Walk to the first '[' of the symbols list within this entry's braces.
     * Two xkb forms appear in keymaps:
     *   bare:   key <X> { [ a, b ] };
     *   group:  key <X> { symbols[1]= [ a, b ], symbols[2]= [...] };
     * In the group form, the `[` of `symbols[1]` is an array index, not the
     * symbol list — distinguish by what precedes it: list `[` follows '=',
     * ',' or '{' (possibly with whitespace); array `[` follows an identifier
     * char. */
    int depth = 0;
    const char *lb = NULL;
    char prev_tok = '{';
    while (p < end) {
        char c = *p;
        if (c == '{') depth++;
        else if (c == '}') { if (--depth == 0) { p++; break; } }
        else if (c == '[' && depth >= 1 && !lb) {
            if (prev_tok == '=' || prev_tok == ',' || prev_tok == '{') {
                lb = p; break;
            }
        }
        if (!isspace((unsigned char)c)) prev_tok = c;
        p++;
    }
    if (!lb) {
        /* No symbols; skip to terminating ';'. */
        while (p < end && *p != ';') p++;
        if (p < end) p++;
        return p;
    }
    /* Parse comma-separated symbol names inside [ ... ]. */
    p = lb + 1;
    uint32_t syms[8] = {0};
    int ns = 0;
    while (p < end && ns < 8) {
        p = skip_ws(p, end);
        if (p >= end || *p == ']') break;
        const char *sb = p;
        while (p < end && *p != ',' && *p != ']' && !isspace((unsigned char)*p)) p++;
        int sl = (int)(p - sb);
        syms[ns++] = name_to_cp(sb, sl);
    }
    /* Skip to '};' that terminates this `key` entry. */
    while (p < end) {
        if (*p == ';') { p++; break; }
        p++;
    }
    int ev = find_evdev(nb, nl);
    if (ev >= 0 && ev < 256) {
        uint32_t lo = syms[0], hi = ns >= 2 ? syms[1] : syms[0];
        xkb_keys[ev].lo = lo;
        xkb_keys[ev].hi = hi;
        /* Mark as alpha (caps-lock-affected) when lo is a Latin-block lower-
         * case letter whose uppercase equals hi. Cheap and avoids parsing key
         * types. Covers ASCII, latin-1 á/é/í/..., latin-2 ě/š/č/.... */
        int alpha = 0;
        if (lo && hi && lo != hi) {
            if (lo >= 'a' && lo <= 'z' && hi == lo - 32) alpha = 1;
            else if (lo >= 0xe0 && lo <= 0xfe && lo != 0xf7 && hi == lo - 0x20) alpha = 1;
            else if ((lo & 1) == 1 && hi == lo - 1 && lo >= 0x100 && lo < 0x180) alpha = 1;
        }
        xkb_keys[ev].alpha = alpha;
    }
    return p;
}

/* Parse the `xkb_symbols` block: scan for `key <...> { ... };` entries. */
static void parse_symbols(const char *p, const char *end) {
    while (p < end) {
        /* Find next "key " at column-ish position (must be word-boundary). */
        const char *k = find_lit(p, end, "key");
        if (!k) return;
        /* Require key be a token: preceded by ws/start, followed by ws/'<'. */
        if (k > p && !isspace((unsigned char)k[-1]) && k[-1] != ';' && k[-1] != '{') {
            p = k + 1; continue;
        }
        const char *after = k + 3;
        if (after >= end || (!isspace((unsigned char)*after) && *after != '<')) {
            p = k + 1; continue;
        }
        p = parse_key_entry(after, end);
    }
}

void xkb_load(int fd, size_t size) {
    if (fd < 0) return;
    /* size comes from the compositor's wl_keyboard.keymap event; if it exceeds
     * the fd's real size, every access past EOF raises SIGBUS. Clamp to the
     * actual file size. */
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0) return;
    if ((size_t)st.st_size < size) size = (size_t)st.st_size;
    if (!size) return;
    void *m = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED) {
        m = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) { msg("xkb: mmap failed"); return; }
    }
    const char *base = (const char *)m;
    const char *end  = base + size;
    /* Locate keycodes block. */
    const char *kc = find_lit(base, end, "xkb_keycodes");
    const char *sy = find_lit(base, end, "xkb_symbols");
    if (!kc || !sy) { munmap(m, size); msg("xkb: section missing"); return; }
    const char *kcb = find_lit(kc, end, "{");
    const char *syb = find_lit(sy, end, "{");
    if (!kcb || !syb) { munmap(m, size); return; }

    /* Clear and rebuild — keymap can change at runtime (e.g. setxkbmap). */
    memset(xkb_keys, 0, sizeof xkb_keys);
    parse_keycodes(kcb + 1, end);
    parse_symbols(syb + 1, end);
    xkb_loaded = 1;
    munmap(m, size);
}

/* US-QWERTY fallback used when no keymap has loaded yet. Matches the
 * pre-xkb hardcoded KMAP that lived in menu.c / lock.c. */
static const uint8_t FALLBACK_LO[128] = {
    0,   0,   '1','2','3','4','5','6','7','8','9','0','-','=', 0,  '\t',
    'q','w','e','r','t','y','u','i','o','p','[',']', 0,   0,  'a','s',
    'd','f','g','h','j','k','l',';','\'','`',  0,  '\\','z','x','c','v',
    'b','n','m',',','.','/', 0,  '*',  0,  ' ',
};
static const uint8_t FALLBACK_HI[128] = {
    0,   0,   '!','@','#','$','%','^','&','*','(',')','_','+', 0,  '\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}', 0,   0,  'A','S',
    'D','F','G','H','J','K','L',':','"','~',  0,  '|','Z','X','C','V',
    'B','N','M','<','>','?', 0,  '*',  0,  ' ',
};

uint32_t xkb_xlat(uint32_t evdev, int shift) {
    if (evdev >= 256) return 0;
    if (xkb_loaded) {
        XkbKey *k = &xkb_keys[evdev];
        int sh = shift ? 1 : 0;
        if (k->alpha) sh ^= (xkb_caps_on ? 1 : 0);
        uint32_t cp = sh ? k->hi : k->lo;
        return cp;
    }
    if (evdev >= 128) return 0;
    int sh = shift ? 1 : 0;
    uint8_t lo = FALLBACK_LO[evdev], hi = FALLBACK_HI[evdev];
    if (lo >= 'a' && lo <= 'z') sh ^= (xkb_caps_on ? 1 : 0);
    return sh ? hi : lo;
}

/* Update modifier latches from wl_keyboard.modifiers. Group is ignored
 * (single-group support). */
void xkb_on_modifiers(uint32_t depressed, uint32_t latched, uint32_t locked) {
    uint32_t eff = depressed | latched | locked;
    xkb_shift_on = (eff & 1) ? 1 : 0;          /* Shift */
    xkb_caps_on  = (eff & 2) ? 1 : 0;          /* Lock (CapsLock) */
#ifdef WISP_HAS_LOCK
    lock_on_caps_changed();
#endif
}

/* UTF-8 encode `cp` into out (≥4 bytes). Returns byte count, 0 if cp invalid. */
int utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) { out[0] = cp; return 1; }
    if (cp < 0x800) {
        out[0] = 0xc0 | (cp >> 6);
        out[1] = 0x80 | (cp & 0x3f);
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = 0xe0 | (cp >> 12);
        out[1] = 0x80 | ((cp >> 6) & 0x3f);
        out[2] = 0x80 | (cp & 0x3f);
        return 3;
    }
    if (cp < 0x110000) {
        out[0] = 0xf0 | (cp >> 18);
        out[1] = 0x80 | ((cp >> 12) & 0x3f);
        out[2] = 0x80 | ((cp >> 6) & 0x3f);
        out[3] = 0x80 | (cp & 0x3f);
        return 4;
    }
    return 0;
}

/* Walk back one UTF-8 codepoint from end of `s` (length len). Returns the
 * new length, or 0 if buffer was already empty. */
int utf8_back(const char *s, int len) {
    if (len <= 0) return 0;
    int n = len - 1;
    while (n > 0 && (((unsigned char)s[n]) & 0xc0) == 0x80) n--;
    return n;
}

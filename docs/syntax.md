# Syntax

A `.wisp` file is a flat list of top-level declarations plus style rules. Order
never matters, sema is two-pass. Statements end with `;`, blocks with `}`.

```
source NAME = call(args) [ { on_change() = stmt; } ] ;
const NAME = expr;      mut NAME = expr;
surface NAME { … }      compound NAME { … }      menu NAME { … }
lock { … }   gamma { … }   wallpaper { … }   media { … }
SELECTOR[, SELECTOR]* { prop = value; … }
```

## Comments

`// line` and `/* block */`. Block comments do **not** nest, the first `*/`
closes.

## Build directives

`//! key = value` is a plain comment to the compiler. Only the Makefile reads
these, with a `sed`, so the last occurrence in the file wins and a leading `~/`
is expanded.

| key | make variable | default |
|---|---|---|
| `font_backend` | `FONT_BACKEND` | `truetype` |
| `font` | `FONT` | `~/.local/share/fonts/MapleMono-NF-Bold.ttf` |
| `font_fallback` | `FONT_FALLBACK` | empty |
| `fractional` | `FRACTIONAL` | `0`, and `1` requires truetype |

```wisp
//! font = ~/.local/share/fonts/MapleMono-NF-Bold.ttf
//! font_fallback = /usr/share/fonts/noto-emoji/NotoColorEmoji.ttf
```

The config in use is the `WISP=` path.

## Identifiers and keywords

`[A-Za-z_][A-Za-z0-9_]*`. Reserved words:

```
source surface widget const mut lock gamma wallpaper media compound
region group for in cell true false exec emit set animate
on_click on_right_click on_middle_click on_change
```

`menu`, `item` and `preset` are contextual, not reserved: `menu NAME {` is a
menu declaration, a bare `menu {` is a style rule.

## Numbers

Decimal integers, `0x` hex integers, and floats written `digits.digits`. There
are no negative literals: `-5` is unary negation, folded at compile time
wherever a negative constant is meaningful (`margin`, `exclusive_zone`).

A `.` followed by another `.` belongs to the range operator, so `16..0` lexes as
`16 .. 0`, not `16.` then `.0`.

**Time suffixes** attach to integers only, and must touch the digits with no
identifier character after them:

| written | value |
|---|---|
| `200ms` | 200 |
| `2s` | 2000 |
| `1.5s` | parse error, floats reject the suffix |

## Colors

`#` plus exactly 6 or 8 hex digits, read as ARGB. Six digits get `0xFF` alpha.
Any other digit count is an error. A `#name` that is not a colour is a style id
selector.

```wisp
bg = #ff0e131c;    // opaque
bg = #00000000;    // fully transparent
fg = #dbe2ee;      // same as #ffdbe2ee
```

## Strings and escapes

Double quoted only. There are no single-quoted strings, `'` outside a string is
an "unexpected character" error. The complete escape set is:

| escape | result |
|---|---|
| `\n` `\t` `\r` | LF, TAB, CR |
| `\0` | NUL byte |
| `\\` `\"` `\'` | that literal character |
| anything else | **both bytes kept verbatim** |

There is no `\xNN`. `\{` does not produce a bare `{`: it suppresses
interpolation and leaves the backslash in the output text.

## Interpolation

Any unescaped `{` before the last character of a string turns it into an
interpolated string. Braces nest, and the slice inside is re-lexed and parsed as
a complete expression, so ternaries, member access and nested interpolation all
work.

```wisp
text = "{bat_s.pct}%";
text = "{cpu_s.pct}% / {mem_s.pct}%";
text = "{tags.title}";
```

A string literal cannot appear inside an interpolation: the closing quote ends
the outer string first, and the result is "unterminated `{...}`". Put the
ternary outside instead.

```wisp
text = wifi_s.up ? wifi_s.ssid : "offline";
```

An unmatched `{` is an error. Limits, all silent except the first:

| limit | value | failure |
|---|---|---|
| interpolated expressions per string | 16 | `--emit` error, `--check` passes |
| format string | 1024 bytes | silently clipped |
| output buffer | 64 to 2048 bytes | snprintf truncation |

There is no string concatenation operator. Interpolation is how you join
strings.

## Expressions

Loosest to tightest:

| level | operators |
|---|---|
| 1 | `?:` (right associative) |
| 2 | `\|\|` |
| 3 | `&&` |
| 4 | `\|` |
| 5 | `&` |
| 6 | `==` `!=` |
| 7 | `<` `>` `<=` `>=` |
| 8 | `+` `-` |
| 9 | `*` `/` `%` |
| 10 | unary `!` `-` |
| 11 | `..` range |
| 12 | literal, `(e)`, `$ident`, `ident`, `ident.field`, `call(args)` |

`..` binds tighter than every binary operator, so `a + 1..2` means `a + (1..2)`.
A range in a value position lowers to its low end only; ranges exist for
animated fillets (`fillet_tl = 0..12`).

`$ident` reads a template argument inside a `spawned_by` surface, see
[[modules#osd-stack]].

Function calls are legal in exactly two positions: the right-hand side of a
`source`, and `cubic_bezier(a,b,c,d)` as an `animate()` easing. Anywhere else
the compiler says calls are only allowed as a source right-hand side.

## String comparison

`==` and `!=` on strings lower to `wisp_streq()`. A non-string operand is
coerced with snprintf first, so `count == "3"` works. Ordering comparisons on
strings are an error, and `+` on strings is an error with a pointer to
interpolation.

```wisp
visible = hid.value == "0";
fg = g.value == "1" ? WARM : TEXT;
```

## Style rules

A selector is a whitespace-separated descendant chain of
`[type][#id|id][.class]*[:pseudo]`. Types are `surface region group widget cell`.
`#name` and bare `name` are the same id selector. Classes are declared as a
suffix on a name, `widget wifi.pill.warn` or `cell.ws`.

Whitespace is significant. `bar.pill` is one node that is both, `bar .pill` is a
descendant chain.

Specificity is `100*ids + 10*(classes+pseudo) + 1*types`, summed over the chain.
Highest wins. A tie on the same property of the same node is a **hard error**
naming both rules. A property written inline in a body beats every rule.

Pseudo-classes, applied in this order so a later one wins:

| pseudo | where it works |
|---|---|
| `:active` | `for` cells, and `$active` inside a spawned template |
| `:urgent` | same, applied after `:active` |
| `:mute` | OSD and pill slabs, reads `$mute` |
| `:warn` | OSD and pill slabs, reads `$warn` |
| `:pressed` | any widget, may only set `bg`, lowers to `press_bg` |

`:hover` is rejected with a pointer to the `hover;` marker used by menus.

## Marker properties

A bare identifier with no `=` is a marker: `slider;` `elide;` `wrap;`
`body_fit;` `show_value;` `hover;`.

## Gotchas

- `\xNN` does not exist; unknown escapes keep both bytes, so `"\x41"` is literally backslash-x-4-1.
- `\{` keeps the backslash, it only suppresses interpolation.
- Floats reject time suffixes: `1.5s` is a parse error, write `1500ms`.
- More than 16 interpolated expressions in one string passes `--check` and fails `--emit`.
- Equal specificity on the same property is a compile error, there is no later-wins rule.
- `bar.pill` and `bar .pill` are different selectors; a stray space changes the meaning.
- Block comments do not nest.
- There are no single-quoted strings, despite `\'` being a valid escape inside a double-quoted one.
- A string literal inside `{…}` interpolation does not work; the quote terminates the outer string.

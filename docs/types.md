# Types

There is no type annotation anywhere. Everything is inferred from literals,
source fields and property schemas.

## Value types

| type | literals | notes |
|---|---|---|
| int | `12`, `0xf241`, `200ms`, `2s` | time suffixes normalise to ms |
| float | `1.5`, `0.5` | no time suffix allowed |
| bool | `true`, `false` | interconverts with int |
| string | `"…"`, interpolated | double quotes only, no `+`, use interpolation |
| color | `#rrggbb`, `#aarrggbb` | ARGB u32 |
| pixmap | `row.icon`, `it.icon`, `$image` | decoded PNG, not constructible |
| enum | bare idents in enum slots | see below |
| unknown | `for` cell fields, `$args`, calls | matches everything, never errors |

int, float and bool convert into each other freely. Every other type is an
island: mixing them in the two arms of a ternary is an error. The unknown type
is the escape hatch and is why a typo inside a `for` cell field slips past the
type checker and dies in codegen instead.

`text` accepts any type: a number in a text slot is snprintf-coerced through a
32-byte scratch buffer.

## Property types

Every property name has one of three types, regardless of which surface kind it
appears on.

**PT_NUM.** A string, colour or enum here is an error.

```
width height pad pad_x pad_y x_offset y_offset border_width
border_top border_bottom border_left border_right margin exclusive_zone
font_size clip_top gap radius radius_tl radius_tr radius_bl radius_br
radius_inner radius_outer thumb_size thumb_radius thumb_border_width
track_radius value_gap value_scale value_max shadow_x shadow_y
shadow_blur shadow_spread body_lines reveal_on_hover reveal_gutter
reveal_anim_ms row_h max_visible size anchor_gap fillet_r fillet_offset_y
armpit_inner armpit_outer armpit_tl armpit_tr armpit_bl armpit_br
enter_anim exit_anim separator_frac wrong_ms day_k night_k
flat_k day_hour night_hour fade_min transition_ms fade_ms dither_px
wipe_soft prog_h icon_gap icon_box
```

**PT_COLOR.**

```
bg fg icon_fg bg_bottom border press_bg shadow track_bg track_fg
thumb_color thumb_border prog_fg prog_track armpit_color separator
ring ring_bad dim caps
```

**PT_ANY** is everything else: `text`, `icon`, `value`, `graph`, `visible`,
`spawned_by`, `path`, `prompt`, the enum props, the markers.

An integer literal is tolerated wherever an enum is expected. An enum
identifier in a numeric or colour slot is rejected.

## Enums

| property | values |
|---|---|
| `layer` | `background` `bottom` `top` `overlay` |
| `anchor` | `top` `bottom` `left` `right`, combinable with `\|` |
| `edge` | `top` `bottom` `left` `right`, single value |
| `align` | `left` `right` `top` `bottom` `center` `start` `end` |
| `axis`, `orientation` | `vertical` `horizontal` |
| `text_align`, `value_align` | `start` `center` `end` `top` `bottom` `left` `right` |
| `thumb_shape` | `bar` `pill` `circle` `disc` `knob` `none` |
| `input` | `none` only |
| `transition` | `fade` `dither` `wipe` |
| `wipe_dir` | `right` `left` `down` `up` `down_right` `down_left` `up_right` `up_left` |

Easing values are `linear` `ease_in` `ease_out` `ease_in_out`, plus
`cubic_bezier(a,b,c,d)` for `animate()` only.

## Units

There are none. Every number is logical pixels, except integers carrying `ms`
or `s`, which are milliseconds. Scale factors are applied at render time, you
never write them.

Colours must constant-fold. A colour property whose value cannot be resolved at
compile time is a hard build error, deliberately, so an accidental alpha-0
surface cannot ship silently. `bg = SOME_CONST` is fine; a `bg` that depends on
a source is not, unless the pseudo-class machinery produces it.

## Ranges

`lo..hi` is a range value. In a plain value slot it collapses to `lo`. The only
consumers are the fillet properties on a HUD, where the range is the
animated span as the surface emerges.

```wisp
fillet_tl = 0..12;
```

Endpoints must be integers, there are no float fillets.

## Coercion summary

| from | to | how |
|---|---|---|
| int | float | implicit |
| bool | int | implicit, `true` is 1 |
| any | string | snprintf, in `text` and in interpolation |
| string | number | never |
| color | anything | never |
| enum | number slot | rejected |
| int | enum slot | accepted |

## Gotchas

- The unknown type never errors, which is why a misspelled `for` cell field survives `--check`.
- A colour that does not constant-fold is a build error, not a runtime fallback.
- Ternary arms from different type groups (a colour and a string) are a sema error.
- Ordering comparisons (`<`, `>`) on strings are an error; only `==` and `!=` exist for them.

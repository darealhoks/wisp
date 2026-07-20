# Your bar from scratch

This builds one bar from an empty file. By the end it shows the time, the
machine's load, your workspaces, and it responds to clicks.

You need wisp built and installed once already. See [install.md](install.md).

Make a file to work in:

    cd ~/next/rice/wisp
    touch configs/mine.wisp

and point the build at it:

    make install WISP=configs/mine.wisp && wispctl reload

That command is the whole loop. You will run it after every step below. The
selection is sticky, so after the first time `make install` alone is enough.

## A bar that exists

Put this in `configs/mine.wisp`:

    surface bar {
      layer = top;
      anchor = top|left|right;
      height = 28;
      exclusive_zone = 28;
      bg = #ff0f1219;
    }

Build and reload. You get a dark strip across the top of the screen, and your
windows move down to make room for it.

`anchor` glues the surface to three edges, so its width comes from the screen
and only `height` is yours to choose. `exclusive_zone` is what pushes the
windows: it reserves those 28 pixels so no window is ever hidden underneath.
Set it to 0 and the bar floats over your windows instead.

## The time

An empty bar is not much. Add a source and something to draw it with:

    source time = clock("%H:%M");

    surface bar {
      layer = top;
      anchor = top|left|right;
      height = 28;
      exclusive_zone = 28;
      bg = #ff0f1219;

      widget clock {
        align = center;
        text = time;
        fg = #ffd6dae1;
      }
    }

`clock("%H:%M")` takes a strftime format. Because that format has no seconds in
it, the daemon arms a timer that fires once a minute rather than once a second.

`text = time` reads the source's first field. It could also be written
`text = time.value`.

## More than one thing

Widgets go into three buckets, chosen with `align`, and inside a bucket they
appear in the order you wrote them.

    source time = clock("%H:%M");
    source cpu  = cpu();
    source mem  = mem();

    surface bar {
      layer = top; anchor = top|left|right; height = 28;
      exclusive_zone = 28; bg = #ff0f1219;

      widget clock { align = center; text = time; fg = #ffd6dae1; }

      widget cpu_w { align = right; text = "cpu {cpu.pct}%"; fg = #ffd6dae1; pad = 8; }
      widget mem_w { align = right; text = "mem {mem.pct}%"; fg = #ffd6dae1; pad = 8; }
    }

The braces inside the string are interpolation. Anything you can write as an
expression can go in them.

`pad` is the gap to the next widget, and on the first and last widget of a
bucket it becomes the inset from the screen edge.

Both new widgets read `cpu` and `mem`, which sample once a second. They only
repaint when a number actually changes, so a bar showing a steady 3% is costing
you nothing between changes.

## Colour in one place

Three widgets now repeat the same hex. Name it once:

    const BG   = #ff0f1219;
    const FG   = #ffd6dae1;
    const DIM  = #ff8b8f99;
    const WARN = #ffe9bc87;

`const` is inlined when the config is compiled, so there is no lookup at
runtime.

Now the colour can carry meaning:

    widget cpu_w {
      align = right;
      text = "cpu {cpu.pct}%";
      fg = cpu.pct >= 90 ? WARN : FG;
      pad = 8;
    }

There is no `if` in the language. The ternary is how you branch, and it works in
any expression, including sizes.

## Style out of the way

Setting `fg` and `pad` on every widget gets old. Move the repetition into a
rule:

    surface bar {
      ...
      widget clock { align = center; text = time; }
      widget cpu_w { align = right; text = "cpu {cpu.pct}%";
                     fg = cpu.pct >= 90 ? WARN : FG; }
      widget mem_w { align = right; text = "mem {mem.pct}%"; }
    }

    widget { fg = FG; pad = 8; }

The rule matches every widget. Note that `cpu_w` keeps its ternary: a property
written inside a node always beats a rule, no matter how specific the rule is.
That is the point of the split. The body says what a widget does, the rule says
what widgets look like, and behaviour is never at risk of being overwritten by
a colour.

Rules match by type (`widget`), by id (`#cpu_w`), by class, and by descendant
chains. Classes are a suffix on the name:

    widget mem_w.stat { ... }
    .stat { fg = DIM; }

If two rules of the same specificity set the same property on the same node,
that is a build error naming both. The compiler will not quietly pick one.

## Grouping

Related widgets look better in one container. A group is a single slot that
packs its members inside a rounded box:

    group sys {
      align = right;
      bg = #ff1c2027; radius = 8; pad_x = 12; gap = 10; pad = 8;

      widget cpu_w { text = "cpu {cpu.pct}%"; fg = cpu.pct >= 90 ? WARN : FG; }
      widget sep.sep { text = "/"; }
      widget mem_w { text = "mem {mem.pct}%"; }
    }
    .sep { fg = DIM; }

The group's own colours must be a literal or a const. A ternary on the
container's `bg` is a build error; members can be as dynamic as you like.

Groups hold plain widgets, so no sliders and no `for` inside one.

## Workspaces

Workspace tags come from the compositor (`ext-workspace-v1`, or mango's IPC as
a fallback), and they arrive as a list.
A list is drawn with `for`, which needs exactly one `cell`:

    source tags = tags();

    surface bar {
      ...
      for tag in tags.list {
        cell.ws {
          text = tag.label;
          visible = tag.occupied || tag.active;
        }
      }
    }

    .ws        { align = left; fg = DIM; bg = #00000000;
                 width = 28; height = 28; radius = 8; }
    .ws:active { fg = FG; bg = #ff1c2027; }

The loop is unrolled when the config is compiled, into the nine tags the
protocol defines. `visible` is what keeps empty workspaces off the bar.

`:active` is a pseudo-class. It compiles into `tag.active ? styled : base`, so
it is shorthand for a ternary you would otherwise write on every property.
There is also `:urgent`, applied after `:active` so it wins when both are true.

Because it wraps a base value, a pseudo-class can only set a property the base
rule already sets. That is why `.ws` gives `bg` a transparent value it never
paints: without it, `.ws:active { bg = ... }` fails the build with

    error: ':active' sets 'bg' but the node has no base value for it

A `for` belongs at surface or widget scope. Nested deeper it still parses and
still passes the type checker, and then codegen drops it without a word. If a
loop draws nothing, check where you put it first.

## Clicking

Give the cell a handler:

    for tag in tags.list {
      cell.ws {
        text = tag.label;
        visible = tag.occupied || tag.active;
        on_click() = exec("wispctl tag {tag.index} {tag.output}");
      }
    }

`exec` runs the string under `sh -c`. It interpolates like any other string, so
the clicked tag's own index goes into the command.

`tag.output` is the slot of the monitor this bar is on. Passing it means the
click switches the workspace of the monitor you clicked, not of whichever
monitor happens to have keyboard focus. On one screen it makes no difference.
On two it is the difference between working and being confusing.

Handlers are ordinary statements. Launch things the same way:

    widget net { icon = 0xf1eb; on_click() = exec("foot -e impala"); }

Icons are Nerd Font codepoints in hex. Only glyphs baked into your build render;
that set comes from `ICONS[]` in `tools/bake.c`.

To show a press, set a background for it:

    .ws:pressed { bg = #ff4d5a6e; }

## A menu

You have been calling `wispctl` from the bar. It works the other way too. Add a
menu surface and the launcher has something to draw into:

    surface menu {
      spawned_by = menu;
      layer = overlay;
      anchor = top|left|right;
      height = 28;
      exclusive_zone = -1;
      keyboard = exclusive;
      bg = BG; fg = FG; sel_bg = #ff1c2027; sel_fg = FG; dim = DIM;
      prompt = "run:";
      sort = "most_used";
    }

`spawned_by = menu` means this surface is a template. It is not created at
startup; something has to ask for an instance. Build, reload, then:

    wispctl apps

`exclusive_zone = -1` is what makes it overlay instead of reserving space, and
`keyboard = exclusive` is what lets you type into it.

Add `axis = vertical;` and the full-width strip becomes a centred launcher float
with scrolling rows and app icons.

The same surface serves your own menus:

    wispctl menu "pick one" alpha beta gamma

which prints the index and text you chose, and exits 1 if you pressed escape.

## Notifications

One more template:

    surface osd {
      spawned_by = osd;
      layer = overlay;
      anchor = top;
      margin = 12;
      bg = BG; fg = FG;
      radius = 10;
      timeout = 4s;
    }

Declaring it compiles in `osd.c` and the D-Bus client, so wisp now owns
`org.freedesktop.Notifications` and every application notification lands here.
Test it without one:

    wispctl notify 1 "hello" "body text"

The slab itself is drawn by `osd.c`. Properties on this surface configure that
renderer; widget blocks inside it do not replace it.

Volume and brightness feed the same surface:

    wispctl volume up

## What you built

Roughly 60 lines that compile into a daemon linking libc and libm, drawing a bar
that costs nothing while it sits still.

Notice what you never did: no plugin was enabled, no module list was configured.
Writing `tags()` linked the workspace client. Declaring the osd surface linked the D-Bus
client. A config without them produces a binary that does not contain that code
at all.

From here, read [dsl.md](dsl.md) for everything the language has that this
tutorial skipped: sliders, animations, hover-revealed HUDs, compounds, corner
fillets, and the gamma, wallpaper, lock and media blocks.

Then read `configs/bee.wisp`, which is all of it in use.

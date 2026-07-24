source time  = clock("%a %b %-d %H:%M");
source tags  = tags();
source cpu_s = cpu(every="2s");
source mem_s = mem(every="2s");

const GRAY   = #ff222222;
const FG     = #ffbbbbbb;
const SELBG  = #ff005577;
const SELFG  = #ffeeeeee;
const URG    = #ffee3300;

surface bar {
	layer = top;
	anchor = top | left | right;
	height = 22;
	exclusive_zone = 22;
	bg = GRAY;
	radius = 0;

	for tag in tags.list {
		cell {
			align = left;
			text  = tag.occupied && !tag.active ? "·{tag.label}" : tag.label;
			on_click() = exec("wispctl tag {tag.index} {tag.output}");
		}
	}

	widget lt    {
		align = left;
		text = "[]=";
		pad = 8;
	}
	widget title {
		align = left;
		text = tags.title;
		pad = 8;
		elide;
	}

	widget status {
		align = right;
		text = " {cpu_s.pct}% cpu  {mem_s.pct}% mem  {time}";
		pad = 8;
	}
}

widget {
	fg = FG;
}

#bar cell {
	fg = FG;
	bg = GRAY;
	width = 24;
	height = 22;
	pad = 4;
}
#bar cell:active {
	fg = SELFG;
	bg = SELBG;
}
#bar cell:urgent {
	fg = SELFG;
	bg = URG;
}

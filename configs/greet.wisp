//! font = ~/.local/share/fonts/MapleMono-NF-Bold.ttf
//! font_fallback = /usr/share/fonts/noto-emoji/NotoColorEmoji.ttf

include "theme.wisp";

source time = clock("%H:%M");
source date = clock("%A %d %B");

wallpaper {
	path = WALL;
	bg   = BLACK;
}

// wallpaper{} has no dim knob; greetd.css got its 0.376 black from a gradient layer
surface scrim {
	layer  = background;
	anchor = top|bottom|left|right;
	bg     = SCRIM;
	input  = none;
}

// font_size is per-surface, so the 64px clock and the 13px date cannot share one
// with the pill. margins are logical px on this box's 1200-tall panel, stacked
// above the centred pill (top edge at (1200-198)/2 = 501).
surface greetclock {
	layer     = bottom;
	anchor    = top;
	margin    = 370;
	width     = 600;
	height    = 84;
	bg        = #00000000;
	font_size = 64;

	widget clk {
		text  = time.value;
		fg    = TEXT;
		align = center;
	}
}

surface greetdate {
	layer     = bottom;
	anchor    = top;
	margin    = 464;
	width     = 600;
	height    = 22;
	bg        = #00000000;
	font_size = 13;

	widget dt {
		text  = date.value;
		fg    = SUBTXT;
		align = center;
	}
}

surface login {
	spawned_by = greet;
	layer      = overlay;
	user       = "hoks";
	sessions   = "/etc/greetd/environments";

	axis   = vertical;
	width  = 420;
	height = 198;
	pad_x  = 14;
	pad_y  = 14;
	font_size = 14;

	bg           = CRUST;
	fg           = TEXT;
	border       = BORD;
	border_width = 2;
	radius       = 8;

	group who {
		height = 30;
		pad    = 8;
		pad_x  = 12;
		gap    = 8;
		cell {
			icon = 0xf007;
			fg   = SUBTXT;
		}
		cell {
			text = greet.user;
			fg   = TEXT;
		}
		cell {
			text = greet.session;
			fg   = EMPTY;
		}
	}

	group field {
		height = 38;
		pad    = 8;
		pad_x  = 12;
		gap    = 10;
		bg     = REST;
		radius = 6;
		cell {
			icon = 0xf023;
			fg   = greet.failed ? RED : SUBTXT;
		}
		cell {
			text = greet.prompt;
			fg   = SUBTXT;
		}
		// one cell: a second cell would put the group's gap before the caret
		cell {
			text = "{greet.dots}{greet.input}{greet.busy ? \"…\" : \"_\"}";
			fg   = TEXT;
		}
	}

	for s in greet.sessions {
		cell {
			height = 30;
			pad    = 4;
			pad_x  = 8;
			radius = 4;
			// the environments file is bare command lines, so the icon is named here
			icon   = s.name == "MangoWM" ? 0xf2d0 : s.name == "zsh" ? 0xf120 : 0xf144;
			icon_gap = 8;
			text   = s.name;
			fg     = s.selected ? TEXT : SUBTXT;
			bg     = s.selected ? REST : #00000000;
		}
	}

	widget status {
		height = 18;
		pad_x  = 2;
		text   = greet.caps ? "caps" : greet.error;
		fg     = greet.caps ? ORANGE : (greet.failed ? RED : EMPTY);
	}
}

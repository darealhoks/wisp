//! font = ~/.local/share/fonts/MapleMono-NF-Bold.ttf
//! font_fallback = /usr/share/fonts/noto-emoji/NotoColorEmoji.ttf

include "lib/theme.wisp";

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
// above the centred pill (top edge at (1200-140)/2 = 530).
surface greetclock {
	layer     = bottom;
	anchor    = top;
	margin    = 399;
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
	margin    = 493;
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
	width  = 340;
	height = 140;
	pad_x  = 16;
	pad_y  = 14;
	font_size = 14;

	bg           = CRUST;
	fg           = TEXT;
	border       = BORD;
	border_width = 2;
	radius       = 8;

	group who {
		height = 22;
		pad    = 8;
		gap    = 8;
		cell {
			text = "login";
			fg   = EMPTY;
		}
		cell {
			text = greet.user;
			fg   = TEXT;
		}
	}

	group field {
		height = 22;
		pad    = 10;
		gap    = 8;
		cell {
			text = greet.prompt == "" ? "password:" : greet.prompt;
			fg   = SUBTXT;
		}
		// one cell: a second cell would put the group's gap before the caret
		cell {
			text = "{greet.dots}{greet.input}{greet.busy ? \"…\" : \"_\"}";
			fg   = TEXT;
		}
	}

	// horizontal band: a session added to /etc/greetd/environments widens the
	// strip instead of pushing the stack past `height`
	group sess {
		height = 22;
		pad    = 10;
		gap    = 4;
		cell {
			text = "session";
			fg   = EMPTY;
		}
		for s in greet.sessions {
			cell {
				text = s.selected ? "[{s.name}]" : " {s.name} ";
				fg   = s.selected ? TEXT : EMPTY;
			}
		}
	}

	// greetd's own description is "unable to create session: pam_authenticate:
	// AUTH_ERR"; PAM's human messages arrive as auth_message and leave failed 0
	widget status {
		height = 18;
		text   = greet.caps ? "caps lock is on"
			: greet.failed ? "incorrect password" : greet.error;
		fg     = greet.caps ? ORANGE : (greet.failed ? RED : EMPTY);
	}
}

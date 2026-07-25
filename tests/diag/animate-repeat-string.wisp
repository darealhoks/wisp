mut fade = 0.0;

surface bar {
	widget n {
		text = "x";
		on_click() = animate(fade, 1.0, 200ms, ease_out, repeat = "3");
	}
}

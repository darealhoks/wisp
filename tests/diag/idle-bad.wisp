idle {
	timeout blank { run = "wispctl dpms off"; }
	timeout blank { after = 0; delay = 5; }
	before_sleep = 3;
}

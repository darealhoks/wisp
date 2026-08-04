idle {
	timeout blank {
		after  = 300s;
		run    = "wispctl dpms off";
		resume = "wispctl dpms on";
	}
	before_sleep = lock;
}

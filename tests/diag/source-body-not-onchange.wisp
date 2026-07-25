source cnt = exec_line("echo 1", every="10s") {
	on_click() = exec("true");
};

surface bar {
	widget n { text = cnt; }
}

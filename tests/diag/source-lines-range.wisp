source tail = exec_line("tail -n 5 /tmp/log", every="2s", lines=999);
surface bar {
  widget log {
    text = tail.value;
  }
}

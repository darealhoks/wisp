const width = 40;
surface bar {
  widget btn {
    on_click() = animate(width, 100, 200ms, ease_out);
  }
}

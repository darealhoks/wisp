source t = clock("%H:%M");
surface bar {
  widget clock {
    text = t.value;
  }
}

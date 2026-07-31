// Two mistakes at once: an interpolated tooltip (the hit table keeps the
// pointer across frames, so only a literal survives) and no surface to draw it.
const PCT = 42;

surface bar {
  widget clk {
    text = "x";
    tooltip = "{PCT}%";
  }
}

// notifications().history is a list — only a `for` head may name it. Reading
// it as a value used to compile to nonsense.
source n = notifications();
surface bar {
  widget x { text = n.history; }
}

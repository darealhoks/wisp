// `sticky` pins a row above the scrolled region, so only the LEADING rows can
// take it — a pinned row below scrolling ones would be overrun by them.
surface panel {
  axis = vertical;
  scroll = rows;
  widget a { text = "a"; }
  widget b { sticky; text = "b"; }
}

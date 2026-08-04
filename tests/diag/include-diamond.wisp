// Diamond: both halves include the same fragment; include-once means
// FRAG_BG is declared exactly once.
include "include-diamond-a.wisp";
include "include-diamond-b.wisp";
const BG = FRAG_BG;
mut tint = BG;

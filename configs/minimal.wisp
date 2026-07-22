//! font_backend = baked

source time = clock("%H:%M");
source cpu  = cpu();
source mem  = mem();

surface bar {
  layer = overlay;
  anchor = top;
  width = 240;
  height = 28;
  margin = 2;
  exclusive_zone = 25;
  bg = #ff0f1219;
  radius = 14;

  widget cpu_w { align = left;   text = " cpu: {cpu.pct}%"; }
  widget clock { align = center; text = time; }
  widget mem_w { align = right;  text = " mem: {mem.pct}%"; }
}

widget { fg = #ffd6dae1; pad = 10; }

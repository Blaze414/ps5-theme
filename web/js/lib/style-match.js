"use strict";

/* Whether an icon (RGBA bytes, w x h) is one the style made: outside the shape `mask` (its alpha), clear of the edge
   by `edge` pixels, the same as the template's backdrop `base` (RGBA of the same size). */
function styleMatch(icon, base, mask, w, h, edge = 2) {
  let n = 0, diff = 0;
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      let near = false;
      for (let dy = -edge; dy <= edge && !near; dy++) {
        for (let dx = -edge; dx <= edge && !near; dx++) {
          const xx = x + dx, yy = y + dy;
          near = xx >= 0 && yy >= 0 && xx < w && yy < h && mask[(yy * w + xx) * 4 + 3] > 0;
        }
      }
      if (near) continue;
      const i = (y * w + x) * 4;
      diff += Math.abs(icon[i] - base[i]) + Math.abs(icon[i + 1] - base[i + 1]) + Math.abs(icon[i + 2] - base[i + 2]);
      n++;
    }
  }
  /* enough of the backdrop to tell (a shape filling the square leaves too little), and the same but for rounding */
  return n >= w * h * 0.03 && diff / (n * 3) < 6;
}

// Regenerates pov_strip/pov_image.h for the shipped config.h (WS2812B, 2 m x 60/m)
// through the same code path the Studio uses. Heart only.
//   node tools/gen_default.js
const path = require("path");
const fs = require("fs");
const ROOT = path.join(__dirname, "..");
const P = require(path.join(ROOT, "studio", "core.js"));
const s = { lengthM: 2, density: 60, distM: 5, sweepDps: 300, glanceDeg: 25, colMode: "auto", colUsManual: 0, chip: "WS2812B", spiMHz: 16 };
const d = P.design(s);
const glanceMs = s.glanceDeg / s.sweepDps * 1000;
const gamma = 2.4;
const slides = [["heart", 0.8, "Heart"]].map(([key, scale, label]) => {
  const cols = P.columnsFor(d, P.patterns[key].aspect, scale);
  const passMs = cols * d.colUs / 1000;
  return { label, rows: d.n, cols, rgb: P.rasterPattern(key, d.n, cols, scale, gamma), holdS: 10,
           gapUs: Math.round(P.recommendedGapMs(glanceMs, passMs) * 1000) };
});
const lines = [
  `Design: ${s.lengthM.toFixed(2)} m x ${s.density}/m = ${d.n} LEDs, WS2812B (chip-limited: ${d.chipUs} us per column)`,
  `Viewer ${s.distM} m, eye ${s.sweepDps} deg/s, glance ${s.glanceDeg} deg`,
  `Column ${d.colUs} us (${Math.round(1e6 / d.colUs)} Hz), direction ltr, gap per slide`,
  `Default demo for the shipped config.h. Replace with an export from POV Strip Studio.`,
];
const hdr = P.makeHeader(slides, { colUs: d.colUs, gapUs: 0, dir: 0, lengthM: s.lengthM, sweepDps: s.sweepDps, lines, date: new Date().toISOString().slice(0, 10) });
fs.writeFileSync(path.join(ROOT, "pov_strip", "pov_image.h"), hdr);
console.log("n", d.n, "colUs", d.colUs, slides.map((x) => `${x.label}: ${x.cols} cols, gap ${x.gapUs} us`).join("; "), "bytes", hdr.length);

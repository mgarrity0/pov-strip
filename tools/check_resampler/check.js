// Cross-checks the firmware's integer resampler (resampleImage in pov_resample.h,
// compiled for the host) against the Studio's float one (POV.areaResample).
//   cd tools/check_resampler && node check.js
// Expect max |C - JS| <= 2 (8-bit rounding of fixed vs float).
const path = require("path"), fs = require("fs"), cp = require("child_process");
const ROOT = path.join(__dirname, "..", "..");
const P = require(path.join(ROOT, "studio", "core.js"));
const hdr = fs.readFileSync(path.join(ROOT, "pov_strip", "pov_resample.h"), "utf8");
const m = hdr.match(/static uint16_t s_lin16\[256\];[\s\S]*$/);
if (!m) throw new Error("resampler block not found in pov_resample.h");
fs.writeFileSync(path.join(__dirname, "resample_extract.inc"), m[0]);
cp.execFileSync("g++", ["-O2", "-o", path.join(__dirname, "host"), path.join(__dirname, "host.cpp")], { cwd: __dirname, stdio: "inherit" });
function cmp(SR, SC, rows, cols, gamma, gen) {
  const src = new Uint8Array(SR * SC * 3);   // column-major, as in pov_image.h
  for (let c = 0; c < SC; c++) for (let r = 0; r < SR; r++) { const p = gen(c, r), o = (c * SR + r) * 3; src[o] = p[0]; src[o + 1] = p[1]; src[o + 2] = p[2]; }
  fs.writeFileSync(path.join(__dirname, "src.bin"), src);
  cp.execFileSync(path.join(__dirname, "host"), [path.join(__dirname, "src.bin"), String(SR), String(SC), String(rows), String(cols), String(gamma), path.join(__dirname, "out.bin")]);
  const c = fs.readFileSync(path.join(__dirname, "out.bin"));
  const rm = new Uint8Array(SR * SC * 3);     // row-major for areaResample
  for (let cc = 0; cc < SC; cc++) for (let r = 0; r < SR; r++) for (let k = 0; k < 3; k++) rm[(r * SC + cc) * 3 + k] = src[(cc * SR + r) * 3 + k];
  const j = P.areaResample(rm, 3, SC, SR, cols, rows, 0, rows, gamma);
  let maxd = 0; for (let i = 0; i < j.length; i++) maxd = Math.max(maxd, Math.abs(j[i] - c[i]));
  console.log(`${SR}x${SC} -> ${rows}x${cols} gamma ${gamma}: max |C - JS| = ${maxd}`);
  if (maxd > 2) process.exitCode = 1;
}
const noise = (c, r) => [(c * 37 + r * 11) % 256, (c * 5 + r * 101) % 256, (c * r) % 256];
cmp(120, 96, 120, 16, 2.4, noise); cmp(120, 120, 60, 20, 2.4, noise); cmp(210, 168, 120, 96, 2.2, noise);
cmp(120, 96, 300, 240, 2.4, noise); cmp(300, 40, 120, 96, 2.4, noise); cmp(7, 13, 5, 3, 1.0, noise);

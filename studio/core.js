// core.js - pure logic for POV Strip Studio (no DOM). Shared by the page and
// the Node tests. Every timing formula here mirrors pov_strip.ino / led_out.h.
"use strict";

const POV = {};

// ---- chip table ------------------------------------------------------------------
// pwmHz: null = not verified; the page asks for it. Sources in the page footer.
POV.CHIPS = {
  APA102:     { label: "APA102",     clocked: true,  pwmHz: 19200, pwmSrc: "19.2 kHz measured (cpldcpu)", maxMHz: null, current: false, skew: true,  order: "BGR" },
  SK9822:     { label: "SK9822",     clocked: true,  pwmHz: 4700,  pwmSrc: "4.7 kHz measured (cpldcpu); one datasheet lists 1.2 kHz", maxMHz: 30, current: true, skew: false, order: "BGR" },
  HD107S:     { label: "HD107S",     clocked: true,  pwmHz: 26000, pwmSrc: "26 kHz (datasheet)", maxMHz: 30, current: true, skew: null, order: "BGR" },
  WS2812B:    { label: "WS2812B",    clocked: false, bitNs: 1250, resetUs: 280, pwmHz: 430,  pwmSrc: "~430 Hz on older parts (cpldcpu)", order: "GRB" },
  WS2812B_V5: { label: "WS2812B-V5", clocked: false, bitNs: 1225, resetUs: 280, pwmHz: 2000, pwmSrc: "2 kHz refresh (datasheet)", order: "GRB" },
  SK6812:     { label: "SK6812",     clocked: false, bitNs: 1200, resetUs: 80,  pwmHz: 1100, pwmSrc: "1.1 kHz measured (cpldcpu)", order: "GRB" },
  WS2811:     { label: "WS2811",     clocked: false, bitNs: 1250, resetUs: 280, pwmHz: null, pwmSrc: "not verified: enter from your datasheet", order: "RGB" },
  WS2813:     { label: "WS2813",     clocked: false, bitNs: 1275, resetUs: 300, pwmHz: null, pwmSrc: "not verified: enter from your datasheet", order: "GRB" },
  WS2815:     { label: "WS2815",     clocked: false, bitNs: 1900, resetUs: 300, pwmHz: null, pwmSrc: "not verified: enter from your datasheet", order: "GRB" },
};

POV.DIRS = { ltr: 0, rtl: 1, alt: 2, pal: 3 };
POV.DIR_MACRO = ["POV_DIR_LTR", "POV_DIR_RTL", "POV_DIR_ALT", "POV_DIR_PAL"];
POV.MODES = { full: 0, safe: 1, current: 2 };
POV.MODE_MACRO = ["RENDER_FULL", "RENDER_PWM_SAFE", "RENDER_CURRENT"];

const DEG = 180 / Math.PI;

// ---- strip and wire timing (matches led_out.h) --------------------------------------
POV.ledCount = (lengthM, density, nMax = 1e9) =>
  Math.min(nMax, Math.max(1, Math.round(lengthM * density)));

POV.frameBytes = (n) => {
  const b = 4 + 4 * n + 4 + (Math.floor((n + 15) / 16) + 1);
  return (b + 3) & ~3;
};

// ESP32 SPI from an 80 MHz source divides by an integer k; the driver picks the
// closest. The firmware prints the real value ("info").
POV.actualSpiHz = (mhz) => 80e6 / Math.max(1, Math.round(80 / Math.max(0.01, mhz)));

POV.chipMinColUs = (chipKey, n, spiMHz, resetOverrideUs = 0) => {
  const c = POV.CHIPS[chipKey];
  if (c.clocked) {
    const us = POV.frameBytes(n) * 8 * 1e6 / POV.actualSpiHz(spiMHz);
    return Math.ceil(us) + 25;
  }
  const reset = resetOverrideUs > 0 ? resetOverrideUs : c.resetUs;
  return Math.floor(n * 24 * c.bitNs / 1000) + reset + 25;
};

// Time from the first LED's update to the last LED's (APA102 updates as data passes).
POV.skewUs = (chipKey, n, spiMHz) => {
  const c = POV.CHIPS[chipKey];
  if (!c.clocked || c.skew === false) return 0;
  return (32 * n) * 1e6 / POV.actualSpiHz(spiMHz);
};

// ---- viewing geometry ----------------------------------------------------------------
// s: settings {lengthM, density, distM, sweepDps, glanceDeg, colMode:'auto'|'manual',
//              colUsManual, chip, spiMHz, gapMode:'auto'|'manual', gapMsManual, dir}
POV.design = (s) => {
  const n = POV.ledCount(s.lengthM, s.density);
  const pitch = 1 / s.density;                       // m
  const rowDeg = 2 * Math.atan(pitch / (2 * s.distM)) * DEG;
  const stripDeg = 2 * Math.atan(s.lengthM / (2 * s.distM)) * DEG;
  const sqUs = rowDeg / s.sweepDps * 1e6;             // square pixels
  const wantUs = s.colMode === "manual" ? Math.max(1, s.colUsManual) : Math.round(sqUs);
  const chipUs = POV.chipMinColUs(s.chip, n, s.spiMHz);
  const colUs = Math.max(wantUs, chipUs);
  const colDeg = s.sweepDps * colUs * 1e-6;           // angle one column covers
  const glanceMs = s.glanceDeg / s.sweepDps * 1e3;
  return { n, pitch, rowDeg, stripDeg, sqUs, wantUs, chipUs, colUs, colDeg, glanceMs,
           chipLimited: wantUs < chipUs, maxColHz: 1e6 / chipUs };
};

// Columns for content of aspect `aspect` (width / height) drawn at `scale` of strip height.
POV.columnsFor = (d, aspect, scale) =>
  Math.max(1, Math.min(8192, Math.round(aspect * scale * d.n * d.rowDeg / d.colDeg)));

// Pass length in columns for a direction mode, and the full cycle period.
POV.cycle = (dir, cols, colUs, gapUs) => {
  const T = cols * colUs;
  if (dir === POV.DIRS.alt) return { passUs: T, periodUs: 2 * (T + gapUs) };
  if (dir === POV.DIRS.pal) return { passUs: (2 * cols - 1) * colUs, periodUs: (2 * cols - 1) * colUs + gapUs };
  return { passUs: T, periodUs: T + gapUs };
};

// Displayed column (0..cols-1) at time t (us) since cycle start, or -1 when dark.
// Same order as runCycle(): LTR plays the right-most column first.
POV.columnAt = (t, dir, cols, colUs, gapUs) => {
  const T = cols * colUs;
  if (dir === POV.DIRS.alt) {
    const P = 2 * (T + gapUs);
    let u = ((t % P) + P) % P;
    if (u < T) return cols - 1 - Math.floor(u / colUs);
    u -= T + gapUs;
    if (u >= 0 && u < T) return Math.floor(u / colUs);
    return -1;
  }
  if (dir === POV.DIRS.pal) {
    const L = (2 * cols - 1) * colUs, P = L + gapUs;
    const u = ((t % P) + P) % P;
    if (u >= L) return -1;
    const k = Math.floor(u / colUs);
    return k < cols ? k : 2 * cols - 2 - k;
  }
  const P = T + gapUs;
  const u = ((t % P) + P) % P;
  if (u >= T) return -1;
  const k = Math.floor(u / colUs);
  return dir === POV.DIRS.rtl ? k : cols - 1 - k;
};

// Chance that one glance of duration D shows exactly one complete image and no
// fragments, for a pass of length T repeating every T + G.
POV.cleanChance = (D, T, G) => {
  if (D <= T) return 0;
  const p = G >= D - T ? (D - T) / (T + G) : (T + 2 * G - D) / (T + G);
  return Math.max(0, Math.min(1, p));
};
POV.recommendedGapMs = (glanceMs, passMs) => Math.max(5, glanceMs - passMs);

// ---- LED output model (matches ledout::setRender / build) ---------------------------
// Returns per-channel {lin, duty}: lin = emitted light 0..1 averaged; duty = PWM on-fraction.
POV.makeRender = (chipKey, mode, bri, gamma) => {
  const c = POV.CHIPS[chipKey];
  const current = !!c.current;
  if (mode === POV.MODES.current && !current) mode = POV.MODES.safe;
  const scale8 = current ? 255 : 255 * bri / 255;
  const lut8 = new Uint8Array(256), lut5 = new Uint8Array(256);
  for (let v = 0; v < 256; v++) {
    const g = Math.pow(v / 255, gamma);
    lut8[v] = Math.round(g * scale8);
    lut5[v] = Math.round(g * 31 * bri / 255);
  }
  let gb = current ? Math.round(31 * bri / 255) : 31;
  if (current && bri > 0 && gb === 0) gb = 1;
  const on = 255;  // PWM-safe: full on or off, so no PWM gaps inside a column
  return { mode, current, lut8, lut5, gb, on };
};

// out: Float32Array(6) = [duty r,g,b, level r,g,b]; light = duty * level.
POV.pixelOut = (R, r, g, b, out) => {
  let dr, dg, db, lv;
  if (R.mode === POV.MODES.full) {
    dr = R.lut8[r]; dg = R.lut8[g]; db = R.lut8[b]; lv = R.current ? R.gb / 31 : 1;
  } else if (R.mode === POV.MODES.safe) {
    dr = r >= 128 ? R.on : 0; dg = g >= 128 ? R.on : 0; db = b >= 128 ? R.on : 0;
    lv = R.current ? R.gb / 31 : 1;
  } else {
    const m = Math.max(r, g, b), l = R.lut5[m];
    if (!l) { dr = dg = db = 0; lv = 0; }
    else { dr = 2 * r >= m ? 255 : 0; dg = 2 * g >= m ? 255 : 0; db = 2 * b >= m ? 255 : 0; lv = l / 31; }
  }
  out[0] = dr / 256; out[1] = dg / 256; out[2] = db / 256;
  out[3] = out[4] = out[5] = lv;
  return out;
};

// ---- image stats ---------------------------------------------------------------------
// rgb: column-major Uint8Array, cols*rows*3. Returns peak/mean light fraction per column
// (sum over LEDs and channels of duty*level / (3*rows)).
POV.columnLoad = (rgb, rows, cols, R) => {
  const o = new Float32Array(6);
  let peak = 0, sum = 0;
  for (let c = 0; c < cols; c++) {
    let acc = 0;
    const base = c * rows * 3;
    for (let i = 0; i < rows; i++) {
      const p = base + i * 3;
      POV.pixelOut(R, rgb[p], rgb[p + 1], rgb[p + 2], o);
      acc += o[0] * o[3] + o[1] * o[4] + o[2] * o[5];
    }
    acc /= 3 * rows;
    if (acc > peak) peak = acc;
    sum += acc;
  }
  return { peak, mean: cols ? sum / cols : 0 };
};

// ---- export: pov_image.h ---------------------------------------------------------------
POV.cIdent = (s) => s.replace(/[^A-Za-z0-9_]/g, "_");
POV.cComment = (s) => String(s).replace(/[\r\n]+/g, " ").replace(/\*\//g, "* /").slice(0, 120);

// slides: [{label, rgb, rows, cols, holdS, gapUs?}]; meta: {colUs, gapUs, dir, lengthM, lines:[...]}
POV.makeHeader = (slides, meta) => {
  const out = [];
  out.push("// pov_image.h - exported by POV Strip Studio" + (meta.date ? ", " + meta.date : ""));
  for (const l of meta.lines || []) out.push("// " + POV.cComment(l));
  out.push("// Column-major RGB888: column c starts at c*rows*3. Row 0 is the top of the image.");
  out.push("#pragma once");
  out.push('#include "pov_types.h"');
  out.push("");
  const hex = [];
  for (let i = 0; i < 256; i++) hex.push("0x" + (i < 16 ? "0" : "") + i.toString(16));
  slides.forEach((sl, k) => {
    out.push(`// ${k + 1}: ${POV.cComment(sl.label)} (${sl.rows} x ${sl.cols})`);
    out.push(`static const uint8_t pov_img_${k}[] = {`);
    const d = sl.rgb, perLine = 24;
    for (let i = 0; i < d.length; i += perLine) {
      const end = Math.min(d.length, i + perLine);
      let line = "  ";
      for (let j = i; j < end; j++) line += hex[d[j]] + ",";
      out.push(line);
    }
    out.push("};");
    out.push("");
  });
  out.push("static const PovImage POV_IMAGES[] = {");
  slides.forEach((sl, k) => {
    const gap = sl.gapUs !== undefined ? sl.gapUs : meta.gapUs;
    out.push(`  {pov_img_${k}, ${sl.rows}, ${sl.cols}, ${Math.round(meta.colUs)}u, ${Math.round(gap)}u, ` +
             `${POV.DIR_MACRO[meta.dir]}, ${meta.lengthM.toFixed(2)}f, ${Math.max(0, Math.round(sl.holdS))}, ` +
             `${Math.max(0, Math.round(meta.sweepDps || 0))}},`);
  });
  out.push("};");
  out.push("");
  return out.join("\n");
};

// ---- export: config.h from the template ------------------------------------------------
POV.setDefine = (src, name, value) => {
  const re = new RegExp("^(#define\\s+" + name + "\\s+)(\\S+)", "m");
  if (!re.test(src)) throw new Error("config template has no #define " + name);
  return src.replace(re, (m, a) => a + value);
};

POV.makeConfig = (tpl, c) => {
  let s = tpl;
  s = POV.setDefine(s, "POV_CHIP", "LED_" + c.chip);
  s = POV.setDefine(s, "POV_LENGTH_M", c.lengthM.toFixed(2) + "f");
  s = POV.setDefine(s, "POV_LEDS_PER_M", String(c.density));
  s = POV.setDefine(s, "POV_NUM_LEDS_MAX", String(Math.max(720, c.n)));
  s = POV.setDefine(s, "POV_DATA_IN_AT_BOTTOM", c.dataInAtBottom ? "1" : "0");
  s = POV.setDefine(s, "POV_PIN_DATA", String(c.pinData));
  s = POV.setDefine(s, "POV_PIN_CLOCK", String(c.pinClock));
  s = POV.setDefine(s, "POV_SPI_MHZ", String(+c.spiMHz.toFixed(3)));
  s = POV.setDefine(s, "POV_CLOCKED_ORDER", '"' + c.order + '"');
  s = POV.setDefine(s, "POV_SINGLE_ORDER", '"' + c.order + '"');
  s = POV.setDefine(s, "POV_BRIGHTNESS", String(c.bri));
  s = POV.setDefine(s, "POV_GAMMA", c.gamma.toFixed(2) + "f");
  s = POV.setDefine(s, "POV_RENDER", POV.MODE_MACRO[c.mode]);
  return s;
};

// ---- zip (stored, no compression) -------------------------------------------------------
const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();
POV.crc32 = (bytes) => {
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) c = CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
};

POV.zip = (files, when = new Date()) => {
  const enc = new TextEncoder();
  const dosTime = (when.getHours() << 11) | (when.getMinutes() << 5) | (when.getSeconds() >> 1);
  const dosDate = ((when.getFullYear() - 1980) << 9) | ((when.getMonth() + 1) << 5) | when.getDate();
  const parts = [], central = [];
  let offset = 0;
  for (const f of files) {
    const name = enc.encode(f.name);
    const data = typeof f.data === "string" ? enc.encode(f.data) : f.data;
    const crc = POV.crc32(data);
    const h = new DataView(new ArrayBuffer(30));
    h.setUint32(0, 0x04034b50, true); h.setUint16(4, 20, true); h.setUint16(6, 0x0800, true);
    h.setUint16(8, 0, true); h.setUint16(10, dosTime, true); h.setUint16(12, dosDate, true);
    h.setUint32(14, crc, true); h.setUint32(18, data.length, true); h.setUint32(22, data.length, true);
    h.setUint16(26, name.length, true); h.setUint16(28, 0, true);
    parts.push(new Uint8Array(h.buffer), name, data);
    const c = new DataView(new ArrayBuffer(46));
    c.setUint32(0, 0x02014b50, true); c.setUint16(4, 20, true); c.setUint16(6, 20, true);
    c.setUint16(8, 0x0800, true); c.setUint16(10, 0, true); c.setUint16(12, dosTime, true);
    c.setUint16(14, dosDate, true); c.setUint32(16, crc, true); c.setUint32(20, data.length, true);
    c.setUint32(24, data.length, true); c.setUint16(28, name.length, true); c.setUint16(30, 0, true);
    c.setUint16(32, 0, true); c.setUint16(34, 0, true); c.setUint16(36, 0, true);
    c.setUint32(38, f.name.endsWith("/") ? 0x10 : 0, true); c.setUint32(42, offset, true);
    central.push(new Uint8Array(c.buffer), name);
    offset += 30 + name.length + data.length;
  }
  const cdSize = central.reduce((a, p) => a + p.length, 0);
  const e = new DataView(new ArrayBuffer(22));
  e.setUint32(0, 0x06054b50, true); e.setUint16(8, files.length, true); e.setUint16(10, files.length, true);
  e.setUint32(12, cdSize, true); e.setUint32(16, offset, true);
  const all = [...parts, ...central, new Uint8Array(e.buffer)];
  const out = new Uint8Array(all.reduce((a, p) => a + p.length, 0));
  let o = 0;
  for (const p of all) { out.set(p, o); o += p.length; }
  return out;
};

// ---- procedural test patterns (rgb column-major, rows x cols) ----------------------------
// Drawn in square "design" space: u in [0,1) across, v in [0,1) down, aspect = cols/rows.
POV.patterns = {
  ring: { label: "Ring", aspect: 1, px(u, v) {
    const x = u * 2 - 1, y = v * 2 - 1, r = Math.hypot(x, y);
    if (Math.abs(r - 0.82) < 0.07) return [255, 255, 255];
    if (Math.abs(x) < 0.035 && r < 0.75) return [255, 40, 40];
    if (Math.abs(y) < 0.035 && r < 0.75) return [40, 120, 255];
    return [0, 0, 0];
  } },
  heart: { label: "Heart", aspect: 1.0, px(u, v) {
    const x = (u - 0.5) * 2.35, y = 1.29 - 2.35 * v;
    const f = Math.pow(x * x + y * y - 1, 3) - x * x * y * y * y;
    if (f > 0) return [0, 0, 0];
    const t = Math.min(1, Math.max(0, v));
    return [255, Math.round(30 + 120 * t), Math.round(140 - 120 * t)];
  } },
  eye: { label: "Eye", aspect: 1.8, px(u, v) {
    // almond lids, teal iris with spokes and a dark limbal ring, black pupil, one catchlight
    const X = (u - 0.5) * 3.6, Y = (v - 0.5) * 2;   // height 2 units, width 3.6
    const t = X / 1.78;
    if (Math.abs(t) >= 1) return [0, 0, 0];
    const lid = 0.95 * Math.pow(1 - t * t, 0.72);
    if (Math.abs(Y) >= lid) return [0, 0, 0];
    if (Math.hypot(X + 0.2, Y + 0.22) < 0.1) return [255, 255, 255];
    const r = Math.hypot(X, Y);
    if (r < 0.26) return [0, 0, 0];
    if (r < 0.62) {
      const a = Math.atan2(Y, X), k = (r - 0.26) / 0.36;
      const spoke = 0.75 + 0.25 * Math.sin(a * 14 + k * 6);
      const b = (1 - 0.35 * k) * spoke * (r > 0.56 ? 0.45 : 1);
      return [Math.round(40 * b), Math.round(170 * b), Math.round(200 * b)];
    }
    const e = Math.abs(Y) / lid, s = 225 - 60 * e * e;   // sclera, greyer toward the lids
    return [Math.round(s), Math.round(s), Math.round(s * 0.96)];
  } },
  chevrons: { label: "Rainbow chevrons", aspect: 0.5, px(u, v) {
    const k = (v + Math.abs(u - 0.5) * 0.9) * 6;
    if ((Math.floor(k) & 1)) return [0, 0, 0];
    const h = (v * 300) % 360;
    return POV.hsv(h, 1, 1);
  } },
  grid: { label: "Grid", aspect: 1, px(u, v) {
    const gx = Math.abs(((u * 8) % 1) - 0.5) > 0.44, gy = Math.abs(((v * 8) % 1) - 0.5) > 0.44;
    return gx || gy ? [200, 200, 200] : [0, 0, 0];
  } },
  bars: { label: "Color bars", aspect: 0.6, px(u, v) {
    const cs = [[255,255,255],[255,255,0],[0,255,255],[0,255,0],[255,0,255],[255,0,0],[0,0,255]];
    return cs[Math.min(6, Math.floor(v * 7))];
  } },
};

POV.hsv = (h, s, v) => {
  const f = (n) => { const k = (n + h / 60) % 6; return v - v * s * Math.max(0, Math.min(k, 4 - k, 1)); };
  return [Math.round(f(5) * 255), Math.round(f(3) * 255), Math.round(f(1) * 255)];
};

// ---- resampling ------------------------------------------------------------------------
// 1-D area weights: target cell k covers source [k*S/T, (k+1)*S/T). Overlaps are exact
// integers in units of 1/T; weights are normalized to sum to 1 per target cell.
// Same scheme as resampleImage() in the firmware.
POV.areaSpans = (S, T) => {
  const out = new Array(T);
  for (let k = 0; k < T; k++) {
    const a = k * S, b = (k + 1) * S;
    const i0 = Math.floor(a / T), i1 = Math.min(S - 1, Math.floor((b - 1) / T));
    const w = new Float32Array(i1 - i0 + 1);
    for (let i = i0; i <= i1; i++) w[i - i0] = (Math.min(b, (i + 1) * T) - Math.max(a, i * T)) / S;
    out[k] = { i0, w };
  }
  return out;
};

// Area-average resample in linear light (through the LED output gamma), so a thin
// line that covers a fraction of a target pixel keeps its share of the emitted
// energy instead of vanishing or snapping to full strength.
// src: row-major, `stride` bytes per pixel (3 RGB or 4 RGBA), sw x sh.
// Output: column-major RGB, dw x dh; the source fills columns [0, dw) and rows
// [top, top + dh2), the rest stays black. nearest: sample centers (pixel art).
POV.areaResample = (src, stride, sw, sh, dw, dh, top, dh2, gamma, nearest = false) => {
  const out = new Uint8Array(dw * dh * 3);
  if (sw < 1 || sh < 1 || dw < 1 || dh2 < 1) return out;
  const lin = new Float32Array(256);
  for (let v = 0; v < 256; v++) lin[v] = Math.pow(v / 255, gamma);
  const enc = (x) => Math.round(255 * Math.pow(Math.min(1, Math.max(0, x)), 1 / gamma));
  const xs = POV.areaSpans(sw, dw), ys = POV.areaSpans(sh, dh2);
  for (let kc = 0; kc < dw; kc++) {
    const X = xs[kc];
    for (let kr = 0; kr < dh2; kr++) {
      let r = 0, g = 0, b = 0;
      if (nearest) {
        const i = Math.min(sw - 1, Math.floor((kc + 0.5) * sw / dw));
        const j = Math.min(sh - 1, Math.floor((kr + 0.5) * sh / dh2));
        const p = (j * sw + i) * stride;
        r = lin[src[p]]; g = lin[src[p + 1]]; b = lin[src[p + 2]];
      } else {
        const Y = ys[kr];
        for (let jj = 0; jj < Y.w.length; jj++) {
          const wy = Y.w[jj], rowBase = (Y.i0 + jj) * sw;
          for (let ii = 0; ii < X.w.length; ii++) {
            const w = wy * X.w[ii], p = (rowBase + X.i0 + ii) * stride;
            r += w * lin[src[p]]; g += w * lin[src[p + 1]]; b += w * lin[src[p + 2]];
          }
        }
      }
      const o = (kc * dh + top + kr) * 3;
      out[o] = enc(r); out[o + 1] = enc(g); out[o + 2] = enc(b);
    }
  }
  return out;
};

// Rasterize a pattern into rows x cols (column-major) at `scale` of the height,
// centered: drawn supersampled, then area-averaged in linear light.
POV.rasterPattern = (key, rows, cols, scale, gamma = 2.4) => {
  const pat = POV.patterns[key];
  const dh2 = Math.max(1, Math.min(rows, Math.round(scale * rows))), top = Math.floor((rows - dh2) / 2);
  const ss = Math.max(1, Math.min(6, Math.floor(Math.sqrt(1.5e6 / (cols * dh2)))));
  const sw = cols * ss, sh = dh2 * ss;
  const src = new Uint8Array(sw * sh * 3);
  for (let j = 0; j < sh; j++) for (let i = 0; i < sw; i++) {
    const p = pat.px((i + 0.5) / sw, (j + 0.5) / sh), o = (j * sw + i) * 3;
    src[o] = p[0]; src[o + 1] = p[1]; src[o + 2] = p[2];
  }
  return POV.areaResample(src, 3, sw, sh, cols, rows, top, dh2, gamma);
};

// ---- perception model ------------------------------------------------------------------
// Light from the strip lands on the retina at angle rAt(t) (deg, + = right of gaze).
// Each step spreads that column's light over the pixels swept during the step, so
// energy is conserved: a slow sweep piles light up, a fast one spreads it thin.
// Exposure E makes a full-on pixel swept at the design speed read 1.0.
//
// P: { W, H, xMin, pxPerDeg, rows, cols, colUs, gapUs, dir, phaseUs, dt, E,
//      rowLed: Int32Array(H)  image row per canvas row, -1 = no LED,
//      wireIdx: Int32Array(H) position along the data line (for update skew),
//      skewPerLedUs, duty: Float32Array(cols*rows*3), level: Float32Array(cols*rows),
//      pwmUs (0 = PWM not modeled), ledPhase: Float32Array(rows) in us,
//      rAt(tUs) -> deg }
POV.simAccumulate = (P, acc, t0, t1) => {
  const W = P.W, H = P.H, rows = P.rows;
  const wts = new Float32Array(W + 2);
  for (let t = t0; t < t1; t += P.dt) {
    const dt = Math.min(P.dt, t1 - t);
    const x0 = (P.rAt(t) - P.xMin) * P.pxPerDeg, x1 = (P.rAt(t + dt) - P.xMin) * P.pxPerDeg;
    let a = Math.min(x0, x1), b = Math.max(x0, x1);
    if (b < 0 || a >= W) continue;
    if (b - a < 1e-4) b = a + 1e-4;
    const span = b - a;
    const ia = Math.max(0, Math.floor(a)), ib = Math.min(W - 1, Math.floor(b));
    for (let px = ia; px <= ib; px++) {
      const cov = Math.min(b, px + 1) - Math.max(a, px);
      wts[px - ia] = cov > 0 ? cov / span : 0;
    }
    const tm = t + dt / 2 + P.phaseUs;
    const shared = P.skewPerLedUs ? -2 : POV.columnAt(tm, P.dir, P.cols, P.colUs, P.gapUs);
    if (shared === -1) continue;
    const e0 = dt * P.E;
    for (let y = 0; y < H; y++) {
      const row = P.rowLed[y];
      if (row < 0) continue;
      let ts = tm, col = shared;
      if (shared === -2) {
        ts = tm - P.skewPerLedUs * (P.wireIdx[y] + 1);
        col = POV.columnAt(ts, P.dir, P.cols, P.colUs, P.gapUs);
        if (col < 0) continue;
      }
      const pi = col * rows + row;
      const lv = P.level[pi];
      if (!lv) continue;
      let r = P.duty[pi * 3], g = P.duty[pi * 3 + 1], bl = P.duty[pi * 3 + 2];
      if (P.pwmUs) {
        const ph = (((ts + P.ledPhase[row]) % P.pwmUs) + P.pwmUs) % P.pwmUs / P.pwmUs;
        r = ph < r ? 1 : 0; g = ph < g ? 1 : 0; bl = ph < bl ? 1 : 0;
      }
      if (!(r || g || bl)) continue;
      const e = e0 * lv, base = y * W;
      for (let px = ia; px <= ib; px++) {
        const w = wts[px - ia] * e;
        if (!w) continue;
        const o = (base + px) * 3;
        acc[o] += r * w; acc[o + 1] += g * w; acc[o + 2] += bl * w;
      }
    }
  }
};

// Glance gaze path: A deg over durMs, rightward if dirSign > 0. Returns rAt(tUs) for the
// strip (r = -gaze). profile 'real' = minimum-jerk (peak 1.875x mean), else constant.
POV.glancePath = (A, durMs, dirSign, profile) => {
  const D = durMs * 1000;
  return (t) => {
    const tau = Math.min(1, Math.max(0, t / D));
    const s = profile === "real" ? tau * tau * tau * (10 - 15 * tau + 6 * tau * tau) : tau;
    const gaze = (s - 0.5) * A * dirSign;
    return -gaze;
  };
};
POV.shakePath = (A, hz) => (t) => -(A / 2) * Math.sin(2 * Math.PI * hz * t * 1e-6);

if (typeof module !== "undefined") module.exports = POV;

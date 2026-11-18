// SPDX-License-Identifier: GPL-3.0-only
// Screenshots of the DrumBox emulator's 240 x 240 screen, as PNG files, after a few gestures: to look at the UI
// without a browser.   node web/emu/drumbox_shot.mjs [out_dir] [build/emu-drumbox/felucca.wasm]
import fs from "fs";
import zlib from "zlib";

const out = process.argv[2] || ".", wasm = process.argv[3] || "build/emu-drumbox/felucca.wasm";
const BLACK = [1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25], WHITE = [0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26];
const EN = { SELECT: 0, ALGO: 1, PRESETS: 2, K1: 3, K2: 4, K3: 5, K4: 6 };
const B = { FX: 0, SCL: 1, EDIT: 4, GLO: 5, HOME: 6, SAVE: 7, SEQ: 9, PLAY: 10 };

const { instance } = await WebAssembly.instantiate(fs.readFileSync(wasm), {});
const ex = instance.exports, mem = ex.memory;
ex._initialize(); ex.web_nor_erase(); ex.web_boot();
const render = (ms) => { for (let n = Math.round(ms * 44.1), k = 0; k < n; k += 128) ex.web_render(Math.min(128, n - k)); };
const key = (k, ms = 80) => { ex.web_keys(1 << k); render(30); ex.web_keys(0); render(ms); };
const turn = (role, n) => { for (let i = 0; i < Math.abs(n); i++) { ex.web_enc(role, Math.sign(n)); render(40); } };

function crc32(buf) {
  let c, crc = 0xFFFFFFFF;
  for (let i = 0; i < buf.length; i++) { c = (crc ^ buf[i]) & 0xFF; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; crc = (crc >>> 8) ^ c; }
  return (crc ^ 0xFFFFFFFF) >>> 0;
}
function png(name, scale = 2) {
  const fb = new Uint16Array(mem.buffer, ex.web_screen(), 240 * 240), W = 240 * scale, rows = [];
  for (let y = 0; y < 240; y++) {
    const row = Buffer.alloc(1 + W * 3);
    for (let x = 0; x < 240; x++) {
      const sw = fb[y * 240 + x], c = ((sw & 0xFF) << 8) | (sw >> 8);
      const r = ((c >> 11) & 31) * 255 / 31, g = ((c >> 5) & 63) * 255 / 63, b = (c & 31) * 255 / 31;
      for (let s = 0; s < scale; s++) { row[1 + (x * scale + s) * 3] = r; row[2 + (x * scale + s) * 3] = g; row[3 + (x * scale + s) * 3] = b; }
    }
    for (let s = 0; s < scale; s++) rows.push(row);
  }
  const chunk = (t, d) => { const b = Buffer.concat([Buffer.from(t), d]), l = Buffer.alloc(4), c = Buffer.alloc(4); l.writeUInt32BE(d.length); c.writeUInt32BE(crc32(b)); return Buffer.concat([l, b, c]); };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(W, 0); ihdr.writeUInt32BE(240 * scale, 4); ihdr[8] = 8; ihdr[9] = 2;
  fs.writeFileSync(`${out}/${name}.png`, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr), chunk("IDAT", zlib.deflateSync(Buffer.concat(rows))), chunk("IEND", Buffer.alloc(0))]));
}

const tap = (b) => { ex.web_buttons(1 << b); render(60); ex.web_buttons(0); render(120); };
render(300);
png("0-splash");
render(700);
png("1-boot");
key(BLACK[0]); [0, 4, 8, 12].forEach((st) => key(WHITE[st], 20));          // a kick on the beats, softer at the last
turn(EN.SELECT, -8);
key(BLACK[1]); [4, 12].forEach((st) => key(WHITE[st], 20));                // a snare
key(BLACK[3]); [0, 2, 4, 6, 8, 10, 12, 14].forEach((st) => key(WHITE[st], 20)); // hats
key(BLACK[0]);
png("2-steps");
ex.web_buttons(1 << B.PLAY); render(60); ex.web_buttons(0);
render(80); png("3-playing-kick");                                         // a few ms after a kick
render(380); png("4-playing-snare");
tap(B.EDIT); turn(EN.K1, 14); turn(EN.K2, 2); turn(EN.K3, -5); turn(EN.K4, 3);
png("5-edit");
tap(B.EDIT); tap(B.FX); key(BLACK[3]);
key(WHITE[4]); key(WHITE[5]); key(WHITE[6]); key(WHITE[12]); turn(EN.SELECT, -6);
png("6-fx-lowpass");
tap(B.FX); png("7-fx-lowpass-2");
tap(B.FX); tap(B.FX); key(BLACK[7]); turn(EN.K1, 2);
png("8-fx-delay");
tap(B.HOME); tap(B.GLO); turn(EN.K4, 1); turn(EN.K2, -6);
png("9-global");
tap(B.GLO); png("10-global-2");
turn(EN.K4, 14); png("11-erase-warning");
tap(B.HOME); tap(B.SAVE); png("12-patterns");
tap(B.SAVE); tap(B.SCL); turn(EN.K1, 14); turn(EN.K2, -12); turn(EN.K3, 20); turn(EN.K4, 9);
png("13-master");

console.log(`screenshots in ${out}`);

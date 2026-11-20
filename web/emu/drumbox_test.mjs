// SPDX-License-Identifier: GPL-3.0-only
// The DrumBox browser build (build/emu-drumbox/felucca.wasm, web/emu/drumbox_web.c) in Node, as the worklet runs it.
//   node web/emu/drumbox_test.mjs [build/emu-drumbox/felucca.wasm]
// Checks: it boots silent; the 11 black keys select their drum (silently) and light their LED; a white key sets a step
// of the selected lane (an empty pattern at power-on); PLAY runs it; SELECT sets the velocity of the step just
// placed; the knobs set TUNE (the kick as a note), DECAY, TONE, LEVEL; the GLO page (a tap opens it, a tap closes
// it): KNOB 1 tempo, KNOB 2 volume, KNOB 3 the computer's MIDI clock, KNOB 4 the subdivision; the EDIT page: a step's
// NUDGE, SUBDIVISION, CHANCE and PITCH; EDIT / HOME held clear; the same gestures give the same audio.
import fs from "fs";
import { storeSectors, restoreSectors } from "./worklet.js";            // what the page keeps in the browser, and gives back

const bytes = fs.readFileSync(process.argv[2] || "build/emu-drumbox/felucca.wasm");
const EN = { SELECT: 0, ALGO: 1, PRESETS: 2, K1: 3, K2: 4, K3: 5, K4: 6 };
const FXN = ["HALFTIME", "BITCRUSH", "SATURATE 1", "LOW PASS", "BAND PASS", "PAN", "FLANGER", "DELAY", "REPEAT", "REVERB", "SATURATE 2"];   // in the order of the signal
const FX = { HALF: 0, CRUSH: 1, SAT1: 2, LP: 3, BP: 4, PAN: 5, FLANGE: 6, DELAY: 7, REPEAT: 8, REVERB: 9, SAT2: 10 };
const B = { FX: 0, SCL: 1, ENV: 2, LFO: 3, EDIT: 4, GLO: 5, HOME: 6, SAVE: 7, ARP: 8, SEQ: 9, PLAY: 10, REC: 11, OCTDN: 12, OCTUP: 13 };
const BLACK = [1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25], WHITE = [0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26];
const NAMES = ["KICK", "SNARE", "CLAP", "HAT CL", "HAT OP", "TOM", "CONGA", "RIM", "CLAVE", "BELL", "CYM"];
const WHITE_MASK = WHITE.reduce((m, k) => m | (1 << k), 0);
let fails = 0;
const check = (what, ok) => { console.log(`drumbox: ${what.padEnd(66)} ${ok ? "ok" : "FAIL"}`); if (!ok) fails++; };
const near = (a, b, tol) => Math.abs(a - b) <= tol;
const np = (w, d) => { const pe = w.ex.web_pe(); w.btn(B.SAVE); w.key(WHITE[(pe + d + 16) % 16], 50); w.btn(B.HOME); };   // the pattern page: the next (or previous) pattern is picked (what OCT did, before it walked a pattern's sections)

async function device(saved, splash) {                              // saved: the flash of another device (what a power cycle keeps)
  const { instance } = await WebAssembly.instantiate(bytes, {});
  const ex = instance.exports, mem = ex.memory;
  ex._initialize();
  ex.web_nor_erase();
  if (saved) new Uint8Array(mem.buffer, ex.web_nor(), ex.web_nor_size()).set(saved);
  ex.web_boot();
  const d = { ex, mem, peak: 0, sig: 0, t: 0, nextPulse: 0 };
  d.renderN = (n) => {                                      // n samples; d.peak is the peak of this call
    d.peak = 0;
    for (let k = 0; k < n; k += 128) {
      const m = Math.min(128, n - k);
      ex.web_render(m);
      const l = new Float32Array(mem.buffer, ex.web_out_l(), m), r = new Float32Array(mem.buffer, ex.web_out_r(), m);
      for (let i = 0; i < m; i++) {
        d.peak = Math.max(d.peak, Math.abs(l[i]), Math.abs(r[i]));
        d.sig = (d.sig * 31 + Math.round(l[i] * 32768) + 7 * Math.round(r[i] * 32768)) % 1000000007;
      }
    }
    d.t += n;
  };
  d.render = (ms) => d.renderN(Math.round(ms * 44.1));
  d.key = (k, ms = 300) => { ex.web_keys(1 << k); d.render(30); ex.web_keys(0); d.render(ms); };
  d.audition = () => {                         // step 1 on (it previews the selected drum), then off again: the peak heard
    ex.web_keys(1 << WHITE[0]); d.render(30);
    const p1 = d.peak;
    ex.web_keys(0); d.render(250);
    const p = Math.max(p1, d.peak);
    ex.web_keys(1 << WHITE[0]); d.render(20); ex.web_keys(0); d.render(20);
    return p;
  };
  d.btn = (b, ms = 60) => { ex.web_buttons(1 << b); d.render(ms); ex.web_buttons(0); d.render(60); };
  d.btnPeak = (b, ms) => { ex.web_buttons(1 << b); d.render(ms); const p = d.peak; ex.web_buttons(0); d.render(60); return p; };
  d.hold = (b, ms) => { ex.web_buttons(1 << b); d.render(ms); ex.web_buttons(0); d.render(60); };
  d.glo = (fn) => { d.btn(B.GLO); fn(); d.btn(B.HOME); };            // fn with the first GLO page open (HOME closes it)
  d.glo2 = (fn) => { d.btn(B.GLO); d.btn(B.GLO); fn(); d.btn(B.HOME); };   // ... the second
  d.edit = (fn) => { d.btn(B.EDIT); fn(); d.btn(B.EDIT); };  // the same with the EDIT page
  d.onsets = (ms, gap = 352, thr = 0.02) => {              // when (in ms from now) a sound starts: it rises past 0.02 after `gap` quiet samples (8 ms)
    const out = [], n = Math.round(ms * 44.1);
    let quiet = 1e9;
    for (let k = 0; k < n; k += 16) {
      ex.web_render(16); d.t += 16;
      const l = new Float32Array(mem.buffer, ex.web_out_l(), 16);
      for (let i = 0; i < 16; i++) {
        if (Math.abs(l[i]) > thr) { if (quiet > gap) out.push((k + i) / 44.1); quiet = 0; } else quiet++;
      }
    }
    return out;
  };
  d.turn = (role, n) => { for (let i = 0; i < Math.abs(n); i++) { ex.web_enc(role, Math.sign(n)); d.render(40); } };
  d.steps = (ms) => {                          // the number of step changes in ms of playing
    let n = 0, last = ex.web_step();
    for (let t = 0; t < ms; t += 2) { d.render(2); const c = ex.web_step(); if (c !== last) { n++; last = c; } }
    return n;
  };
  d.setSteps = (steps) => { for (const st of steps) d.key(WHITE[st], 40); };    // toggles steps of the selected lane
  d.clock = (bpm, sec, withSteps) => {         // the computer's MIDI clock for sec seconds (24 pulses a beat); counts steps
    const per = 44100 * 60 / (bpm * 24), end = d.t + sec * 44100;
    let n = 0, last = ex.web_step();
    if (d.nextPulse < d.t) d.nextPulse = d.t;                // (a clock that was silent does not come back with a burst)
    while (d.t < end) {
      while (d.nextPulse <= d.t) { ex.web_midi(0xF8, 0, 0); d.nextPulse += per; }
      d.renderN(64);
      if (withSteps) { const c = ex.web_step(); if (c !== last) { n++; last = c; } }
    }
    return n;
  };
  d.cap = (ms) => {                            // the stereo sound of the next ms: {l, r} as Float32Arrays
    const n = Math.round(ms * 44.1), l = new Float32Array(n), r = new Float32Array(n);
    for (let k = 0; k < n; k += 128) {
      const m = Math.min(128, n - k);
      ex.web_render(m);
      l.set(new Float32Array(mem.buffer, ex.web_out_l(), m), k);
      r.set(new Float32Array(mem.buffer, ex.web_out_r(), m), k);
    }
    d.t += n;
    return { l, r };
  };
  d.fxOpen = (e) => { d.btn(B.FX); d.key(BLACK[e], 20); };          // the FX page 1, effect e picked
  d.fxSet = (e, i, target) => {               // parameter i of effect e set to a raw value (clicks on its page)
    d.ex.web_page() === 5 || d.ex.web_page() === 6 || d.btn(B.FX);
    if (d.ex.web_page() === 5 && i >= 4) d.btn(B.FX);
    if (d.ex.web_page() === 6 && i < 4) { d.btn(B.FX); d.btn(B.FX); }
    d.key(BLACK[e], 20);
    const cur = d.ex.web_fxparam(e, i), role = EN.K1 + (i % 4);
    const opts = { 0: [3], 6: [3], 10: [6] };
    if (target === "on") d.turn(role, 1); else if (target === "off") d.turn(role, -1);
    else d.turn(role, Math.round((target - cur) / 2));
  };
  d.screen = () => new Uint16Array(mem.buffer, ex.web_screen(), 240 * 240).slice();
  d.flash = () => new Uint8Array(mem.buffer, ex.web_nor(), ex.web_nor_size()).slice();
  d.led = (k) => (ex.web_lit_keys() >>> k) & 1;
  d.btnLed = (b) => (ex.web_lit_buttons() >>> b) & 1;
  if (!splash) d.render(950);                               // the boot screen (900 ms) is over: the pages are what the tests look at
  return d;
}
const colours = (fb) => new Set(fb).size;
const differ = (a, b) => { let n = 0; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) n++; return n; };

// ---- power-on, the 11 lanes
const a = await device();
a.render(200);
check("boots silent", a.peak === 0);
check("the screen is drawn (several colours)", colours(a.screen()) > 3);
check("the idle glow is on the keys (27 dim LEDs)", a.ex.web_dim_keys() === (1 << 27) - 1);
check("power-on: no step is set (no white key lit)", (a.ex.web_lit_keys() & WHITE_MASK) === 0);
a.btn(B.PLAY); a.render(2500);
check("power-on: playing an empty pattern is silent", a.peak === 0);
for (let i = 0; i < BLACK.length; i++) {
  const d = await device(); d.render(100);
  d.ex.web_keys(1 << BLACK[i]); d.render(30);
  const lit = d.ex.web_lit_keys(), held = d.peak;
  d.ex.web_keys(0); d.render(300);
  check(`black key ${BLACK[i]} selects ${NAMES[i]} and makes no sound`, held === 0 && d.peak === 0);
  check(`  its LED is lit`, (lit >>> BLACK[i]) & 1);
  const p = d.audition();
  check(`  ${NAMES[i]} previews on a step (peak ${p.toFixed(2)})`, p > 0.02);
}

// ---- the steps
{
  const w = await device(); w.render(200);
  w.key(WHITE[0], 100);
  check("white key 1: step 1 on, its LED lit", w.led(WHITE[0]) === 1);
  check("  turning a step on while stopped previews the drum", w.peak > 0.02);
  w.key(WHITE[0], 100);
  check("pressed again: step 1 off, its LED out", w.led(WHITE[0]) === 0);
  w.key(WHITE[0], 3000);                                    // on again, and the preview rings out
  w.key(WHITE[0], 500);                                     // off
  check("  turning a step off makes no sound", w.peak === 0);
  w.key(WHITE[1], 100);
  w.key(BLACK[1], 100);                                     // SNARE: its own (empty) steps
  check("another lane has its own steps (step 2 is out)", w.led(WHITE[1]) === 0);
  w.key(BLACK[0], 100);
  check("back on the kick: its step 2 is lit again", w.led(WHITE[1]) === 1);
}
{
  const w = await device(); w.render(200);
  w.setSteps([0, 4, 8, 12]);                                // four on the floor, by hand
  check("stopped: PLAY's LED is off", w.btnLed(14) === 0);
  w.btn(B.PLAY); w.render(1000);
  check("PLAY: the pattern sounds", w.peak > 0.05);
  check("PLAY: its green LED is lit", w.btnLed(14) === 1);
  const n120 = w.steps(4000);
  check(`120 BPM, 1/16: 32 steps in 4 s (${n120})`, near(n120, 32, 1));
  w.btn(B.PLAY); w.render(1500);                            // (the last hits ring out)
  w.render(1500);
  check("STOP: silent once the tails are over", w.peak === 0);
  check("STOP: PLAY's LED is off", w.btnLed(14) === 0);
}
{
  const w = await device(); w.render(200);
  w.setSteps([0, 8]);                                       // kick
  w.key(BLACK[1], 50); w.setSteps([4, 12]);                 // snare
  w.btn(B.PLAY); w.render(1200);
  check("the steps set by hand play", w.peak > 0.05);
  w.btn(B.PLAY); w.render(100);
  w.hold(B.EDIT, 800);                                      // EDIT held: the snare lane cleared
  check("EDIT held clears the selected lane (its LEDs out)", (w.ex.web_lit_keys() & WHITE_MASK) === 0);
  w.key(BLACK[0], 50);
  check("  the kick's steps are still there", w.led(WHITE[0]) === 1);
  w.hold(B.HOME, 800);                                      // HOME held: the whole pattern cleared
  check("HOME held clears the whole pattern (kick's LEDs out)", (w.ex.web_lit_keys() & WHITE_MASK) === 0);
  w.btn(B.PLAY); w.render(2500);
  check("  and playing it is silent", w.peak === 0);
}

// ---- SELECT: the velocity of the step just placed
{
  const w = await device(); w.render(200);
  const v = (st) => w.ex.web_vel(0, st);
  w.key(WHITE[0], 100); w.key(WHITE[4], 100);               // two kick steps
  check(`a step is placed at velocity 100 (${v(4)})`, v(4) === 100 && v(0) === 100);
  w.turn(EN.SELECT, -5);
  check(`SELECT sets the last placed step's velocity: -5 clicks = 80 (${v(4)})`, v(4) === 80);
  check(`  the other step is untouched (${v(0)})`, v(0) === 100);
  w.turn(EN.SELECT, -100);
  check(`  down to 1 and no further (${v(4)})`, v(4) === 1);
  w.turn(EN.SELECT, 100);
  check(`  up to 127 and no further (${v(4)})`, v(4) === 127);
  w.key(BLACK[1], 50); w.turn(EN.SELECT, -5);
  check("another lane selected: SELECT edits nothing (kick step still 127)", v(4) === 127 && w.ex.web_vel(1, 4) === 100);
  w.key(BLACK[0], 50);
  w.key(WHITE[4], 50);                                      // the step off: no target
  w.turn(EN.SELECT, -5);
  check("a step turned off is no target", v(4) === 127);
}
{
  const peakOf = async (clicks) => {                        // the loudness of one step played at a velocity
    const w = await device(); w.render(200);
    w.key(WHITE[0], 50);
    w.turn(EN.SELECT, clicks);
    w.render(1500);
    return w.btnPeak(B.PLAY, 400);
  };
  const p127 = await peakOf(7), p100 = await peakOf(0), p40 = await peakOf(-15);
  check(`velocity changes the loudness when played: ${p40.toFixed(3)} < ${p100.toFixed(3)} < ${p127.toFixed(3)}`, p40 < p100 && p100 < p127);
  check("  40 is well below 100 (at least 6 dB)", p40 < p100 * 0.5);
}
{
  const w = await device(); w.render(200);
  w.key(WHITE[0], 50);
  const t0 = w.ex.web_bpm();
  w.turn(EN.SELECT, 10);
  check("SELECT is not the tempo (BPM unchanged)", w.ex.web_bpm() === t0);
}

// ---- the knobs of the lane
{
  const w = await device(); w.render(200);
  const n = () => w.ex.web_note16(0);
  check(`the kick's designed pitch is 31.4 (50 Hz): ${n() / 16}`, n() === 502);
  w.turn(EN.K1, 1);
  check(`TUNE, kick: one click up snaps to the next note, G#1 (${n() / 16})`, n() === 32 * 16);
  w.turn(EN.K1, 1);
  check(`  another click: A1 (${n() / 16})`, n() === 33 * 16);
  w.turn(EN.K1, -3);
  check(`  three down: F#1 (${n() / 16})`, n() === 30 * 16);
  w.turn(EN.K1, 100);
  check(`  up to the end of the kick's range: F2 (${n() / 16})`, n() === 41 * 16);
  w.turn(EN.K1, -100);
  check(`  down to the other end: D1 (${n() / 16})`, n() === 26 * 16);
  const w2 = await device(); w2.render(200);
  w2.turn(EN.K1, -1);
  check(`  from the designed pitch, one click down snaps to G1 (${w2.ex.web_note16(0) / 16})`, w2.ex.web_note16(0) === 31 * 16);
  const s0 = w2.screen(); w2.turn(EN.K1, -1);
  check("  TUNE redraws the screen", differ(s0, w2.screen()) > 20);
  w2.key(BLACK[1], 50);                                     // the snare: TUNE in quarter semitones, not notes
  const sn = w2.ex.web_note16(1);
  w2.turn(EN.K1, 4);
  check(`TUNE, snare: 4 clicks = +1 semitone (${sn} -> ${w2.ex.web_note16(1)})`, w2.ex.web_note16(1) === sn + 16);
}
{
  const b = await device(); b.render(200);
  const s0 = b.screen();
  b.turn(EN.K4, -10);
  check("KNOB 4 (LEVEL) redraws the screen", differ(s0, b.screen()) > 100);
  const quiet = b.audition();
  b.render(2000);
  b.turn(EN.K4, 10);
  const loud = b.audition();
  check(`LEVEL changes the loudness (${quiet.toFixed(2)} -> ${loud.toFixed(2)})`, loud > quiet);
  const c = await device(); c.render(200);
  c.key(BLACK[1], 300);
  const sn = c.screen();
  c.key(BLACK[0], 300);
  check("selecting another lane redraws the screen", differ(sn, c.screen()) > 100);
}

// ---- GLO: the global page, opened by a tap and closed by a tap
{
  const w = await device(); w.render(200);
  const home = w.screen();
  w.btn(B.GLO); w.render(100);
  check("GLO tapped: the page stays open after the button is let go", w.ex.web_page() === 1);
  check("  its LED is lit", w.btnLed(B.GLO) === 1);
  check("  the global page is shown", differ(home, w.screen()) > 500);
  const first = w.screen();
  w.btn(B.GLO); w.render(100);
  check("GLO tapped again: the second page", w.ex.web_page() === 4 && w.btnLed(B.GLO) === 1);
  check("  it is another page", differ(first, w.screen()) > 200);
  w.btn(B.GLO); w.render(100);
  check("GLO tapped a third time: the pages close", w.ex.web_page() === 0 && w.btnLed(B.GLO) === 0);
  check("  the lane page is back, as it was", differ(home, w.screen()) === 0);
  w.btn(B.GLO); w.btn(B.HOME);
  check("HOME tapped closes the page", w.ex.web_page() === 0);
  w.turn(EN.K1, 10);
  check("KNOB 1 on the lane page is not the tempo", w.ex.web_bpm() === 120);
  w.glo(() => w.turn(EN.K1, 30));
  check(`GLO page + KNOB 1: tempo 120 -> 150 (${w.ex.web_bpm()})`, w.ex.web_bpm() === 150);
  w.glo(() => w.turn(EN.K1, 500));
  check(`  up to 300 and no further (${w.ex.web_bpm()})`, w.ex.web_bpm() === 300);
  w.glo(() => w.turn(EN.K1, -500));
  check(`  down to 30 and no further (${w.ex.web_bpm()})`, w.ex.web_bpm() === 30);
}
{
  const w = await device(); w.render(200);
  w.setSteps([0]);
  w.glo(() => w.turn(EN.K1, 30));                           // 150 BPM
  w.btn(B.PLAY); w.render(500);
  const n16 = w.steps(4000);
  check(`150 BPM, 1/16 (4 beats): 40 steps in 4 s (${n16})`, near(n16, 40, 1));
  w.glo(() => w.turn(EN.K4, 1));
  check(`GLO page + KNOB 4 up: subdivision 1/32 (${w.ex.web_div()})`, w.ex.web_div() === 32);
  const n32 = w.steps(4000);
  check(`  1/32 (2 beats): 80 steps in 4 s (${n32})`, near(n32, 80, 1));
  w.glo(() => w.turn(EN.K4, -5));
  check(`GLO page + KNOB 4 down: 1/8, the end (${w.ex.web_div()})`, w.ex.web_div() === 8);
  const n8 = w.steps(4000);
  check(`  1/8 (8 beats): 20 steps in 4 s (${n8})`, near(n8, 20, 1));
  w.glo(() => w.turn(EN.K4, 1));
  check(`  one up: 1/16 (${w.ex.web_div()})`, w.ex.web_div() === 16);
}
{
  const w = await device(); w.render(200);
  check(`volume is 100 at power-on (${w.ex.web_vol()})`, w.ex.web_vol() === 100);
  const full = w.audition();
  w.render(1500);
  w.glo(() => w.turn(EN.K2, -10));
  check(`GLO page + KNOB 2: -10 clicks = 80 (${w.ex.web_vol()})`, w.ex.web_vol() === 80);
  w.render(300);
  const quiet = w.audition();
  check(`  the sound is quieter (${full.toFixed(3)} -> ${quiet.toFixed(3)})`, quiet < full * 0.8);
  w.render(1500);
  w.glo(() => w.turn(EN.K2, -100));
  w.render(300);
  check(`  down to 0: silence (${w.ex.web_vol()})`, w.ex.web_vol() === 0 && w.audition() === 0);
  w.glo(() => w.turn(EN.K2, 100));
  check(`  up to 127 and no further (${w.ex.web_vol()})`, w.ex.web_vol() === 127);
  w.render(300);
  check("  louder than at 100", w.audition() > full * 1.1);
}

// ---- GLO + KNOB 3: follow the computer's MIDI clock
{
  const w = await device(); w.render(200);
  w.setSteps([0]);
  w.render(200);
  w.btn(B.PLAY);
  const own = w.steps(2000);
  w.clock(150, 2, false);                                   // a clock arrives, but we are on our own
  check(`own clock, 120 BPM: 16 steps in 2 s (${own})`, near(own, 16, 1));
  check(`  the computer's clock is ignored: the tempo did not move (${w.ex.web_bpm()})`, w.ex.web_bpm() === 120);
  w.btn(B.PLAY); w.render(100);
  w.glo(() => w.turn(EN.K3, 1));
  check("GLO page + KNOB 3 up: follows the computer's clock", w.ex.web_ext() === 1);
  check("  no clock yet: not locked", w.ex.web_locked() === 0);
  w.btn(B.PLAY);
  const waiting = w.steps(1500);
  check(`  PLAY with no clock: it waits, no step (${waiting})`, waiting === 0);
  w.clock(120, 6, false);
  check("  with the clock: locked", w.ex.web_locked() === 1);
  check(`  tempo measured from the clock: 120 (${w.ex.web_bpm()})`, near(w.ex.web_bpm(), 120, 1));
  const steps = w.clock(120, 4, true);
  check(`  steps follow the clock: 32 in 4 s at 120 BPM, 1/16 (${steps})`, near(steps, 32, 1));
  const home = w.screen();
  w.clock(150, 6, false);
  check(`  the computer speeds up to 150: tempo follows (${w.ex.web_bpm()})`, near(w.ex.web_bpm(), 150, 2));
  check("  the screen shows it", differ(home, w.screen()) > 20);
  w.glo(() => w.turn(EN.K4, 1));
  const n32 = w.clock(150, 4, true);
  check(`  1/32 on the clock at 150 BPM: 80 steps in 4 s (${n32})`, near(n32, 80, 1));
  w.glo(() => w.turn(EN.K4, -1));
  w.ex.web_midi(0xFC, 0, 0); w.render(50);
  check("  MIDI Stop from the computer stops it", w.ex.web_playing() === 0);
  w.ex.web_midi(0xFA, 0, 0); w.render(20);
  check("  MIDI Start plays again", w.ex.web_playing() === 1);
  w.clock(150, 0.1, false);
  check(`  ... from its first step, on the first clock (${w.ex.web_step()})`, w.ex.web_step() === 0);
  w.glo(() => w.turn(EN.K3, -1));
  check("GLO page + KNOB 3 down: back to our own clock", w.ex.web_ext() === 0);
  const back = w.steps(4000);
  check(`  at the tempo it was (150 BPM): 40 steps in 4 s (${back})`, near(back, 40, 1));
  w.glo(() => w.turn(EN.K1, 10));
  check(`  and KNOB 1 sets the tempo again (${w.ex.web_bpm()})`, w.ex.web_bpm() === 160);
}
{
  // a clock that arrives late and unevenly, as it does over USB (0 to 12 ms late a pulse): the tempo shown does not wander
  const w = await device(); w.render(200);
  w.glo(() => w.turn(EN.K3, 1)); w.btn(B.PLAY);
  let seed = 12345;
  const rnd = () => (seed = (seed * 1103515245 + 12345) >>> 0) / 4294967296;
  const per = 44100 * 60 / (120 * 24), t0 = w.t;
  let k = 0, at = t0 + rnd() * 529, shown = new Set(), steps = 0, last = w.ex.web_step();
  while (w.t < t0 + 16 * 44100) {
    while (at <= w.t) { w.ex.web_midi(0xF8, 0, 0); k++; at = t0 + k * per + rnd() * 529; }
    w.renderN(64);
    if (w.t > t0 + 6 * 44100) shown.add(w.ex.web_bpm());
    if (w.t > t0 + 6 * 44100 && w.ex.web_step() !== last) { steps++; last = w.ex.web_step(); }
  }
  check(`a jittery clock at 120 BPM: the tempo stays on one value after the first beats (${[...shown].join(" ")})`, shown.size === 1 && shown.has(120));
  check(`  and the steps follow it (${steps} in 10 s: 80)`, near(steps, 80, 2));
}
{
  // the clock of our own, locked to the computer's: a late, uneven clock gives even steps (the step to step time stays within 4 ms of 125 ms; a pulse each would be 12)
  const mk = async () => { const w = await device(); w.render(200); w.setSteps([0]); w.render(100); w.glo(() => w.turn(EN.K3, 1)); w.btn(B.PLAY); return w; };
  const rig = (w, bpmOf, jit, stall) => {                      // the pulses 0..jit ms late each; stall: [from s, to s] the pulses of that time arrive together at its end
    let seed = 777, k = 0, t = 0, times = [], last = w.ex.web_step(), held = 0, perT = 0, sent = 0;
    const rnd = () => (seed = (seed * 1103515245 + 12345) >>> 0) / 4294967296;
    const t0 = w.t; let nom = t0, at = nom + rnd() * jit * 44.1;
    const pend = [];
    const clk = () => {
      while (nom <= w.t + 4000) { pend.push(nom + rnd() * jit * 44.1); nom += 44100 * 60 / (bpmOf((nom - t0) / 44100) * 24); }
      pend.sort((a, b) => a - b);
      while (pend.length && pend[0] <= w.t) {
        const due = pend[0];
        if (stall && due > t0 + stall[0] * 44100 && due < t0 + stall[1] * 44100 && w.t < t0 + stall[1] * 44100) break;   // held back until the stall ends
        pend.shift(); w.ex.web_midi(0xF8, 0, 0); sent++;
      }
    };
    return { run: (sec) => { const end = w.t + sec * 44100; while (w.t < end) { clk(); w.renderN(64); const c = w.ex.web_step(); if (c !== last) { times.push(w.t); last = c; } } return times; }, sent: () => sent };
  };
  const gaps = (a) => a.slice(1).map((x, i) => (x - a[i]) / 44.1);
  {
    const w = await mk(), r = rig(w, () => 120, 12);
    const t = r.run(14), g = gaps(t.filter((x) => x > w.t - 8 * 44100));
    const dev = Math.max(...g.map((x) => Math.abs(x - 125)));
    check(`a clock 0 to 12 ms late a pulse: the steps stay even (${g.length} steps after the loop took over, worst ${dev.toFixed(1)} ms off 125)`, g.length >= 55 && dev <= 4);
  }
  {
    const w = await mk(), r = rig(w, (t) => (t < 6 ? 120 : 150), 8);
    const t = r.run(16), g = gaps(t.filter((x) => x > w.t - 3 * 44100));
    const dev = Math.max(...g.map((x) => Math.abs(x - 100)));
    check(`the computer goes from 120 to 150 BPM: the tempo follows (${w.ex.web_bpm()}), and the steps go to 100 ms (${g.length} of the last 3 s, worst ${dev.toFixed(1)} ms off)`, w.ex.web_bpm() === 150 && g.length >= 25 && dev <= 6);
  }
  {
    const w = await mk(), r = rig(w, () => 120, 6, [8, 8.12]);
    const t = r.run(16);
    const expected = Math.round(r.sent() / 6);
    check(`a clock that stalls 120 ms and comes all at once: no step lost or doubled (${t.length} steps, ${r.sent()} pulses = ${expected})`, Math.abs(t.length - expected) <= 2);
  }
}

// ---- EDIT: a step's NUDGE, SUBDIVISION, CHANCE, PITCH
{
  const w = await device(); w.render(200);
  const home = w.screen();
  w.key(BLACK[7], 50);                                      // RIM: a short sound, so its hits can be told apart
  w.key(WHITE[0], 50); w.key(WHITE[4], 50);                 // steps 1 and 5; 5 is the one just placed
  const before = w.screen();
  w.btn(B.EDIT); w.render(100);
  check("EDIT tapped: its page opens and stays", w.ex.web_page() === 2 && w.btnLed(B.EDIT) === 1);
  check("  the edit page is shown", differ(before, w.screen()) > 300);
  const nu = (s) => w.ex.web_nudge(7, s), ra = (s) => w.ex.web_ratchet(7, s), ch = (s) => w.ex.web_chance(7, s), se = (s) => w.ex.web_semi(7, s);
  check(`a step is placed with no nudge, 1 hit, 100 %, no pitch (${nu(4)} ${ra(4)} ${ch(4)} ${se(4)})`, nu(4) === 0 && ra(4) === 1 && ch(4) === 100 && se(4) === 0);
  w.turn(EN.K1, 5);  check(`KNOB 1 nudges the step just placed: +5 notches (${nu(4)})`, nu(4) === 5);
  w.turn(EN.K1, 100); check(`  late by half a step at most: +24 (${nu(4)})`, nu(4) === 24);
  w.turn(EN.K1, -100); check(`  early by half a step at most: -24 (${nu(4)})`, nu(4) === -24);
  w.turn(EN.K2, 3);  check(`KNOB 2 sets the hits: x4 (${ra(4)})`, ra(4) === 4);
  w.turn(EN.K2, 20); check(`  at most 8 (${ra(4)})`, ra(4) === 8);
  w.turn(EN.K2, -20); check(`  at least 1 (${ra(4)})`, ra(4) === 1);
  w.turn(EN.K3, -4); check(`KNOB 3 sets the chance, 5 % a click: 80 (${ch(4)})`, ch(4) === 80);
  w.turn(EN.K3, -100); check(`  0 at least (${ch(4)})`, ch(4) === 0);
  w.turn(EN.K3, 100); check(`  100 at most (${ch(4)})`, ch(4) === 100);
  w.turn(EN.K4, 5);  check(`KNOB 4 sets the pitch: +5 semitones (${se(4)})`, se(4) === 5);
  w.turn(EN.K4, 100); check(`  +12 at most (${se(4)})`, se(4) === 12);
  w.turn(EN.K4, -100); check(`  -12 at least (${se(4)})`, se(4) === -12);
  check("  the other step is untouched", nu(0) === 0 && ra(0) === 1 && ch(0) === 100 && se(0) === 0);
  w.key(WHITE[0], 50);                                      // a step that is on: picked, not turned off
  check("a white key on a step that is on picks it (it stays on)", w.led(WHITE[0]) === 1);
  w.turn(EN.K1, 7);
  check(`  the knobs now edit it (${nu(0)}), not the first one (${nu(4)})`, nu(0) === 7 && nu(4) === -24);
  w.key(WHITE[0], 50);
  check("the picked step pressed again turns it off", w.led(WHITE[0]) === 0);
  w.key(WHITE[8], 50);
  check("a step that is off is placed and picked in one press", w.led(WHITE[8]) === 1 && ra(8) === 1);
  w.turn(EN.K2, 2);
  check(`  and edited (${ra(8)})`, ra(8) === 3);
  w.btn(B.EDIT); w.render(100);
  check("EDIT tapped again: the step's sound (EDIT 2), its LED still lit", w.ex.web_page() === 14 && w.btnLed(B.EDIT) === 1);
  w.btn(B.EDIT); w.render(100);
  check("  and then the page closes", w.ex.web_page() === 0 && w.btnLed(B.EDIT) === 0);
  w.btn(B.EDIT); w.btn(B.HOME);
  check("HOME tapped closes the page too", w.ex.web_page() === 0);
}
{
  const lanePage = async (edit) => {                        // the lane page with two kick steps, one edited or not
    const w = await device(); w.render(200);
    w.setSteps([0, 4]);
    if (edit) { w.btn(B.EDIT); w.turn(EN.K2, 1); w.btn(B.EDIT); w.btn(B.EDIT); }
    w.render(300);
    return w.screen();
  };
  const plain = await lanePage(false), marked = await lanePage(true);
  const d = differ(plain, marked);
  check(`the grid marks a step that is edited (${d} pixels differ: a 3 x 3 mark)`, d >= 6 && d <= 40);
}
{
  const w = await device(); w.render(200);
  w.key(BLACK[7], 50);
  w.setSteps([0, 4]);
  w.btn(B.EDIT);                                            // step 5 (the last placed) is picked
  const noEdit = w.screen();
  w.turn(EN.K2, 1);
  check("the edit page shows the new value", differ(noEdit, w.screen()) > 20);
  w.render(500);
  w.turn(EN.K2, 2);
  check("turning a knob while stopped previews the step (its hits)", w.peak > 0.02);
  w.btn(B.EDIT); w.btn(B.EDIT);
  w.hold(B.EDIT, 800);                                      // held 0.6 s: clears the lane, does not open the page
  check("EDIT held 0.6 s clears the lane and does not open its page", w.ex.web_page() === 0 && (w.ex.web_lit_keys() & WHITE_MASK) === 0);
  w.setSteps([0]);
  w.btn(B.EDIT); w.hold(B.HOME, 800);
  check("HOME held 0.6 s clears everything, from a page too", (w.ex.web_lit_keys() & WHITE_MASK) === 0);
}
{
  // the timing: a RIM on steps 1 and 5 at 120 BPM, 1/16 (a step is 125 ms): the gap between them is 500 ms plus the nudge
  const gap = async (nudge) => {
    const w = await device(); w.render(200);
    w.key(BLACK[7], 50);
    w.setSteps([0, 4]);
    w.btn(B.EDIT);
    w.turn(EN.K1, nudge);
    w.btn(B.EDIT);
    w.render(1500);
    w.ex.web_buttons(1 << B.PLAY);                          // (held: listening starts with the press)
    const o = w.onsets(1200);
    w.ex.web_buttons(0);
    return o.length >= 2 ? o[1] - o[0] : -1;
  };
  const g0 = await gap(0), gl = await gap(24), ge = await gap(-24), gs = await gap(12);
  check(`no nudge: the second hit is 500 ms after the first (${g0.toFixed(1)})`, near(g0, 500, 3));
  check(`+24 (half a step late): 562.5 ms (${gl.toFixed(1)})`, near(gl, 562.5, 3));
  check(`-24 (half a step early): 437.5 ms (${ge.toFixed(1)})`, near(ge, 437.5, 3));
  check(`+12 (a quarter step late): 531.3 ms (${gs.toFixed(1)})`, near(gs, 531.3, 3));
}
{
  // the ratchet: 4 hits spread over the step of 125 ms: 31.25 ms apart
  const w = await device(); w.render(200);
  w.key(BLACK[7], 50);
  w.setSteps([0, 4]);
  w.btn(B.EDIT);
  w.turn(EN.K2, 3);
  w.btn(B.EDIT);
  w.render(1500);
  w.ex.web_buttons(1 << B.PLAY);
  const o = w.onsets(1000);
  w.ex.web_buttons(0);
  check(`SUBDIVISION x4 on step 5: 1 hit on step 1 and 4 on step 5 (${o.length})`, o.length === 5);
  const gaps = o.slice(2).map((t, i) => t - o[i + 1]);
  check(`  the 4 hits are 31.25 ms apart (${gaps.map((g) => g.toFixed(1)).join(" ")})`, gaps.length === 3 && gaps.every((g) => near(g, 31.25, 2.5)));
  const w8 = await device(); w8.render(200);
  w8.key(BLACK[7], 50); w8.setSteps([0]);
  w8.btn(B.EDIT); w8.turn(EN.K2, 7); w8.btn(B.EDIT);
  w8.render(1500);
  w8.ex.web_buttons(1 << B.PLAY);
  const o8 = w8.onsets(300, 100, 0.06);                           // (hits 15.6 ms apart: a shorter quiet tells them apart)
  w8.ex.web_buttons(0);
  check(`  x8: 8 hits in the step (${o8.length})`, o8.length === 8);
}
{
  // the chance: one RIM step, 300 BPM at 1/32 (a pass of the 16 steps is 400 ms)
  const hits = async (clicks, sec) => {
    const w = await device(); w.render(200);
    w.key(BLACK[7], 50); w.setSteps([0]);
    if (clicks !== 0) { w.btn(B.EDIT); w.turn(EN.K3, clicks); w.btn(B.EDIT); }
    w.glo(() => { w.turn(EN.K1, 500); w.turn(EN.K4, 1); });
    w.render(500);
    w.ex.web_buttons(1 << B.PLAY);
    const n = w.onsets(sec * 1000).length;
    w.ex.web_buttons(0);
    return n;
  };
  const full = await hits(0, 8), none = await hits(-20, 8), half = await hits(-10, 16);
  check(`chance 100 %: it plays every pass, 20 in 8 s (${full})`, near(full, 20, 1));
  check(`chance 0: it never plays (${none})`, none === 0);
  check(`chance 50 %: about half the passes, 40 passes in 16 s (${half})`, half >= 12 && half <= 28);
}
{
  // the pitch: a TOM on steps 1 and 5, +5 semitones on the second
  const w = await device(); w.render(200);
  w.key(BLACK[5], 50);
  w.setSteps([0, 4]);
  w.btn(B.EDIT); w.turn(EN.K4, 5); w.btn(B.EDIT);
  w.render(1000);
  w.ex.web_buttons(1 << B.PLAY); w.render(40); w.ex.web_buttons(0);
  const seen = {};
  for (let t = 0; t < 1200; t += 2) { w.render(2); seen[w.ex.web_step()] = w.ex.web_eff16(5); }
  check(`PITCH +5: step 1 plays the tom as it is (${seen[1]}), step 5 five semitones up (${seen[5]})`, seen[1] === 720 && seen[5] === 720 + 80);
  const plain = await device(); plain.render(200);
  plain.key(BLACK[5], 50); plain.setSteps([0, 4]); plain.render(1000);
  plain.ex.web_buttons(1 << B.PLAY); plain.render(40); plain.ex.web_buttons(0); plain.render(1200);
  const pitched = await device(); pitched.render(200);
  pitched.key(BLACK[5], 50); pitched.setSteps([0, 4]);
  pitched.btn(B.EDIT); pitched.turn(EN.K4, 5); pitched.btn(B.EDIT); pitched.render(1000);
  pitched.ex.web_buttons(1 << B.PLAY); pitched.render(40); pitched.ex.web_buttons(0); pitched.render(1200);
  check("  and it sounds different", plain.sig !== pitched.sig);
}

// ---- PRESETS: scroll through the steps already placed on the lane
{
  const w = await device(); w.render(200);
  const tg = () => w.ex.web_target();
  w.turn(EN.PRESETS, 1);
  check("PRESETS on a lane with no step does nothing", tg() === -1);
  w.setSteps([0, 4, 8]);                                    // kick; 8 (step 9) is the one just placed
  check(`the step just placed is the one picked (${tg()})`, tg() === 8);
  w.turn(EN.PRESETS, 1);
  check(`PRESETS +1: the next step, round to the first (${tg()})`, tg() === 0);
  w.turn(EN.PRESETS, 1);
  check(`  +1: the next one (${tg()})`, tg() === 4);
  w.turn(EN.PRESETS, 1);
  check(`  +1 (${tg()})`, tg() === 8);
  w.turn(EN.PRESETS, -1);
  check(`PRESETS -1: the one before (${tg()})`, tg() === 4);
  w.turn(EN.PRESETS, -2);
  check(`  -2: round the other way (${tg()})`, tg() === 8);
  w.turn(EN.PRESETS, 17);
  check(`  +17 clicks of 3 steps: (8 + 17) round = 4 (${tg()})`, tg() === 4);
  w.render(2000);
  const s0 = w.screen();
  w.turn(EN.PRESETS, 1);
  check("  scrolling redraws the grid and the info", differ(s0, w.screen()) > 50);
  check("  and previews the step when stopped", w.peak > 0.02);
  w.turn(EN.PRESETS, -1);
  w.turn(EN.SELECT, -5);
  const v = (st) => w.ex.web_vel(0, st);
  check(`SELECT edits the step that was scrolled to (step 5 is ${v(4)}, the others ${v(0)} ${v(8)})`, v(4) === 80 && v(0) === 100 && v(8) === 100);
  w.btn(B.EDIT); w.turn(EN.K1, 3); w.turn(EN.PRESETS, -1); w.turn(EN.K1, 5);
  check(`the EDIT page works on it too, and scrolls (${w.ex.web_nudge(0, 4)} ${w.ex.web_nudge(0, 0)})`, w.ex.web_nudge(0, 4) === 3 && w.ex.web_nudge(0, 0) === 5);
  w.btn(B.EDIT); w.btn(B.EDIT);                              // (EDIT 1, 2, then it closes)
  w.key(WHITE[2], 50);                                      // a step placed between: the scroll includes it
  w.turn(EN.PRESETS, 1);
  check(`a step placed later is in the scroll: after 3 comes 5 (${tg()})`, tg() === 4);
  w.key(BLACK[1], 50);
  check("another lane: nothing is picked", tg() === -1);
  w.key(BLACK[0], 50);
  check(`  back on the kick: its picked step is still there (${tg()})`, tg() === 4);
  w.turn(EN.PRESETS, -1);
  check(`  and PRESETS goes on from it (${tg()})`, tg() === 2);
  w.key(BLACK[2], 50); w.setSteps([6, 9]);                  // a lane that never had a pick: the first click is the last / first
  w.key(BLACK[3], 50); w.setSteps([3]); w.key(BLACK[2], 50);
  check(`  the pick is the last step placed or picked, on whatever lane: back on the clap, none (${tg()})`, tg() === -1);
  w.turn(EN.PRESETS, 1);
  check(`  and the first click picks its first step (${tg()})`, tg() === 6);
  w.key(BLACK[4], 50); w.setSteps([5, 10]); w.key(BLACK[0], 50); w.key(BLACK[4], 50);
  check(`  coming back on a lane keeps its last picked step (${tg()})`, tg() === 10);
}

// ---- the grid shows a nudge: the note slides in its cell, cut where it leaves
{
  const px = (fb, x, y) => fb[y * 240 + x];
  const shot = async (nudge, onNudgePage) => {
    const w = await device(); w.render(200);
    w.setSteps([0]);                                        // kick step 1: the cell at (12, 70), 24 px, velocity 100
    w.btn(B.EDIT); w.turn(EN.K1, nudge);
    if (!onNudgePage) w.btn(B.EDIT);
    w.render(300);
    return w.screen();
  };
  // the first step's cell is 11 px wide at x = 12 and 22 px tall at y = 134 (its velocity bar rises from the bottom, 17 px)
  const bar = px(await shot(0, false), 13, 150), off = px(await shot(0, false), 13, 136);
  const plain = await shot(0, false), late = await shot(24, false), early = await shot(-24, false);
  check("a note without nudge fills its cell: bar at both ends", px(plain, 13, 150) === bar && px(plain, 21, 150) === bar);
  check("a late note is cut at the left: no bar at the left end, bar at the right", px(late, 13, 150) !== bar && px(late, 21, 150) === bar);
  check("an early note is cut at the right: bar at the left end, none at the right", px(early, 13, 150) === bar && px(early, 21, 150) !== bar);
  const pn = await shot(0, true), pl = await shot(24, true), pe = await shot(-24, true);
  const tb = px(pn, 14, 176);                               // the NUDGE row is the first row of the page: its bar is at y = 175 .. 177
  check("the NUDGE row: the whole bar when on the grid", tb !== undefined && px(pn, 225, 176) === tb && tb !== px(pl, 14, 176));
  check("  late: cut at the left", px(pl, 14, 176) !== tb && px(pl, 225, 176) === tb);
  check("  early: cut at the right", px(pe, 14, 176) === tb && px(pe, 225, 176) !== tb);
  check("  (the cell's own background is not the bar's colour)", off !== bar);
}

// ---- SAVE: the 16 patterns
{
  const w = await device(); w.render(200);
  const led = (k) => w.led(k), mid = (k) => (w.ex.web_mid_keys() >>> k) & 1;
  check(`power-on: pattern 1 is shown, playing (${w.ex.web_pe()} ${w.ex.web_pp()})`, w.ex.web_pe() === 0 && w.ex.web_pp() === 0 && w.ex.web_pq() === 255);
  const lane = w.screen();
  w.btn(B.SAVE); w.render(100);
  check("SAVE tapped: the pattern page opens and stays", w.ex.web_page() === 3 && w.btnLed(B.SAVE) === 1);
  check("  its page is shown", differ(lane, w.screen()) > 500);
  check("  the current pattern's key is lit, the others are not", led(WHITE[0]) === 1 && led(WHITE[1]) === 0 && (w.ex.web_lit_keys() & WHITE_MASK) === 1 << WHITE[0]);
  w.key(WHITE[2], 100);
  check(`a white key picks that pattern (${w.ex.web_pe() + 1})`, w.ex.web_pe() === 2 && w.ex.web_pp() === 2);
  check("  its key is lit now", led(WHITE[2]) === 1 && led(WHITE[0]) === 0);
  check("  and the page stays", w.ex.web_page() === 3);
  w.btn(B.SAVE); w.render(100);
  check("SAVE tapped again: the song page (16), its LED lit", w.ex.web_page() === 16 && w.btnLed(B.SAVE) === 1);
  w.btn(B.SAVE); w.render(100);
  check("  and once more the page closes", w.ex.web_page() === 0 && w.btnLed(B.SAVE) === 0);
  check("  pattern 3 has no step", (w.ex.web_lit_keys() & WHITE_MASK) === 0);
  w.setSteps([0, 4]);                                       // two kick steps in pattern 3
  w.btn(B.SAVE); w.key(WHITE[0], 50);                       // back to pattern 1
  w.btn(B.SAVE); w.btn(B.SAVE);
  check("pattern 1 has its own (no) steps", (w.ex.web_lit_keys() & WHITE_MASK) === 0 && w.ex.web_pe() === 0);
  w.setSteps([8]);
  check("  a step set in pattern 1 is only in pattern 1", w.ex.web_step_on(0, 0, 8) === 1 && w.ex.web_step_on(2, 0, 8) === 0 && w.ex.web_step_on(2, 0, 4) === 1);
  w.btn(B.SAVE); w.render(100);
  check("on the page, the patterns that hold something are at the mid level", mid(WHITE[0]) === 0 && mid(WHITE[2]) === 1 && mid(WHITE[5]) === 0 && w.led(WHITE[0]) === 1);
  w.btn(B.SAVE); w.btn(B.SAVE); w.btn(B.SAVE); w.btn(B.HOME);
  check("HOME tapped closes the page", w.ex.web_page() === 0);
}
{
  // copy: hold one pattern's key and press another's
  const w = await device(); w.render(200);
  w.key(BLACK[7], 50); w.setSteps([0, 4, 8]);               // RIM in pattern 1
  w.ex.web_buttons(1 << B.SAVE); w.render(60); w.ex.web_buttons(0); w.render(60);
  w.ex.web_keys(1 << WHITE[0]); w.render(60);               // hold pattern 1 ...
  w.ex.web_keys((1 << WHITE[0]) | (1 << WHITE[5])); w.render(60);   // ... press pattern 6
  w.ex.web_keys(0); w.render(100);
  check("hold pattern 1, press pattern 6: it is copied", [0, 4, 8].every((st) => w.ex.web_step_on(5, 7, st)) && w.ex.web_step_on(5, 7, 1) === 0);
  check("  the copy is the one shown, pattern 1 is as it was", w.ex.web_pe() === 5 && [0, 4, 8].every((st) => w.ex.web_step_on(0, 7, st)));
  w.ex.web_keys(1 << WHITE[9]); w.render(60); w.ex.web_keys(0); w.render(60);
  check("a key alone only picks (pattern 10 stays empty)", w.ex.web_pe() === 9 && w.ex.web_pat_used(9) === 0);
  w.btn(B.SAVE); w.btn(B.SAVE);                             // (the song page, then closed)
  w.key(WHITE[5], 50);
  check("  back on the copy: HOME held clears the pattern shown only", true);
  w.btn(B.SAVE); w.key(WHITE[5], 50); w.btn(B.SAVE); w.btn(B.SAVE);
  w.hold(B.HOME, 800);
  check("HOME held clears only the pattern shown (6 is empty, 1 is not)", w.ex.web_pat_used(5) === 0 && w.ex.web_pat_used(0) === 1);
}
{
  // the pattern page: EDIT held deletes the pattern shown, the next ones move down (3 patterns, the 2nd deleted: 1 stays, 3 becomes 2)
  const w = await device(); w.render(200);
  w.key(BLACK[0], 30); w.setSteps([0]);                                                 // 1: a kick on step 1
  np(w, 1); w.key(BLACK[0], 30); w.setSteps([4]);                                      // 2: step 5
  np(w, 1); w.key(BLACK[0], 30); w.setSteps([8]);                                      // 3: step 9, an effect step
  w.btn(B.FX); w.key(BLACK[7], 30); w.key(WHITE[6], 30); w.btn(B.HOME);
  w.btn(B.SAVE); w.btn(B.SAVE);                                                         // the song page: slots 1, 2, 3 are the patterns 1, 2, 3
  w.key(WHITE[0], 30); w.turn(EN.K1, 1); w.key(WHITE[1], 30); w.turn(EN.K1, 2); w.key(WHITE[2], 30); w.turn(EN.K1, 3);
  w.btn(B.SAVE); w.btn(B.SAVE); w.key(WHITE[1], 50); w.render(100);                    // (closed, then) the pattern page, on pattern 2
  const page = w.ex.web_page();
  check("3 patterns, the song 1 2 3, pattern 2 shown", w.ex.web_pe() === 1 && w.ex.web_pat_used(2) === 1 && [0, 1, 2].every((i) => w.ex.web_song(i, 0) === i) && w.ex.web_song(3, 0) === 255);
  w.hold(B.EDIT, 800); w.render(100);
  check("EDIT held on the pattern page: 2 is gone, 3 is 2 (its steps, its effect step), 3 is empty", w.ex.web_step_on(0, 0, 0) === 1 && w.ex.web_step_on(1, 0, 8) === 1 && w.ex.web_step_on(1, 0, 4) === 0 && w.ex.web_fxstep(1, 7, 6) === 1 && w.ex.web_pat_used(2) === 0 && w.ex.web_fxstep(2, 7, 6) === 0);
  check("  the page stays, the number shown stays (2), the header says DELETED", w.ex.web_page() === page && w.ex.web_pe() === 1 && w.ex.web_erased() === 1);
  w.render(200);
  check("  the song lost its slot of pattern 2, and 3 is now 2 (1 2 -)", w.ex.web_song(0, 0) === 0 && w.ex.web_song(1, 0) === 1 && w.ex.web_song(2, 0) === 255);
  w.key(WHITE[0], 50); w.render(200);
  check("  pattern 1 is as it was (its steps)", w.ex.web_pe() === 0 && w.ex.web_step_on(0, 0, 0) === 1);
  w.render(2500);
  const r = await device(w.flash());
  check("  it is saved: after a restart the same", r.ex.web_step_on(1, 0, 8) === 1 && r.ex.web_step_on(1, 0, 4) === 0 && r.ex.web_pat_used(2) === 0 && r.ex.web_fxstep(1, 7, 6) === 1 && r.ex.web_song(1, 0) === 1 && r.ex.web_song(2, 0) === 255);
  w.btn(B.EDIT); w.render(60);
  check("EDIT tapped (not held) on the page still opens the step page", w.ex.web_page() !== page);
}
{
  // deleting while it plays: what plays follows its pattern
  const w = await device(); w.render(200);
  w.key(BLACK[0], 30); w.setSteps([0]);
  np(w, 1); w.key(BLACK[0], 30); w.setSteps([4]);
  np(w, 1); w.key(BLACK[0], 30); w.setSteps([8]);                                       // pattern 3 plays
  w.btn(B.PLAY); w.render(300);
  w.btn(B.SAVE); w.key(WHITE[0], 50);                                                   // pattern 1 shown, 3 plays on
  check(`pattern 3 plays, 1 is shown (${w.ex.web_pp() + 1}, ${w.ex.web_pe() + 1})`, w.ex.web_pp() === 2 && w.ex.web_pe() === 0);
  w.hold(B.EDIT, 800); w.render(300);
  check(`pattern 1 deleted: the one that plays is now 2, still playing (${w.ex.web_pp() + 1}, ${w.ex.web_playing()}), 1 shows the old 2 (step 5)`, w.ex.web_pp() === 1 && w.ex.web_playing() === 1 && w.ex.web_step_on(0, 0, 4) === 1 && w.ex.web_step_on(1, 0, 8) === 1);
  check(`  the one picked (1, now the old 2) still waits for the next pass (${w.ex.web_pq() + 1})`, w.ex.web_pq() === 0);
}
{
  // the battery: 3 bars from the ADC (the stock's 531 / 561 / 591), a bolt when USB powers the box, top right of the header
  const w = await device(); w.render(600);
  check(`the battery starts full (${w.ex.web_batt_level()} of 3)`, w.ex.web_batt_level() === 3);
  const full = w.screen();
  w.ex.web_set_batt(575, 0); w.render(5000);
  check(`  a reading of 575: 2 bars, and the header is drawn again (${w.ex.web_batt_level()})`, w.ex.web_batt_level() === 2 && differ(full, w.screen()) > 10);
  w.ex.web_set_batt(560, 0); w.render(5000);
  check(`  560, just under 561: still 2 bars (a bar goes 3 counts past its threshold) (${w.ex.web_batt_level()})`, w.ex.web_batt_level() === 2);
  w.ex.web_set_batt(545, 0); w.render(5000);
  check(`  545: 1 bar (${w.ex.web_batt_level()})`, w.ex.web_batt_level() === 1);
  const one = w.screen();
  w.ex.web_set_batt(500, 0); w.render(5000);
  check(`  500: none left (${w.ex.web_batt_level()}), the glyph changes`, w.ex.web_batt_level() === 0 && differ(one, w.screen()) > 5);
  w.ex.web_set_batt(620, 0); w.render(8000);
  check(`  back to 620: full again (${w.ex.web_batt_level()})`, w.ex.web_batt_level() === 3);
  const off = w.screen();
  w.ex.web_set_batt(620, 1); w.render(300);
  check("  a host on USB: a bolt in place of the bars (the screen changes), the level is kept", w.ex.web_batt_level() === 3 && differ(off, w.screen()) > 10);
  w.ex.web_set_batt(620, 0); w.render(300);
  check("  and the bars come back", differ(off, w.screen()) === 0);
}
{
  // playing: a pattern picked waits for the end of the pass
  const w = await device(); w.render(200);
  w.key(BLACK[7], 50); w.setSteps([0]);                     // RIM on step 1 of pattern 1; pattern 2 stays empty
  w.btn(B.PLAY); w.render(300);
  w.btn(B.SAVE); w.key(WHITE[1], 50);
  check(`playing: pattern 2 is picked and shown, pattern 1 goes on (${w.ex.web_pe() + 1} ${w.ex.web_pp() + 1} ${w.ex.web_pq() + 1})`, w.ex.web_pe() === 1 && w.ex.web_pp() === 0 && w.ex.web_pq() === 1);
  let at = -1;
  for (let t = 0; t < 2500 && at < 0; t += 2) { w.render(2); if (w.ex.web_pp() === 1) at = w.ex.web_step(); }
  check(`  it takes over at the end of the pass (step ${at + 1} of 16 playing, then the pass restarts)`, at === 15);
  check("  and the wait is over", w.ex.web_pq() === 255);
}
{
  const w = await device(); w.render(200);
  w.key(BLACK[7], 50); w.setSteps([0]);
  w.btn(B.PLAY); w.render(300);
  w.btn(B.SAVE); w.key(WHITE[1], 50);
  check("playing: a pattern picked waits", w.ex.web_pq() === 1);
  w.key(WHITE[0], 50);
  check(`picking the one that plays takes the wait back (${w.ex.web_pq()})`, w.ex.web_pq() === 255 && w.ex.web_pe() === 0 && w.ex.web_pp() === 0);
}
{
  // what is heard: the pass after the switch is the other pattern's
  const hits = async (doSwitch) => {
    const w = await device(); w.render(200);
    w.key(BLACK[7], 50); w.setSteps([0]);                   // RIM on step 1 of pattern 1; pattern 2 empty
    w.btn(B.SAVE);
    w.ex.web_buttons(1 << B.PLAY);
    let o = [];
    o = o.concat(w.onsets(500));
    if (doSwitch) { w.ex.web_keys(1 << WHITE[1]); w.render(30); w.ex.web_keys(0); }
    o = o.concat(w.onsets(4500));
    w.ex.web_buttons(0);
    return o.length;
  };
  const same = await hits(false), switched = await hits(true);
  check(`left alone, 5 s at 120 BPM play the rim 3 times (${same})`, same >= 3 && same <= 4);
  check(`switched to the empty pattern at 0.5 s: only the first rim is heard (${switched})`, switched === 1);
}
{
  // stopped: the pattern picked plays at once, and PLAY plays the one shown
  const w = await device(); w.render(200);
  w.key(BLACK[7], 50); w.setSteps([2]);
  w.btn(B.SAVE); w.key(WHITE[3], 50); w.btn(B.SAVE); w.btn(B.SAVE);
  w.setSteps([6]);
  check(`stopped: the pattern picked is the one that plays (${w.ex.web_pp() + 1})`, w.ex.web_pp() === 3 && w.ex.web_pq() === 255);
  w.btn(B.PLAY); w.render(100);
  check("PLAY plays the pattern shown", w.ex.web_pp() === 3 && w.ex.web_playing() === 1);
}

// ---- what is changed is saved a moment after, and comes back
{
  const a = await device(); a.render(200);
  check("nothing changed: nothing to save, nothing written", a.ex.web_dirty() === 0 && a.ex.web_flash_writes_count() === 0);
  a.key(BLACK[0], 50);
  a.setSteps([0, 4]);                                       // kick, pattern 1
  a.btn(B.EDIT); a.turn(EN.K1, -7); a.turn(EN.K2, 2); a.turn(EN.K3, -4); a.turn(EN.K4, 3); a.btn(B.EDIT);   // step 5 edited
  a.turn(EN.SELECT, -5);
  a.turn(EN.K1, 3);                                         // the kick's TUNE: a note
  a.turn(EN.K2, -4);                                        // its DECAY
  a.glo(() => { a.turn(EN.K1, 13); a.turn(EN.K4, 1); a.turn(EN.K2, -6); });     // 133 BPM, 1/32, volume 88
  a.glo2(() => a.turn(EN.K1, 5));                                               // swing 25
  a.btn(B.SAVE); a.key(WHITE[5], 50); a.btn(B.SAVE); a.btn(B.SAVE);        // pattern 6
  a.key(BLACK[7], 50); a.setSteps([1, 9]);                  // rim
  a.btn(B.SAVE); a.key(WHITE[11], 50); a.btn(B.SAVE); a.btn(B.SAVE);       // pattern 12 (the last object)
  a.key(BLACK[3], 50); a.setSteps([2]);
  a.btn(B.SAVE); a.key(WHITE[5], 50); a.btn(B.SAVE); a.btn(B.SAVE);        // and back to pattern 6, the one shown
  check("changes are waiting to be saved", a.ex.web_dirty() !== 0);
  check("  and the screen is not saying SAVED", a.ex.web_saved() === 0);
  a.render(600);
  check("  not yet: the flash waits for the changes to settle", a.ex.web_flash_writes_count() === 0);
  let saw = 0;
  for (let t = 0; t < 1500; t += 15) { a.render(15); saw |= a.ex.web_saved(); }
  check("  the screen says SAVED for a moment (the header: SAVED in place of STOP)", saw === 1);
  a.render(1500);
  const w1 = a.ex.web_flash_writes_count();
  check(`  then it is saved (${w1} flash operations)`, w1 > 0 && a.ex.web_dirty() === 0);
  a.render(5000);
  check("  once saved, nothing is written again", a.ex.web_flash_writes_count() === w1 && a.ex.web_saved() === 0);

  const b = await device(a.flash()); b.render(200);
  check("the next power-on: nothing to save after the load", b.ex.web_dirty() === 0 && b.ex.web_saved() === 0);
  check(`  the pattern shown is pattern 6 (${b.ex.web_pe() + 1})`, b.ex.web_pe() === 5 && b.ex.web_pp() === 5);
  check(`  tempo 133, subdivision 1/32, volume 88, swing 25 (${b.ex.web_bpm()} ${b.ex.web_div()} ${b.ex.web_vol()} ${b.ex.web_swing()})`, b.ex.web_bpm() === 133 && b.ex.web_div() === 32 && b.ex.web_vol() === 88 && b.ex.web_swing() === 25);
  check(`  the kick's TUNE (${b.ex.web_note16(0)}) and DECAY come back`, b.ex.web_note16(0) === a.ex.web_note16(0));
  check("  the steps of pattern 1 (kick 1 and 5)", [0, 4].every((st) => b.ex.web_step_on(0, 0, st)) && b.ex.web_step_on(0, 0, 1) === 0);
  b.btn(B.SAVE); b.key(WHITE[0], 50); b.btn(B.SAVE); b.btn(B.SAVE);        // show pattern 1: the exports read the pattern shown
  check(`  the step's own settings: nudge, hits, chance, pitch, velocity (${b.ex.web_nudge(0, 4)} ${b.ex.web_ratchet(0, 4)} ${b.ex.web_chance(0, 4)} ${b.ex.web_semi(0, 4)} ${b.ex.web_vel(0, 4)})`,
        b.ex.web_nudge(0, 4) === -7 && b.ex.web_ratchet(0, 4) === 3 && b.ex.web_chance(0, 4) === 80 && b.ex.web_semi(0, 4) === 3 && b.ex.web_vel(0, 4) === 80);
  check("  pattern 6 (rim 2 and 10) and 12 (hat 3)", [1, 9].every((st) => b.ex.web_step_on(5, 7, st)) && b.ex.web_step_on(11, 3, 2) === 1);
  check("  the others are empty", [1, 2, 3, 4, 6, 7, 8, 9, 10, 12, 13, 14, 15].every((p) => b.ex.web_pat_used(p) === 0));
  const c = await device(a.flash()); c.render(200);
  c.key(BLACK[7], 50);
  check("a lane's kit is saved too: the rim's TUNE is its own", c.ex.web_note16(7) === a.ex.web_note16(7));
}
{
  // not while playing; then after it stops
  const w = await device(); w.render(200);
  w.setSteps([0]);
  w.btn(B.PLAY); w.render(300);
  w.key(WHITE[4], 50);                                      // a step placed while playing
  w.render(4000);
  check("playing: the change waits (nothing written, still to save)", w.ex.web_flash_writes_count() === 0 && w.ex.web_dirty() !== 0);
  w.btn(B.PLAY); w.render(1000);
  check("  stopped: it is not written at once", w.ex.web_flash_writes_count() === 0);
  w.render(1500);
  check("  a moment later it is", w.ex.web_flash_writes_count() > 0 && w.ex.web_dirty() === 0);
  const r = await device(w.flash());
  check("  and it comes back", r.ex.web_step_on(0, 0, 0) === 1 && r.ex.web_step_on(0, 0, 4) === 1);
}
{
  // a second save goes to the other copy of the object; the newest wins; a damaged copy leaves the other
  const w = await device(); w.render(200);
  w.setSteps([0]); w.render(2500);
  const f1 = w.flash();
  w.setSteps([4]); w.render(2500);
  const f2 = w.flash();
  const r2 = await device(f2);
  check("two saves in a row: the newest comes back", r2.ex.web_step_on(0, 0, 0) === 1 && r2.ex.web_step_on(0, 0, 4) === 1);
  const r1 = await device(f1);
  check("  the flash after the first save still holds the first state", r1.ex.web_step_on(0, 0, 0) === 1 && r1.ex.web_step_on(0, 0, 4) === 0);
  const torn = f2.slice();                                  // the newest copy of the pattern object damaged (a power cut mid-write)
  for (const off of [0x97000, 0x98000]) {
    const sector = torn.subarray(off, off + 4096);
    if (sector.some((v) => v !== 0xFF)) { const h = new DataView(sector.buffer, sector.byteOffset, 32); if (h.getUint32(8, true) > 1) sector[300] ^= 0xFF; }
  }
  const rt = await device(torn);
  check("  a copy damaged by a power cut: the other copy comes back, nothing crashes", rt.ex.web_step_on(0, 0, 0) === 1);
  const blank = f2.slice(); blank.fill(0xFF, 0xFC000, 0x100000);          // the settings erased: the patterns stay, the rest is as new
  const rb = await device(blank);
  check("  no settings: the defaults, with the patterns", rb.ex.web_bpm() === 120 && rb.ex.web_div() === 16 && rb.ex.web_step_on(0, 0, 0) === 1);
  const junk = f2.slice(); for (let i = 0; i < 4096; i++) junk[0x97000 + i] = (i * 37 + 11) & 0xFF;      // not a commit record at all
  const rj = await device(junk);
  check("  junk in a sector: ignored", rj.ex.web_pe() === 0);
}

// ---- what the page keeps: the worklet's own storeSectors / restoreSectors, through a JSON string as localStorage holds it
{
  const a = await device(); a.render(200);
  a.key(BLACK[7], 50); a.setSteps([1, 5, 9]);               // rim, in pattern 1
  a.btn(B.SAVE); a.key(WHITE[8], 50); a.btn(B.SAVE); a.btn(B.SAVE);        // pattern 9
  a.key(BLACK[0], 50); a.setSteps([0, 4]);                  // kick, in pattern 9
  a.btn(B.EDIT); a.turn(EN.K1, 9); a.turn(EN.K4, -2); a.btn(B.EDIT);
  a.glo(() => { a.turn(EN.K1, 21); a.turn(EN.K4, -1); });   // 141 BPM, 1/8
  a.render(3000);
  check("the changes are saved in the device's flash", a.ex.web_dirty() === 0 && a.ex.web_flash_writes_count() > 0);
  const kept = storeSectors(a.flash());                     // what the worklet sends to the page after a save
  const json = JSON.stringify(Object.fromEntries(Object.entries(kept).map(([off, u8]) => [off, Buffer.from(u8).toString("base64")])));
  check(`  the page keeps ${Object.keys(kept).length} sectors (${(json.length / 1024).toFixed(0)} KB of text in the browser)`, Object.keys(kept).length >= 2 && json.length < 1.5e6);
  const back = Object.fromEntries(Object.entries(JSON.parse(json)).map(([off, b64]) => [off, new Uint8Array(Buffer.from(b64, "base64"))]));
  const nor = new Uint8Array(0x100000).fill(0xFF);          // a power-on: the flash blank, then what the page gives back
  restoreSectors(nor, back);
  const b = await device(nor); b.render(200);
  check(`  after the page gives it back: pattern 9 shown, 141 BPM, 1/8 (${b.ex.web_pe() + 1} ${b.ex.web_bpm()} ${b.ex.web_div()})`, b.ex.web_pe() === 8 && b.ex.web_bpm() === 141 && b.ex.web_div() === 8);
  check("  the steps of both patterns (rim in 1, kick in 9)", [1, 5, 9].every((st) => b.ex.web_step_on(0, 7, st)) && [0, 4].every((st) => b.ex.web_step_on(8, 0, st)));
  check(`  and a step's own settings, in the pattern shown (${b.ex.web_nudge(0, 4)} ${b.ex.web_semi(0, 4)})`, b.ex.web_nudge(0, 4) === 9 && b.ex.web_semi(0, 4) === -2);
}

// ---- GLO, second page: SWING
{
  const gap = async (swing, steps) => {                    // the time between two RIM steps, at 120 BPM, 1/16 (a step is 125 ms)
    const w = await device(); w.render(200);
    w.key(BLACK[7], 50); w.setSteps(steps);
    if (swing) w.glo2(() => w.turn(EN.K1, swing / 5));
    w.render(1500);
    w.ex.web_buttons(1 << B.PLAY);
    const o = w.onsets(1200);
    w.ex.web_buttons(0);
    return o.length >= 2 ? o[1] - o[0] : -1;
  };
  const w = await device(); w.render(200);
  check(`swing is 0 at power-on (${w.ex.web_swing()})`, w.ex.web_swing() === 0);
  w.glo2(() => w.turn(EN.K1, 7));
  check(`GLO, second page, KNOB 1: 5 % a click (${w.ex.web_swing()})`, w.ex.web_swing() === 35);
  w.glo2(() => w.turn(EN.K1, 100)); check(`  100 at most (${w.ex.web_swing()})`, w.ex.web_swing() === 100);
  w.glo2(() => w.turn(EN.K1, -100)); check(`  0 at least (${w.ex.web_swing()})`, w.ex.web_swing() === 0);
  const before = w.screen();
  w.btn(B.GLO); w.btn(B.GLO); w.render(100);
  const page2 = w.screen();
  w.turn(EN.K2, 3); w.turn(EN.K3, -2); w.render(100);
  check("  KNOB 2 (FX mix at its top) and KNOB 3 (the first kit) go no further", differ(page2, w.screen()) === 0);
  w.btn(B.HOME);
  const g0 = await gap(0, [0, 1]), g50 = await gap(50, [0, 1]), g100 = await gap(100, [0, 1]);
  check(`no swing: two steps 125 ms apart (${g0.toFixed(1)})`, near(g0, 125, 3));
  check(`swing 50: the second step comes a quarter step late, 156.3 ms (${g50.toFixed(1)})`, near(g50, 156.25, 3));
  check(`swing 100: half a step late, 187.5 ms (${g100.toFixed(1)})`, near(g100, 187.5, 3));
  const h100 = await gap(100, [1, 2]), e100 = await gap(100, [0, 2]);
  check(`swing 100: the step after the late one is closer, 62.5 ms (${h100.toFixed(1)})`, near(h100, 62.5, 3));
  check(`  two steps of the same kind are not moved, 250 ms (${e100.toFixed(1)})`, near(e100, 250, 3));
}

// ---- GLO, second page: ERASE, a full turn of KNOB 4
{
  const w = await device(); w.render(200);
  w.key(BLACK[7], 50); w.setSteps([0, 4]);                  // pattern 1: rim
  w.btn(B.SAVE); w.key(WHITE[1], 50); w.btn(B.SAVE); w.btn(B.SAVE);        // pattern 2
  w.key(BLACK[0], 50); w.setSteps([8, 12]);                 // kick
  w.render(3000);
  const was = w.ex.web_flash_writes_count();
  w.btn(B.GLO); w.btn(B.GLO); w.render(100);
  const idle = w.screen();
  w.turn(EN.K4, 1);
  check(`KNOB 4 turned one click: the turn begins (${w.ex.web_erase()} of 20)`, w.ex.web_erase() === 1);
  check("  the screen shows the progress", differ(idle, w.screen()) > 100);
  w.turn(EN.K4, 8);
  const under = w.screen();
  w.turn(EN.K4, 1);
  check(`  at half the turn (${w.ex.web_erase()} of 20) the warning shows (the screen changes)`, differ(under, w.screen()) > 150);
  w.turn(EN.K4, 9);
  check(`  one click short (${w.ex.web_erase()} of 20): nothing is erased yet`, w.ex.web_erase() === 19 && w.ex.web_pat_used(1) === 1 && w.ex.web_erased() === 0);
  w.turn(EN.K4, 1);
  check("  the full turn: the pattern shown (2) is erased, 1 is not", w.ex.web_pat_used(1) === 0 && w.ex.web_pat_used(0) === 1);
  check("  the turn starts again from zero, and the header says ERASED", w.ex.web_erase() === 0 && w.ex.web_erased() === 1);
  w.render(1700);
  check("  ERASED goes away", w.ex.web_erased() === 0);
  w.render(2000);
  check("  the erased pattern is saved", w.ex.web_dirty() === 0 && w.ex.web_flash_writes_count() > was);
  const r = await device(w.flash());
  check("  and it stays erased after a restart (pattern 1 still has its steps)", r.ex.web_pat_used(1) === 0 && r.ex.web_pat_used(0) === 1);
  w.turn(EN.K4, 8); w.render(1700);
  check(`a turn that stops halfway is forgotten after a moment (${w.ex.web_erase()})`, w.ex.web_erase() === 0);
  w.turn(EN.K4, 10); w.turn(EN.K4, -4);
  check(`turning back takes the clicks back (${w.ex.web_erase()})`, w.ex.web_erase() === 6);
  w.turn(EN.K4, 12); w.btn(B.HOME);
  check("leaving the page forgets the turn", w.ex.web_erase() === 0 && w.ex.web_page() === 0);
  w.btn(B.GLO); w.turn(EN.K4, 30);
  check("KNOB 4 on the first GLO page is the subdivision, it erases nothing", w.ex.web_erase() === 0 && w.ex.web_pat_used(0) === 1 && w.ex.web_div() === 32);
  w.btn(B.HOME);
}
{
  const w = await device(); w.render(200);
  w.key(BLACK[7], 50); w.setSteps([0, 4, 8, 12]);
  w.btn(B.PLAY); w.render(600);
  check("playing: the pattern sounds", w.peak > 0.02);
  w.btn(B.GLO); w.btn(B.GLO); w.turn(EN.K4, 20);
  check("erased while playing: the pattern is empty", w.ex.web_pat_used(0) === 0 && w.ex.web_playing() === 1);
  w.render(1500); w.render(1500);
  check("  and it falls silent: what was queued does not play", w.peak === 0);
}

// ---- FX: the pages, the steps, the parameters
{
  const w = await device(); w.render(200);
  const page = () => w.ex.web_page(), e0 = FX.LP;
  check("the effects are off at power-on (nothing in any step, no envelope)", [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10].every((e) => w.ex.web_fxenv(e) === 0 && w.ex.web_fxamt(e) === 0));
  w.btn(B.FX); w.render(100);
  check("FX tapped: the first effect page opens and stays", page() === 5 && w.btnLed(B.FX) === 1);
  const p1 = w.screen();
  w.btn(B.FX); w.render(100);
  check("FX tapped again: the second page", page() === 6 && w.btnLed(B.FX) === 1);
  check("  it is another page", differ(p1, w.screen()) > 300);
  w.btn(B.FX); w.render(100);
  check("FX tapped a third time: it closes", page() === 0 && w.btnLed(B.FX) === 0);
  w.btn(B.FX); w.btn(B.HOME);
  check("HOME closes the effect pages too", page() === 0);
  w.btn(B.FX);
  check(`the first effect, HALFTIME, is picked (${FXN[w.ex.web_fxsel()]})`, w.ex.web_fxsel() === 0);
  w.key(BLACK[e0], 30);
  check(`a black key picks an effect: ${FXN[e0]} (${w.ex.web_fxsel()})`, w.ex.web_fxsel() === e0 && w.led(BLACK[e0]) === 1);
  w.turn(EN.ALGO, 1);
  check(`ALGORITHM: the next effect (${FXN[w.ex.web_fxsel()]})`, w.ex.web_fxsel() === e0 + 1);
  w.turn(EN.ALGO, -7);
  check(`  and round the other way (${FXN[w.ex.web_fxsel()]})`, w.ex.web_fxsel() === (e0 + 1 - 7 + 11) % 11);
  w.key(BLACK[e0], 30);
  check("every effect has its own black key, in the order of the signal", FXN.every((_, e) => { w.key(BLACK[e], 20); return w.ex.web_fxsel() === e; }));
  w.key(BLACK[e0], 30);
  // the steps
  check("no step is on", [0, 1, 2, 3].every((s) => w.led(WHITE[s]) === 0));
  w.key(WHITE[3], 30); w.key(WHITE[4], 30);
  check("a white key sets a step of the effect: its LED lights, nothing is heard", w.led(WHITE[3]) === 1 && w.led(WHITE[4]) === 1 && w.ex.web_fxstep(0, e0, 3) === 1 && w.peak === 0);
  check("  the drums' steps are not touched", w.ex.web_step_on(0, 0, 3) === 0 && w.ex.web_pat_used(0) === 1);
  check(`  its mix starts at 100 (${w.ex.web_fxmix(0, e0, 4)})`, w.ex.web_fxmix(0, e0, 4) === 100);

  w.turn(EN.SELECT, -5);
  check(`SELECT sets the mix of the step just placed: -5 clicks = 75 (${w.ex.web_fxmix(0, e0, 4)}); the other step stays (${w.ex.web_fxmix(0, e0, 3)})`, w.ex.web_fxmix(0, e0, 4) === 75 && w.ex.web_fxmix(0, e0, 3) === 100);
  w.turn(EN.SELECT, -100); check(`  0 at least (${w.ex.web_fxmix(0, e0, 4)})`, w.ex.web_fxmix(0, e0, 4) === 0);
  w.turn(EN.SELECT, 100); check(`  100 at most (${w.ex.web_fxmix(0, e0, 4)})`, w.ex.web_fxmix(0, e0, 4) === 100);
  w.key(WHITE[4], 30);
  check("pressed again: the step is off", w.ex.web_fxstep(0, e0, 4) === 0 && w.led(WHITE[4]) === 0);
  w.key(BLACK[2], 30);
  check("another effect has its own steps", w.led(WHITE[3]) === 0 && w.ex.web_fxstep(0, 2, 3) === 0);
  w.key(BLACK[e0], 30);
  check("  and the first one keeps its own", w.led(WHITE[3]) === 1);
  w.key(WHITE[9], 30); w.key(WHITE[12], 30);
  w.turn(EN.PRESETS, 1);
  const tg = w.ex.web_target();
  check(`PRESETS scrolls through the effect's steps (${tg})`, tg === 3 || tg === 9);
  w.turn(EN.PRESETS, 1); w.turn(EN.PRESETS, 1); w.turn(EN.PRESETS, 1);
  w.turn(EN.SELECT, -4);
  check(`  SELECT then edits the one scrolled to (${[3, 9, 12].map((s) => w.ex.web_fxmix(0, e0, s)).join(" ")})`, [3, 9, 12].filter((s) => w.ex.web_fxmix(0, e0, s) === 80).length === 1);
  // the parameters
  const pr = (i) => w.ex.web_fxparam(e0, i);
  check(`LOW PASS starts with CUTOFF 50, RESO 30, FROM 127, 12 dB (${pr(0)} ${pr(1)} ${pr(2)} ${pr(3)})`, pr(0) === 50 && pr(1) === 30 && pr(2) === 127 && pr(3) < 64);
  w.turn(EN.K1, 5); w.turn(EN.K2, -3); w.turn(EN.K3, -10); w.turn(EN.K4, 1);
  check(`page 1, KNOBS 1 to 4: CUTOFF +10, RESO -6, FROM -20, SLOPE 24 dB (${pr(0)} ${pr(1)} ${pr(2)} ${pr(3)})`, pr(0) === 60 && pr(1) === 24 && pr(2) === 107 && pr(3) >= 64);
  w.turn(EN.K4, 5);
  check(`  a choice stops at its last option (${pr(3)})`, pr(3) >= 64);
  w.btn(B.FX);
  check(`ATTACK and RELEASE start at ${[40, 50].join(" and ")} for a filter (${pr(4)} ${pr(5)}), CONTINUOUS is off (${pr(6)}), MIX full (${pr(7)})`, pr(4) === 40 && pr(5) === 50 && pr(6) < 64 && pr(7) === 127);
  w.turn(EN.K1, -10); w.turn(EN.K2, 4); w.turn(EN.K3, 1); w.turn(EN.K4, -20);
  check(`page 2: ATTACK -20, RELEASE +8, CONTINUOUS on, MIX -40 (${pr(4)} ${pr(5)} ${pr(6)} ${pr(7)})`, pr(4) === 20 && pr(5) === 58 && pr(6) >= 64 && pr(7) === 87);
  w.turn(EN.K3, -1);
  check("  CONTINUOUS turned back off", pr(6) < 64);
  check("the screen shows the values", differ(p1, w.screen()) > 300);
  // clearing
  w.btn(B.FX); w.btn(B.FX);                                  // (page 2 closes, then page 1 opens)
  w.key(WHITE[3], 30); w.key(WHITE[9], 30); w.key(WHITE[12], 30);
  w.hold(B.EDIT, 800);
  check("EDIT held on the effect page clears that effect's steps", [3, 9, 12].every((s) => w.ex.web_fxstep(0, e0, s) === 0) && page() === 5);
  w.key(WHITE[1], 30); w.key(BLACK[2], 30); w.key(WHITE[2], 30);
  w.hold(B.HOME, 800);
  check("HOME held clears the pattern: the drums' and every effect's steps", w.ex.web_fxstep(0, e0, 1) === 0 && w.ex.web_fxstep(0, 2, 2) === 0);
}
{
  // a pattern holds effect steps: it is used, copied, erased with them
  const w = await device(); w.render(200);
  w.btn(B.FX); w.key(BLACK[7], 30); w.key(WHITE[6], 30); w.key(WHITE[7], 30); w.btn(B.HOME);
  check("a pattern with only effect steps counts as used", w.ex.web_pat_used(0) === 1);
  w.btn(B.SAVE); w.ex.web_keys(1 << WHITE[0]); w.render(60); w.ex.web_keys((1 << WHITE[0]) | (1 << WHITE[5])); w.render(60); w.ex.web_keys(0); w.render(100); w.btn(B.HOME);
  check("a copied pattern has the effects' steps too", w.ex.web_fxstep(5, 7, 6) === 1 && w.ex.web_fxstep(5, 7, 7) === 1 && w.ex.web_fxstep(0, 7, 6) === 1);
  w.btn(B.GLO); w.btn(B.GLO); w.turn(EN.K4, 20); w.render(100);
  check("the erase turn takes the pattern's effects' steps too (the other pattern keeps them)", w.ex.web_fxstep(5, 7, 6) === 0 && w.ex.web_fxstep(0, 7, 6) === 1);
  w.btn(B.HOME);
  w.btn(B.GLO); w.turn(EN.K2, 0);
  const g = await device(); g.render(200);
  g.btn(B.GLO); g.btn(B.GLO);
  check(`GLO, second page: FX MIX starts at 100 (${g.ex.web_fxmaster()})`, g.ex.web_fxmaster() === 100);
  g.turn(EN.K2, -6);
  check(`  KNOB 2: 5 % a click: 70 (${g.ex.web_fxmaster()})`, g.ex.web_fxmaster() === 70);
  g.turn(EN.K2, -100); check(`  0 at least (${g.ex.web_fxmaster()})`, g.ex.web_fxmaster() === 0);
  g.turn(EN.K2, 100); check(`  100 at most (${g.ex.web_fxmaster()})`, g.ex.web_fxmaster() === 100);
}

{
  const w = await device(); w.render(200);
  w.btn(B.FX);
  for (const e of [FX.DELAY, FX.SAT1, FX.HALF, FX.PAN, FX.REVERB]) { w.key(BLACK[e], 20); w.key(WHITE[0], 20); }
  check(`a step of every effect starts at a mix of 100 (the delay's and the reverb's amount is the effect's MIX: ${w.ex.web_fxparam(FX.DELAY, 7)} = 20 %, ${w.ex.web_fxparam(FX.REVERB, 7)} = 20 %)`,
        [FX.DELAY, FX.REVERB, FX.SAT1, FX.HALF, FX.PAN].every((e) => w.ex.web_fxmix(0, e, 0) === 100) && w.ex.web_fxparam(FX.DELAY, 7) === 26 && w.ex.web_fxparam(FX.REVERB, 7) === 26 &&
        [FX.SAT1, FX.HALF, FX.PAN, FX.LP].every((e) => w.ex.web_fxparam(e, 7) === 127));
  check(`  the REVERB starts at SIZE 20 %, WIDTH 20 % (${w.ex.web_fxparam(FX.REVERB, 0)}, ${w.ex.web_fxparam(FX.REVERB, 3)} of 127 = ${Math.floor(w.ex.web_fxparam(FX.REVERB, 0) * 100 / 127)} %, ${Math.floor(w.ex.web_fxparam(FX.REVERB, 3) * 100 / 127)} %), MIX 20 %`, Math.floor(w.ex.web_fxparam(FX.REVERB, 0) * 100 / 127) === 20 && Math.floor(w.ex.web_fxparam(FX.REVERB, 3) * 100 / 127) === 20 && Math.floor(w.ex.web_fxparam(FX.REVERB, 7) * 100 / 127) === 20);
  w.turn(EN.SELECT, -2);
  check(`  SELECT moves it from there: -2 clicks on the REVERB step = 90 (${w.ex.web_fxmix(0, FX.REVERB, 0)})`, w.ex.web_fxmix(0, FX.REVERB, 0) === 90);
}

// ---- FX: the envelope, a run of steps attacks at the first and releases after the last
{
  const w = await device(); w.render(200);
  w.btn(B.FX); w.key(BLACK[FX.LP], 30);                      // LOW PASS
  [4, 5, 6].forEach((s) => w.key(WHITE[s], 30));            // a run of three steps
  w.turn(EN.K1, 0);
  w.btn(B.FX);                                              // page 2: ATTACK 20 (11 ms), RELEASE 60 (455 ms)
  w.turn(EN.K1, -10); w.turn(EN.K2, 5);
  w.btn(B.FX); w.btn(B.FX);
  w.ex.web_buttons(1 << B.PLAY);                            // (held: no second press)
  const rec = [];
  for (let t = 0; t < 1700; t += 3) { w.render(3); rec.push([t, w.ex.web_step(), w.ex.web_fxenv(FX.LP), w.ex.web_fxamt(FX.LP)]); }
  w.ex.web_buttons(0);
  const first = rec.findIndex(([, st, env]) => st >= 4 && env > 0);
  const before = rec.filter(([, st]) => st < 4).map((r) => r[2]);
  check(`before its steps the effect is closed (the envelope stays 0)`, before.every((v) => v === 0));
  const run = rec.filter(([, st]) => st === 5 || st === 6);
  check(`a run of three steps: the attack is at the first (the envelope is up from step 6 on, ${Math.min(...run.map((r) => r[2]))})`, run.every((r) => r[2] === 65536));
  check(`  and it does not fall between the steps (no dip at their joins)`, run.length > 60);
  const lastEnd = rec.findIndex(([, st]) => st === 7);
  const e7 = rec[lastEnd][2], e7b = rec[lastEnd + 33][2], e7c = rec[Math.min(lastEnd + 120, rec.length - 1)][2];
  check(`after the last step the release begins: ${e7} -> ${e7b} -> ${e7c}`, e7 > e7b && e7b > e7c && e7c > 0);
  check("  and the effect is closed again some time after (the release is over)", rec.slice(-5).every((r) => r[2] === 0));
}
{
  // two separate steps: two attacks and two releases; a step's own mix; the effect's MIX; the global FX MIX
  const w = await device(); w.render(200);
  w.btn(B.FX); w.key(BLACK[FX.LP], 30);
  w.key(WHITE[2], 30); w.turn(EN.SELECT, -10);               // step 3 at 50 %
  w.key(WHITE[8], 30);                                       // step 9 at 100 %
  w.btn(B.FX);
  w.turn(EN.K2, -25);                                        // RELEASE short
  w.btn(B.FX); w.btn(B.FX);
  w.ex.web_buttons(1 << B.PLAY);
  const rec = [];
  for (let t = 0; t < 2100; t += 3) { w.render(3); rec.push([w.ex.web_step(), w.ex.web_fxenv(FX.LP), w.ex.web_fxamt(FX.LP)]); }
  w.ex.web_buttons(0);
  const at = (st) => Math.max(...rec.filter((r) => r[0] === st).map((r) => r[1]));
  check(`a step's own mix is its level: step 3 at 50 % (${at(2)}), step 9 at 100 % (${at(8)})`, near(at(2), 32768, 200) && at(8) === 65536);
  check("  between the two steps the envelope falls back to 0 (two attacks)", rec.filter((r) => r[0] === 5).some((r) => r[1] === 0));
  const m = await device(); m.render(200);
  m.btn(B.FX); m.key(BLACK[FX.LP], 30); m.key(WHITE[0], 30);
  m.btn(B.FX); m.turn(EN.K4, -32); m.btn(B.FX); m.btn(B.FX);        // the effect's MIX 64 / 127 (50 %)
  m.btn(B.GLO); m.btn(B.GLO); m.turn(EN.K2, -10); m.btn(B.HOME);   // the global FX MIX 50
  m.ex.web_buttons(1 << B.PLAY);
  let top = 0; for (let t = 0; t < 600; t += 3) { m.render(3); top = Math.max(top, m.ex.web_fxamt(FX.LP)); }
  check(`the effect's MIX (50 %) and the global FX MIX (50 %) multiply: a quarter (${top} of 65536)`, near(top, 16384, 600));
}
{
  // CONTINUOUS: on at every step, and when stopped
  const w = await device(); w.render(200);
  w.btn(B.FX); w.key(BLACK[FX.LP], 30); w.btn(B.FX); w.turn(EN.K3, 1);
  w.render(2000);
  check(`CONTINUOUS: the effect is on with no step set, and while stopped (${w.ex.web_fxenv(FX.LP)})`, w.ex.web_fxenv(FX.LP) === 65536);
  check("  the other effects are not", w.ex.web_fxenv(0) === 0 && w.ex.web_fxenv(10) === 0);
  w.turn(EN.K3, -1); w.render(3000);
  check("  turned off: it releases", w.ex.web_fxenv(FX.LP) === 0);
}

// ---- FX: the sound of each effect, measured. The same gestures with the effect on and off, so they line up
const rms = (a, from = 0, to = a.length) => { let t = 0; for (let i = from; i < to; i++) t += a[i] * a[i]; return Math.sqrt(t / Math.max(1, to - from)); };
const peakOf = (a) => a.reduce((m, x) => Math.max(m, Math.abs(x)), 0);
const hfRatio = (a) => { const d = new Float32Array(a.length - 1); for (let i = 1; i < a.length; i++) d[i - 1] = a[i] - a[i - 1]; return rms(d) / (rms(a) + 1e-9); };
const finite = (a) => a.every((x) => Number.isFinite(x));
const onsetsOf = (a, gapMs = 8, thr = 0.02) => {            // when (ms) a sound starts in a captured array
  const out = [], gap = gapMs * 44.1; let quiet = 1e9;
  for (let i = 0; i < a.length; i++) { if (Math.abs(a[i]) > thr) { if (quiet > gap) out.push(i / 44.1); quiet = 0; } else quiet++; }
  return out;
};
const crossings = (a, from, to) => { let n = 0; for (let i = from + 1; i < to; i++) if ((a[i - 1] < 0) !== (a[i] < 0)) n++; return n; };
{
  check("the effect pages do not change the drum page", await (async () => {
    const x = await device(); x.render(300); const base = x.screen();
    const y = await device(); y.render(200); y.btn(B.FX); y.key(BLACK[5], 30); y.turn(EN.ALGO, 2); y.btn(B.HOME); y.render(100);
    return differ(base, y.screen()) === 0;
  })());
  // a run: the drums, with one effect on (CONTINUOUS), or off; the gestures are the same either way
  const run = async (e, params, { cont = true, drums, ms = 2000, steps = null, settle = 2500 } = {}) => {
    const w = await device(); w.render(200);
    (drums || ((d) => { d.key(BLACK[0], 30); d.setSteps([0, 4, 8, 12]); d.key(BLACK[1], 30); d.setSteps([4, 12]); d.key(BLACK[3], 30); d.setSteps([0, 2, 4, 6, 8, 10, 12, 14]); }))(w);
    w.btn(B.FX); w.key(BLACK[e], 30);
    for (const [i, v] of Object.entries(params || {})) w.fxSet(e, +i, v);
    if (steps) { w.ex.web_page() === 5 || w.btn(B.FX); if (w.ex.web_page() === 6) { w.btn(B.FX); w.btn(B.FX); } steps.forEach((st) => w.key(WHITE[st], 30)); }
    w.fxSet(e, 6, "on"); if (!cont) w.fxSet(e, 6, "off"); else w.fxSet(e, 6, "on");
    w.btn(B.HOME);
    w.render(settle);                                       // (an effect turned off has released: the same start for both)
    w.ex.web_buttons(1 << B.PLAY);
    const c = w.cap(ms);
    w.ex.web_buttons(0);
    return c;
  };
  const pairs = {};
  for (let e = 0; e < FXN.length; e++) {
    const fx = await run(e, {}), dry = await run(e, {}, { cont: false });
    pairs[e] = { fx, dry };
    const dl = new Float32Array(fx.l.length), dr = new Float32Array(fx.l.length);
    for (let i = 0; i < dl.length; i++) { dl[i] = fx.l[i] - dry.l[i]; dr[i] = fx.r[i] - dry.r[i]; }
    const change = Math.max(rms(dl), rms(dr)), pk = Math.max(peakOf(fx.l), peakOf(fx.r));
    check(`${FXN[e].padEnd(10)} on: it changes the sound (${change.toFixed(4)}), it stays in range (peak ${pk.toFixed(2)}), no NaN`,
          change > 0.003 && pk < 0.98 && finite(fx.l) && finite(fx.r));
  }
  const dry = pairs[0].dry;
  // LOW PASS: the highs go; the sound is darker
  { const f = await run(FX.LP, { 0: 20 }), d = pairs[FX.LP].dry;
    check(`LOW PASS (CUTOFF low): the highs are down (${hfRatio(f.l).toFixed(3)} against ${hfRatio(d.l).toFixed(3)})`, hfRatio(f.l) < hfRatio(d.l) * 0.5);
    const g = await run(FX.LP, { 0: 20 }, { cont: false, steps: [] });
    check("  with the effect closed the sound is the dry one, whatever its parameters", Math.abs(rms(g.l) / rms(d.l) - 1) < 0.03);
  }
  // BAND PASS: a narrow band far above the drums' body takes most of the sound
  { const f = await run(FX.BP, { 0: 120, 1: 10, 2: 120 }), d = pairs[FX.BP].dry;
    check(`BAND PASS (CENTER high, narrow): most of the energy is gone (${rms(f.l).toFixed(4)} against ${rms(d.l).toFixed(4)})`, rms(f.l) < rms(d.l) * 0.5);
  }
  // SATURATION: louder on average for the same peaks
  for (const e of [FX.SAT1, FX.SAT2]) {
    const f = await run(e, { 0: 110, 3: 64 }), d = pairs[e].dry;
    const crest = (c) => rms(c.l) / peakOf(c.l);
    check(`${FXN[e]} (high DRIVE): a fuller sound, its RMS is closer to its peak (${crest(f).toFixed(3)} against ${crest(d).toFixed(3)})`, crest(f) > crest(d) * 1.15);
  }
  // BITCRUSH: few levels
  { const f = await run(FX.CRUSH, { 0: 102, 1: 0, 2: 0 }), d = pairs[FX.CRUSH].dry;
    const levels = (c) => new Set(Array.from(c.l, (x) => Math.round(x * 8388608))).size;
    check(`BITCRUSH (4 bits, no hold): a few levels only (${levels(f)} against ${levels(d)})`, levels(f) < 80 && levels(d) > 500);
  }
  // PAN: hard left
  { const f = await run(FX.PAN, { 0: 0 }), d = pairs[FX.PAN].dry;
    check(`PAN (POSITION left): the right side is down (L ${rms(f.l).toFixed(4)}, R ${rms(f.r).toFixed(4)})`, rms(f.r) < rms(f.l) * 0.05 && Math.abs(rms(d.l) - rms(d.r)) < 1e-6);
    const g = await run(FX.PAN, { 0: 127 });
    check(`  and POSITION right: the left side is (L ${rms(g.l).toFixed(4)}, R ${rms(g.r).toFixed(4)})`, rms(g.l) < rms(g.r) * 0.05);
  }
  // FLANGER: the two sides are not the same sound
  { const f = pairs[FX.FLANGE].fx;
    let diff = 0; for (let i = 0; i < f.l.length; i++) diff += Math.abs(f.l[i] - f.r[i]);
    check(`FLANGER (SPREAD): the left and the right differ (${(diff / f.l.length).toFixed(5)})`, diff / f.l.length > 0.0005);
  }
  // DELAY (a send with a dry / wet mix): one rim, a tempo-synced echo (1/8 at 120 BPM: 250 ms), without feedback, PING PONG off: the same echo on both sides
  { const one = (w) => { w.key(BLACK[7], 30); w.setSteps([0]); };
    const off = { 1: 0, 2: "off", 3: 0, 7: 64 };
    const f = await run(FX.DELAY, off, { drums: one, ms: 900 }), d = await run(FX.DELAY, off, { drums: one, ms: 900, cont: false });
    const of = onsetsOf(f.l), od = onsetsOf(d.l);
    check(`DELAY (1/8, no feedback): the rim, then its echo 250 ms later (${of.map((x) => x.toFixed(0)).join(" ")} against ${od.map((x) => x.toFixed(0)).join(" ")})`,
          od.length === 1 && of.length >= 2 && near(of[1] - of[0], 250, 4));
    check("  PING PONG off: the same echo on both sides", onsetsOf(f.r).length === of.length && near(onsetsOf(f.r)[1] - onsetsOf(f.r)[0], 250, 4) && near(rms(f.l), rms(f.r), rms(f.l) * 0.001));
    const g = await run(FX.DELAY, { 1: 100, 2: "off", 3: 0, 7: 64 }, { drums: one, ms: 1500 });
    check(`  with FEEDBACK the echo repeats (${onsetsOf(g.l, 8, 0.004).length} hits)`, onsetsOf(g.l, 8, 0.004).length >= 4);
    const h = await run(FX.DELAY, { 1: 0, 2: "off", 3: 127, 7: 64 }, { drums: one, ms: 900 });
    const hl = onsetsOf(h.l), hr = onsetsOf(h.r);
    check(`  SPREAD (ping pong off): the right echo comes later, ${hr[1] !== undefined ? (hr[1] - hl[1]).toFixed(0) : "?"} ms after the left (half a step: 62)`, hl.length >= 2 && hr.length >= 2 && near(hr[1] - hl[1], 62.5, 6));
    // PING PONG on: the echoes alternate, the first on the left, the second on the right
    const pp = await run(FX.DELAY, { 1: 70, 2: "on", 3: 127, 7: 64 }, { drums: one, ms: 1200 });
    const ol = onsetsOf(pp.l, 8, 0.008), or = onsetsOf(pp.r, 8, 0.008);
    const has = (a, t) => a.some((x) => near(x, t, 6));
    check(`  PING PONG on: the first echo (250 ms) is on the left (${ol.map((x) => x.toFixed(0)).join(" ")}), not the right (${or.map((x) => x.toFixed(0)).join(" ")})`, has(ol, ol[0] + 250) && !has(or, or[0] + 250));
    check("  the second (500 ms) is on the right, not the left", has(or, ol[0] + 500) && !has(ol, ol[0] + 500));
    const zero = await run(FX.DELAY, { 1: 70, 2: "on", 3: 0, 7: 64 }, { drums: one, ms: 1200 });
    check("  SPREAD 0: both echoes in the middle (left and right alike)", near(rms(zero.l), rms(zero.r), rms(zero.l) * 0.05));
  }
  // REVERB: a tail where the dry sound is silent
  { const one = (w) => { w.key(BLACK[7], 30); w.setSteps([0]); };
    const big = { 0: 80, 3: 127, 7: 127 };                  // (SIZE 63 %, WIDTH and MIX full: what it started with, before it started at 20 %)
    const f = await run(FX.REVERB, big, { drums: one, ms: 1500 }), d = await run(FX.REVERB, big, { drums: one, ms: 1500, cont: false });
    const from = Math.round(300 * 44.1), to = Math.round(1200 * 44.1);
    check(`REVERB: a tail after the hit (${rms(f.l, from, to).toFixed(4)} against ${rms(d.l, from, to).toFixed(4)} dry)`, rms(f.l, from, to) > 0.002 && rms(d.l, from, to) < 1e-6);
    check("  on both sides, a little different", rms(f.r, from, to) > 0.002 && Math.abs(rms(f.l, from, to) - rms(f.r, from, to)) < rms(f.l, from, to) * 0.5);
    const q = await run(FX.REVERB, {}, { drums: one, ms: 1500 });
    check(`  as it starts (SIZE, WIDTH, MIX 20 %): a short, quiet room, still there (${rms(q.l, from, to).toFixed(4)} against ${rms(f.l, from, to).toFixed(4)})`, rms(q.l, from, to) > 0.0001 && rms(q.l, from, to) < rms(f.l, from, to));
  }
  // REPEAT: the sound of the step it opens on is taken, goes through, then comes back
  { const one = (w) => { w.key(BLACK[7], 30); w.setSteps([0]); };
    const f = await run(FX.REPEAT, { 0: 0 }, { drums: one, ms: 700, cont: false, steps: [0, 1, 2] }), d = await run(FX.REPEAT, { 0: 0 }, { drums: one, ms: 700, cont: false, steps: [] });
    const of = onsetsOf(f.l, 8, 0.01), od = onsetsOf(d.l, 8, 0.01);
    check(`REPEAT (DIV 1) on the same steps as the hit: the rim, then it comes back every step (${of.map((x) => x.toFixed(0)).join(" ")} against ${od.map((x) => x.toFixed(0)).join(" ")})`,
          od.length === 1 && of.length >= 4 && near(of[1] - of[0], 125, 6) && near(of[2] - of[1], 125, 6));
    const g = await run(FX.REPEAT, { 0: 120 }, { drums: one, ms: 700, cont: false, steps: [0, 1, 2] });
    const og = onsetsOf(g.l, 8, 0.01);
    check(`  DIV 3: the slice is a third of the step, 42 ms apart (${og.slice(0, 5).map((x) => x.toFixed(0)).join(" ")})`, og.length >= 6 && near(og[2] - og[1], 41.7, 5));
    const h = await run(FX.REPEAT, { 0: 0 }, { drums: one, ms: 900, cont: false, steps: [0, 1, 2], settle: 2500 });
    check("  the repeats are quieter, pass by pass (DECAY)", rms(h.l, Math.round(300 * 44.1), Math.round(400 * 44.1)) <= rms(h.l, Math.round(120 * 44.1), Math.round(220 * 44.1)) + 1e-9);
    const c = await run(FX.REPEAT, { 0: 0 }, { drums: one, ms: 1200 });
    check(`  CONTINUOUS (every step takes a new slice): the hit on the first step is heard (${onsetsOf(c.l, 8, 0.01).map((x) => x.toFixed(0)).join(" ")})`, peakOf(c.l) > 0.02 && onsetsOf(c.l, 8, 0.01).length === 1);
    const t = await device(); t.render(200);
    t.btn(B.FX); t.key(BLACK[0], 30); t.btn(B.FX); t.turn(EN.K3, 1); t.btn(B.HOME); t.render(1500);
    t.key(BLACK[7], 30);
    check("  and while stopped CONTINUOUS leaves the sound alone (a step previewed is heard)", t.audition() > 0.02);
  }
  // HALFTIME: the pitch falls
  { const tone = (w) => { w.key(BLACK[6], 30); w.setSteps([0]); };       // a conga, a decaying tone
    const f = await run(FX.HALF, {}, { drums: tone, ms: 400 }), d = await run(FX.HALF, {}, { drums: tone, ms: 400, cont: false });
    const a = Math.round(60 * 44.1), b = Math.round(200 * 44.1);
    const rf = crossings(f.l, a, b), rd = crossings(d.l, a, b);
    check(`HALFTIME (half speed): the pitch falls, about half the zero crossings (${rf} against ${rd})`, rd > 20 && rf < rd * 0.7 && rf > rd * 0.3);
  }
}

// ---- FX: saved with the pattern and the kit, and back after a restart
{
  const a = await device(); a.render(200);
  a.btn(B.FX); a.key(BLACK[FX.DELAY], 30);                  // DELAY
  [2, 6, 10].forEach((st) => a.key(WHITE[st], 30));
  a.turn(EN.SELECT, -4);                                    // the last step's mix: 100 - 20 = 80
  a.turn(EN.K1, 18); a.turn(EN.K2, -7);                     // TIME (a choice), FEEDBACK
  a.btn(B.FX); a.turn(EN.K1, -9); a.turn(EN.K2, 11); a.turn(EN.K3, 1); a.turn(EN.K4, -6);   // ATTACK, RELEASE, CONTINUOUS, MIX
  a.btn(B.FX); a.btn(B.FX);                                 // back to page 1 for the next effect
  a.key(BLACK[5], 30); a.key(WHITE[0], 30);
  a.btn(B.SAVE); a.key(WHITE[7], 30); a.btn(B.SAVE); a.btn(B.SAVE);        // pattern 8
  a.btn(B.FX); a.key(BLACK[10], 30); a.key(WHITE[15], 30);
  a.btn(B.GLO); a.btn(B.GLO); a.turn(EN.K2, -4);            // the global FX MIX: 80
  a.btn(B.HOME);
  check("the effects' changes are waiting to be saved", a.ex.web_dirty() !== 0);
  a.render(3500);
  check("  they are saved", a.ex.web_dirty() === 0 && a.ex.web_flash_writes_count() > 0);
  const p = (e, i) => a.ex.web_fxparam(e, i);
  const want = [p(FX.DELAY, 0), p(FX.DELAY, 1), p(FX.DELAY, 4), p(FX.DELAY, 5), p(FX.DELAY, 6), p(FX.DELAY, 7)];
  const b = await device(a.flash()); b.render(200);
  check(`after a restart the parameters are back (${[0, 1, 4, 5, 6, 7].map((i) => b.ex.web_fxparam(FX.DELAY, i)).join(" ")} for ${want.join(" ")})`, [0, 1, 4, 5, 6, 7].every((i, k) => b.ex.web_fxparam(FX.DELAY, i) === want[k]));
  check("  the steps, and the step's mix", [2, 6, 10].every((st) => b.ex.web_fxstep(0, FX.DELAY, st) === 1) && b.ex.web_fxmix(0, FX.DELAY, 10) === 80 && b.ex.web_fxmix(0, FX.DELAY, 2) === 100);
  check("  another effect and another pattern's steps", b.ex.web_fxstep(0, 5, 0) === 1 && b.ex.web_fxstep(7, 10, 15) === 1 && b.ex.web_fxstep(0, 10, 15) === 0);
  check(`  the global FX MIX (${b.ex.web_fxmaster()})`, b.ex.web_fxmaster() === 80);
  check("  nothing to save after the load", b.ex.web_dirty() === 0);
  const t = await device(); t.render(200);
  check("a first start has the defaults: DELAY 1/8, CONTINUOUS off", t.ex.web_fxparam(FX.DELAY, 6) < 64 && t.ex.web_fxmaster() === 100);
  const oldsave = a.flash();                                // a save from before the effects: no effects object, no FX MIX
  oldsave.fill(0xFF, 0xDC000, 0xDE000); oldsave.fill(0xFF, 0x9F000, 0xA0000); oldsave.fill(0xFF, 0xFE000, 0xFF000);      // (and no sound object either: it holds the effects' parameters too)
  const c = await device(oldsave); c.render(200);
  check("a flash without the effects' object boots with the effects at their defaults, the patterns intact", c.ex.web_fxparam(FX.DELAY, 6) < 64 && c.ex.web_pe() === a.ex.web_pe());
}
{
  // a save made when the effects stood in another order is read into the order of today
  const crcTable = new Uint32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
  const crc32 = (u8) => { let c = 0xFFFFFFFF; for (const b of u8) c = crcTable[(c ^ b) & 255] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; };
  const object = (magic, perm, patch) => {                          // storage.c's object 5 (the effects) in the old order: effect e has parameters e*10+i, steps on bit e of step... and mix e*5+s
    const len = 3264, pay = new Uint8Array(len), dv = new DataView(pay.buffer);
    for (let i = 0; i < 4; i++) pay[i] = magic.charCodeAt(i);
    for (let e = 0; e < 11; e++) for (let i = 0; i < 8; i++) pay[8 + e * 8 + i] = e * 10 + i;
    for (let k = 0; k < 16; k++) for (let e = 0; e < 11; e++) {
      const base = 96 + k * 198;
      dv.setUint16(base + e * 2, k === 0 ? 1 << e : 0, true);
      for (let st = 0; st < 16; st++) pay[base + 22 + e * 16 + st] = k === 0 ? e * 5 + st : 100;
    }
    if (patch) patch(pay);
    const flash = new Uint8Array(0x100000).fill(0xFF), hdr = new DataView(flash.buffer, 0xDC000, 32);
    flash.set(pay, 0xDC000 + 256);
    hdr.setUint32(0, 0x554C4546, true); hdr.setUint16(4, 5, true); hdr.setUint16(6, 0, true); hdr.setUint32(8, 1, true);
    hdr.setUint32(12, len, true); hdr.setUint32(16, crc32(pay), true); hdr.setUint32(20, 0xFFFFFFFF, true); hdr.setUint32(24, 0xFFFFFFFF, true);
    hdr.setUint32(28, crc32(new Uint8Array(flash.buffer, 0xDC000, 28)), true);
    return flash;
  };
  const F = [8, 0, 1, 2, 10, 3, 4, 5, 6, 7, 9], G = [0, 1, 2, 10, 3, 4, 5, 6, 7, 8, 9];   // old index -> today's
  for (const [name, magic, perm] of [["the first order (REPEAT first)", "DBXF", F], ["the second (REPEAT before the reverb)", "DBXG", G]]) {
    const w = await device(object(magic, perm)); w.render(200);
    const ok = (dev) => perm.every((n, e) => [0, 1, 2, 3, 4, 5, 6, 7].every((i) => dev.ex.web_fxparam(n, i) === e * 10 + i) &&
                                  dev.ex.web_fxstep(0, n, e) === 1 && [0, 5, 15].every((st) => { const v = e * 5 + st; return dev.ex.web_fxmix(0, n, st) === ((n === FX.DELAY && v === 50) || (n === FX.REVERB && v === 30) ? 100 : v); }));   // (the old default mixes of the delay and the reverb became 100)
    check(`a save in ${name}: every effect's parameters, steps and mix are where that effect is now`, ok(w));
    check("  the effects it names are in today's order (REPEAT second to last, SATURATE 2 last)", w.ex.web_fxparam(FX.REPEAT, 0) === perm.indexOf(FX.REPEAT) * 10 && w.ex.web_fxparam(FX.SAT2, 0) === perm.indexOf(FX.SAT2) * 10);
    w.render(3000);
    check("  and it is written again in the new order", w.ex.web_dirty() === 0 && w.ex.web_flash_writes_count() > 0);
    const r = await device(w.flash()); r.render(200);
    check("  after that restart it reads the same (not converted twice)", ok(r));
  }
  // a save from before the delay's and the reverb's amount moved to their MIX (the steps at 50 and 30, the MIX at 127): steps 100, MIX 50 % and 30 %
  {
    const patch = (pay) => {
      pay[8 + FX.DELAY * 8 + 7] = 127; pay[8 + FX.REVERB * 8 + 7] = 127;
      for (let st = 0; st < 16; st++) { pay[96 + 22 + FX.DELAY * 16 + st] = st < 8 ? 50 : 70; pay[96 + 22 + FX.REVERB * 16 + st] = st < 8 ? 30 : 45; }
    };
    const w = await device(object("DBXH", [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10], patch)); w.render(200);
    const ok = (d) => d.ex.web_fxparam(FX.DELAY, 7) === 64 && d.ex.web_fxparam(FX.REVERB, 7) === 39 && d.ex.web_fxparam(FX.SAT1, 7) === 127 * 0 + d.ex.web_fxparam(FX.SAT1, 7) &&
      [0, 7].every((st) => d.ex.web_fxmix(0, FX.DELAY, st) === 100 && d.ex.web_fxmix(0, FX.REVERB, st) === 100) && [8, 15].every((st) => d.ex.web_fxmix(0, FX.DELAY, st) === 70 && d.ex.web_fxmix(0, FX.REVERB, st) === 45);
    check(`an older save: the delay's and the reverb's steps at their old default (50, 30) become 100, the ones set by hand stay (${[0, 8].map((st) => w.ex.web_fxmix(0, FX.DELAY, st)).join(" ")} ${[0, 8].map((st) => w.ex.web_fxmix(0, FX.REVERB, st)).join(" ")}), their MIX is 50 % and 30 % (${w.ex.web_fxparam(FX.DELAY, 7)} ${w.ex.web_fxparam(FX.REVERB, 7)})`, ok(w));
    w.render(3000);
    const r = await device(w.flash()); r.render(200);
    check("  and it is not converted twice", ok(r));
  }
}
{
  // what the effects cost: the same pattern, with every effect on
  const time = async (all) => {
    const w = await device(); w.render(200);
    w.key(BLACK[0], 30); w.setSteps([0, 4, 8, 12]); w.key(BLACK[3], 30); w.setSteps([2, 6, 10, 14]);
    if (all) { w.btn(B.FX); for (let e = 0; e < 11; e++) { w.key(BLACK[e], 20); w.fxSet(e, 6, "on"); } w.btn(B.HOME); }
    w.ex.web_buttons(1 << B.PLAY); w.render(100);
    const t0 = process.hrtime.bigint();
    w.cap(6000);
    const ms = Number(process.hrtime.bigint() - t0) / 1e6;
    w.ex.web_buttons(0);
    return ms / 6000;                                       // the share of real time
  };
  const dry = await time(false), full = await time(true);
  console.log(`drumbox: cost of 6 s of sound in this browser engine: drums ${(dry * 100).toFixed(2)} % of real time, with the 11 effects on ${(full * 100).toFixed(2)} %`);
  check("every effect on at once is still far below real time here (under 25 %)", full < 0.25);
}

// ---- SCL: the master page, HPF / LPF, OTT, soft clipper
{
  const w = await device(); w.render(200);
  const m = (i) => w.ex.web_mst(i);
  check(`the master page starts open: HPF off, LPF off, OTT dry, CLIP 0 dB (${[0, 1, 2, 3].map(m).join(" ")})`, m(0) === 0 && m(1) === 127 && m(2) === 0 && m(3) === 0);
  const lane = w.screen();
  w.btn(B.SCL); w.render(100);
  check("SCL tapped: the master page opens and stays", w.ex.web_page() === 7 && w.btnLed(B.SCL) === 1);
  check("  its page is shown", differ(lane, w.screen()) > 300);
  w.turn(EN.K1, 5); w.turn(EN.K2, -5); w.turn(EN.K3, 10); w.turn(EN.K4, 3);
  check(`KNOBS 1 to 4: HPF +10, LPF -10, OTT +20, CLIP +6 (${[0, 1, 2, 3].map(m).join(" ")})`, m(0) === 10 && m(1) === 117 && m(2) === 20 && m(3) === 6);
  w.turn(EN.K1, -100); w.turn(EN.K2, 100); w.turn(EN.K3, -100); w.turn(EN.K4, -100);
  check(`  the ends: HPF off at the left, LPF off at the right, OTT dry, 0 dB (${[0, 1, 2, 3].map(m).join(" ")})`, m(0) === 0 && m(1) === 127 && m(2) === 0 && m(3) === 0);
  w.turn(EN.K1, 100); w.turn(EN.K2, -100); w.turn(EN.K3, 100); w.turn(EN.K4, 100);
  check(`  and the other ends (${[0, 1, 2, 3].map(m).join(" ")})`, m(0) === 127 && m(1) === 0 && m(2) === 127 && m(3) === 127);
  w.btn(B.SCL); w.render(100);
  check("SCL tapped again: the page closes", w.ex.web_page() === 0 && w.btnLed(B.SCL) === 0);
  w.btn(B.SCL); w.btn(B.HOME);
  check("HOME closes it too", w.ex.web_page() === 0);
  w.render(2500);
  check("the changes are saved", w.ex.web_dirty() === 0 && w.ex.web_flash_writes_count() > 0);
  const r = await device(w.flash()); r.render(200);
  check(`  and they come back after a restart (${[0, 1, 2, 3].map((i) => r.ex.web_mst(i)).join(" ")})`, [0, 1, 2, 3].every((i) => r.ex.web_mst(i) === m(i)));
  const o = await device(); o.render(200);
  const f = new Uint8Array(o.flash()); f.fill(0xFF, 0xDC000, 0xDE000);                     // a save from before the page: no master settings
  const old = await device(f); old.render(200);
  check("  an older save reads them as off", [0, 2, 3].every((i) => old.ex.web_mst(i) === 0) && old.ex.web_mst(1) === 127);
}
{
  // each of the four, measured on the sound
  const play = async (set, drums) => {
    const w = await device(); w.render(200);
    (drums || ((d) => { d.key(BLACK[0], 30); d.setSteps([0, 4, 8, 12]); d.key(BLACK[3], 30); d.setSteps([0, 2, 4, 6, 8, 10, 12, 14]); }))(w);
    w.btn(B.SCL);
    set(w);
    w.btn(B.HOME); w.render(2500);
    w.ex.web_buttons(1 << B.PLAY);
    const c = w.cap(2500);
    w.ex.web_buttons(0);
    return c;
  };
  const base = await play(() => {});
  const kickOnly = (d) => { d.key(BLACK[0], 30); d.setSteps([0, 4, 8, 12]); };
  const hatOnly = (d) => { d.key(BLACK[3], 30); d.setSteps([0, 2, 4, 6, 8, 10, 12, 14]); };
  { const dry = await play(() => {}, kickOnly), hp = await play((w) => w.turn(EN.K1, 50), kickOnly);
    check(`HPF (KNOB 1 up): the low end goes, the kick almost vanishes (${rms(hp.l).toFixed(4)} against ${rms(dry.l).toFixed(4)})`, rms(hp.l) < rms(dry.l) * 0.3);
    const lo = await play((w) => w.turn(EN.K1, 5), kickOnly);
    check(`  a little HPF takes a little (${rms(lo.l).toFixed(4)})`, rms(lo.l) < rms(dry.l) && rms(lo.l) > rms(hp.l)); }
  { const dry = await play(() => {}, hatOnly), lp = await play((w) => w.turn(EN.K2, -60), hatOnly);
    check(`LPF (KNOB 2 down): the highs go (${hfRatio(lp.l).toFixed(3)} against ${hfRatio(dry.l).toFixed(3)})`, hfRatio(lp.l) < hfRatio(dry.l) * 0.5);
    const kd = await play(() => {}, kickOnly), kl = await play((w) => w.turn(EN.K2, -30), kickOnly);
    check(`  and the kick passes (${rms(kl.l).toFixed(4)} against ${rms(kd.l).toFixed(4)})`, rms(kl.l) > rms(kd.l) * 0.6); }
  { const one = (d) => { d.key(BLACK[0], 30); d.setSteps([0]); };                 // one kick, then a long quiet tail
    const dry = await play(() => {}, one), ott = await play((w) => w.turn(EN.K3, 64), one);
    const a = Math.round(250 * 44.1), b = Math.round(600 * 44.1);
    check(`OTT (KNOB 3, wet): what is quiet is lifted (${rms(ott.l, a, b).toFixed(4)} against ${rms(dry.l, a, b).toFixed(4)})`, rms(ott.l, a, b) > rms(dry.l, a, b) * 1.4);
    const half = await play((w) => w.turn(EN.K3, 32), one);
    check(`  half wet is between (${rms(half.l, a, b).toFixed(4)})`, rms(half.l, a, b) > rms(dry.l, a, b) * 1.05 && rms(half.l, a, b) < rms(ott.l, a, b));
    check(`  and it stays in range (peak ${peakOf(ott.l).toFixed(3)})`, peakOf(ott.l) <= 1.0 && finite(ott.l)); }
  { const clip = await play((w) => w.turn(EN.K4, 64)), soft = await play((w) => w.turn(EN.K4, 12));
    check(`CLIP (KNOB 4, +18 dB): the sound is much louder (${rms(clip.l).toFixed(3)} against ${rms(base.l).toFixed(3)}) and cannot go over the ceiling (${peakOf(clip.l).toFixed(3)})`, rms(clip.l) > rms(base.l) * 2 && peakOf(clip.l) < 0.5);
    check(`  it is a soft clip: the peaks flatten, the sound stays smooth (the crest: ${(peakOf(clip.l) / rms(clip.l)).toFixed(2)} against ${(peakOf(base.l) / rms(base.l)).toFixed(2)})`, peakOf(clip.l) / rms(clip.l) < peakOf(base.l) / rms(base.l) * 0.8);
    check(`  a little gain is a little louder (${rms(soft.l).toFixed(3)})`, rms(soft.l) > rms(base.l) * 1.1 && rms(soft.l) < rms(clip.l)); }
}

// ---- the output: full scale is the ceiling, soft clip or not, and nothing goes over it
{
  const loud = async (clipClicks, vol) => {
    const w = await device(); w.render(200);
    w.ex.web_master(1023); w.render(1500);                      // the MASTER pot at the top
    for (let lane = 0; lane < 11; lane++) { w.key(BLACK[lane], 20); w.key(WHITE[0], 20); w.turn(EN.SELECT, 7); w.turn(EN.K4, 15); }   // every drum at step 1, hard
    if (vol) { w.btn(B.GLO); w.turn(EN.K2, vol); w.btn(B.HOME); }
    if (clipClicks) { w.btn(B.SCL); w.turn(EN.K4, clipClicks); w.btn(B.HOME); }
    w.render(1500);
    w.ex.web_buttons(1 << B.PLAY);
    const c = w.cap(1500);
    w.ex.web_buttons(0);
    return c;
  };
  const off = await loud(0, 20), on = await loud(64, 20), plain = await loud(0, 0);
  const pk = (c) => Math.max(peakOf(c.l), peakOf(c.r));
  check(`soft clip off, pushed (volume 127): the peak comes up to the ceiling and does not pass it (${pk(off).toFixed(5)})`, pk(off) > 0.9 && pk(off) <= 1.0);
  check(`soft clip on (+18 dB): the same ceiling, whatever the gain (${pk(on).toFixed(5)})`, pk(on) > 0.9 && pk(on) <= 1.0);
  check(`  the sound is fuller with it, the ceiling is the same (RMS ${rms(on.l).toFixed(3)} against ${rms(off.l).toFixed(3)})`, rms(on.l) > rms(off.l) * 1.05);
  check(`  and a sound that does not reach it is left alone (volume 100: peak ${pk(plain).toFixed(3)})`, pk(plain) <= 1.0);
  const flat = (c) => c.l.filter((x) => Math.abs(x) > 0.9999).length;
  check(`soft clip off: the peaks are bent by the limiter's knee, none is cut flat at 0 dBFS (${flat(off)} samples there)`, flat(off) === 0);
  const quiet = async () => { const w = await device(); w.render(200); w.key(BLACK[7], 30); w.setSteps([0]); w.render(300); w.ex.web_buttons(1 << B.PLAY); const c = w.cap(300); w.ex.web_buttons(0); return c; };
  const q0 = await quiet(), q1 = await quiet();
  check("the same gestures give the same samples (the limiter has no memory)", q0.l.every((x, i) => x === q1.l[i]));
}

// ---- the screen: the oscilloscope between the title and the steps
{
  const rows = (a, b, y0, y1) => { let n = 0; for (let y = y0; y < y1; y++) for (let x = 0; x < 240; x++) if (a[y * 240 + x] !== b[y * 240 + x]) n++; return n; };
  const SC0 = 58, SC1 = 130;                                // the scope's rows: 58 .. 129
  const w = await device(); w.render(500);
  const s0 = w.screen(); w.render(200); const s1 = w.screen();
  check("in silence the scope is a flat line, and still", rows(s0, s1, SC0, SC1) === 0);
  const flat = [...Array(216).keys()].every((x) => { let col = -1; for (let y = SC0 + 8; y < SC1 - 8; y++) if (s0[y * 240 + 12 + x] === s0[(SC0 + 36) * 240 + 12 + x]) col = y; return col >= 0; });
  check("  the centre line is there, across the panel", flat);
  const d0 = w.ex.web_screen_draws(); w.render(1000); const per = w.ex.web_screen_draws() - d0;
  check(`  and it is drawn every other frame, about 33 times a second (${per} draws in a second)`, per >= 28 && per <= 36);
  w.key(BLACK[0], 30);
  w.ex.web_keys(1 << WHITE[0]); w.render(40); w.ex.web_keys(0);                     // a step placed: the drum is heard
  const a = w.screen(); w.render(40); const b = w.screen();
  check(`a sound moves the wave (${rows(a, b, SC0, SC1)} pixels of the scope changed in 40 ms)`, rows(a, b, SC0, SC1) > 200);
  check("  and nothing else on the screen: the header, the title, the steps and the rows are as they were", rows(a, b, 0, SC0) === 0 && rows(a, b, SC1, 240) === 0);
  const c = w.screen();
  let lit = 0; for (let y = SC0; y < SC1; y++) for (let x = 12; x < 228; x++) if (c[y * 240 + x] !== s0[y * 240 + x]) lit++;
  w.render(3000);
  check("  it comes back to the flat line when the sound is over (the corners still say which step)", rows(s0, w.screen(), SC0 + 20, SC1) === 0);
  // the layout: the scope does not touch the title or the steps whatever the page
  for (const [name, go] of [["EDIT", (d) => d.btn(B.EDIT)], ["FX", (d) => d.btn(B.FX)], ["SAVE", (d) => d.btn(B.SAVE)], ["SCL", (d) => d.btn(B.SCL)], ["GLO", (d) => d.btn(B.GLO)]]) {
    const q = await device(); q.render(200); go(q); q.render(400);
    const before = q.screen();
    q.ex.web_keys(1 << WHITE[0]); q.render(30); q.ex.web_keys(0);
    q.render(40);
    check(`the ${name} page keeps the same frame (the scope redraws only its own rows)`, rows(before, q.screen(), 0, SC0) <= 300 && rows(before, q.screen(), 0, 20) <= 40);
  }
}

// ---- the boot screen is the logo and the name and nothing else; the playhead is a whole cell, red; the rest is green, no white
{
  const w = await device(undefined, true); w.render(200);
  const sp = w.screen(), bgc = sp[0];
  let ink = 0, outside = 0, minx = 240, maxx = 0;
  for (let y = 0; y < 240; y++) for (let x = 0; x < 240; x++) if (sp[y * 240 + x] !== bgc) {
    ink++; minx = Math.min(minx, x); maxx = Math.max(maxx, x);
    if (y < 40 || y > 190) outside++;
  }
  check("the boot screen has the logo and the name on it (some ink in the middle)", ink > 100);
  check("  and nothing else: no ink above the logo or below the three lines", outside === 0);
  check("  centred: the ink is as far from the left edge as from the right (+-2 px)", Math.abs(minx - (239 - maxx)) <= 2);
  const band = (y0, y1) => { let n = 0; for (let y = y0; y < y1; y++) for (let x = 0; x < 240; x++) if (sp[y * 240 + x] !== bgc) n++; return n; };
  check(`  the logo (${band(44, 96)} px) over three lines: U0U (${band(104, 134)} px), By (${band(146, 160)}), @Y_oulisse (${band(164, 182)}), and gaps between`,
        band(44, 96) > 300 && band(104, 134) > 100 && band(146, 160) > 20 && band(164, 182) > 80 && band(97, 103) === 0 && band(135, 145) === 0 && band(161, 163) === 0);
  w.key(BLACK[0], 20); w.key(WHITE[0], 20);                 // the keys work under it
  w.render(800);
  const after = w.screen();
  let rest = 0; for (let y = 0; y < 96; y++) for (let x = 0; x < 240; x++) if (after[y * 240 + x] !== bgc) rest++;
  check("after it the page replaces the name (the header and the title are drawn)", rest > 200);
  // the playhead: the cell's interior corner (x+1, y+1) is red #ff4646 (byte-swapped in the frame buffer) for the cell playing
  const PH = 0x28FA, cx = (s) => 12 + Math.floor(s / 4) * 56 + (s % 4) * 12 + 1, GY = 134;
  const lit = (fb) => { const o = []; for (let s = 0; s < 16; s++) if (fb[(GY + 1) * 240 + cx(s)] === PH) o.push(s); return o; };
  check("stopped: no cell is lit as the playhead", lit(w.screen()).length === 0);
  w.setSteps([4, 8]); w.turn(EN.SELECT, 40);                // the last step at full velocity: its bar must not hide the playhead
  w.btn(B.PLAY); let ok = 0, n = 0, seen = new Set();
  for (let i = 0; i < 60; i++) {
    w.render(37); const l = lit(w.screen()), c = w.ex.web_step(); n++;
    if (l.length === 1 && (l[0] === c || l[0] === (c + 15) % 16)) ok++;
    if (l.length === 1) seen.add(l[0]);
  }
  check(`playing: one cell, the one playing, is lit (${ok} of ${n} looks)`, ok >= n - 3);
  check(`  it moves along the steps (${seen.size} cells seen in 2.2 s)`, seen.size >= 12);
  w.btn(B.PLAY); w.render(200);
  check("stopped again: the cell goes out", lit(w.screen()).length === 0);
  // no white on the screen: the text, the values, the bars and the wave are the green, what moves is the violet
  const whites = (fb) => { let n = 0; for (const v of fb) { const c = ((v & 0xFF) << 8) | (v >> 8), r = (c >> 11) * 255 / 31, g = ((c >> 5) & 63) * 255 / 63, b = (c & 31) * 255 / 31; if (Math.min(r, g, b) > 140 && Math.max(r, g, b) - Math.min(r, g, b) <= 24) n++; } return n; };
  const green = (fb) => fb.filter((v) => v === 0xB1C7).length;
  const pages = [["the lane page", () => {}], ["GLO 1", () => w.btn(B.GLO)], ["GLO 2", () => w.btn(B.GLO)], ["the edit page", () => { w.btn(B.HOME); w.btn(B.EDIT); }],
    ["an effect page", () => { w.btn(B.HOME); w.btn(B.FX); w.key(BLACK[7], 20); }], ["the master page", () => { w.btn(B.HOME); w.btn(B.SCL); }],
    ["the pattern page", () => { w.btn(B.HOME); w.btn(B.SAVE); }],
    ["LFO 1", () => { w.btn(B.HOME); w.btn(B.LFO); }], ["LFO 2", () => w.btn(B.LFO)], ["ENV 1", () => { w.btn(B.HOME); w.btn(B.ENV); }], ["ENV 2", () => w.btn(B.ENV)],
    ["ARP 1", () => { w.btn(B.HOME); w.btn(B.ARP); }], ["ARP 2", () => w.btn(B.ARP)],
    ["EDIT 2", () => { w.btn(B.HOME); w.btn(B.EDIT); w.btn(B.EDIT); }], ["the song", () => { w.btn(B.HOME); w.btn(B.SAVE); w.btn(B.SAVE); }],
    ["help 1", () => { w.btn(B.HOME); w.btn(B.SEQ); }], ["help 2", () => w.btn(B.SEQ)], ["help 3", () => w.btn(B.SEQ)]];
  for (const [name, go] of pages) { go(); w.render(100); const fb = w.screen(); check(`no white on ${name} (${whites(fb)} pixels), and green (${green(fb)} pixels)`, whites(fb) === 0 && green(fb) > 50); }
}

// ---- SEQ held: the last beat loops, and on release the pattern is where it would have got to; SEQ + KNOB 1: the subdivision
{
  const w = await device();
  w.key(BLACK[0], 20); w.key(WHITE[10], 20);                 // a kick on step 10 (0 based), nothing else
  w.btn(B.PLAY);
  const waitStep = (n) => { for (let i = 0; i < 4000 && w.ex.web_step() !== n; i++) w.render(2); };
  const sd = () => w.ex.web_steps_done();
  waitStep(9);
  w.ex.web_buttons(1 << B.SEQ);
  const seen = []; let last = -1;
  for (let t = 0; t < 2600; t += 2) { w.render(2); const c = w.ex.web_step(); if (c !== last) { seen.push([sd(), c]); last = c; } }
  // the loop starts a quarter of a second after the press (a tap is the help): the first jump back is where it caught the last beat
  const cap = seen.findIndex(([, c], i) => i > 0 && c !== (seen[i - 1][1] + 1) % 16);
  const win = cap > 0 ? [0, 1, 2, 3].map((k) => (seen[cap - 1][1] - 3 + k + 16) % 16) : [];            // the 4 steps up to the one that began at the catch
  check("SEQ held while playing: the loop is on, and its LED too", w.ex.web_loop() === 1 && w.btnLed(B.SEQ) === 1);
  const after = seen.slice(cap);
  check(`  it loops the last beat: only the 4 steps up to the one that began at the catch (${win.map((s) => s + 1).join(",")}) sound after it (${seen.slice(0, 11).map((x) => x[1] + 1).join(" ")} ...)`, cap >= 2 && after.length >= 8 && after.every(([, c]) => win.includes(c)));
  const cyc = after.every(([, c], i, a) => i === 0 || c === (a[i - 1][1] === win[3] ? win[0] : (a[i - 1][1] + 1) % 16));
  check("  in order, round and round", cyc);
  const hits = w.onsets(2000).length;
  check(`  the kick on step 10 comes every beat (${hits} in 2 s: a beat lasts 0.5 s at 120 BPM)`, win.includes(10) && hits >= 3 && hits <= 5);
  const x0 = seen[cap - 1][0], c0 = seen[cap - 1][1];       // the boundary where the loop caught: the pattern's own position goes on from there
  w.ex.web_buttons(0);
  w.render(1200);
  const now = sd(), c = w.ex.web_step();
  check(`  let go: the loop is off, and the pattern is where it would have got to (step ${c}, expected ${(c0 + now - x0) % 16})`,
    w.ex.web_loop() === 0 && c === (c0 + now - x0) % 16);
  w.render(333); const c2 = w.ex.web_step(), n2 = sd();
  check("  and it goes on from there, step by step", c2 === (c0 + n2 - x0) % 16);
  w.btn(B.PLAY);
  // the loop's own subdivision: SEQ held + KNOB 1 (2 beats, 1 beat, 1/2, 1/4); the tempo grid and the TUNE are not touched
  const nt = w.ex.web_note16(0), dv = () => w.ex.web_div();
  const loopLen = (delta) => {                               // hold SEQ at step 3, turn KNOB 1, count the different steps heard
    w.btn(B.PLAY); waitStep(3); w.ex.web_buttons(1 << B.SEQ);
    if (delta) w.turn(EN.K1, delta);
    w.render(400);
    const set = new Set();
    for (let t = 0; t < 1400; t += 2) { w.render(2); set.add(w.ex.web_step()); }
    const shot = w.screen();
    w.ex.web_buttons(0); w.render(100); w.btn(B.PLAY);
    return [set.size, shot];
  };
  const [m4] = loopLen(0), [m2] = loopLen(1), [m1] = loopLen(1), [m1b] = loopLen(1), [m8] = loopLen(-3), [m4b] = loopLen(1);
  check(`SEQ held: the loop is a beat (${m4} steps), KNOB 1 clockwise: half a beat (${m2}), a quarter (${m1}), and no further (${m1b})`, m4 === 4 && m2 === 2 && m1 === 1 && m1b === 1);
  check(`  counter-clockwise: two beats (${m8} steps), and back to a beat (${m4b})`, m8 === 8 && m4b === 4);
  check("  the tempo grid is not touched (still 1/16)", dv() === 16);
  check("  nor is the lane's TUNE", w.ex.web_note16(0) === nt);
  w.turn(EN.K1, 1);
  check("  KNOB 1 alone is TUNE again", w.ex.web_note16(0) !== nt);
  // the scope says what loops while SEQ is held
  const rowsDiff = (a, b) => { let n = 0; for (let y = 62; y < 76; y++) for (let x = 140; x < 226; x++) if (a[y * 240 + x] !== b[y * 240 + x]) n++; return n; };
  w.btn(B.PLAY); w.render(100); const idle = w.screen(); w.ex.web_buttons(1 << B.SEQ); w.render(400); const held = w.screen(); w.ex.web_buttons(0); w.render(100);
  check(`  the scope's corner reads LOOP while it is held (${rowsDiff(idle, held)} pixels differ)`, rowsDiff(idle, held) > 40);
  w.btn(B.PLAY);
  // at 1/32 a beat is 8 steps
  w.btn(B.GLO); w.turn(EN.K4, 1); w.btn(B.HOME);
  const [m32] = loopLen(0);
  check(`at 1/32 the loop is a beat of 8 steps (${m32} different steps heard while held)`, dv() === 32 && m32 === 8);
}

// ---- the kits: GLO page 2, KNOB 3 (CLASSIC (STD) and the others)
{
  const KITS = ["CLASSIC", "TIGHT"];
  const w = await device();
  const setKit = (k) => { w.btn(B.GLO); w.btn(B.GLO); w.turn(EN.K3, k - w.ex.web_kit()); w.btn(B.HOME); w.render(400); };
  const feat = (lane) => {                                   // the drum of a lane, stopped: placing step 16 plays it
    w.key(BLACK[lane], 20);
    w.ex.web_keys(1 << WHITE[15]); const c = w.cap(300); w.ex.web_keys(0); w.render(100);
    w.key(WHITE[15], 20);                                    // (off again)
    let e = 0, zc = 0, pk = 0;
    for (let i = 0; i < c.l.length; i++) { e += c.l[i] * c.l[i]; pk = Math.max(pk, Math.abs(c.l[i]), Math.abs(c.r[i])); if (i && (c.l[i] < 0) !== (c.l[i - 1] < 0)) zc++; }
    return { sig: `${Math.sqrt(e / c.l.length).toFixed(3)}|${zc}`, pk };
  };
  check("the first kit is CLASSIC (STD)", w.ex.web_kit() === 0);
  w.btn(B.GLO); w.btn(B.GLO);
  w.turn(EN.K3, 1);
  w.btn(B.HOME);
  check("KNOB 3 on the second GLO page picks the next kit (TIGHT)", w.ex.web_kit() === 1);
  w.btn(B.GLO); w.btn(B.GLO); w.turn(EN.K3, 20); w.btn(B.HOME);
  check(`  and stops at the last (${w.ex.web_kit()}: ${KITS[w.ex.web_kit()]})`, w.ex.web_kit() === KITS.length - 1);
  w.btn(B.GLO); w.btn(B.GLO); w.turn(EN.K3, -20); w.btn(B.HOME);
  check("  and at the first", w.ex.web_kit() === 0);
  // picking a kit plays its kick
  w.btn(B.GLO); w.btn(B.GLO); w.render(300);
  w.ex.web_enc(EN.K3, 1); const pk0 = w.cap(300); let pkv = 0; for (const v of pk0.l) pkv = Math.max(pkv, Math.abs(v));
  check(`  picking a kit plays its kick (peak ${pkv.toFixed(2)})`, pkv > 0.05 && pkv <= 1);
  w.btn(B.HOME);
  const sigs = [], bad = [];
  for (let k = 0; k < KITS.length; k++) {
    setKit(k);
    const row = []; 
    for (let lane = 0; lane < 11; lane++) { const f = feat(lane); row.push(f.sig); if (!(f.pk > 0.004 && f.pk <= 1)) bad.push(`${KITS[k]}/${NAMES[lane]}:${f.pk.toFixed(3)}`); }
    sigs.push(row);
  }
  check(`every drum of every kit sounds, and never over 0 dBFS (${bad.length ? bad.join(" ") : `all ${KITS.length * 11}`})`, bad.length === 0);
  const diffs = sigs.map((r) => r.filter((x, i) => x !== sigs[0][i]).length);
  check(`every model kit sounds unlike STD (lanes that differ: ${diffs.slice(1).join(" ")} of 11)`, diffs.slice(1).every((n) => n >= 4));
  const uniq = new Set(sigs.map((r) => r.join(";")));
  check(`  and the kits are all different (${uniq.size} of ${KITS.length})`, uniq.size === KITS.length);
  // the kick's pitch is a note in every kit, and TUNE steps by semitones inside the kit's range
  setKit(1); w.key(BLACK[0], 20);
  const n80 = w.ex.web_note16(0); w.turn(EN.K1, 1); const n80b = w.ex.web_note16(0);
  check(`the kick of kit 80 has its own pitch (${n80}) and TUNE steps a semitone from it (${n80b})`, n80 !== 502 && n80b - n80 > 0 && n80b - n80 <= 16);
  w.turn(EN.K1, 40); const top = w.ex.web_note16(0); w.turn(EN.K1, -80); const bot = w.ex.web_note16(0);
  check(`  and stays within a octave each way (${bot} .. ${top})`, top - bot <= 384 && top - bot >= 300);
  // a kit change starts the lanes over: the kit's own designed sound; LEVEL stays
  setKit(0); w.key(BLACK[0], 20);
  check("a kit change gives the lanes the kit's own pitch", w.ex.web_note16(0) !== n80b);
  // saved
  setKit(1); w.render(3000);
  const kick6 = feat(0).sig;
  const r = await device(w.flash()); r.render(200);
  check(`the kit is saved: the next power-on is on kit ${KITS[1]} (${r.ex.web_kit()})`, r.ex.web_kit() === 1);
  r.key(BLACK[0], 20);
  r.ex.web_keys(1 << WHITE[15]); const cr = r.cap(300); r.ex.web_keys(0);
  let e = 0, zc = 0; for (let i = 0; i < cr.l.length; i++) { e += cr.l[i] * cr.l[i]; if (i && (cr.l[i] < 0) !== (cr.l[i - 1] < 0)) zc++; }
  check(`  and its kick sounds as it did (${kick6} / ${Math.sqrt(e / cr.l.length).toFixed(3)}|${zc})`, kick6.split("|")[0] === Math.sqrt(e / cr.l.length).toFixed(3) && Math.abs(+kick6.split("|")[1] - zc) <= 6);   // (its click is noise: the rms is the same, a zero crossing or two may move)
}

// ---- the kit is saved by its id, not by its place in the list: TIGHT keeps the number it always had (7), so that older saves find it
{
  const w = await device(); w.render(200);
  w.btn(B.GLO); w.btn(B.GLO); w.turn(EN.K3, 1); w.btn(B.HOME); w.render(3500);
  const f = w.flash(); const ids = [];
  for (const base of [0xFC000, 0xFD000]) if (f[base + 256] === 0x44 && f[base + 257] === 0x42 && f[base + 258] === 0x58 && f[base + 259] === 0x31) ids.push(f[base + 256 + 11]);
  check(`the settings hold the id of TIGHT (${ids.join(",")}: 7), not its place in the list (1)`, ids.includes(7) && w.ex.web_kit() === 1);
}

// ---- the LFO button: two LFOs, LFO 1 on the volume, LFO 2 on the pan (RATE, AMOUNT, SHAPE, PHASE), locked to the tempo
{
  const PG = { LFO1: 8, LFO2: 9 };
  const RATE_NAMES = 8, SHAPE_SQUARE = 4, SHAPE_RAMP_UP = 2;
  const make = async () => {                                  // the cymbal on every step: a steady sound, the same on both sides
    const w = await device();
    w.key(BLACK[10], 20); for (let st = 0; st < 16; st++) w.key(WHITE[st], 10);
    return w;
  };
  const openLfo = (w, k) => { while (w.ex.web_page() !== (k ? PG.LFO2 : PG.LFO1)) w.btn(B.LFO); };
  const closeLfo = (w) => { while (w.ex.web_page() === PG.LFO1 || w.ex.web_page() === PG.LFO2) w.btn(B.LFO); };
  const set = (w, k, { rate, amt, shape, phase }) => {        // the LFO k's settings, by clicks from where they are
    openLfo(w, k);
    const now = (i) => w.ex.web_lfo(k, i);
    if (rate !== undefined) w.turn(EN.K1, rate - now(0));
    if (amt !== undefined) w.turn(EN.K2, (amt - now(1)) / 5);
    if (shape !== undefined) w.turn(EN.K3, shape - now(2));
    if (phase !== undefined) w.turn(EN.K4, phase - now(3));
    closeLfo(w);
  };
  const wins = (w, n, ms = 50) => { const o = []; for (let j = 0; j < n; j++) { const c = w.cap(ms); let a = 0, b = 0; for (let i = 0; i < c.l.length; i++) { a += c.l[i] * c.l[i]; b += c.r[i] * c.r[i]; } o.push([Math.sqrt(a / c.l.length), Math.sqrt(b / c.l.length)]); } return o; };
  const tap = (w, ms) => { w.ex.web_buttons(1 << B.PLAY); w.render(ms); w.ex.web_buttons(0); };
  const play = (w) => { if (w.ex.web_playing()) { tap(w, 20); w.render(40); } tap(w, 20); };   // from the top (stopped first if need be): the LFOs start their cycles
  const avg = (a, i0, i1, side) => { let s = 0; for (let i = i0; i < i1; i++) s += a[i][side]; return s / (i1 - i0); };

  // the pages
  {
    const w = await device();
    check("LFO: a tap opens LFO 1, again LFO 2, again it closes", (() => { w.btn(B.LFO); const a = w.ex.web_page(); w.btn(B.LFO); const b = w.ex.web_page(); w.btn(B.LFO); return a === PG.LFO1 && b === PG.LFO2 && w.ex.web_page() === 0; })());
    w.btn(B.LFO);
    check("  the LFO's LED is lit while a page is open", w.btnLed(B.LFO) === 1);
    w.btn(B.HOME);
    check("  and out when it is closed", w.btnLed(B.LFO) === 0);
    check(`  LFO 1 starts at 1/8, LFO 2 at 1/2, both off (${[0, 1].map((k) => [0, 1, 2, 3].map((i) => w.ex.web_lfo(k, i)).join(",")).join(" | ")})`,
      w.ex.web_lfo(0, 0) === 5 && w.ex.web_lfo(1, 0) === 3 && [0, 1].every((k) => w.ex.web_lfo(k, 1) === 0 && w.ex.web_lfo(k, 2) === 0 && w.ex.web_lfo(k, 3) === 0));
    openLfo(w, 0);
    w.turn(EN.K1, 20); w.turn(EN.K2, 30); w.turn(EN.K3, 20); w.turn(EN.K4, 25);
    check(`  the knobs stop at the ends (rate ${w.ex.web_lfo(0, 0)} of 0..7, amount ${w.ex.web_lfo(0, 1)}, shape ${w.ex.web_lfo(0, 2)} of 0..5), and PHASE goes round (${w.ex.web_lfo(0, 3)})`,
      w.ex.web_lfo(0, 0) === 7 && w.ex.web_lfo(0, 1) === 100 && w.ex.web_lfo(0, 2) === 5 && w.ex.web_lfo(0, 3) === 1);
    w.turn(EN.K1, -20); w.turn(EN.K2, -30); w.turn(EN.K3, -20); w.turn(EN.K4, -2);
    check(`  and down (rate ${w.ex.web_lfo(0, 0)}, amount ${w.ex.web_lfo(0, 1)}, shape ${w.ex.web_lfo(0, 2)}, phase ${w.ex.web_lfo(0, 3)} round to 23)`,
      w.ex.web_lfo(0, 0) === 0 && w.ex.web_lfo(0, 1) === 0 && w.ex.web_lfo(0, 2) === 0 && w.ex.web_lfo(0, 3) === 23);
    check("  LFO 2 is its own (still as it was)", w.ex.web_lfo(1, 0) === 3 && w.ex.web_lfo(1, 3) === 0);
  }
  // amount 0 leaves the sound exactly as it is, whatever the rest says: two runs with the same clicks (the same time), opposite ways
  {
    const run = async (dir) => {
      const w = await make();
      openLfo(w, 0); w.turn(EN.K1, 4 * dir); w.turn(EN.K3, 3 * dir); w.turn(EN.K4, 7 * dir); closeLfo(w);
      openLfo(w, 1); w.turn(EN.K1, 3 * dir); w.turn(EN.K3, 5 * dir); w.turn(EN.K4, 11 * dir); closeLfo(w);
      const vals = [0, 1].map((k) => [0, 1, 2, 3].map((i) => w.ex.web_lfo(k, i)).join(",")).join("|");
      w.render(5000); w.sig = 0;
      play(w); w.render(1500);
      return { sig: w.sig, vals };
    };
    const a = await run(1), b = await run(-1);
    check(`amount 0 on both: the sound is the same whatever the rate, shape and phase (${a.vals} against ${b.vals})`, a.vals !== b.vals && a.sig === b.sig);
  }
  // the pan: LFO 2, square, full amount, a cycle of a half note (1 s at 120 BPM): right for half of it, then left
  {
    const w = await make();
    set(w, 1, { rate: 3, amt: 100, shape: SHAPE_SQUARE, phase: 0 });
    play(w); w.render(80);
    const a = wins(w, 20);                                    // 100 ms .. 1.1 s
    const r1 = avg(a, 1, 7, 1), l1 = avg(a, 1, 7, 0), r2 = avg(a, 11, 17, 1), l2 = avg(a, 11, 17, 0);
    check(`PAN LFO, square, 100 %: right first (R ${r1.toFixed(3)} against L ${l1.toFixed(3)}), then left (L ${l2.toFixed(3)} against R ${r2.toFixed(3)})`, r1 > 0.02 && l1 < r1 * 0.1 && l2 > 0.02 && r2 < l2 * 0.1);
    check("  and the side that plays is as loud as the sound alone", Math.abs(r1 - l2) / r1 < 0.25);
    // the phase: 180 degrees (12 clicks): left first
    set(w, 1, { phase: 12 }); play(w); w.render(80);
    const b = wins(w, 20);
    check(`  PHASE 180: left first (L ${avg(b, 1, 7, 0).toFixed(3)}, R ${avg(b, 1, 7, 1).toFixed(3)}), then right`, avg(b, 1, 7, 1) < avg(b, 1, 7, 0) * 0.1 && avg(b, 11, 17, 0) < avg(b, 11, 17, 1) * 0.1);
    // the amount: 50 % pushes the far side down by half a quarter turn of a cosine, not to nothing
    set(w, 1, { phase: 0, amt: 50 }); play(w); w.render(80);
    const c = wins(w, 20), farL = avg(c, 1, 7, 0), nearR = avg(c, 1, 7, 1);
    check(`  AMOUNT 50: the far side is down, not out (L ${farL.toFixed(3)}, R ${nearR.toFixed(3)})`, farL > nearR * 0.5 && farL < nearR * 0.95);
  }
  // the rate follows the tempo: flips of the side in 4 s
  {
    const flips = (a) => { let n = 0, last = 0; for (const [l, r] of a) { const s = l > r ? -1 : 1; if (last && s !== last && Math.abs(l - r) > 0.01) n++; if (Math.abs(l - r) > 0.01) last = s; } return n; };
    const w = await make();
    set(w, 1, { rate: 3, amt: 100, shape: SHAPE_SQUARE });
    play(w); w.render(30); const f2 = flips(wins(w, 80));     // 4 s: 4 cycles
    set(w, 1, { rate: 4 }); play(w); w.render(30); const f4 = flips(wins(w, 80));
    check(`the rate: a half note flips ${f2} times in 4 s (4 cycles = 7 or 8), a quarter ${f4} (8 cycles = 15 or 16)`, f2 >= 6 && f2 <= 8 && f4 >= 14 && f4 <= 16);
    w.btn(B.GLO); w.turn(EN.K1, 40); w.btn(B.HOME);           // 160 BPM: faster
    set(w, 1, { rate: 3 }); play(w); w.render(30); const f160 = flips(wins(w, 60));     // 3 s at 160: 4 cycles
    check(`  and it is locked to the tempo (160 BPM: ${f160} flips in 3 s, cycles of 0.75 s = 7 or 8)`, w.ex.web_bpm() === 160 && f160 >= 6 && f160 <= 8);
  }
  // the shapes: a square only has two sides, a sine goes through the ones between
  {
    const mid = (a) => a.filter(([l, r]) => Math.max(l, r) > 0.02 && Math.min(l, r) / Math.max(l, r) > 0.15 && Math.min(l, r) / Math.max(l, r) < 0.85).length;
    const w = await make();
    set(w, 1, { rate: 3, amt: 100, shape: SHAPE_SQUARE }); play(w); w.render(80); const sq = mid(wins(w, 40));
    set(w, 1, { shape: 0 }); play(w); w.render(80); const sn = mid(wins(w, 40));
    set(w, 1, { shape: 1 }); play(w); w.render(80); const tr = mid(wins(w, 40));
    set(w, 1, { shape: 5 }); play(w); w.render(80); const rn = wins(w, 60), sides = new Set(rn.map(([l, r]) => (l > r * 1.5 ? "L" : r > l * 1.5 ? "R" : "C")));
    check(`the shapes: windows between the sides, square ${sq}, sine ${sn}, triangle ${tr} of 40; RANDOM visits ${[...sides].join("")}`, sq <= 6 && sn >= 8 && tr >= 8 && sides.size >= 2);
  }
  // the volume: LFO 1
  {
    const w = await make();
    set(w, 0, { rate: 3, amt: 100, shape: SHAPE_SQUARE, phase: 0 });
    play(w); w.render(80);
    const a = wins(w, 20);
    const hi = (avg(a, 1, 7, 0) + avg(a, 1, 7, 1)) / 2, lo = (avg(a, 11, 17, 0) + avg(a, 11, 17, 1)) / 2;
    check(`VOLUME LFO, square, 100 %: the sound (${hi.toFixed(3)}) for half the cycle, then nothing (${lo.toFixed(4)})`, hi > 0.02 && lo < hi * 0.05);
    check("  on both sides alike", Math.abs(avg(a, 1, 7, 0) - avg(a, 1, 7, 1)) < 0.002);
    set(w, 0, { amt: 50 }); play(w); w.render(80);
    const b = wins(w, 20), hi2 = avg(b, 1, 7, 0), lo2 = avg(b, 11, 17, 0);
    check(`  AMOUNT 50: the low half is half the high one (${(lo2 / hi2).toFixed(2)})`, Math.abs(lo2 / hi2 - 0.5) < 0.08);
    set(w, 0, { amt: 100, shape: SHAPE_RAMP_UP, rate: 4 }); play(w); w.render(30);       // a pump: down on the beat, back up by the next
    const c = wins(w, 20);
    check(`  RAMP UP ducks at the start of its cycle and swells back (${avg(c, 0, 2, 0).toFixed(3)} then ${avg(c, 8, 10, 0).toFixed(3)})`, avg(c, 0, 2, 0) < avg(c, 8, 10, 0) * 0.4);
    // together: the volume and the pan do not disturb each other
    set(w, 0, { rate: 3, amt: 100, shape: SHAPE_SQUARE }); set(w, 1, { rate: 3, amt: 100, shape: SHAPE_SQUARE, phase: 0 });
    play(w); w.render(80);
    const d = wins(w, 20);
    check(`  with the pan on too: the first half is right and loud, the second silent (${avg(d, 1, 7, 0).toFixed(4)} ${avg(d, 1, 7, 1).toFixed(4)} | ${avg(d, 11, 17, 0).toFixed(4)} ${avg(d, 11, 17, 1).toFixed(4)})`, avg(d, 1, 7, 1) > 0.02 && avg(d, 1, 7, 0) < avg(d, 1, 7, 1) * 0.1 && avg(d, 11, 17, 0) < 0.002 && avg(d, 11, 17, 1) < 0.002);
  }
  // the picture: an LFO page shows the LFO in the oscilloscope's place
  {
    const w = await make();
    const GREEN = 0xB1C7, VIOLET = 0x7FB4, SC_X = 12, SC_Y = 58;           // (frame buffer words, byte-swapped)
    const panel = (fb) => { const o = []; for (let y = SC_Y; y < SC_Y + 72; y++) for (let x = 0; x < 216; x++) o.push(fb[y * 240 + SC_X + x]); return o; };
    const diff = (a, b) => { let n = 0; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) n++; return n; };
    const traceRows = (fb) => { const rows = new Set(); for (let y = SC_Y + 18; y < SC_Y + 66; y++) for (let x = 30; x < 90; x++) if (fb[y * 240 + SC_X + x] === GREEN) rows.add(y); return rows.size; };
    play(w); w.render(300);
    openLfo(w, 0); w.render(120);
    const off = w.screen();
    check(`LFO 1 at 0 %: the picture is a flat line, though the cymbal plays (${traceRows(off)} rows of trace)`, traceRows(off) <= 4);
    w.turn(EN.K2, 20); w.render(120);
    const on = w.screen();
    check(`  AMOUNT up: the curve appears (${diff(panel(off), panel(on))} pixels changed)`, diff(panel(off), panel(on)) > 150 && traceRows(on) > 10);
    w.turn(EN.K3, 2); w.render(120);
    const ramp = w.screen();
    check(`  SHAPE: another curve (${diff(panel(on), panel(ramp))} pixels)`, diff(panel(on), panel(ramp)) > 150);
    w.turn(EN.K4, 6); w.render(120);
    check(`  PHASE: the curve moves along (${diff(panel(ramp), panel(w.screen()))} pixels)`, diff(panel(ramp), panel(w.screen())) > 150);
    // the cursor: a violet line that runs along the cycle (1/8 at 120 BPM: a cycle every 250 ms)
    const colOf = (fb) => { for (let x = 0; x < 216; x++) if (fb[(SC_Y + 66) * 240 + SC_X + x] === VIOLET) return x; return -1; };
    const c0 = colOf(w.screen()); w.render(90); const c1 = colOf(w.screen());
    const dx = ((c1 - c0) % 102 + 102) % 102;
    check(`  the cursor runs along the cycle (column ${c0} then ${c1} 90 ms later: ${dx} of the cycle's 102, 37 expected)`, c0 >= 0 && c1 >= 0 && dx >= 25 && dx <= 50);
    closeLfo(w); w.render(120);
    check("  closing the page brings the oscilloscope back", traceRows(w.screen()) > 4 || diff(panel(w.screen()), panel(ramp)) > 150);
  }
  // before the effects: LFO 1 pumps the dry sound, not the delay's echo (a kick on step 1, the delay always on at 3/8 = 750 ms,
  // LFO 1 a square on a 1 s cycle: open for the kick, closed when its echo comes)
  {
    const run = async (amt) => {
      const w = await device();
      w.key(BLACK[0], 20); w.key(WHITE[0], 20);
      w.fxSet(FX.DELAY, 6, "on"); w.fxSet(FX.DELAY, 0, 38); w.btn(B.HOME);       // (3 clicks, an option each: TIME 3/8)
      set(w, 0, { rate: 3, amt, shape: SHAPE_SQUARE });
      w.render(5000);
      play(w);
      const a = wins(w, 40, 25);
      const m = (i0, i1) => { let e = 0; for (let i = i0; i < i1; i++) e += (a[i][0] ** 2 + a[i][1] ** 2) / 2; return Math.sqrt(e / (i1 - i0)); };
      return { dry: m(0, 6), echo: m(29, 36) };
    };
    const off = await run(0), on = await run(100);
    const ro = on.echo / on.dry, rf = off.echo / off.dry;
    check(`LFO 1 is before the effects: the delay's echo is not pumped (echo / kick ${ro.toFixed(2)} with the gate, ${rf.toFixed(2)} without)`,
      off.echo > 0.002 && ro > rf * 0.8 && ro < rf * 1.25);
  }
  // the pan acts above 100 Hz: the kick (its body is under it) stays in the middle, the cymbal goes right
  {
    const w = await device();
    w.key(BLACK[0], 20); for (const st of [0, 2, 4, 6, 8, 10, 12, 14]) w.key(WHITE[st], 10);      // a kick on every second step: a steady bass
    set(w, 1, { rate: 3, amt: 100, shape: SHAPE_SQUARE, phase: 0 });
    w.render(500); play(w); w.render(80);
    const a = wins(w, 20);
    const l = avg(a, 1, 7, 0), r = avg(a, 1, 7, 1);
    check(`the pan leaves the bass in the middle (a kick: L ${l.toFixed(3)}, R ${r.toFixed(3)} with the pan hard right)`, l > 0.02 && Math.min(l, r) / Math.max(l, r) > 0.6);
  }
  // the toms and congas move too (the split was 300 Hz: they hardly did)
  for (const [lane, name] of [[5, "tom"], [6, "conga"], [1, "snare"]]) {
    const w = await device();
    w.key(BLACK[lane], 20); for (const st of [0, 4, 8, 12]) w.key(WHITE[st], 10);
    set(w, 1, { rate: 3, amt: 100, shape: SHAPE_SQUARE, phase: 0 });
    w.render(500); play(w); w.render(80);
    const a = wins(w, 20);
    const r = avg(a, 1, 7, 1), l = avg(a, 1, 7, 0);
    check(`the pan moves a ${name} (hard right: R ${r.toFixed(3)}, L ${l.toFixed(3)})`, r > 0.001 && l < r * 0.5);
  }
  // never over 0 dBFS, and saved
  {
    const w = await make();
    set(w, 0, { rate: 2, amt: 70, shape: 3, phase: 5 }); set(w, 1, { rate: 6, amt: 85, shape: 1, phase: 9 });
    play(w); w.render(3000);
    w.ex.web_buttons(1 << B.PLAY); w.render(60); w.ex.web_buttons(0); w.render(2500);      // stopped: saved
    const r = await device(w.flash());
    const got = [0, 1].map((k) => [0, 1, 2, 3].map((i) => r.ex.web_lfo(k, i)).join(","));
    check(`the LFOs are saved (${got.join(" | ")})`, got[0] === "2,70,3,5" && got[1] === "6,85,1,9");
    play(r); let pk = 0; for (let i = 0; i < 40; i++) { r.render(50); pk = Math.max(pk, r.peak); }
    check(`  and still never over 0 dBFS (peak ${pk.toFixed(3)})`, pk <= 1);
  }
}

// ---- OCT- / OCT+ walk a pattern's two sections: the track doubled with OCT+ (asked, then confirmed), taken away with OCT- (asked, then confirmed)
{
  const w = await device(); w.render(200);
  const pe = () => w.ex.web_pe(), oct = (up) => { w.btn(up ? B.OCTUP : B.OCTDN); w.render(60); };
  w.key(BLACK[0], 20); w.setSteps([0, 8]);                       // a kick on steps 1 and 9 of the first section
  oct(false);
  check(`OCT- with one section does nothing (asks nothing: ${w.ex.web_oct_ask()}, section ${w.ex.web_vs()}, pattern ${pe() + 1})`, w.ex.web_oct_ask() === 0 && w.ex.web_vs() === 0 && pe() === 0 && w.ex.web_two() === 0);
  const plain = w.screen();
  oct(true);
  check(`OCT+ once asks to double the track (${w.ex.web_oct_ask()}), nothing is doubled yet (${w.ex.web_two()}), the screen says it`, w.ex.web_oct_ask() === 1 && w.ex.web_two() === 0 && w.screen().some((v, i) => v !== plain[i]));
  oct(true);
  check(`OCT+ again doubles it: the second section has the first one's steps (${w.ex.web_two()}, shown ${w.ex.web_vs() + 1} of 2, steps ${w.ex.web_step_on(16, 0, 0)}${w.ex.web_step_on(16, 0, 8)}${w.ex.web_step_on(16, 0, 4)})`, w.ex.web_two() === 1 && w.ex.web_vs() === 1 && w.ex.web_oct_ask() === 0 && w.ex.web_step_on(16, 0, 0) === 1 && w.ex.web_step_on(16, 0, 8) === 1 && w.ex.web_step_on(16, 0, 4) === 0);
  check("  the pattern stays the same one (1), the grid shows the second section's steps", pe() === 0 && (w.ex.web_lit_keys() & WHITE_MASK) === ((1 << WHITE[0]) | (1 << WHITE[8])));
  w.setSteps([4]);                                               // a step on the second section only
  check("  a step set in the second section is not in the first", w.ex.web_step_on(16, 0, 4) === 1 && w.ex.web_step_on(0, 0, 4) === 0);
  oct(false);
  check(`OCT- goes back to the first section (${w.ex.web_vs()}), which is as it was (step 5 ${w.ex.web_step_on(0, 0, 4)})`, w.ex.web_vs() === 0 && w.ex.web_step_on(0, 0, 4) === 0 && w.ex.web_oct_ask() === 0);
  oct(true);
  check("OCT+ goes to the second section again (no question: it exists)", w.ex.web_vs() === 1 && w.ex.web_oct_ask() === 0);
  oct(true);
  check("  and OCT+ there does nothing: two sections at most", w.ex.web_vs() === 1 && w.ex.web_oct_ask() === 0 && w.ex.web_two() === 1);
  oct(false); oct(false);
  check(`OCT- in the first section asks to take the second away (${w.ex.web_oct_ask()}), it is still there (${w.ex.web_two()})`, w.ex.web_oct_ask() === 2 && w.ex.web_two() === 1 && w.ex.web_vs() === 0);
  w.key(BLACK[1], 20);                                           // any other key: no
  check("  another key drops the question, nothing is taken away", w.ex.web_oct_ask() === 0 && w.ex.web_two() === 1);
  oct(false); oct(true);
  check("  the other OCT drops it too", w.ex.web_oct_ask() === 0 && w.ex.web_two() === 1);
  oct(false);
  w.render(4500);
  check("  and so does the time", w.ex.web_oct_ask() === 0 && w.ex.web_two() === 1);
  oct(false); oct(false);
  check(`a second OCT- takes it away (${w.ex.web_two()}), its steps with it (${w.ex.web_step_on(16, 0, 0)}), the first section is as it was (${w.ex.web_step_on(0, 0, 0)}${w.ex.web_step_on(0, 0, 8)})`, w.ex.web_two() === 0 && w.ex.web_vs() === 0 && w.ex.web_step_on(16, 0, 0) === 0 && w.ex.web_step_on(0, 0, 0) === 1 && w.ex.web_step_on(0, 0, 8) === 1);
  oct(false);
  check("  and OCT- does nothing again", w.ex.web_oct_ask() === 0);
}
// ---- doubling or taking a section away writes few flash objects (each erase stops the audio and the key scan for a moment: the keys' LEDs blink)
{
  const w = await device(); w.render(200);
  w.key(BLACK[0], 20); w.setSteps([0, 8]); w.render(4000);
  const n0 = w.ex.web_flash_writes_count();
  w.btn(B.OCTUP); w.render(60); w.btn(B.OCTUP); w.render(60);
  w.render(5000);
  const n1 = w.ex.web_flash_writes_count();
  check(`doubling a track writes ${n1 - n0} flash operations (five objects: the second section's pattern, effects' steps and own values, and the flags in the first object; fourteen was ~200)`, n1 - n0 >= 17 && n1 - n0 <= 100 && w.ex.web_dirty() === 0);
  w.btn(B.OCTDN); w.render(60); w.btn(B.OCTDN); w.render(60); w.btn(B.OCTDN); w.render(60);
  w.render(5000);
  const n2 = w.ex.web_flash_writes_count();
  check(`taking it away writes ${n2 - n1} operations (one object: the flag)`, n2 - n1 >= 1 && n2 - n1 <= 20 && w.ex.web_two() === 0 && w.ex.web_dirty() === 0);
}
// ---- a doubled track plays its first section, then its second, and goes round; the section shown is the one that is edited, whatever plays
{
  const w = await device(); w.render(200);
  w.key(BLACK[0], 20); w.setSteps([0]);                            // a kick on step 1 of the first section
  w.btn(B.OCTUP); w.render(60); w.btn(B.OCTUP); w.render(60);      // doubled: the second section is shown
  w.key(BLACK[0], 20); w.setSteps([0]); w.setSteps([4]);           // in it: no kick on step 1 (taken off), a kick on step 5
  check("the second section holds a kick on step 5 only", w.ex.web_step_on(16, 0, 0) === 0 && w.ex.web_step_on(16, 0, 4) === 1 && w.ex.web_step_on(0, 0, 0) === 1 && w.ex.web_step_on(0, 0, 4) === 0);
  w.btn(B.OCTDN); w.render(60);                                    // the first section shown
  w.btn(B.PLAY); w.render(100);
  const seen = [];
  for (let t = 0; t < 9000 && seen.length < 4; t += 2) { w.render(2); const c = w.ex.web_cs(); if (!seen.length || seen[seen.length - 1] !== c) seen.push(c); }
  check(`it plays the first section, then the second, then the first (sections ${seen.join(" ")})`, seen.length >= 4 && seen.slice(0, 4).join("") === (seen[0] === 0 ? "0101" : "1010"));
  check("  the section shown stays where it was while the other plays (shown 1)", w.ex.web_vs() === 0);
  // the playhead is on the grid only while the section shown plays: step 1 (a kick on it) is lit, and dark under the playhead
  let lit2 = 0, dark1 = 0, other2 = 0;
  for (let t = 0; t < 5000; t += 20) {
    w.render(20);
    const led = w.led(WHITE[0]), cs = w.ex.web_cs(), cur = w.ex.web_step();
    if (cs === 1) { lit2++; if (!led) other2++; }
    else if (cur === 0 && !led) dark1++;
  }
  check(`  the first section is shown: while the second plays its playhead is not drawn (step 1 lit ${lit2 - other2} of ${lit2}), while the first plays it is (dark under it ${dark1} times)`, lit2 > 20 && other2 === 0 && dark1 > 3);
  w.btn(B.PLAY);
  // saved
  w.render(4000);
  const r = await device(w.flash()); r.render(300);
  check(`a doubled track is saved: after a restart it has its second section (${r.ex.web_two()}, step 5 ${r.ex.web_step_on(16, 0, 4)}, step 1 ${r.ex.web_step_on(16, 0, 0)})`, r.ex.web_two() === 1 && r.ex.web_step_on(16, 0, 4) === 1 && r.ex.web_step_on(16, 0, 0) === 0 && r.ex.web_step_on(0, 0, 0) === 1);
}
// ---- headroom: a drum loop with a feedback DELAY, the MASTER pot at its top, does not reach 0 dBFS (the delay adds 1 to 4 dB: it was over, and crackled)
{
  const loop = async (delayFb) => {
    const w = await device(); w.render(200);
    w.key(BLACK[0], 20); w.setSteps([0, 4, 8, 12]);
    w.key(BLACK[1], 20); w.setSteps([4, 12]);
    w.key(BLACK[3], 20); w.setSteps([2, 6, 10, 14]);
    if (delayFb !== null) { w.btn(B.FX); w.key(BLACK[7], 30); for (let st = 0; st < 16; st++) w.key(WHITE[st], 10); w.btn(B.HOME); w.fxSet(FX.DELAY, 1, delayFb); w.btn(B.HOME); }
    w.ex.web_master(1023);                                           // MASTER at the top
    w.btn(B.PLAY); w.render(1500);
    const c = w.cap(4000); let pk = 0, over = 0;
    for (const x of c.l) { const a = Math.abs(x); if (a > pk) pk = a; if (a > 0.9999) over++; }
    return { pk, over };
  };
  const plain = await loop(null), dly = await loop(127);
  check(`MASTER at the top: the loop peaks at ${plain.pk.toFixed(3)}, with a delay at full feedback ${dly.pk.toFixed(3)}: below 0 dBFS, nothing cut (${dly.over} samples at the ceiling)`, plain.pk < 0.9 && dly.pk < 0.97 && dly.over === 0);
}
// ---- the output limiter: MASTER and VOLUME at the top, three drums on every step, a delay at full MIX and feedback: nothing goes past -0.2 dBFS, nothing is cut flat
{
  const loud = async (delay) => {
    const w = await device(); w.render(200);
    w.ex.web_master(1023);
    for (const k of [0, 1, 2]) { w.key(BLACK[k], 20); w.setSteps([...Array(16).keys()]); }
    w.btn(B.GLO); w.turn(EN.K2, 127 - w.ex.web_vol()); w.btn(B.HOME);
    if (delay) { w.btn(B.FX); w.key(BLACK[7], 30); for (let st = 0; st < 16; st++) w.key(WHITE[st], 10); w.btn(B.HOME); w.fxSet(FX.DELAY, 1, 100); w.fxSet(FX.DELAY, 7, 127); w.btn(B.HOME); }
    w.ex.web_over();
    w.ex.web_buttons(1 << B.PLAY); w.render(1500);
    const c = w.cap(4000); w.ex.web_buttons(0);
    let pk = 0, near = 0; for (const x of c.l) { const a = Math.abs(x); pk = Math.max(pk, a); }
    for (const x of c.l) if (Math.abs(x) > pk * 0.995) near++;
    return { pk, near, over: w.ex.web_over() };
  };
  const a = await loud(false), b = await loud(true);
  check(`a mix that went past 0 dBFS (${a.over} samples) and with the delay (${b.over}): the peaks stay at ${a.pk.toFixed(3)} and ${b.pk.toFixed(3)}, under -0.2 dBFS, none cut flat (${b.near} samples within 0.5 % of the top)`,
        a.over > 0 && b.over > 0 && a.pk <= 0.98 && b.pk <= 0.98 && b.near < 400);
}
// ---- the DELAY and the REVERB are sends with a dry / wet mix, in parallel with the dry: x is the amount (the step's, the MIX, through ATTACK and RELEASE);
// the send is min(1, 2x) of the level and the dry min(1, 2 (1 - x)): 0 all dry, 50 % all of both, 100 % all sent and no dry
{
  const lone = async (fx, mix) => {
    const w = await device(); w.render(200);
    w.key(BLACK[7], 20); w.setSteps([0]);
    w.btn(B.FX); w.key(BLACK[fx], 30); for (let st = 0; st < 16; st++) w.key(WHITE[st], 10); w.btn(B.HOME);
    w.ex.web_set_fxparam(fx, 7, mix); w.ex.web_set_fxparam(fx, 1, 0); w.ex.web_set_fxparam(fx, 6, 127);
    w.render(2500);                                                              // (CONTINUOUS: the amount is there before the hit)
    w.ex.web_buttons(1 << B.PLAY); const c = w.cap(700); w.ex.web_buttons(0);
    const pk = (t0, t1) => { let m = 0; for (let i = Math.round(t0 * 44.1); i < Math.round(t1 * 44.1); i++) m = Math.max(m, Math.abs(c.l[i]), Math.abs(c.r[i])); return m; };
    return { dry: pk(0, 60), echo: pk(245, 330) };
  };
  const m0 = await lone(FX.DELAY, 0), m25 = await lone(FX.DELAY, 32), m50 = await lone(FX.DELAY, 64), m100 = await lone(FX.DELAY, 127);
  const ref = m0.dry;
  const r = (v) => (v / ref).toFixed(2);
  check(`DELAY mix 0 / 25 / 50 / 100 %: the hit itself is ${r(m0.dry)} / ${r(m25.dry)} / ${r(m50.dry)} / ${r(m100.dry)} of the dry (all dry up to half, then down to none)`,
        Math.abs(m25.dry - ref) < ref * 0.03 && Math.abs(m50.dry - ref) < ref * 0.03 && m100.dry < ref * 0.05);
  check(`  the echo: none at 0 (${r(m0.echo)}), then ${r(m25.echo)} / ${r(m50.echo)} / ${r(m100.echo)}: it grows to half and stays there (a send at min(1, 2x))`,
        m0.echo < ref * 0.02 && m25.echo > m0.echo && Math.abs(m50.echo - m100.echo) < m50.echo * 0.08 && m50.echo > ref * 0.4 && Math.abs(m25.echo - m50.echo * 0.5) < m50.echo * 0.12);
  const v0 = await lone(FX.REVERB, 0), v100 = await lone(FX.REVERB, 127);
  check(`REVERB mix 0 / 100 %: the hit itself ${(v0.dry / ref).toFixed(2)} then ${(v100.dry / ref).toFixed(2)} of the dry: all dry, then only what the room gives back at once`, Math.abs(v0.dry - ref) < ref * 0.03 && v100.dry < ref * 0.5);
}
// ---- the loudness of a loop with the DELAY open on every step: the dry and the echoes at the level that was sent add up to 4 dB at most (at the middle of the mix), PING PONG on or off,
// SPREAD at 0 or at the top, and the delay, all wet, is quieter than the dry; the feedback changes the length of the echoes and not the level (the send is scaled by it)
{
  const loop = async (lanes, fx) => {
    const w = await device(); w.render(200);
    for (const k of lanes) { w.key(BLACK[k], 20); w.setSteps([...Array(16).keys()]); }
    if (fx) {
      w.btn(B.FX); w.key(BLACK[7], 30); for (let st = 0; st < 16; st++) w.key(WHITE[st], 10); w.btn(B.HOME);
      for (const [i, v] of [[7, fx.mix], [1, fx.fb], [2, fx.pong ? 96 : 0], [3, fx.sp]]) w.ex.web_set_fxparam(FX.DELAY, i, v);
    }
    w.ex.web_buttons(1 << B.PLAY); w.render(2500);
    const c = w.cap(4000); w.ex.web_buttons(0);
    let t = 0, pk = 0; for (let i = 0; i < c.l.length; i++) { t += c.l[i] * c.l[i] + c.r[i] * c.r[i]; pk = Math.max(pk, Math.abs(c.l[i]), Math.abs(c.r[i])); }
    return { rms: Math.sqrt(t / (2 * c.l.length)), pk };
  };
  let worst = -99, wet = -99, spread = 0, worstp = -99;
  const rows = [];
  for (const lanes of [[0], [0, 1, 2], [0, 1, 2, 3, 4, 5]]) {
    const dry = await loop(lanes, null);
    for (const [pong, sp] of [[true, 100], [true, 0], [false, 100], [false, 0]]) {
      const byFb = [];
      for (const fb of [60, 127]) {
        const m = await loop(lanes, { mix: 51, fb, pong, sp }), all = await loop(lanes, { mix: 127, fb, pong, sp });
        const mid = 20 * Math.log10(m.rms / dry.rms), wt = 20 * Math.log10(all.rms / dry.rms);
        worst = Math.max(worst, mid); wet = Math.max(wet, wt); byFb.push(mid);
        worstp = Math.max(worstp, 20 * Math.log10(m.pk / dry.pk), 20 * Math.log10(all.pk / dry.pk));
      }
      spread = Math.max(spread, Math.abs(byFb[0] - byFb[1]));
      rows.push(`${lanes.length}d ${pong ? "pp" : "--"} sp${sp}: ${byFb.map((x) => x.toFixed(1)).join("/")}`);
    }
  }
  console.log("   (" + rows.join("; ") + ")");
  check(`DELAY open on every step, 1 / 3 / 6 drums, PING PONG on and off, SPREAD 0 and 100: at 40 % the loop is at most ${worst.toFixed(1)} dB louder (under 4), all wet at most ${wet.toFixed(1)} dB (under 0), the peaks at most ${worstp.toFixed(1)} dB (under 4), and feedback 60 against 127 differs by ${spread.toFixed(1)} dB (under 1)`,
        worst < 4 && wet < 0 && worstp < 4 && spread < 1);
}
// ---- the DELAY's TIME moved while it plays (the options one after the other), or its SPREAD (PING PONG off: the right line's time), glides and does not click
{
  const run = async (pong, param, move) => {
    const w = await device(); w.render(200);
    w.key(BLACK[0], 20); w.setSteps([...Array(16).keys()]);
    w.btn(B.FX); w.key(BLACK[7], 30); for (let st = 0; st < 16; st++) w.key(WHITE[st], 10); w.btn(B.HOME);
    for (const [i, v] of [[7, 64], [1, 60], [2, pong ? 96 : 0], [3, 20], [0, 20]]) w.ex.web_set_fxparam(FX.DELAY, i, v);
    w.ex.web_buttons(1 << B.PLAY); w.render(2500);
    const L = [], R = []; let v = 20;
    for (let k = 0; k < 20; k++) { if (move) { v += param === 3 ? 5 : 6; w.ex.web_set_fxparam(FX.DELAY, param, v); } const c = w.cap(100); L.push(...c.l); R.push(...c.r); }
    w.ex.web_buttons(0);
    let mx = 0; for (const X of [L, R]) for (let i = 3; i < X.length; i++) mx = Math.max(mx, Math.abs(X[i] - 3 * X[i - 1] + 3 * X[i - 2] - X[i - 3]));
    return mx;
  };
  const sp0 = await run(false, 3, false), sp1 = await run(false, 3, true), t0 = await run(true, 0, false), t1 = await run(true, 0, true);
  check(`DELAY SPREAD moved a click at a time with PING PONG off: the largest edge ${sp1.toFixed(3)} against ${sp0.toFixed(3)} held (it was 9 times that)`, sp1 < sp0 * 1.5);
  check(`  TIME moved through its options while it plays: ${t1.toFixed(3)} against ${t0.toFixed(3)} held`, t1 < t0 * 2.5);
}
// ---- the DELAY with PING PONG off: one loop, the right side the same echoes SPREAD later at every repeat (it would be a loop of its own, drifting further each time)
{
  const w = await device(); w.render(200);
  w.key(BLACK[7], 20); w.setSteps([0]);
  w.btn(B.FX); w.key(BLACK[7], 30); for (let st = 0; st < 16; st++) w.key(WHITE[st], 10); w.btn(B.HOME);
  for (const [i, v] of [[7, 127], [1, 100], [2, 0], [3, 100]]) w.ex.web_set_fxparam(FX.DELAY, i, v);
  w.ex.web_buttons(1 << B.PLAY); const c = w.cap(1700); w.ex.web_buttons(0);
  const ol = onsetsOf(c.l, 8, 0.004), or = onsetsOf(c.r, 8, 0.004);
  const lag = [1, 2, 3, 4].map((k) => or[k] - ol[k]);
  check(`PING PONG off, feedback 100: the right echoes come ${lag.map((x) => x.toFixed(0)).join(", ")} ms after the left ones (the same each time, about half a step at SPREAD 100: 49)`,
        ol.length >= 5 && or.length >= 5 && lag.every((x) => Math.abs(x - lag[0]) < 6) && Math.abs(lag[0] - 49) < 8);
}
// ---- the DELAY's SPREAD (PING PONG on) moves the echoes from the middle to the sides at the same power: an echo's level on its side changes by under 4 dB from SPREAD 0 to 127
{
  const pkOf = (c, t0, t1) => { let m = 0; for (let i = Math.round(t0 * 44.1); i < Math.round(t1 * 44.1); i++) m = Math.max(m, Math.abs(c[i])); return m; };
  const first = async (sp) => {
    const w = await device(); w.render(200);
    w.key(BLACK[7], 20); w.setSteps([0]);
    w.btn(B.FX); w.key(BLACK[7], 30); for (let st = 0; st < 16; st++) w.key(WHITE[st], 10); w.btn(B.HOME);
    for (const [i, v] of [[7, 127], [1, 60], [2, 96], [3, sp]]) w.ex.web_set_fxparam(FX.DELAY, i, v);
    w.ex.web_buttons(1 << B.PLAY); const c = w.cap(400); w.ex.web_buttons(0);
    return 20 * Math.log10(Math.max(pkOf(c.l, 245, 330), pkOf(c.r, 245, 330)));
  };
  const lv = []; for (const sp of [0, 40, 80, 127]) lv.push(await first(sp));
  const spread = Math.max(...lv) - Math.min(...lv);
  check(`the first echo's loudest side at SPREAD 0, 40, 80, 127: ${lv.map((x) => x.toFixed(1)).join(", ")} dB (${spread.toFixed(1)} dB apart; it was 6)`, spread < 4);
}
// ---- a pan does not make the sound louder: a hot mix, MASTER at the top, panned hard (the lanes' own PAN, the PAN effect) is as loud on its side as it is in the middle
// (the chain used to bend the middle with a soft knee and not what a pan puts beside it: +3 dB of peak, more with a delay or a reverb on top)
{
  const level = async (setup) => {
    const w = await device(); w.render(200);
    w.ex.web_master(1023);
    for (const k of [0, 1, 2, 3]) { w.key(BLACK[k], 20); w.setSteps([...Array(16).keys()]); w.turn(EN.K4, 20); }
    setup(w);
    w.render(1500);
    w.ex.web_buttons(1 << B.PLAY); w.render(1500);
    const c = w.cap(4000); w.ex.web_buttons(0);
    let tl = 0, tr = 0, pl = 0, pr = 0; for (let i = 0; i < c.l.length; i++) { tl += c.l[i] * c.l[i]; tr += c.r[i] * c.r[i]; pl = Math.max(pl, Math.abs(c.l[i])); pr = Math.max(pr, Math.abs(c.r[i])); }
    return { l: Math.sqrt(tl / c.l.length), r: Math.sqrt(tr / c.l.length), pl, pr };
  };
  const db = (a, b) => 20 * Math.log10(a / b);
  const centre = await level(() => {});
  const lanes = await level((w) => { for (const k of [0, 1, 2, 3]) { w.key(BLACK[k], 20); w.turn(EN.ALGO, 32); } });
  const fx = await level((w) => { w.btn(B.FX); w.key(BLACK[FX.PAN], 30); w.ex.web_set_fxparam(FX.PAN, 0, 127); w.ex.web_set_fxparam(FX.PAN, 6, 127); w.btn(B.HOME); });
  check(`the lanes panned hard right: the right side is ${db(lanes.r, centre.r).toFixed(1)} dB (rms) and ${db(lanes.pr, centre.pr).toFixed(1)} dB (peak) from the centre's: the same`, Math.abs(db(lanes.r, centre.r)) < 0.7 && Math.abs(db(lanes.pr, centre.pr)) < 0.7);
  check(`  the PAN effect hard right: ${db(fx.r, centre.r).toFixed(1)} dB (rms), ${db(fx.pr, centre.pr).toFixed(1)} dB (peak)`, Math.abs(db(fx.r, centre.r)) < 0.7 && Math.abs(db(fx.pr, centre.pr)) < 0.7);
}
// ---- the filters (LOW PASS, BAND PASS) come last: a filter on the step where an echo comes shapes the echo too (it sat ahead of the delay, which echoed the unfiltered sound)
{
  const echoHighs = async (withLp) => {
    const w = await device(); w.render(200);
    w.key(BLACK[2], 20); w.setSteps([0]);                            // a clap on step 1
    w.btn(B.FX); w.key(BLACK[FX.DELAY], 20); w.key(WHITE[0], 20);    // the DELAY on step 1 only: its echo (1/8, 250 ms) comes at step 3
    w.fxSet(FX.DELAY, 1, 0); w.fxSet(FX.DELAY, 7, 127);              // no feedback, all of it
    if (withLp) { w.key(BLACK[FX.LP], 20); w.key(WHITE[2], 20); w.fxSet(FX.LP, 0, 6); }      // the LOW PASS on step 3 alone, a low cutoff
    w.btn(B.HOME);
    w.btn(B.PLAY);
    const c = w.cap(420), L = c.l, k0 = Math.round(262 * 44.1), k1 = Math.round(380 * 44.1);
    let hi = 0, tot = 0;
    for (let i = k0 + 1; i < k1; i++) { const d = L[i] - L[i - 1]; hi += d * d; tot += L[i] * L[i]; }
    w.btn(B.PLAY);
    return { highs: Math.sqrt(hi / (k1 - k0)), rms: Math.sqrt(tot / (k1 - k0)) };
  };
  const open = await echoHighs(false), shut = await echoHighs(true);
  check(`a DELAY echo that comes on a LOW PASS step is filtered (its highs ${open.highs.toFixed(4)} without, ${shut.highs.toFixed(4)} with)`, open.highs > 0.0004 && shut.highs < open.highs * 0.5);
}
// ---- the effects follow the section that plays: a step of an effect set in the second section opens it there (and not in the first)
{
  const w = await device(); w.render(200);
  w.key(BLACK[0], 20); w.setSteps([0]);                            // a kick on step 1 of the first section
  w.btn(B.OCTUP); w.render(60); w.btn(B.OCTUP); w.render(60);      // doubled, the second section shown
  w.btn(B.FX); w.key(BLACK[7], 30); w.key(WHITE[4], 30); w.btn(B.HOME);     // the DELAY on step 5 of the second section only
  check("the DELAY has step 5 in the second section, none in the first", w.ex.web_fxstep(16, FX.DELAY, 4) === 1 && w.ex.web_fxstep(0, FX.DELAY, 4) === 0);
  w.btn(B.OCTDN); w.render(60);                                    // the first section shown
  w.btn(B.PLAY);
  let openB = 0, openA = 0, secB = 0, secA = 0;
  for (let t = 0; t < 6000; t += 10) {
    w.render(10);
    const env = w.ex.web_fxenv(FX.DELAY);
    if (w.ex.web_cs() === 1) { secB++; if (env > 30000) openB++; }
    else { secA++; if (env > 0) openA++; }
  }
  check(`the DELAY opens in the second section (${openB} of ${secB} looks), and stays shut in the first (${openA} of ${secA})`, openB > 3 && openA === 0 && secA > 100);
  w.btn(B.PLAY);
  // and the first section's steps do not open it in the second
  const x = await device(); x.render(200);
  x.key(BLACK[0], 20); x.setSteps([0]); x.btn(B.FX); x.key(BLACK[7], 30); x.key(WHITE[4], 30); x.btn(B.HOME);       // the DELAY on step 5 of the first section
  x.btn(B.OCTUP); x.render(60); x.btn(B.OCTUP); x.render(60);      // doubled: the second one is a copy, the effects' steps too
  check("a doubled track copies the effects' steps too", x.ex.web_fxstep(16, FX.DELAY, 4) === 1);
  x.btn(B.FX); x.key(BLACK[7], 30); x.key(WHITE[4], 30); x.btn(B.HOME);       // ... off in the second
  check("  and they are the section's own afterwards (off in the second, on in the first)", x.ex.web_fxstep(16, FX.DELAY, 4) === 0 && x.ex.web_fxstep(0, FX.DELAY, 4) === 1);
  x.btn(B.PLAY);
  let a1 = 0, b1 = 0;
  for (let t = 0; t < 6000; t += 10) { x.render(10); const env = x.ex.web_fxenv(FX.DELAY); if (x.ex.web_cs() === 1 && env > 0) b1++; if (x.ex.web_cs() === 0 && env > 30000) a1++; }
  check(`  the DELAY opens in the first section (${a1}) and never in the second (${b1})`, a1 > 3 && b1 === 0);
  x.btn(B.PLAY);
}
// ---- deleting a pattern takes its second section with it, and the ones after it keep theirs
{
  const w = await device(); w.render(200);
  const dbl = () => { w.btn(B.OCTUP); w.render(60); w.btn(B.OCTUP); w.render(60); };
  w.key(BLACK[0], 20); w.setSteps([0]); np(w, 1); w.key(BLACK[0], 20); w.setSteps([4]); dbl(); w.setSteps([8]);   // 2 is doubled, its second section has a step 9 more
  np(w, 1); w.key(BLACK[0], 20); w.setSteps([12]);                                                             // 3 has step 13
  check("pattern 2 is doubled (its second section: steps 5 and 9), 3 is not", w.ex.web_two() === 2 && w.ex.web_step_on(17, 0, 8) === 1 && w.ex.web_step_on(17, 0, 4) === 1 && w.ex.web_step_on(2, 0, 12) === 1);
  np(w, -1); w.btn(B.SAVE); w.key(WHITE[1], 50); w.hold(B.EDIT, 800); w.render(100);
  check(`pattern 2 deleted: 3 is 2 now, not doubled (${w.ex.web_two()}), its steps (${w.ex.web_step_on(1, 0, 12)}), nothing is left of the second section (${w.ex.web_step_on(17, 0, 8)})`, w.ex.web_two() === 0 && w.ex.web_step_on(1, 0, 12) === 1 && w.ex.web_step_on(1, 0, 4) === 0 && w.ex.web_step_on(17, 0, 8) === 0 && w.ex.web_step_on(17, 0, 4) === 0);
}
// ---- the mutation is each pattern's own
{
  const w = await device(); w.render(200);
  w.btn(B.ARP); w.btn(B.ARP);                                      // ARP 2: MUTATE
  w.turn(EN.K1, 6); w.render(60);
  const a1 = w.ex.web_arp_lane(0, 0);
  w.btn(B.HOME); np(w, 1); w.btn(B.ARP); w.btn(B.ARP);
  const a2 = w.ex.web_arp_lane(0, 0);
  w.btn(B.HOME); np(w, -1);
  const a3 = w.ex.web_arp_lane(0, 0);
  check(`pattern 1 mutates (AMOUNT ${a1}), pattern 2 does not (${a2}), and 1 still does (${a3})`, a1 === 30 && a2 === 0 && a3 === 30);
}
{
  const w = await device();
  const m = () => [0, 1, 2, 3].map((i) => w.ex.web_mst(i)).join(",");
  const setMaster = () => { if (w.ex.web_page() !== 7) w.btn(B.SCL); w.turn(EN.K1, 10); w.turn(EN.K2, -10); w.turn(EN.K3, 10); w.turn(EN.K4, 10); };
  w.key(BLACK[0], 20); w.key(WHITE[0], 20);
  setMaster();
  const before = m();
  w.hold(B.EDIT, 800);
  check(`EDIT held on the master page: HPF, LPF, OTT and CLIP back to their state 0 (${before} -> ${m()})`, before !== "0,127,0,0" && m() === "0,127,0,0");
  check("  and the steps are not touched", w.ex.web_step_on(0, 0, 0) === 1);
  setMaster();
  w.btn(B.HOME);
  const again = m();
  w.hold(B.HOME, 800);
  check(`HOME held (a full clear): the master page too (${again} -> ${m()}), the pattern is empty`, again !== "0,127,0,0" && m() === "0,127,0,0" && w.ex.web_step_on(0, 0, 0) === 0);
  w.render(3000);
  const r = await device(w.flash()); r.render(200);
  check("  and the reset is saved", [0, 1, 2, 3].map((i) => r.ex.web_mst(i)).join(",") === "0,127,0,0");
}

// ---- ENV: the ducking (page 1) and the attack, decay and gate of every lane (page 2)
{
  const PG = { ENV1: 10, ENV2: 11 };
  const make = async () => {                                   // the cymbal on every step (a steady sound), the kick on steps 5 and 13 with its LEVEL at 0 (it only ducks)
    const w = await device();
    w.key(BLACK[10], 20); for (let st = 0; st < 16; st++) w.key(WHITE[st], 10);
    w.key(BLACK[0], 20); w.key(WHITE[4], 10); w.key(WHITE[12], 10); w.turn(EN.K4, -70);
    return w;
  };
  const openEnv = (w, k) => { while (w.ex.web_page() !== (k ? PG.ENV2 : PG.ENV1)) w.btn(B.ENV); };
  const closeEnv = (w) => { while (w.ex.web_page() === PG.ENV1 || w.ex.web_page() === PG.ENV2) w.btn(B.ENV); };
  const setEnv = (w, k, v) => {                                // the 4 values of a page (null: leave), by clicks (AMOUNT 5, KICK ATK 4, others 2 a click, SOURCE and TARGET 1)
    openEnv(w, k);
    const step = k ? [4, 2, 2, 2] : [5, 2, 1, 1];
    v.forEach((t, i) => { if (t !== null) w.turn(EN.K1 + i, Math.round((t - w.ex.web_env(k * 4 + i)) / step[i])); });
    closeEnv(w);
  };
  const wins = (w, n, ms = 25) => { const o = []; for (let j = 0; j < n; j++) { const c = w.cap(ms); let a = 0; for (let i = 0; i < c.l.length; i++) a += c.l[i] * c.l[i] + c.r[i] * c.r[i]; o.push(Math.sqrt(a / (2 * c.l.length))); } return o; };
  const play = (w) => { if (w.ex.web_playing()) { w.ex.web_buttons(1 << B.PLAY); w.render(20); w.ex.web_buttons(0); w.render(40); } w.ex.web_buttons(1 << B.PLAY); w.render(20); w.ex.web_buttons(0); };
  const mean = (a, i0, i1) => a.slice(i0, i1).reduce((s, v) => s + v, 0) / (i1 - i0);
  // the pages
  {
    const w = await device();
    check("ENV: a tap opens ENV 1, again ENV 2, again it closes, its LED lit while it is open", (() => { w.btn(B.ENV); const a = w.ex.web_page(), l = w.btnLed(B.ENV); w.btn(B.ENV); const b = w.ex.web_page(); w.btn(B.ENV); return a === PG.ENV1 && b === PG.ENV2 && w.ex.web_page() === 0 && l === 1 && w.btnLed(B.ENV) === 0; })());
    check(`  the first values: AMOUNT 0, RELEASE 50, SOURCE KICK, TARGET BOTH, KICK ATTACK 0, ATTACK 64, DECAY 64, GATE off (${[0, 1, 2, 3, 4, 5, 6, 7].map((i) => w.ex.web_env(i)).join(" ")})`, [0, 50, 0, 2, 0, 64, 64, 127].every((v, i) => w.ex.web_env(i) === v));
    openEnv(w, 0); w.turn(EN.K1, 40); w.turn(EN.K2, 80); w.turn(EN.K3, 30); w.turn(EN.K4, 9);
    check(`  page 1 stops at the ends (${[0, 1, 2, 3].map((i) => w.ex.web_env(i)).join(" ")}: 100 127 10 2)`, [100, 127, 10, 2].every((v, i) => w.ex.web_env(i) === v));
    w.turn(EN.K1, -40); w.turn(EN.K2, -80); w.turn(EN.K3, -30); w.turn(EN.K4, -9);
    check(`  and at the other (${[0, 1, 2, 3].map((i) => w.ex.web_env(i)).join(" ")}: 0 0 0 0)`, [0, 0, 0, 0].every((v, i) => w.ex.web_env(i) === v));
    openEnv(w, 1); w.turn(EN.K1, 40); w.turn(EN.K2, 40); w.turn(EN.K3, -70); w.turn(EN.K4, -70);
    check(`  page 2 stops at the ends (${[4, 5, 6, 7].map((i) => w.ex.web_env(i)).join(" ")}: 127 127 0 0)`, [127, 127, 0, 0].every((v, i) => w.ex.web_env(4 + i) === v));
  }
  // the ducking of the dry sound: the cymbal falls on the kick's hits and comes back. The kick (silent) is on steps 5 and 13: 0.5 s and 1.5 s after PLAY.
  // The windows are 10 ms from about 35 ms after it: the first kick is in windows 47-48, the second in 147-148; the deepest window of each is "the dip"
  // (the same measure on the run with no ducking says how deep the cymbal's own ups and downs go)
  const duckRun = async (amt, rel, tgt, setup) => {
    const w = await make();
    if (setup) setup(w);
    setEnv(w, 0, [amt, rel, 0, tgt]);
    w.render(3000); play(w); w.render(15);
    return wins(w, 170, 10);
  };
  const dip = (a, j0 = 45, j1 = 60) => Math.min(...a.slice(j0, j1));
  const at = (a, j0 = 45, j1 = 60) => a.indexOf(dip(a, j0, j1));
  {
    const off = await duckRun(0, 50, 1), on = await duckRun(100, 50, 1);
    const ref = mean(off, 10, 40), d0 = dip(off);
    check(`DUCK, DRY, 100 %: the cymbal falls on the kick (the dip ${dip(on).toFixed(4)} against ${d0.toFixed(4)} with no ducking), only there (${mean(on, 10, 40).toFixed(4)} before)`, dip(on) < d0 * 0.3 && mean(on, 10, 40) > ref * 0.9);
    check(`  and it is back before the next hit (${mean(on, 128, 138).toFixed(4)}, ${(mean(on, 128, 138) / ref).toFixed(2)})`, mean(on, 128, 138) > ref * 0.9);
    const fast = await duckRun(100, 10, 1), slow = await duckRun(100, 110, 1);
    const sm = (a) => a.map((v, i) => mean(a, Math.max(0, i - 1), Math.min(a.length, i + 2)));
    const t50 = (a) => { const s = sm(a), k = at(a); for (let i = k; i < 130; i++) if (s[i] > ref * 0.5) return (i - k) * 10; return 999; };
    check(`  RELEASE sets the way back (half the level again after ${t50(fast)} ms at 40, ${t50(on)} at 115, ${t50(slow)} at 540)`, t50(fast) < t50(on) && t50(on) < t50(slow) && t50(slow) >= 3 * Math.max(t50(fast), 20) && t50(slow) < 999);
    const half = await duckRun(50, 50, 1);
    check(`  AMOUNT 50: half of the way (the dip ${dip(half).toFixed(4)}, between ${dip(on).toFixed(4)} and ${d0.toFixed(4)})`, dip(half) > dip(on) * 1.5 && dip(half) < d0 * 0.9);
    const fx = await duckRun(100, 50, 0);
    check(`  TARGET EFFECTS leaves the dry sound alone (the dip ${dip(fx).toFixed(4)}, as with no ducking ${d0.toFixed(4)})`, dip(fx) > d0 * 0.8);
    const soft = await duckRun(100, 50, 1, (w) => { w.key(BLACK[0], 20); w.key(WHITE[12], 10); w.key(WHITE[12], 10); w.turn(EN.SELECT, -15); });
    check(`  a soft kick (velocity 40) ducks less than a loud one: the second dip ${dip(soft, 145, 160).toFixed(4)} against ${dip(on, 145, 160).toFixed(4)}`, dip(soft, 145, 160) > dip(on, 145, 160) * 2.5);
    // it follows the hits that play: CHANCE 0 on the first kick: no duck there; 4 hits in the step: ducked for the whole step
    const never = await duckRun(100, 50, 1, (w) => { w.btn(B.EDIT); w.key(WHITE[4], 10); w.turn(EN.K3, -25); w.btn(B.EDIT); });
    check(`  it follows the hits that play: CHANCE 0 on the first kick, no dip there (${dip(never).toFixed(4)} against ${d0.toFixed(4)}), the second kick still ducks (${dip(never, 145, 160).toFixed(4)})`, dip(never) > d0 * 0.8 && dip(never, 145, 160) < dip(off, 145, 160) * 0.3);
    const four = await duckRun(100, 10, 1, (w) => { w.btn(B.EDIT); w.key(WHITE[4], 10); w.turn(EN.K2, 3); w.btn(B.EDIT); });
    const one = await duckRun(100, 10, 1);
    check(`  4 hits in a step keep it down for the step (the 125 ms after: ${mean(four, 48, 60).toFixed(4)} against ${mean(one, 48, 60).toFixed(4)} with one hit)`, mean(four, 48, 60) < mean(one, 48, 60) * 0.75);
  }
  // the source is not ducked by its own hits
  {
    const peakOf = async (amt) => { const d = await device(); d.key(BLACK[0], 20); d.key(WHITE[0], 10); setEnv(d, 0, [amt, 50, 0, 1]); d.render(3000); play(d); d.render(15); const c = d.cap(120); let m = 0; for (const v of c.l) m = Math.max(m, Math.abs(v)); return m; };
    const p0 = await peakOf(0), p100 = await peakOf(100);
    check(`the source is not ducked by its own hits (the kick ${p100.toFixed(3)} against ${p0.toFixed(3)} with no ducking)`, Math.abs(p100 - p0) < p0 * 0.08);
  }
  // the effects: the reverb's tail is ducked by the (silent) kick at 0.5 s, not the dry sound
  {
    const run = async (tgt) => {
      const w = await device();
      w.key(BLACK[2], 20); w.key(WHITE[0], 10);                 // a clap, step 1
      w.key(BLACK[0], 20); w.key(WHITE[4], 10); w.turn(EN.K4, -70);       // the (silent) kick on step 5: 0.5 s
      w.fxSet(FX.REVERB, 6, "on"); w.fxSet(FX.REVERB, 7, 127); w.btn(B.HOME);
      setEnv(w, 0, [100, 50, 0, tgt]);
      w.render(4000); play(w); w.render(15);
      return wins(w, 100, 10);                                    // 1 s
    };
    const none = await run(1), fx = await run(0), both = await run(2);
    const tail = mean(none, 30, 45);
    check(`TARGET EFFECTS: the reverb's tail falls on the kick (${dip(fx).toFixed(4)} against ${tail.toFixed(4)} before it; the dry target leaves it, ${dip(none).toFixed(4)})`, tail > 0.001 && dip(fx) < dip(none) * 0.4);
    check(`  BOTH ducks it too (${dip(both).toFixed(4)})`, dip(both) < dip(none) * 0.4);
  }
  // page 2: the attack, the decay, the gate (a hit's samples from its first one)
  {
    const hit = async (lane, n, set) => {
      const w = await device();
      w.key(BLACK[lane], 20);
      if (set) set(w);
      w.render(300);
      w.ex.web_keys(1 << WHITE[15]); const c = w.cap(900 + n / 44.1); w.ex.web_keys(0);
      let on = 0; while (on < c.l.length && Math.abs(c.l[on]) < 1e-4) on++;
      return c.l.slice(on, on + n);
    };
    const peak = (a, i0, i1) => { let m = 0; for (let i = i0; i < i1; i++) m = Math.max(m, Math.abs(a[i])); return m; };
    const rms = (a, i0, i1) => { let s = 0; for (let i = i0; i < i1; i++) s += a[i] * a[i]; return Math.sqrt(s / (i1 - i0)); };
    const k0 = await hit(0, 4000), k1 = await hit(0, 4000, (w) => setEnv(w, 1, [127, null, null, null]));
    check(`KICK ATTACK up: the kick's first 6 ms are louder (${peak(k1, 0, 265).toFixed(3)} against ${peak(k0, 0, 265).toFixed(3)}), the rest is as it was (${peak(k1, 1500, 4000).toFixed(3)} / ${peak(k0, 1500, 4000).toFixed(3)})`, peak(k1, 0, 265) > peak(k0, 0, 265) * 1.4 && Math.abs(peak(k1, 1500, 4000) - peak(k0, 1500, 4000)) < peak(k0, 1500, 4000) * 0.02);
    const s0 = await hit(1, 2000), s1 = await hit(1, 2000, (w) => setEnv(w, 1, [null, 0, null, null])), s2 = await hit(1, 2000, (w) => setEnv(w, 1, [null, 127, null, null]));
    check(`ATTACK soft: the snare fades in (the first 5 ms ${rms(s1, 0, 220).toFixed(4)} against ${rms(s0, 0, 220).toFixed(4)}), hard: it bites (${rms(s2, 0, 220).toFixed(4)})`, rms(s1, 0, 220) < rms(s0, 0, 220) * 0.4 && rms(s2, 0, 220) > rms(s0, 0, 220) * 1.3);
    const k2 = await hit(0, 4000, (w) => setEnv(w, 1, [null, 127, null, null]));
    check(`  the kick is a lane like the others (its first 5 ms louder with it too: ${rms(k2, 0, 220).toFixed(3)} against ${rms(k0, 0, 220).toFixed(3)})`, rms(k2, 0, 220) > rms(k0, 0, 220) * 1.2);
    const o0 = await hit(4, 40000), o1 = await hit(4, 40000, (w) => setEnv(w, 1, [null, null, 0, null])), o2 = await hit(4, 40000, (w) => setEnv(w, 1, [null, null, 127, null]));
    check(`DECAY: the open hat rings shorter (${rms(o1, 14000, 30000).toFixed(4)} against ${rms(o0, 14000, 30000).toFixed(4)}) or longer (${rms(o2, 14000, 30000).toFixed(4)})`, rms(o1, 14000, 30000) < rms(o0, 14000, 30000) * 0.5 && rms(o2, 14000, 30000) > rms(o0, 14000, 30000) * 1.5);
    const g1 = await hit(4, 40000, (w) => setEnv(w, 1, [null, null, null, 16]));
    check(`GATE: the open hat is cut after 40 ms (${rms(g1, 3000, 30000).toFixed(5)} after, ${rms(g1, 0, 1500).toFixed(4)} before, against ${rms(o0, 0, 1500).toFixed(4)})`, rms(g1, 3000, 30000) < rms(o0, 3000, 30000) * 0.02 && Math.abs(rms(g1, 0, 1500) - rms(o0, 0, 1500)) < rms(o0, 0, 1500) * 0.1);
  }
  // saved
  {
    const w = await device();
    setEnv(w, 0, [65, 70, 4, 2]); setEnv(w, 1, [48, 20, 100, 61]);
    w.render(3000);
    const r = await device(w.flash());
    const got = [0, 1, 2, 3, 4, 5, 6, 7].map((i) => r.ex.web_env(i)).join(" ");
    check(`ENV is saved (${got})`, got === "65 70 4 2 48 20 100 61");
  }
}

// ---- ARP: a Euclidean generator for the selected lane (page 1) and a mutation of the pattern that plays (page 2)
{
  const PG = { ARP1: 12, ARP2: 13 };
  const openArp = (w, k) => { while (w.ex.web_page() !== (k ? PG.ARP2 : PG.ARP1)) w.btn(B.ARP); };
  const bits = (m) => [...Array(16).keys()].filter((s) => (m >> s) & 1);
  const pop = (m) => bits(m).length;
  {
    const w = await device();
    check("ARP: a tap opens ARP 1, again ARP 2, again it closes, its LED lit while it is open", (() => { w.btn(B.ARP); const a = w.ex.web_page(), l = w.btnLed(B.ARP); w.btn(B.ARP); const b = w.ex.web_page(); w.btn(B.ARP); return a === PG.ARP1 && b === PG.ARP2 && w.ex.web_page() === 0 && l === 1 && w.btnLed(B.ARP) === 0; })());
    check(`  the first values: HITS 4, ROTATE 0, LENGTH 16, ACCENT 0, AMOUNT 0, EVERY 1, SCOPE lane, DRIFT 0 (${[0, 1, 2, 3, 4, 5, 6, 7].map((i) => w.ex.web_arp(i)).join(" ")})`, [4, 0, 16, 0, 0, 0, 0, 0].every((v, i) => w.ex.web_arp(i) === v));
    check("  opening the page writes nothing", pop(w.ex.web_steps_of(0)) === 0);
    openArp(w, 0); w.turn(EN.K1, 30); w.turn(EN.K2, 17); w.turn(EN.K3, -30); w.turn(EN.K4, 30);
    check(`  page 1: HITS and ACCENT stop (${w.ex.web_arp(0)} 16, ${w.ex.web_arp(3)} 100), LENGTH stops (${w.ex.web_arp(2)}: 2), ROTATE goes round (${w.ex.web_arp(1)}: 1)`, w.ex.web_arp(0) === 16 && w.ex.web_arp(3) === 100 && w.ex.web_arp(2) === 2 && w.ex.web_arp(1) === 1);
    openArp(w, 1); w.turn(EN.K1, 40); w.turn(EN.K2, 9); w.turn(EN.K3, 5); w.turn(EN.K4, 40);
    check(`  page 2 stops at the ends (${[4, 5, 6, 7].map((i) => w.ex.web_arp(i)).join(" ")}: 100 3 1 100)`, [100, 3, 1, 100].every((v, i) => w.ex.web_arp(4 + i) === v));
  }
  // the generator
  {
    const w = await device();
    w.key(BLACK[3], 20);                                       // HAT CL
    openArp(w, 0);
    w.turn(EN.K1, 1); w.turn(EN.K1, -1);                       // (a write)
    const set = (hits, rot, len) => { w.turn(EN.K1, hits - w.ex.web_arp(0)); w.turn(EN.K2, (rot - w.ex.web_arp(1) + 16) % 16); w.turn(EN.K3, len - w.ex.web_arp(2)); return w.ex.web_steps_of(3); };
    let m = set(4, 0, 16);
    check(`EUCLID 4 hits in 16: the beats (${bits(m).join(",")})`, m === 0x1111);
    m = set(4, 1, 16);
    check(`  ROTATE 1: one step later (${bits(m).join(",")})`, m === 0x2222);
    m = set(3, 0, 8); const t = bits(set(3, 0, 8));
    check(`  3 hits in 8, repeated to fill the 16: the tresillo (${t.join(",")})`, t.join() === "0,3,6,8,11,14");
    const even = (k, n) => { m = set(k, 0, n); const on = bits(m & ((1 << n) - 1)); const gaps = on.map((s, i) => ((on[(i + 1) % on.length] - s) + n) % n || n); return on.length === k && Math.max(...gaps) - Math.min(...gaps) <= 1; };
    check("  and as even as it can be for 5 in 8, 7 in 12, 5 in 16, 9 in 16, 4 in 9, 11 in 16, 1 in 16", [[5, 8], [7, 12], [5, 16], [9, 16], [4, 9], [11, 16], [1, 16]].every(([k, n]) => even(k, n)));
    m = set(16, 0, 16);
    check(`  16 hits: every step; 8: every second (${bits(set(8, 0, 16)).join(",")}); 0 hits: none; LENGTH 8 with 4 hits repeats to 8 over the lane (${pop(set(4, 0, 8))})`, m === 0xFFFF && set(8, 0, 16) === 0x5555 && set(0, 0, 16) === 0 && pop(set(4, 0, 8)) === 8);
    check("  the other lanes are not touched", [0, 1, 2, 4, 5, 6, 7, 8, 9, 10].every((l) => w.ex.web_steps_of(l) === 0));
  }
  {
    const w = await device();
    w.key(BLACK[0], 20); w.key(WHITE[0], 10); w.turn(EN.SELECT, -10);       // a kick on step 1 at velocity 60, and a step with its own settings
    w.btn(B.EDIT); w.turn(EN.K1, 5); w.btn(B.EDIT);
    const v0 = w.ex.web_vel(0, 0), nu = w.ex.web_nudge(0, 0);
    openArp(w, 0); w.turn(EN.K1, 0);
    w.turn(EN.K4, 1); w.turn(EN.K4, -1);                       // (a write: 4 in 16, the same)
    check(`a step that is on and stays on keeps what it has (velocity ${w.ex.web_vel(0, 0)}, nudge ${w.ex.web_nudge(0, 0)}: ${v0}, ${nu})`, w.ex.web_steps_of(0) === 0x1111 && w.ex.web_vel(0, 0) === v0 && w.ex.web_nudge(0, 0) === nu && v0 !== 100);
    w.turn(EN.K4, 20);                                          // ACCENT 100: what comes on off the beat is softer
    w.turn(EN.K1, 2);
    const offbeat = bits(w.ex.web_steps_of(0)).filter((s) => s % 4);
    check(`  ACCENT 100: the hits it adds off the beat are at 50, the beats' at 100 (${offbeat.map((s) => w.ex.web_vel(0, s)).join(",")} / ${[0, 4, 8, 12].filter((s) => (w.ex.web_steps_of(0) >> s) & 1).map((s) => w.ex.web_vel(0, s)).join(",")})`, offbeat.length > 0 && offbeat.every((s) => w.ex.web_vel(0, s) === 50) && w.ex.web_vel(0, 4) === 100);
  }
  // the mutation
  {
    const run = async (amt, every, scope, drift, passes) => {
      const w = await device();
      w.key(BLACK[0], 20); [0, 4, 8, 12].forEach((s) => w.key(WHITE[s], 10));
      w.key(BLACK[1], 20); [4, 12].forEach((s) => w.key(WHITE[s], 10));
      w.key(BLACK[0], 20);
      openArp(w, 1);
      if (amt) w.turn(EN.K1, amt / 5);
      w.turn(EN.K2, every); if (scope) w.turn(EN.K3, 1); if (drift) w.turn(EN.K4, drift / 5);
      w.btn(B.HOME);
      w.btn(B.PLAY); w.render(passes * 2000 + 300);
      return w;
    };
    const a = await run(100, 0, 0, 0, 3);
    check(`MUTATE 100 %: after three passes the kick's hits have moved (${bits(a.ex.web_steps_of(0)).join(",")}), there are still four, the snare is untouched (${bits(a.ex.web_steps_of(1)).join(",")})`, pop(a.ex.web_steps_of(0)) === 4 && a.ex.web_steps_of(0) !== 0x1111 && a.ex.web_steps_of(1) === 0x1010);
    const b = await run(0, 0, 0, 0, 3);
    check("  AMOUNT 0: nothing moves", b.ex.web_steps_of(0) === 0x1111 && b.ex.web_vel(0, 0) === 100);
    const c = await run(100, 2, 0, 0, 2);                       // EVERY 4 passes: not yet after 2
    check("  EVERY 4 passes: not yet after two", c.ex.web_steps_of(0) === 0x1111);
    const d = await run(100, 2, 0, 0, 6);
    check(`  ... and it has after six (${bits(d.ex.web_steps_of(0)).join(",")})`, d.ex.web_steps_of(0) !== 0x1111 && pop(d.ex.web_steps_of(0)) === 4);
    const e = await run(100, 0, 1, 0, 3);
    check(`  SCOPE ALL lanes: the snare moves too (${bits(e.ex.web_steps_of(1)).join(",")}), two hits still`, e.ex.web_steps_of(1) !== 0x1010 && pop(e.ex.web_steps_of(1)) === 2 && pop(e.ex.web_steps_of(0)) === 4);
    const f = await run(0, 0, 0, 100, 3);
    const vels = bits(f.ex.web_steps_of(0)).map((s) => f.ex.web_vel(0, s));
    check(`  DRIFT: the steps stay, the velocities wander (${vels.join(",")})`, f.ex.web_steps_of(0) === 0x1111 && vels.some((v) => v !== 100) && vels.every((v) => v >= 1 && v <= 127));
    const ex = f.ex, on = bits(ex.web_steps_of(0));
    const set = (l, st) => [ex.web_nudge(l, st), ex.web_ratchet(l, st), ex.web_chance(l, st), ex.web_semi(l, st), ...[0, 1, 2, 3].map((k) => ex.web_lock(k, l, st))];
    const fresh = [0, 1, 100, 0, 0, 0, 0, 0];
    const kinds = [0, 1, 2, 3].map((k) => on.some((st) => set(0, st)[k] !== fresh[k]));
    const ranged = on.every((st) => { const [nu, ra, ch, se, d, t, e, pa] = set(0, st); return Math.abs(nu) <= 24 && ra >= 1 && ra <= 8 && ch >= 0 && ch <= 100 && Math.abs(se) <= 12 && [d, t, e, pa].every((v) => v >= -64 && v <= 56); });
    check(`  DRIFT moves the nudge, subdivision, chance and pitch too (${kinds.join(",")}), and the sound's DECAY, TONE, EXTRA, PAN (${[0, 1, 2, 3].map((k) => on.some((st) => ex.web_lock(k, 0, st) !== 0)).join(",")}), each within its range`,
          kinds.every(Boolean) && ranged && [0, 1, 2, 3].some((k) => on.some((st) => ex.web_lock(k, 0, st) !== 0)));
    check("  ... on its lane only (the snare's steps keep theirs)", bits(ex.web_steps_of(1)).every((st) => set(1, st).every((v, k) => v === fresh[k])));
    a.btn(B.PLAY); a.render(3000);
    const r = await device(a.flash());
    check("  and the mutated pattern is saved", r.ex.web_steps_of(0) === a.ex.web_steps_of(0) && r.ex.web_steps_of(0) !== 0x1111);
  }
  // each lane has its own mutation: the kick's goes on while the hi-hat is the lane shown
  {
    const w = await device();
    w.key(BLACK[0], 20); [0, 4, 8, 12].forEach((s) => w.key(WHITE[s], 10));        // the kick, four on the floor
    w.key(BLACK[3], 20); [2, 6, 10, 14].forEach((s) => w.key(WHITE[s], 10));       // the closed hat, off the beat
    w.key(BLACK[0], 20); openArp(w, 1); w.turn(EN.K1, 20);                          // the kick: AMOUNT 100 %
    w.key(BLACK[3], 20);
    check(`MUTATE per lane: the hi-hat selected shows its own (AMOUNT ${w.ex.web_arp(4)}: 0), the kick keeps its (${w.ex.web_arp_lane(0, 0)}: 100)`, w.ex.web_arp(4) === 0 && w.ex.web_arp_lane(0, 0) === 100 && w.ex.web_arp_lane(3, 0) === 0);
    w.btn(B.HOME); w.btn(B.PLAY); w.render(3 * 2000 + 300);
    check(`  with the hi-hat shown the kick still mutates (${bits(w.ex.web_steps_of(0)).join(",")}) and the hi-hat stays (${bits(w.ex.web_steps_of(3)).join(",")})`,
      w.ex.web_steps_of(0) !== 0x1111 && pop(w.ex.web_steps_of(0)) === 4 && w.ex.web_steps_of(3) === 0x4444);
    openArp(w, 1); w.turn(EN.K2, 1);
    check(`  turning the hi-hat's EVERY leaves the kick's (${w.ex.web_arp_lane(3, 1)}, kick ${w.ex.web_arp_lane(0, 1)})`, w.ex.web_arp_lane(3, 1) === 1 && w.ex.web_arp_lane(0, 1) === 0);
    w.turn(EN.K3, 1);
    check(`  SCOPE ALL: every lane takes the same (${[0, 1, 3, 10].map((l) => w.ex.web_arp_lane(l, 1)).join(",")}: 1)`, [0, 1, 3, 10].every((l) => w.ex.web_arp_lane(l, 1) === 1 && w.ex.web_arp_lane(l, 0) === 0));
  }
}

// ---- REC: the black keys sound, and while playing they are written where they fall (an overdub)
{
  const bits = (m) => [...Array(16).keys()].filter((s) => (m >> s) & 1);
  const waitStep = (w, n) => { for (let i = 0; i < 4000 && w.ex.web_step() !== n; i++) w.render(2); };
  const peakOf = (w, ms) => { const c = w.cap(ms); let m = 0; for (const v of c.l) m = Math.max(m, Math.abs(v)); return m; };
  {
    const w = await device();
    w.btn(B.REC);
    check("REC: a tap arms it (its LED lit), another puts it away", w.ex.web_rec() === 1 && w.btnLed(B.REC) === 1 && (w.btn(B.REC), w.ex.web_rec() === 0 && w.btnLed(B.REC) === 0));
    w.ex.web_keys(1 << BLACK[0]); const silent = peakOf(w, 150); w.ex.web_keys(0); w.render(100);
    w.btn(B.REC);
    w.ex.web_keys(1 << BLACK[0]); const loud = peakOf(w, 150); w.ex.web_keys(0); w.render(100);
    check(`  armed, a black key sounds its drum (peak ${loud.toFixed(3)} against ${silent.toFixed(3)} when it is not)`, silent < 0.001 && loud > 0.05);
    check("  stopped, nothing is written", w.ex.web_steps_of(0) === 0);
    w.ex.web_keys(1 << BLACK[3]); w.render(100); w.ex.web_keys(0); w.render(50);
    check("  the key still picks its lane", w.ex.web_sel() === 3);
  }
  {
    const w = await device();
    w.btn(B.REC); w.btn(B.PLAY);
    waitStep(w, 3); w.render(12);
    w.ex.web_keys(1 << BLACK[0]); w.render(30); w.ex.web_keys(0); w.render(20);        // just after step 4 began (index 3): it belongs to it
    waitStep(w, 6); w.render(98);
    w.ex.web_keys(1 << BLACK[0]); w.render(30); w.ex.web_keys(0); w.render(20);        // most of the way through step 7 (index 6): the next, 8 (7)
    const m = w.ex.web_steps_of(0);
    check(`REC while playing: a hit just after a step began is on it, one late is on the next (${bits(m).join(",")}: 3,7)`, bits(m).join() === "3,7");
    check(`  a hit starts as a step does (velocity ${w.ex.web_vel(0, 3)}), and SELECT sets it (${(w.turn(EN.SELECT, -5), w.ex.web_vel(0, 7))})`, w.ex.web_vel(0, 3) === 100 && w.ex.web_vel(0, 7) < 100);
    w.btn(B.REC);
    w.ex.web_keys(1 << BLACK[0]); w.render(30); w.ex.web_keys(0); w.render(100);
    waitStep(w, 9); w.render(12); w.ex.web_keys(1 << BLACK[0]); w.render(30); w.ex.web_keys(0); w.render(20);
    check(`  not armed, it writes nothing (${bits(w.ex.web_steps_of(0)).join(",")})`, bits(w.ex.web_steps_of(0)).join() === "3,7");
    w.btn(B.PLAY); w.render(3000);
    const r = await device(w.flash());
    check("  and what was played is saved with the pattern", bits(r.ex.web_steps_of(0)).join() === "3,7");
  }
  {
    // an overdub: a step already on keeps what it has; the others are added beside it
    const w = await device();
    w.key(BLACK[0], 20); w.key(WHITE[3], 10); w.turn(EN.SELECT, -10); w.key(WHITE[10], 10);
    const v3 = w.ex.web_vel(0, 3);
    w.key(BLACK[1], 20);
    w.btn(B.REC); w.btn(B.PLAY);
    waitStep(w, 3); w.render(12);
    w.ex.web_keys(1 << BLACK[0]); w.render(30); w.ex.web_keys(0); w.render(20);        // on the step that is on
    waitStep(w, 5); w.render(12);
    w.ex.web_keys(1 << BLACK[1]); w.render(30); w.ex.web_keys(0); w.render(20);        // the snare, on 6
    check(`REC overdubs: the step that was on stays as it was (velocity ${w.ex.web_vel(0, 3)}: ${v3}), the others too (${bits(w.ex.web_steps_of(0)).join(",")}), the snare is added (${bits(w.ex.web_steps_of(1)).join(",")})`,
      w.ex.web_vel(0, 3) === v3 && bits(w.ex.web_steps_of(0)).join() === "3,10" && bits(w.ex.web_steps_of(1)).join() === "5");
    // on an FX page the black keys are the effects: nothing is played or written
    w.btn(B.FX); w.ex.web_keys(1 << BLACK[0]); w.render(40); w.ex.web_keys(0); w.render(100);
    check("  on the FX pages the black keys pick effects: no drum is written", bits(w.ex.web_steps_of(0)).join() === "3,10");
  }
}

// ---- HOME held: the pattern is cleared and every parameter of the sound goes back to its state 0, saved; the tempo, the volume and the subdivision stay
{
  const sound = (d) => {                                       // every parameter of the sound, as one string
    const o = [];
    for (let l = 0; l < 11; l++) for (let f = 0; f < 6; f++) o.push(d.ex.web_lane(l, f));
    for (let e = 0; e < 11; e++) for (let i = 0; i < 8; i++) o.push(d.ex.web_fxparam(e, i));
    for (let i = 0; i < 4; i++) o.push(d.ex.web_mst(i));
    for (let k = 0; k < 2; k++) for (let i = 0; i < 4; i++) o.push(d.ex.web_lfo(k, i));
    for (let i = 0; i < 8; i++) o.push(d.ex.web_env(i), d.ex.web_arp(i));
    o.push(d.ex.web_fxmaster(), d.ex.web_swing(), d.ex.web_kit());
    return o.join(",");
  };
  const fresh = await device(); fresh.render(300);
  const w = await device();
  w.key(BLACK[0], 20); w.key(WHITE[0], 10); w.key(WHITE[8], 10);
  w.turn(EN.K1, 5); w.turn(EN.K2, 10); w.turn(EN.K3, -8); w.turn(EN.K4, -10);        // the kick's TUNE, DECAY, TONE, LEVEL
  w.key(BLACK[4], 20); w.turn(EN.K2, 7);
  w.btn(B.GLO); w.turn(EN.K1, 20); w.turn(EN.K2, -6); w.btn(B.GLO); w.turn(EN.K1, 4); w.turn(EN.K2, -6); w.turn(EN.K3, 3); w.btn(B.HOME);   // tempo, volume; swing, FX MIX, kit
  w.fxSet(FX.DELAY, 1, 100); w.fxSet(FX.REVERB, 7, 64); w.fxSet(FX.HALF, 6, "on"); w.btn(B.HOME);
  w.btn(B.SCL); w.turn(EN.K1, 10); w.turn(EN.K3, 10); w.btn(B.SCL);
  w.btn(B.LFO); w.turn(EN.K2, 10); w.btn(B.LFO); w.turn(EN.K2, 12); w.turn(EN.K4, 3); w.btn(B.HOME);
  w.btn(B.ENV); w.turn(EN.K1, 8); w.btn(B.ENV); w.turn(EN.K2, 5); w.btn(B.HOME);
  w.btn(B.ARP); w.turn(EN.K3, -3); w.btn(B.HOME);
  const bpm = w.ex.web_bpm(), vol = w.ex.web_vol(), div = w.ex.web_div();
  check(`before: the sound is not the one a fresh start has, the pattern is not empty, the tempo is ${bpm}`, sound(w) !== sound(fresh) && w.ex.web_step_on(0, 0, 0) === 1 && bpm !== 120);
  w.hold(B.HOME, 800);
  check("HOME held: every parameter of the sound (the kit and the drums, the effects, the master page, the swing, the LFOs, ENV, ARP) is back to a fresh start's", sound(w) === sound(fresh));
  check("  and so are the steps, while the tempo, the volume and the subdivision stay", w.ex.web_step_on(0, 0, 0) === 0 && w.ex.web_bpm() === bpm && w.ex.web_vol() === vol && w.ex.web_div() === div);
  w.render(3500);
  const r = await device(w.flash()); r.render(300);
  check("  it is saved: the next start loads it (the sound as a fresh start's, the tempo as it was)", sound(r) === sound(fresh) && r.ex.web_bpm() === bpm);
  // the full turn of KNOB 4 on the second GLO page is the same reset
  const v = await device();
  v.key(BLACK[0], 20); v.key(WHITE[0], 10); v.turn(EN.K2, 10); v.turn(EN.K4, -10);
  v.btn(B.SCL); v.turn(EN.K1, 10); v.turn(EN.K3, 10); v.btn(B.SCL);
  v.btn(B.LFO); v.turn(EN.K2, 10); v.btn(B.LFO); v.turn(EN.K2, 12); v.btn(B.HOME);
  v.btn(B.GLO); v.btn(B.GLO); v.turn(EN.K1, 4); v.turn(EN.K3, 2);
  check("before: the sound is not a fresh start's", sound(v) !== sound(fresh));
  v.turn(EN.K4, 20);
  check(`the full turn of KNOB 4 on GLO 2 is the same reset: the master page, the LFOs, the effects, the lanes (${[0, 1, 2, 3].map((i) => v.ex.web_mst(i)).join(",")} | ${[0, 1].map((k) => [0, 1, 2, 3].map((i) => v.ex.web_lfo(k, i)).join(",")).join(" | ")})`, sound(v) === sound(fresh) && v.ex.web_step_on(0, 0, 0) === 0 && v.ex.web_erased() === 1);
}

// ---- EDIT held on an LFO, ENV or ARP page: that page goes back to its state 0 (the steps are not touched); HOME held does all of them
{
  const w = await device();
  w.key(BLACK[0], 20); w.key(WHITE[0], 10);
  const lfo = (k) => [0, 1, 2, 3].map((i) => w.ex.web_lfo(k, i)).join(",");
  w.btn(B.LFO); w.turn(EN.K1, -2); w.turn(EN.K2, 10); w.turn(EN.K3, 3); w.turn(EN.K4, 5);
  w.btn(B.LFO); w.turn(EN.K1, 2); w.turn(EN.K2, 12); w.turn(EN.K3, 2); w.turn(EN.K4, 7);
  const l2 = lfo(1), l1 = lfo(0);
  w.hold(B.EDIT, 800);
  check(`EDIT held on LFO 2: it is off, as it starts (${l2} -> ${lfo(1)}), LFO 1 is as it was (${lfo(0)}), the steps stay`, lfo(1) === "3,0,0,0" && lfo(0) === l1 && w.ex.web_step_on(0, 0, 0) === 1 && l2 !== "3,0,0,0");
  w.btn(B.LFO); w.btn(B.LFO);                                       // (back to LFO 1: LFO 2 -> closed -> LFO 1)
  while (w.ex.web_page() !== 8) w.btn(B.LFO);
  w.hold(B.EDIT, 800);
  check(`  and on LFO 1 (${l1} -> ${lfo(0)})`, lfo(0) === "5,0,0,0");
  w.btn(B.HOME);
  w.btn(B.ENV); w.turn(EN.K1, 8); w.turn(EN.K3, 4); w.btn(B.ENV); w.turn(EN.K1, 10); w.turn(EN.K4, -10);
  const env = (a, b) => [a, a + 1, a + 2, a + 3].map((i) => w.ex.web_env(i)).join(",");
  const p1 = env(0);
  w.hold(B.EDIT, 800);
  check(`EDIT held on ENV 2: its four are as they start (${env(4)}: 0,64,64,127), ENV 1 is as it was (${env(0)})`, env(4) === "0,64,64,127" && env(0) === p1 && p1 !== "0,50,0,2");
  while (w.ex.web_page() !== 10) w.btn(B.ENV);
  w.hold(B.EDIT, 800);
  check(`  and ENV 1 (${env(0)}: 0,50,0,2)`, env(0) === "0,50,0,2");
  w.btn(B.HOME);
  w.btn(B.ARP); w.turn(EN.K1, 3); w.turn(EN.K2, 2); w.turn(EN.K3, -4); w.btn(B.ARP); w.turn(EN.K1, 6); w.turn(EN.K3, 1);
  const arp = (a) => [a, a + 1, a + 2, a + 3].map((i) => w.ex.web_arp(i)).join(",");
  const steps = w.ex.web_steps_of(0);
  w.hold(B.EDIT, 800);
  check(`EDIT held on ARP 2: AMOUNT, EVERY, SCOPE, DRIFT are as they start (${arp(4)}: 0,0,0,0), ARP 1 is as it was (${arp(0)}), the lane is not touched`, arp(4) === "0,0,0,0" && arp(0) !== "4,0,16,0" && w.ex.web_steps_of(0) === steps);
  while (w.ex.web_page() !== 12) w.btn(B.ARP);
  w.hold(B.EDIT, 800);
  check(`  and ARP 1 (${arp(0)}: 4,0,16,0), the lane is not touched`, arp(0) === "4,0,16,0" && w.ex.web_steps_of(0) === steps);
  // HOME held does all of them, with the LFOs
  w.btn(B.HOME);
  w.btn(B.LFO); w.turn(EN.K2, 20); w.btn(B.LFO); w.turn(EN.K2, 20); w.btn(B.HOME);
  w.hold(B.HOME, 800);
  check(`HOME held: both LFOs are off (${lfo(0)} | ${lfo(1)})`, lfo(0) === "5,0,0,0" && lfo(1) === "3,0,0,0");
}

// ---- the sound (the kit, the lanes' parameters, the effects' parameters, the master page, the LFOs, ENV) is the project's: no pattern has its own
{
  const w = await device();
  const sound = () => [w.ex.web_kit(), w.ex.web_fxparam(FX.DELAY, 1), w.ex.web_fxparam(FX.REVERB, 7), w.ex.web_fxparam(FX.HALF, 6), w.ex.web_mst(0), w.ex.web_mst(1), w.ex.web_mst(3), w.ex.web_lane(0, 1), w.ex.web_lane(3, 4), w.ex.web_fxmaster(), w.ex.web_lfo(0, 1), w.ex.web_lfo(1, 1), w.ex.web_lfo(1, 2), w.ex.web_env(0), w.ex.web_env(5), w.ex.web_env(7)].join(",");
  const waitStep = (n) => { for (let i = 0; i < 4000 && w.ex.web_step() !== n; i++) w.render(2); };
  const fresh = sound();
  // pattern 1: kit TIGHT, a longer kick, the delay's feedback, the master's HPF and CLIP, the FX MIX, the LFOs, ENV
  w.btn(B.GLO); w.btn(B.GLO); w.turn(EN.K3, 1); w.turn(EN.K2, -4); w.btn(B.HOME);
  w.key(BLACK[0], 20); w.turn(EN.K2, 10); w.key(BLACK[3], 20); w.turn(EN.K4, -6);
  w.fxSet(FX.DELAY, 1, 100); w.fxSet(FX.HALF, 6, "on"); w.btn(B.HOME);
  w.btn(B.SCL); w.turn(EN.K1, 10); w.turn(EN.K4, 8); w.btn(B.HOME);
  w.btn(B.LFO); w.turn(EN.K2, 8); w.btn(B.LFO); w.turn(EN.K2, 6); w.turn(EN.K3, 3); w.btn(B.HOME);
  w.btn(B.ENV); w.turn(EN.K1, 6); w.btn(B.ENV); w.turn(EN.K2, 5); w.turn(EN.K4, -10); w.btn(B.HOME);
  w.key(BLACK[0], 20); w.key(WHITE[0], 10);                    // a kick on step 1
  const A = sound();
  check(`a first start: ${fresh}; pattern 1 now has another sound (${A})`, A !== fresh && w.ex.web_kit() === 1);
  np(w, 1);
  check(`pattern 2 has the same sound: it is the project's (${sound()})`, sound() === A);
  w.btn(B.GLO); w.btn(B.GLO); w.turn(EN.K3, -1); w.btn(B.HOME);
  w.key(BLACK[0], 20); w.turn(EN.K2, -8);
  w.fxSet(FX.DELAY, 1, 20); w.fxSet(FX.REVERB, 7, 100); w.btn(B.HOME);
  w.btn(B.SCL); w.turn(EN.K2, -10); w.btn(B.HOME);
  w.btn(B.LFO); w.turn(EN.K2, 3); w.btn(B.HOME);
  w.btn(B.ENV); w.turn(EN.K1, 12); w.btn(B.HOME);
  const B2 = sound();
  check(`  a change made there is the project's (${B2})`, B2 !== A && B2 !== fresh && w.ex.web_kit() === 0);
  np(w, -1);
  check("back on pattern 1: the same sound, with that change", sound() === B2);
  // copying a pattern copies its steps, not a sound; deleting one moves no sound
  w.btn(B.SAVE); w.ex.web_keys(1 << WHITE[0]); w.render(60); w.ex.web_keys((1 << WHITE[0]) | (1 << WHITE[4])); w.render(60); w.ex.web_keys(0); w.render(100); w.btn(B.SAVE); w.btn(B.SAVE);
  check(`copying pattern 1 onto 5: its steps (${w.ex.web_step_on(4, 0, 0)}), the sound as it is (${sound() === B2})`, w.ex.web_pe() === 4 && w.ex.web_step_on(4, 0, 0) === 1 && sound() === B2);
  // while playing: the pattern that comes in changes the steps and nothing else
  np(w, -4); np(w, 1); w.key(BLACK[0], 20); w.key(WHITE[0], 10); np(w, -1);
  w.btn(B.PLAY); waitStep(3);
  np(w, 1);
  check(`playing pattern 1, 2 is picked: it waits (${w.ex.web_pq()}), the sound is as it was`, w.ex.web_pq() === 1 && sound() === B2);
  waitStep(15); w.render(5);
  waitStep(0); w.render(5);
  check("  and it comes in with its first step, with the same sound", w.ex.web_pp() === 1 && sound() === B2);
  w.btn(B.PLAY);
  // reset: the whole sound, whichever pattern is shown
  w.render(300);
  w.hold(B.HOME, 800);
  check(`HOME held: the project's sound is back to a first start's (${sound()})`, sound() === fresh);
  np(w, -1);
  check("  whichever pattern is shown", sound() === fresh);
  w.render(4000);
  // saved: one sound
  w.btn(B.GLO); w.btn(B.GLO); w.turn(EN.K3, 3); w.btn(B.HOME);
  const C = sound(); w.render(4000);
  const r = await device(w.flash()); r.render(300);
  const rs = () => [r.ex.web_kit(), r.ex.web_fxparam(FX.DELAY, 1), r.ex.web_fxparam(FX.REVERB, 7), r.ex.web_fxparam(FX.HALF, 6), r.ex.web_mst(0), r.ex.web_mst(1), r.ex.web_mst(3), r.ex.web_lane(0, 1), r.ex.web_lane(3, 4), r.ex.web_fxmaster(), r.ex.web_lfo(0, 1), r.ex.web_lfo(1, 1), r.ex.web_lfo(1, 2), r.ex.web_env(0), r.ex.web_env(5), r.ex.web_env(7)].join(",");
  check(`it is saved (${rs()})`, rs() === C);
  np(r, 1); r.render(60);
  check("  and it is the same under the other patterns", rs() === C);
  // an older save: one sound a pattern; the one of the pattern shown is the project's
  const old = w.flash(); old.fill(0xFF, 0x9F000, 0xA0000); old.fill(0xFF, 0xFE000, 0xFF000);
  const o = await device(old); o.render(300);
  const os = () => [o.ex.web_kit(), o.ex.web_fxparam(FX.DELAY, 1), o.ex.web_mst(0), o.ex.web_lane(0, 1)].join(",");
  const o1 = os(); np(o, 1); o.render(60); const o2 = os();
  check(`an older save (no sounds): the sound it had is the project's (${o1} | ${o2})`, o1 === o2);
  // the first sounds (184 bytes each, no LFOs, no ENV) are read too: the sound of the pattern shown, as it was
  {
    const crcTable = new Uint32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
    const crc32 = (u8) => { let c = 0xFFFFFFFF; for (const b of u8) c = crcTable[(c ^ b) & 255] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; };
    const len = 4 + 184 * 16, pay = new Uint8Array(len);
    pay.set([0x44, 0x42, 0x58, 0x53], 0);                      // "DBXS"
    for (let p = 0; p < 16; p++) {
      const o = 4 + 184 * p;
      for (let l = 0; l < 11; l++) { pay[o + l * 8 + 2] = 40 + p; pay[o + l * 8 + 3] = 64; pay[o + l * 8 + 4] = 64; pay[o + l * 8 + 5] = 64; pay[o + l * 8 + 6] = 100; }
      for (let e = 0; e < 11; e++) for (let i = 0; i < 8; i++) pay[o + 88 + e * 8 + i] = 10 + i;
      pay[o + 176] = 0; pay[o + 177] = 127; pay[o + 180] = p % 9; pay[o + 181] = 100;
    }
    const flash = new Uint8Array(0x100000).fill(0xFF), hdr = new DataView(flash.buffer, 0x9F000, 32);
    flash.set(pay, 0x9F000 + 256);
    hdr.setUint32(0, 0x554C4546, true); hdr.setUint16(4, 8, true); hdr.setUint16(6, 0, true); hdr.setUint32(8, 1, true);
    hdr.setUint32(12, len, true); hdr.setUint32(16, crc32(pay), true); hdr.setUint32(20, 0xFFFFFFFF, true); hdr.setUint32(24, 0xFFFFFFFF, true);
    hdr.setUint32(28, crc32(new Uint8Array(flash.buffer, 0x9F000, 28)), true);
    const m = await device(flash); m.render(300);
    const ms = () => [m.ex.web_kit(), m.ex.web_lane(0, 1), m.ex.web_lfo(0, 1), m.ex.web_lfo(1, 1), m.ex.web_env(0)].join(",");
    const m1 = ms(); np(m, 1); m.render(60); const m2 = ms();
    check(`the first sounds are read (the pattern shown's: ${m1}), and it is the same under another (${m2})`, m1 === "0,40,0,0,0" && m2 === m1);
  }
}


// ---- the step SELECT works on is framed in violet and marked under its cell, so a bar at full velocity does not hide it
{
  const w = await device();
  const VIOLET = 0x7FB4, GREEN = 0xB1C7, GY = 134;
  const cx = (s) => 12 + Math.floor(s / 4) * 56 + (s % 4) * 12;
  const px = (fb, x, y) => fb[y * 240 + x];
  w.key(BLACK[0], 20); w.key(WHITE[0], 10); w.turn(EN.SELECT, 40);                // step 1 at velocity 127: its bar is green to the edge
  w.key(WHITE[4], 10); w.turn(EN.SELECT, 8);                                       // step 5, the one picked: velocity 108
  w.render(100);
  let fb = w.screen();
  check(`the step picked is framed in violet (the corner ${px(fb, cx(4), GY).toString(16)}, the side ${px(fb, cx(4), GY + 10).toString(16)}) and marked under its cell (${px(fb, cx(4) + 5, GY + 25).toString(16)})`,
        px(fb, cx(4), GY) === VIOLET && px(fb, cx(4) + 10, GY + 10) === VIOLET && px(fb, cx(4) + 5, GY + 25) === VIOLET);
  w.key(WHITE[0], 10); w.key(WHITE[0], 10); w.render(100);                         // off and on again: step 1 is the one picked, at the default velocity; the full bar needs SELECT
  w.turn(EN.SELECT, 40); w.render(100); fb = w.screen();
  check(`  at full velocity (${w.ex.web_vel(0, 0)}) the picked step's frame is violet against the green bar (${px(fb, cx(0), GY + 10).toString(16)} on the edge, the bar inside ${px(fb, cx(0) + 5, GY + 10).toString(16)})`,
        w.ex.web_vel(0, 0) === 127 && px(fb, cx(0), GY + 10) === VIOLET && px(fb, cx(0) + 5, GY + 10) === GREEN && px(fb, cx(0) + 5, GY + 25) === VIOLET);
  check("  the mark moved with the pick: the one under step 5 is gone", px(fb, cx(4) + 5, GY + 25) !== VIOLET);
}

// ---- the LFOs keep to the bars: RATE or the tempo turned while playing does not take the cycle off the steps
{
  const w = await device();
  const near = (a, b, tol = 0.04) => { const d = Math.abs(a - b); return Math.min(d, 1 - d) < tol; };
  const looks = (k, cycle8, tol) => {                               // the phase right after each of 24 steps against where the step puts it
    let ok = 0, n = 0, last = w.ex.web_pos();
    for (let i = 0; i < 4000 && n < 24; i++) {
      w.render(1);
      const pos = w.ex.web_pos();
      if (pos !== last) { last = pos; w.render(2); n++; const ph = w.ex.web_lfo_phase(k) / 4294967296, den = cycle8 * w.ex.web_div(); if (near(ph, (pos * 32 % den) / den, tol)) ok++; }
    }
    return [ok, n];
  };
  w.btn(B.PLAY); w.render(300);
  w.btn(B.LFO); w.turn(EN.K1, -3); w.btn(B.HOME);               // LFO 1: 1/8 -> 1 BAR while it plays
  const [ok1, n1] = looks(0, 32);
  check(`RATE turned while playing: LFO 1 stays on the bars (${ok1} of ${n1} steps in place)`, n1 >= 20 && ok1 === n1);
  w.btn(B.GLO); w.turn(EN.K1, 25); w.btn(B.HOME);                // the tempo up to 145: the same
  const [ok2, n2] = looks(0, 32);
  check(`  and the tempo turned (${w.ex.web_bpm()} BPM: ${ok2} of ${n2})`, n2 >= 20 && ok2 === n2 && w.ex.web_bpm() > 140);
  w.btn(B.LFO); w.btn(B.LFO); w.turn(EN.K1, 3); w.btn(B.HOME);   // LFO 2: 1/2 -> 1/16, faster than a step
  const [ok3, n3] = looks(1, 2, 0.1);                                    // (a step is 100 ms and a cycle one step: a few ms is a few %)
  check(`  a rate faster than a step is on the step too (1/16: ${ok3} of ${n3})`, n3 >= 20 && ok3 === n3);
  w.btn(B.PLAY); w.render(100); w.btn(B.PLAY); w.render(50);
  check(`  and from the top again (PLAY): the first step is the start of the cycle (${(w.ex.web_lfo_phase(0) / 4294967296).toFixed(3)})`, w.ex.web_lfo_phase(0) / 4294967296 < 0.15);
}

// ---- SAVE held 0.6 s writes what waits at once, even while playing; a tap still opens the pattern page
{
  const w = await device();
  w.key(BLACK[0], 20); w.key(WHITE[0], 10);
  w.btn(B.PLAY); w.render(300);
  w.btn(B.GLO); w.turn(EN.K1, 20); w.btn(B.HOME);               // the tempo changes while playing
  const bpm = w.ex.web_bpm();
  w.render(3500);
  check(`playing, a change waits (${w.ex.web_flash_writes_count()} writes, dirty ${w.ex.web_dirty()})`, w.ex.web_dirty() !== 0 && w.ex.web_playing() === 1);
  const before = w.ex.web_flash_writes_count();
  w.hold(B.SAVE, 800);
  w.render(600);
  check(`SAVE held writes it at once, still playing (${w.ex.web_flash_writes_count() - before} flash operations, dirty ${w.ex.web_dirty()})`, w.ex.web_playing() === 1 && w.ex.web_dirty() === 0 && w.ex.web_flash_writes_count() > before);
  check("  and the held SAVE did not open the pattern page", w.ex.web_page() === 0);
  const r = await device(w.flash()); r.render(300);
  check(`  what was written is what the next start loads (${r.ex.web_bpm()} BPM)`, r.ex.web_bpm() === bpm);
  w.btn(B.SAVE);
  check("a tap of SAVE opens the pattern page, another the song's, a third closes it", w.ex.web_page() === 3 && (w.btn(B.SAVE), w.ex.web_page() === 16) && (w.btn(B.SAVE), w.ex.web_page() === 0));
}

// ---- MUTE and SOLO: GLO held, then a lane's key (let go: mute; held half a second: solo)
{
  const w = await device();
  const mutes = () => [...Array(11).keys()].map((l) => w.ex.web_lane(l, 5)).join("");
  const chord = (lane, keyMs) => {                              // GLO held, the lane's key pressed for keyMs, both let go
    w.ex.web_buttons(1 << B.GLO); w.render(60);
    w.ex.web_keys(1 << BLACK[lane]); w.render(keyMs); w.ex.web_keys(0); w.render(60);
    w.ex.web_buttons(0); w.render(120);
  };
  w.key(BLACK[0], 20); w.key(WHITE[0], 10); w.key(BLACK[3], 20); w.key(WHITE[1], 10);
  const unmutedScreen = w.screen();
  chord(0, 60);
  check(`GLO held + the kick's key, let go: the kick is muted (${mutes()}), and GLO did not open its page (${w.ex.web_page()})`, mutes() === "10000000000" && w.ex.web_page() === 0);
  check("  its key breathes (a muted lane shows on its key)", ((w.ex.web_breath_keys() >>> BLACK[0]) & 1) === 1 && ((w.ex.web_breath_keys() >>> BLACK[3]) & 1) === 0);
  w.key(BLACK[0], 20); w.render(100);
  check("  the lane page says MUTED (the screen differs from the unmuted one)", w.screen().some((v, i) => v !== unmutedScreen[i]));
  // a muted lane is silent, the others are not
  const peak = (lane, step) => { w.key(BLACK[lane], 20); w.ex.web_keys(1 << WHITE[step]); const c = w.cap(150); w.ex.web_keys(0); w.render(100); w.key(WHITE[step], 10); w.key(WHITE[step], 10); let m = 0; for (const v of c.l) m = Math.max(m, Math.abs(v)); return m; };
  const pk = peak(0, 5), ph = peak(3, 6);
  check(`  muted, a kick step gives no sound; a hat step does (${pk.toFixed(3)} / ${ph.toFixed(3)})`, pk < 0.001 && ph > 0.02);
  chord(0, 60);
  check(`  chord again: the kick is back (${mutes()})`, mutes() === "00000000000");
  const pk2 = peak(0, 7);
  check(`  and it sounds (${pk2.toFixed(3)})`, pk2 > 0.05);
  // SOLO: held half a second
  chord(3, 700);
  check(`GLO held + a key held 0.7 s: that lane alone (${mutes()})`, mutes() === "11101111111");
  chord(3, 700);
  check(`  again, on the lane that is alone: every lane back (${mutes()})`, mutes() === "00000000000");
  // a tap of GLO alone is still the page
  w.btn(B.GLO);
  check("GLO tapped alone: the page, as before", w.ex.web_page() === 1);
  w.btn(B.GLO); w.btn(B.GLO);
  // the mutes follow the project, not the pattern, and are not kept at power-off; a reset puts them back
  chord(0, 60); chord(4, 60);
  np(w, 1); w.render(100);
  check(`a pattern change does not change them (${mutes()})`, mutes() === "10001000000");
  np(w, -1); w.render(3000);
  const r = await device(w.flash()); r.render(300);
  check("  they are not saved: the next start has every lane sounding", [...Array(11).keys()].every((l) => r.ex.web_lane(l, 5) === 0));
  w.hold(B.HOME, 800);
  check(`  a reset puts them back (${mutes()})`, mutes() === "00000000000");
  // on an FX page the black keys are effects: the chord does not mute a lane
  w.btn(B.FX);
  w.ex.web_buttons(1 << B.GLO); w.render(60); w.ex.web_keys(1 << BLACK[0]); w.render(60); w.ex.web_keys(0); w.ex.web_buttons(0); w.render(120);
  check("  and on the FX pages it mutes nothing", mutes() === "00000000000");
}

// ---- PAN per lane (the ALGORITHM knob, EDIT 2), EXTRA and ACCENT (EDIT 2), CHOKE groups
{
  const rmsLR = (c, i0 = 0, i1 = c.l.length) => { let a = 0, b = 0; for (let i = i0; i < i1; i++) { a += c.l[i] ** 2; b += c.r[i] ** 2; } return [Math.sqrt(a / (i1 - i0)), Math.sqrt(b / (i1 - i0))]; };
  const hitOf = async (lane, setup, ms = 400) => {              // one hit of a lane, stereo, from its first sample
    const w = await device();
    w.key(BLACK[lane], 20);
    if (setup) setup(w);
    w.render(300);
    w.ex.web_keys(1 << WHITE[15]); const c = w.cap(ms); w.ex.web_keys(0);
    let on = 0; while (on < c.l.length && Math.abs(c.l[on]) < 1e-4 && Math.abs(c.r[on]) < 1e-4) on++;
    return { l: c.l.slice(on), r: c.r.slice(on), w };
  };
  // the ALGORITHM knob is the selected lane's PAN
  {
    const w = await device();
    w.key(BLACK[0], 20); w.turn(EN.ALGO, 10);
    check(`ALGORITHM on a lane page: the lane's PAN (${w.ex.web_lane(0, 7)}: +10), the others' stay (${w.ex.web_lane(1, 7)})`, w.ex.web_lane(0, 7) === 10 && w.ex.web_lane(1, 7) === 0);
    w.turn(EN.ALGO, 60); const hi = w.ex.web_lane(0, 7); w.turn(EN.ALGO, -100); const lo = w.ex.web_lane(0, 7);
    check(`  it stops at the ends (${lo}, ${hi}: -32, 32)`, hi === 32 && lo === -32);
    const a = w.screen(); w.turn(EN.ALGO, 8); w.render(100);
    check("  and the lane page shows it (PAN L ... in the title row)", w.screen().some((v, i) => v !== a[i]));
    w.btn(B.EDIT); w.btn(B.EDIT); w.turn(EN.ALGO, 5);
    check(`  and on EDIT 2 too (${w.ex.web_lane(0, 7)}: -19)`, w.ex.web_page() === 14 && w.ex.web_lane(0, 7) === -19);
  }
  // heard: a lane hard left is left only, hard right right only, the middle both
  {
    const c0 = await hitOf(0), cl = await hitOf(0, (w) => w.turn(EN.ALGO, -40)), cr = await hitOf(0, (w) => w.turn(EN.ALGO, 40));
    const [l0, r0] = rmsLR(c0), [ll, rl] = rmsLR(cl), [lr, rr] = rmsLR(cr);
    check(`PAN: the middle is both sides alike (${l0.toFixed(3)} ${r0.toFixed(3)}), hard left is left only (${ll.toFixed(3)} ${rl.toFixed(3)}), hard right right only (${lr.toFixed(3)} ${rr.toFixed(3)})`,
      Math.abs(l0 - r0) < 1e-6 && rl < ll * 0.01 && lr < rr * 0.01 && ll > l0 * 0.9 && rr > r0 * 0.9);
    const half = await hitOf(0, (w) => w.turn(EN.ALGO, 16));
    const [lh, rh] = rmsLR(half);
    check(`  halfway right: the left side is down, not out (${lh.toFixed(3)} against ${rh.toFixed(3)})`, lh < rh * 0.85 && lh > rh * 0.3);
  }
  // each pattern has its own pan, and a reset puts it back
  {
    const w = await device();
    w.key(BLACK[0], 20); w.turn(EN.ALGO, -12);
    np(w, 1); w.render(100);
    check(`PAN is part of the pattern's sound (pattern 2: ${w.ex.web_lane(0, 7)}, back on 1: ${(np(w, -1), w.render(100), w.ex.web_lane(0, 7))})`, w.ex.web_lane(0, 7) === -12);
    w.hold(B.HOME, 800);
    check(`  a reset puts it back (${w.ex.web_lane(0, 7)})`, w.ex.web_lane(0, 7) === 0);
  }
  // CHOKE: only the two hats, and nothing to set
  {
    const run = async (open, shut, ms) => {                                                 // an open hat on step 1, a lane `shut` on step 3: the open hat's ring after
      const w = await device();
      w.key(BLACK[open], 20); w.key(WHITE[0], 10); w.turn(EN.K2, 40);                       // (a long DECAY: it would ring on)
      w.key(BLACK[shut], 20); w.key(WHITE[2], 10); if (shut !== 3) w.turn(EN.K4, -70);        // (any other lane, silent)
      w.render(3000); w.btn(B.PLAY); w.render(15);
      const c = w.cap(ms), win = (a, b) => rmsLR(c, Math.round(a * 44.1), Math.round(b * 44.1))[0];
      return { before: win(100, 250), after: win(330, 430) };
    };
    const hat = await run(4, 3, 600), snare = await run(4, 1, 600);
    check(`CHOKE: a closed hat cuts the open one (${hat.after.toFixed(4)}; before it ${hat.before.toFixed(4)}), a snare does not (${snare.after.toFixed(4)})`, hat.after < hat.before * 0.03 && snare.after > snare.before * 0.3);
    const w = await device();
    check(`  the hats are in group 1 (${w.ex.web_lane(3, 8)}, ${w.ex.web_lane(4, 8)}), the others in none (${[0, 1, 2, 5, 6, 7, 8, 9, 10].map((l) => w.ex.web_lane(l, 8)).join("")})`, w.ex.web_lane(3, 8) === 1 && w.ex.web_lane(4, 8) === 1 && [0, 1, 2, 5, 6, 7, 8, 9, 10].every((l) => w.ex.web_lane(l, 8) === 0));
  }
}

// ---- EDIT 2: the step's own EXTRA (DRIVE, SNAPPY ...) and PAN, with its DECAY and TONE
{
  const w = await device();
  w.key(BLACK[0], 20); w.key(WHITE[0], 10); w.key(WHITE[4], 10);                     // the kick, steps 1 and 5 (5 is the one picked)
  w.btn(B.EDIT); w.btn(B.EDIT);
  const lane0 = [3, 7].map((f) => w.ex.web_lane(0, f));
  w.turn(EN.K3, 2); w.turn(EN.K4, 3);
  const own = (st) => [2, 3].map((f) => w.ex.web_lock(f, 0, st)).join(",");
  check(`EDIT 2, a step picked: its own EXTRA and PAN (step 5: ${own(4)}: 16,12), step 1 as it is (${own(0)})`, own(4) === "16,12" && own(0) === "0,0");
  check("  the lane's own are as they were", [3, 7].every((f, k) => w.ex.web_lane(0, f) === lane0[k]));
  w.turn(EN.K3, 9); w.turn(EN.K4, 20);
  check(`  they stop at the ends (${own(4)}: 56,28)`, own(4) === "56,28");
  w.turn(EN.K3, -30); w.turn(EN.K4, -30);
  check(`  ... at both (${own(4)}: -64,-32)`, own(4) === "-64,-32");
  w.key(WHITE[0], 10); w.turn(EN.K4, 1);                                             // a step that is on: picked, not turned off
  check(`  a white key picks another step (still on: ${w.ex.web_steps_of(0) & 1}), with its own PAN (${own(0)}: 0,4), the first kept (${own(4)})`, (w.ex.web_steps_of(0) & 1) === 1 && own(0) === "0,4" && own(4) === "-64,-32");
  w.key(BLACK[0], 20); w.turn(EN.K3, 1);
  check(`  the lane picked again: all its steps (${own(0)} ${own(4)}: 8,4 and -56,-32)`, own(0) === "8,4" && own(4) === "-56,-32");
  check("  another lane has none", [2, 3].every((f) => w.ex.web_lock(f, 1, 0) === 0 && w.ex.web_lock(f, 1, 4) === 0));
  check("  and the page names the knob after the drum, the lane's own EXTRA is the design's", w.ex.web_lane(0, 3) === lane0[0]);
}
{
  // the bits of one step are its own: the last step of the kick (16) and the first of the snare (1) neighbour in the store
  const w = await device();
  w.key(BLACK[0], 20); w.key(WHITE[15], 10); w.btn(B.EDIT); w.btn(B.EDIT); w.turn(EN.K1, 9); w.turn(EN.K2, -9); w.turn(EN.K3, 9); w.turn(EN.K4, -9);
  w.key(BLACK[1], 20); w.key(WHITE[0], 10); w.turn(EN.K1, -9); w.turn(EN.K2, 9); w.turn(EN.K3, -9); w.turn(EN.K4, 9);
  const v = (l, st) => [0, 1, 2, 3].map((f) => w.ex.web_lock(f, l, st)).join(",");
  check(`a step's nibbles do not leak into its neighbours (kick 16: ${v(0, 15)}, snare 1: ${v(1, 0)}, snare 2: ${v(1, 1)})`, v(0, 15) === "56,-64,56,-32" && v(1, 0) === "-64,56,-64,28" && v(1, 1) === "0,0,0,0" && v(0, 14) === "0,0,0,0");
}
{
  // heard: the snare's SNAPPY, and an ACCENT, on a step
  const rmsLR = (c, i0, i1) => { let a = 0; for (let i = i0; i < i1; i++) a += c.l[i] ** 2; return Math.sqrt(a / (i1 - i0)); };
  const hit = async (lane, setup) => {                                             // the lane's step 1, set up on EDIT 2, played from the top
    const w = await device();
    w.key(BLACK[lane], 20); w.key(WHITE[0], 10); w.btn(B.EDIT); w.btn(B.EDIT); setup(w); w.btn(B.HOME);
    w.render(300); w.btn(B.PLAY); w.render(15);
    return w.cap(300);
  };
  const s0 = await hit(1, (w) => w.turn(EN.K3, -50)), s1 = await hit(1, (w) => w.turn(EN.K3, 50));
  check(`EDIT 2: the snare step's SNAPPY (the third knob, named by the drum): down ${rmsLR(s0, 600, 6000).toFixed(4)}, up ${rmsLR(s1, 600, 6000).toFixed(4)} (the tail's noise)`, rmsLR(s1, 600, 6000) > rmsLR(s0, 600, 6000) * 1.15);
  const w = await device();
  check(`  the lane's own EXTRA is the design's (${w.ex.web_lane(1, 3)}: 80)`, w.ex.web_lane(1, 3) === 80);
}
{
  // stopped, a click of a knob on EDIT 2 plays the step
  const w = await device();
  w.key(BLACK[1], 20); w.key(WHITE[0], 10); w.btn(B.EDIT); w.btn(B.EDIT); w.render(500);
  w.ex.web_enc(EN.K3, 1); w.render(100);
  check(`EDIT 2 stopped: a click of KNOB 3 plays the step (peak ${w.peak.toFixed(3)})`, w.peak > 0.02);
}
{
  // heard: the step with a PAN to the right is on the right, the other in the middle
  const w = await device();
  w.key(BLACK[0], 20); w.key(WHITE[0], 10); w.key(WHITE[4], 10);
  w.btn(B.EDIT); w.btn(B.EDIT); w.turn(EN.K4, 9); w.btn(B.HOME);                    // step 5: PAN +28
  w.render(300); w.btn(B.PLAY); w.render(15);
  const c = w.cap(900), rms = (ch, a, b) => { let s = 0; for (let i = Math.round(a * 44.1); i < Math.round(b * 44.1); i++) s += ch[i] ** 2; return Math.sqrt(s / ((b - a) * 44.1)); };
  const l1 = rms(c.l, 0, 100), r1 = rms(c.r, 0, 100), l5 = rms(c.l, 500, 640), r5 = rms(c.r, 500, 640);
  check(`EDIT 2: the step with PAN +28 is on the right (${l5.toFixed(3)} left, ${r5.toFixed(3)} right), the other in the middle (${l1.toFixed(3)}, ${r1.toFixed(3)})`, Math.abs(l1 - r1) < 1e-6 && r5 > l5 * 1.3);
}
{
  // saved with the pattern, copied with it
  const w = await device();
  w.key(BLACK[0], 20); w.key(WHITE[3], 10); w.btn(B.EDIT); w.btn(B.EDIT); w.turn(EN.K3, 3); w.turn(EN.K4, -2);
  w.btn(B.HOME); w.render(3500);
  const r = await device(w.flash()); r.render(300);
  const got = (x) => [2, 3].map((f) => x.ex.web_lock(f, 0, 3)).join(",");
  check(`a step's own EXTRA and PAN are saved (${got(r)}: 24,-8)`, got(r) === "24,-8");
  w.btn(B.SAVE); w.ex.web_keys(1 << WHITE[0]); w.render(60); w.ex.web_keys((1 << WHITE[0]) | (1 << WHITE[3])); w.render(60); w.ex.web_keys(0); w.render(100);
  check(`  and the copy of the pattern has them (${got(w)})`, w.ex.web_pe() === 3 && got(w) === "24,-8");
}

// ---- EDIT 2: a step's own DECAY and TONE (and EXTRA, ACCENT: further down)
{
  const w = await device();
  w.key(BLACK[4], 20); w.key(WHITE[0], 10); w.key(WHITE[8], 10);          // the open hat, steps 1 and 9
  w.key(WHITE[8], 10); w.key(WHITE[8], 10);                                // (step 9 off and on again: the one picked)
  w.btn(B.EDIT); w.btn(B.EDIT);
  check("EDIT tapped twice: the step's sound page", w.ex.web_page() === 14);
  w.turn(EN.K1, 5); w.turn(EN.K2, -3);
  check(`its DECAY and TONE: knob 1 and knob 2 (${w.ex.web_lock(0, 4, 8)}, ${w.ex.web_lock(1, 4, 8)}: +40, -24), the other step is as it is (${w.ex.web_lock(0, 4, 0)})`, w.ex.web_lock(0, 4, 8) === 40 && w.ex.web_lock(1, 4, 8) === -24 && w.ex.web_lock(0, 4, 0) === 0);
  w.turn(EN.K1, 20); w.turn(EN.K2, -20);
  check(`  they stop at the ends (${w.ex.web_lock(0, 4, 8)}, ${w.ex.web_lock(1, 4, 8)}: 56, -64)`, w.ex.web_lock(0, 4, 8) === 56 && w.ex.web_lock(1, 4, 8) === -64);
  w.key(WHITE[0], 10);                                                     // a white key on a step that is on: picked
  w.turn(EN.K1, -20);
  check(`  another step picked: its own (${w.ex.web_lock(0, 4, 0)}: -64), the first kept (${w.ex.web_lock(0, 4, 8)})`, w.ex.web_lock(0, 4, 0) === -64 && w.ex.web_lock(0, 4, 8) === 56);
  check("  the grid marks both as edited (the page still shows)", w.ex.web_page() === 14);
  w.btn(B.EDIT);
  // heard: the step with a short DECAY rings less than the one with a long DECAY
  w.render(300); w.btn(B.PLAY); w.render(15);
  const c = w.cap(1900);
  const rms = (a, b) => { let s = 0; for (let i = Math.round(a * 44.1); i < Math.round(b * 44.1); i++) s += c.l[i] ** 2; return Math.sqrt(s / ((b - a) * 44.1)); };
  const shortTail = rms(120, 400), longTail = rms(1120, 1400);
  check(`the open hat's step 1 (DECAY -64) rings shorter than step 9 (+56): ${shortTail.toFixed(4)} against ${longTail.toFixed(4)}`, longTail > shortTail * 2.5);
  w.btn(B.PLAY);
  // re-placed, a step starts as a step does
  w.key(WHITE[8], 10); w.key(WHITE[8], 10);
  check(`a step placed again has none (${w.ex.web_lock(0, 4, 8)})`, w.ex.web_lock(0, 4, 8) === 0);
  w.btn(B.EDIT); w.btn(B.EDIT);
  w.turn(EN.K1, 3); w.turn(EN.K2, 2);
  // copied with the pattern, saved, moved by the mutation
  w.btn(B.HOME);
  w.btn(B.SAVE); w.ex.web_keys(1 << WHITE[0]); w.render(60); w.ex.web_keys((1 << WHITE[0]) | (1 << WHITE[3])); w.render(60); w.ex.web_keys(0); w.render(100); w.btn(B.SAVE); w.btn(B.SAVE);
  check(`a copied pattern takes the steps' DECAY and TONE (${w.ex.web_lock(0, 4, 8)}, ${w.ex.web_lock(1, 4, 8)}: 24, 16)`, w.ex.web_pe() === 3 && w.ex.web_lock(0, 4, 8) === 24 && w.ex.web_lock(1, 4, 8) === 16);
  np(w, -1); np(w, -1); np(w, -1); w.render(3500);
  const r = await device(w.flash()); r.render(300);
  check(`they are saved (${r.ex.web_lock(0, 4, 8)}, ${r.ex.web_lock(1, 4, 8)})`, r.ex.web_lock(0, 4, 8) === 24 && r.ex.web_lock(1, 4, 8) === 16);
  np(r, 1); np(r, 1); np(r, 1); r.render(100);
  check("  and the copy's", r.ex.web_lock(0, 4, 8) === 24 && r.ex.web_lock(1, 4, 8) === 16);
  // a mutation moves the hit with them
  const m = await device();
  m.key(BLACK[4], 20); m.key(WHITE[0], 10); m.key(WHITE[0], 10); m.key(WHITE[0], 10);
  m.btn(B.EDIT); m.btn(B.EDIT); m.turn(EN.K1, 4); m.turn(EN.K2, -2); m.btn(B.HOME);
  m.btn(B.ARP); m.btn(B.ARP); m.turn(EN.K1, 20); m.btn(B.HOME);
  m.btn(B.PLAY); m.render(2600); m.btn(B.PLAY); m.render(200);
  const where = [...Array(16).keys()].filter((s) => (m.ex.web_steps_of(4) >> s) & 1)[0];
  check(`the mutation moves a hit with its DECAY and TONE (it is on step ${where + 1} now: ${m.ex.web_lock(0, 4, where)}, ${m.ex.web_lock(1, 4, where)}: 32, -16)`, where !== 0 && m.ex.web_lock(0, 4, where) === 32 && m.ex.web_lock(1, 4, where) === -16);
}

// ---- SONG: a chain of patterns, each played its passes, on the second SAVE page
{
  const w = await device();
  const passes = (n, from) => {                                  // the pattern playing in each of the next n passes (read at the start of each)
    const o = []; let last = from === undefined ? w.ex.web_pass() : from;
    for (let i = 0; i < 12000 && o.length < n; i++) { w.render(4); const p = w.ex.web_pass(); if (p !== last) { last = p; w.render(8); o.push(w.ex.web_pp()); } }
    return o;
  };
  for (let k = 0; k < 3; k++) { if (k) np(w, 1); w.key(BLACK[0], 20); w.key(WHITE[0], 10); }   // a kick in patterns 1, 2 and 3
  w.btn(B.SAVE); w.btn(B.SAVE);
  check(`SAVE tapped twice: the song page (${w.ex.web_page()}), a first start has no song (${[0, 1, 2].map((s) => w.ex.web_song(s, 0)).join(",")}, off ${w.ex.web_song_on()})`, w.ex.web_page() === 16 && w.ex.web_song(0, 0) === 255 && w.ex.web_song_on() === 0);
  w.key(WHITE[0], 40); w.turn(EN.K1, 1); w.turn(EN.K2, 1);        // slot 1: pattern 1, twice
  w.key(WHITE[1], 40); w.turn(EN.K1, 2);                          // slot 2: pattern 2
  w.key(WHITE[2], 40); w.turn(EN.K1, 3);                          // slot 3: pattern 3
  w.turn(EN.K4, 1);
  check(`the slots: 1 x2, 2, 3, then the end; the song on (${[0, 1, 2, 3].map((s) => w.ex.web_song(s, 0)).join(",")} | ${[0, 1, 2].map((s) => w.ex.web_song(s, 1)).join(",")} | ${w.ex.web_song_on()})`,
        [0, 1, 2, 255].every((v, s) => w.ex.web_song(s, 0) === v) && w.ex.web_song(0, 1) === 1 && w.ex.web_song(1, 1) === 0 && w.ex.web_song_on() === 1);
  check("  the slot picked has its key lit, the others of the chain the mid level", w.led(WHITE[2]) === 1 && w.led(WHITE[0]) === 0 && ((w.ex.web_mid_keys() >>> WHITE[0]) & 1) === 1);
  w.btn(B.SAVE);
  np(w, 1); np(w, 1);                                 // (pattern 3 shown: PLAY starts from the song's first slot all the same)
  const base = w.ex.web_pass(), shown = w.ex.web_pe();
  w.btn(B.PLAY);
  const seq = passes(9, base);
  check(`PLAY: the song from its first slot, round and round (${seq.map((p) => p + 1).join(" ")}: 1 1 2 3 1 1 2 3 1)`, seq.join() === "0,0,1,2,0,0,1,2,0");
  check(`  the pattern shown stays the one picked (${w.ex.web_pe() + 1}: ${shown + 1}) while the song plays others (${w.ex.web_pp() + 1})`, w.ex.web_pe() === shown);
  // a pattern picked while the song plays is only the one shown and edited: the song goes on as planned
  np(w, 1); np(w, 1);
  const picked = w.ex.web_pe(), after = passes(3), plan = [0, 0, 1, 2];
  check(`a pattern picked while it plays (${picked + 1}) is shown, not played (next ${w.ex.web_pq() === 255 ? "none" : w.ex.web_pq() + 1}; the passes ${after.map((p) => p + 1).join(" ")} follow the song)`,
        w.ex.web_pe() === picked && w.ex.web_pq() === 255 && after.length === 3 && [0, 1, 2, 3].some((o) => after.every((p, k) => p === plan[(o + k) % 4])));
  w.key(BLACK[0], 20); w.key(WHITE[8], 10);                                  // ... and its steps are the ones that change (the kick's step 9 on the pattern shown)
  check(`  its steps are edited (${w.ex.web_steps_of(0).toString(2)}) while the song plays on (${w.ex.web_pp() + 1})`, (w.ex.web_steps_of(0) >> 8 & 1) === 1 && w.ex.web_pe() === picked);
  w.key(WHITE[8], 10);                                                     // (and off again: this pattern holds nothing)
  w.btn(B.PLAY); w.render(300);
  // off: the pattern alone
  w.btn(B.SAVE); w.btn(B.SAVE); w.turn(EN.K4, -1); w.btn(B.HOME);
  np(w, -1); w.render(60);
  w.btn(B.PLAY);
  const alone = passes(3);
  check(`SONG off: the pattern shown loops (${alone.map((p) => p + 1).join(" ")})`, alone.every((p) => p === alone[0]));
  w.btn(B.PLAY); w.render(300);
  // the chain built by hand is still there (the song was only put off), and saved
  w.btn(B.SAVE); w.btn(B.SAVE);
  check(`the chain is as it was (${[0, 1, 2, 3].map((s) => w.ex.web_song(s, 0)).join(",")}: 0,1,2,255)`, [0, 1, 2, 255].every((v, s) => w.ex.web_song(s, 0) === v) && w.ex.web_song(0, 1) === 1);
  w.turn(EN.K4, 1); const was = w.ex.web_song_on(); w.turn(EN.K4, -1);
  check(`  KNOB 4 turns the song on (${was}) and off (${w.ex.web_song_on()}), the chain as it was`, was === 1 && w.ex.web_song_on() === 0 && [0, 1, 2, 255].every((v, s) => w.ex.web_song(s, 0) === v));
  w.turn(EN.K4, 1);
  w.render(3500);
  const r = await device(w.flash()); r.render(300);
  check(`the song is saved (${[0, 1, 2, 3].map((s) => r.ex.web_song(s, 0)).join(",")}, on ${r.ex.web_song_on()})`, [0, 1, 2, 255].every((v, s) => r.ex.web_song(s, 0) === v) && r.ex.web_song_on() === 1);
  // `-` in the first slot empties the chain: with the song on, the pattern shown plays
  w.key(WHITE[0], 40); w.turn(EN.K1, -5);
  check(`\`-\` in the first slot ends the chain at once (${[0, 1, 2].map((s) => w.ex.web_song(s, 0)).join(",")}), the song still on (${w.ex.web_song_on()})`, w.ex.web_song(0, 0) === 255 && w.ex.web_song_on() === 1);
  w.btn(B.PLAY);
  const bare = passes(3);
  check(`  an empty chain plays the pattern shown (${bare.map((p) => p + 1).join(" ")}: ${w.ex.web_pe() + 1})`, bare.every((p) => p === w.ex.web_pe()));
  w.btn(B.PLAY); w.render(300);
  w.key(WHITE[1], 40); w.turn(EN.K1, -5); w.key(WHITE[2], 40); w.turn(EN.K1, -5);      // (the other slots emptied too)
  // a song of one slot loops its pattern
  w.key(WHITE[0], 40); w.turn(EN.K1, 2); w.turn(EN.K4, 1); w.btn(B.HOME);
  w.btn(B.PLAY);
  const one = passes(3);
  check(`a song of one slot (pattern 2) loops it (${one.map((p) => p + 1).join(" ")})`, one.every((p) => p === 1));
  w.btn(B.PLAY);
  // AGAIN: the chance (0 to 100 %, by 5) that a slot plays its passes once more instead of the next slot
  w.btn(B.SAVE); w.btn(B.SAVE);                                            // (the song page again)
  w.key(WHITE[0], 40); w.turn(EN.K3, 4);
  check(`KNOB 3: the slot's AGAIN, 5 % a click (${w.ex.web_song(0, 2)}: 20), the other slots' own (${w.ex.web_song(1, 2)})`, w.ex.web_song(0, 2) === 20 && w.ex.web_song(1, 2) === 0);
  w.turn(EN.K3, 30); const top = w.ex.web_song(0, 2); w.turn(EN.K3, -30);
  check(`  it stops at 100 (${top}) and 0 (${w.ex.web_song(0, 2)})`, top === 100 && w.ex.web_song(0, 2) === 0);
  w.turn(EN.K2, -5);                                                        // (slot 1: one pass)
  w.key(WHITE[1], 40); w.turn(EN.K1, 3);                                    // slot 2: pattern 3 (slot 1 is pattern 2)
  w.btn(B.PLAY); const never = passes(4); w.btn(B.PLAY); w.render(300);
  check(`  AGAIN 0: the slots take turns (${never.map((p) => p + 1).join(" ")}: 2 and 3, one after the other)`, never.every((p, k) => (p === 1 || p === 2) && (k === 0 || p !== never[k - 1])));
  w.key(WHITE[0], 40); w.turn(EN.K3, 30);
  w.btn(B.PLAY); const always = passes(4); w.btn(B.PLAY); w.render(300);
  check(`  AGAIN 100 on the first: it plays again and again (${always.map((p) => p + 1).join(" ")}: 2 2 2 2)`, always.every((p) => p === 1));
  w.render(3500);
  const q = await device(w.flash()); q.render(300);
  check(`  and it is saved (${q.ex.web_song(0, 2)}: 100, passes ${q.ex.web_song(0, 1)}, pattern ${q.ex.web_song(0, 0) + 1})`, q.ex.web_song(0, 2) === 100 && q.ex.web_song(0, 0) === 1 && q.ex.web_song(1, 0) === 2);
}

// ---- the help: a tap of SEQ opens a sheet of the gestures, three pages; held, SEQ still loops
{
  const w = await device();
  w.key(BLACK[0], 20); w.key(WHITE[0], 10); w.render(3000);                 // (the preview has rung out: the scope is a flat line)
  const home = w.screen();
  w.btn(B.SEQ); w.render(100);
  const p1 = w.screen();
  check(`SEQ tapped: the help opens (page ${w.ex.web_page()}, sheet ${w.ex.web_help() + 1} of 3), a screen of its own`, w.ex.web_page() === 17 && w.ex.web_help() === 0 && home.some((v, i) => v !== p1[i]));
  w.btn(B.SEQ); w.render(100);
  const p2 = w.screen();
  w.btn(B.SEQ); w.render(100);
  const p3 = w.screen();
  check(`  tapped again: the second sheet, then the third (${w.ex.web_help() + 1}), all different`, w.ex.web_help() === 2 && p1.some((v, i) => v !== p2[i]) && p2.some((v, i) => v !== p3[i]));
  w.btn(B.SEQ); w.render(100);
  const back = w.screen();
  check("  and once more it closes: the lane page is back, as it was", w.ex.web_page() === 0 && back.every((v, i) => v === home[i]));
  w.btn(B.SEQ); w.btn(B.SEQ); w.btn(B.HOME); w.render(100);
  check("  HOME closes it too", w.ex.web_page() === 0 && w.screen().every((v, i) => v === home[i]));
  // held: the loop, not the help
  w.btn(B.PLAY); w.render(300);
  w.ex.web_buttons(1 << B.SEQ); w.render(700); w.ex.web_buttons(0); w.render(100);
  check(`SEQ held 0.7 s: the loop, not the help (page ${w.ex.web_page()})`, w.ex.web_page() === 0);
  // a knob turned with SEQ held: not a tap
  w.ex.web_buttons(1 << B.SEQ); w.render(60); w.turn(EN.K1, 1); w.ex.web_buttons(0); w.render(100);
  check(`  SEQ and KNOB 1 (the loop's length): not the help either (page ${w.ex.web_page()})`, w.ex.web_page() === 0);
  w.btn(B.PLAY);
}

// ---- EDIT with no step picked: all the lane's steps, by the same clicks
{
  const w = await device();
  w.key(BLACK[0], 20); [0, 4, 8, 12].forEach((s) => w.key(WHITE[s], 10));
  w.key(BLACK[3], 20); [2, 6].forEach((s) => w.key(WHITE[s], 10));        // a hat on 2 and 6, to show it is only the lane
  w.key(BLACK[0], 20);
  w.btn(B.EDIT);                                                           // (the black key on the EDIT page: the whole lane)
  w.key(BLACK[0], 20); w.render(100);
  const all = (f) => [0, 4, 8, 12].map((s) => f(s)).join(",");
  const shown = w.screen();
  w.turn(EN.K1, 4); w.turn(EN.K2, 1); w.turn(EN.K3, -2); w.turn(EN.K4, 3);
  check(`EDIT, no step picked: the knobs act on all the kick's steps (nudge ${all((s) => w.ex.web_nudge(0, s))}, x ${all((s) => w.ex.web_ratchet(0, s))}, chance ${all((s) => w.ex.web_chance(0, s))}, pitch ${all((s) => w.ex.web_semi(0, s))})`,
        all((s) => w.ex.web_nudge(0, s)) === "4,4,4,4" && all((s) => w.ex.web_ratchet(0, s)) === "2,2,2,2" && all((s) => w.ex.web_chance(0, s)) === "90,90,90,90" && all((s) => w.ex.web_semi(0, s)) === "3,3,3,3");
  check("  and the hat is as it was", [2, 6].every((s) => w.ex.web_nudge(3, s) === 0 && w.ex.web_ratchet(3, s) === 1 && w.ex.web_chance(3, s) === 100 && w.ex.web_semi(3, s) === 0));
  check("  the page shows it (the corner says ALL STEPS, the rows have the value they all have)", w.screen().some((v, i) => v !== shown[i]));
  w.key(WHITE[8], 10); w.turn(EN.K1, 5);                                  // a step picked: only that one
  check(`a step picked: the knobs are for it alone (nudge ${all((s) => w.ex.web_nudge(0, s))})`, all((s) => w.ex.web_nudge(0, s)) === "4,4,9,4");
  const one = w.screen();
  w.key(BLACK[0], 20); w.render(100);                                      // the lane again: all of it
  const mixed = w.screen();
  check("  the lane again: MIXED shows where the steps differ (the NUDGE row changes)", mixed.some((v, i) => v !== one[i]));
  w.turn(EN.K1, 1);
  check(`  and a click moves them all, each from where it is (nudge ${all((s) => w.ex.web_nudge(0, s))})`, all((s) => w.ex.web_nudge(0, s)) === "5,5,10,5");
  // EDIT 2: the same for the steps' own DECAY and TONE
  w.btn(B.EDIT); w.key(BLACK[0], 20);
  w.turn(EN.K1, 2); w.turn(EN.K2, -1);
  check(`EDIT 2, no step picked: all the steps' DECAY and TONE (${all((s) => w.ex.web_lock(0, 0, s))} | ${all((s) => w.ex.web_lock(1, 0, s))})`, all((s) => w.ex.web_lock(0, 0, s)) === "16,16,16,16" && all((s) => w.ex.web_lock(1, 0, s)) === "-8,-8,-8,-8" && w.ex.web_lock(0, 3, 2) === 0);
  // a lane with no step: nothing happens
  w.key(BLACK[6], 20); w.turn(EN.K1, 3);
  check("a lane with no step: the knobs change nothing", [...Array(16).keys()].every((s) => w.ex.web_lock(0, 6, s) === 0));
}

// ---- determinism: the same gestures twice give the same audio
async function run() {
  const d = await device(); d.render(100);
  for (const k of [BLACK[0], BLACK[3], BLACK[1], BLACK[4], BLACK[10]]) { d.key(k, 50); d.audition(); }
  d.setSteps([0, 4]); d.turn(EN.SELECT, -3);
  d.btn(B.PLAY); d.render(2000);
  return d.sig;
}
const s1 = await run(), s2 = await run();
check("the same gestures twice give the same audio", s1 === s2);

console.log(fails ? `drumbox: ${fails} FAILED` : "drumbox: all ok");
process.exit(fails ? 1 : 0);

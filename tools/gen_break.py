#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The BREAK kit's sound: 11 slices of one break, one a lane, as a C header (firmware/src/dbx_break.h).

  tools/gen_break.py                      the break U0U ships: drums synthesised here (nothing sampled from a record), so it is free
  tools/gen_break.py --wav my.wav [--cuts 0,0.17,...]   your own break (a WAV you have the right to use): sliced at --cuts (11 start
                                          times in seconds, in lane order; each slice runs to the next cut), or at its 11 strongest
                                          onsets (the default), re-sampled to 22.05 kHz mono 16 bit. Keep your header out of the repo.

The lanes are KICK SNARE CLAP "HAT CL" "HAT OP" TOM CONGA RIM CLAVE BELL CYM; the synthesised break gives them
kick, snare, ghost snare, closed hat, open hat, a tight kick, a loud snare, a second ghost snare, a stick click, a ride bell, a crash.
Each slice is normalised to the level of U0U's own drums (a LEVEL 100 hit peaks near -7 dBFS)."""
import argparse
import sys
import wave
from pathlib import Path

import numpy as np

SR = 22050
PEAK = 14700                      # -7 dBFS of 32767: where the synthesised drums peak at LEVEL 100
WEIGHT = [1, 1, 0.5, 0.6, 0.6, 0.9, 1, 0.45, 0.7, 0.6, 0.7]     # the synthesised slices' own levels (the ghost snares are soft)
LANES = ["KICK", "SNARE", "CLAP", "HAT CL", "HAT OP", "TOM", "CONGA", "RIM", "CLAVE", "BELL", "CYM"]
rng = np.random.default_rng(1969)


def t_of(n):
    return np.arange(n) / SR


def decay(n, tau):
    return np.exp(-t_of(n) / tau)


def band(x, lo, hi):              # a band of the noise (FFT mask with soft edges)
    f = np.fft.rfft(x)
    fr = np.fft.rfftfreq(len(x), 1 / SR)
    m = np.clip((fr - lo) / (0.15 * lo + 1), 0, 1) * np.clip((hi - fr) / (0.15 * hi + 1), 0, 1)
    return np.fft.irfft(f * m, len(x))


def room(x, amount, tail=0.09):   # a little room: the hit through a short decaying noise
    n = int(tail * SR)
    ir = rng.standard_normal(n) * np.exp(-t_of(n) / (tail / 4))
    ir[0] = 0
    return x + amount * np.convolve(x, ir)[: len(x)] / np.sqrt(n / 40)


def kick(sec, f0, f1, tau):
    n = int(sec * SR)
    t = t_of(n)
    ph = 2 * np.pi * np.cumsum(f1 + (f0 - f1) * np.exp(-t / 0.03)) / SR
    return room(np.sin(ph) * np.exp(-t / tau) + 0.5 * band(rng.standard_normal(n), 1000, 4500) * np.exp(-t / 0.004), 0.12)


def snare(sec, tone, noise, tau_n, level=1.0):
    n = int(sec * SR)
    t = t_of(n)
    body = 0.6 * np.sin(2 * np.pi * tone * t) * np.exp(-t / 0.05) + 0.3 * np.sin(2 * np.pi * tone * 1.62 * t) * np.exp(-t / 0.035)
    crack = noise * band(rng.standard_normal(n), 1600, 10500) * np.exp(-t / tau_n)
    rattle = 0.25 * band(rng.standard_normal(n), 3000, 9500) * np.exp(-t / 0.12) * (1 - np.exp(-t / 0.006))
    return level * room(body + crack + rattle, 0.2)


def hat(sec, tau):
    n = int(sec * SR)
    t = t_of(n)
    metal = sum(np.sin(2 * np.pi * f * t + p) for f, p in [(6100, 0.3), (7300, 1.1), (8650, 2.0), (9900, 0.7)]) / 6
    return (band(rng.standard_normal(n), 5200, 10800) + metal) * np.exp(-t / tau) * (1 - np.exp(-t / 0.0008))


def bell(sec):
    n = int(sec * SR)
    t = t_of(n)
    x = sum(a * np.sin(2 * np.pi * f * t) * np.exp(-t / d) for f, a, d in [(760, 1, 0.35), (1130, 0.8, 0.28), (1790, 0.6, 0.22), (2640, 0.45, 0.16), (3710, 0.3, 0.12)])
    return x + 0.25 * band(rng.standard_normal(n), 4000, 10000) * np.exp(-t / 0.05)


def crash(sec):
    n = int(sec * SR)
    t = t_of(n)
    return band(rng.standard_normal(n), 2200, 10800) * (1 - np.exp(-t / 0.003)) * (0.7 * np.exp(-t / 0.28) + 0.3 * np.exp(-t / 0.09))


def synth():
    return [kick(0.22, 130, 52, 0.07),
            snare(0.26, 190, 1.0, 0.09),
            snare(0.09, 200, 0.8, 0.03, 0.35),                              # CLAP: a ghost snare
            hat(0.07, 0.016), hat(0.30, 0.09),
            kick(0.15, 105, 62, 0.04),                                      # TOM: a tight kick
            snare(0.26, 235, 1.2, 0.07),                                    # CONGA: a loud, higher snare
            snare(0.07, 215, 0.7, 0.025, 0.3),                              # RIM: a second ghost snare
            band(rng.standard_normal(int(0.04 * SR)), 1200, 4000) * decay(int(0.04 * SR), 0.006),   # CLAVE: a stick click
            bell(0.40), crash(0.70)]


def from_wav(path, cuts):
    with wave.open(str(path)) as w:
        ch, sw, sr, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        if sw != 2:
            sys.exit("gen_break: a 16-bit WAV, please")
        x = np.frombuffer(w.readframes(n), "<i2").reshape(-1, ch).mean(axis=1) / 32768.0
    f = np.fft.rfft(x)
    f[int(len(f) * min(1.0, SR / 2 / (sr / 2))):] = 0                      # low-pass at the new Nyquist, then re-sample
    x = np.fft.irfft(f, len(x))
    x = np.interp(np.arange(0, len(x) / sr, 1 / SR), np.arange(len(x)) / sr, x)
    if cuts is None:                                                       # the 11 strongest onsets (a hop's rise in level), 60 ms apart at least
        hop = 128
        e = np.sqrt(np.convolve(x * x, np.ones(hop) / hop, "same"))[::hop]
        rise = np.maximum(e[1:] - e[:-1], 0)
        pick = []
        for i in np.argsort(rise)[::-1]:
            if all(abs(i - j) * hop / SR > 0.06 for j in pick):
                pick.append(i)
            if len(pick) == len(LANES):
                break
        cuts = sorted(max(0.0, (i + 1) * hop / SR - 0.003) for i in pick)
    if len(cuts) != len(LANES):
        sys.exit(f"gen_break: {len(LANES)} cuts, not {len(cuts)} (the lanes: {' '.join(LANES)})")
    ends = list(cuts[1:]) + [len(x) / SR]
    return [x[int(a * SR): int(min(b, a + 0.7) * SR)] for a, b in zip(cuts, ends)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--wav")
    ap.add_argument("--cuts", help="11 start times in seconds, comma separated")
    ap.add_argument("--out", default=str(Path(__file__).resolve().parent.parent / "firmware/src/dbx_break.h"))
    a = ap.parse_args()
    cuts = [float(c) for c in a.cuts.split(",")] if a.cuts else None
    sl = from_wav(a.wav, cuts) if a.wav else synth()
    pcm, rows, off = [], [], 0
    top = max(np.max(np.abs(x)) for x in sl)                                 # (a WAV's slices keep their levels from one to the next)
    for i, (name, x) in enumerate(zip(LANES, sl)):
        x = x.copy()
        k = min(len(x), 24)                                                  # a short fade in (1 ms: the hit starts at once) and out (4 ms): no click
        x[:k] *= np.linspace(0, 1, k)
        k = min(len(x), 96)
        x[-k:] *= np.linspace(1, 0, k)
        g = PEAK / top if a.wav else PEAK * WEIGHT[i] / max(np.max(np.abs(x)), 1e-9)
        x = np.round(x * g).astype(np.int16)
        rows.append((name, off, len(x)))
        pcm.append(x)
        off += len(x)
    pcm = np.concatenate(pcm)
    lines = ["/* SPDX-License-Identifier: GPL-3.0-only */",
             "/* The BREAK kit's slices: generated by tools/gen_break.py (" + ("your WAV" if a.wav else "synthesised drums: nothing sampled from a record") + "), 22.05 kHz, 16 bit, mono. Do not edit. */",
             f"#define BRK_RATE {SR}u", f"#define BRK_NSAMP {len(pcm)}u",
             "static const struct { uint32_t off, len; } BRK_SLICE[DBX_NLANE] = {"]
    lines += [f"    {{{o}u, {n}u}},   /* {name} */" for name, o, n in rows] + ["};", "static const int16_t BRK_PCM[BRK_NSAMP] = {"]
    for i in range(0, len(pcm), 16):
        lines.append("    " + ",".join(str(int(v)) for v in pcm[i:i + 16]) + ",")
    lines.append("};")
    Path(a.out).write_text("\n".join(lines) + "\n")
    print(f"gen_break: {len(pcm)} samples ({len(pcm) * 2} B, {len(pcm) / SR:.2f} s) in {len(rows)} slices -> {a.out}")
    for name, o, n in rows:
        print(f"  {name:7s} {n / SR * 1000:6.0f} ms")


if __name__ == "__main__":
    main()

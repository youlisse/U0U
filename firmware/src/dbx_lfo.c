/* SPDX-License-Identifier: GPL-3.0-only */
/* DrumBox LFOs: two, on the LFO button's two pages, both BEFORE the effects. LFO 1 moves the VOLUME of the drums' sum, ahead of the whole
 * chain (the delay's and the reverb's tails are the pumped sound's, not pumped themselves). LFO 2 moves the PAN, at the point where the
 * chain turns stereo (the chain is mono up to the PAN effect): after PAN, before FLANGER, DELAY, REPEAT, REVERB and SATURATE 2, which carry
 * its motion. Each has the same four settings:
 *   RATE   a note value, locked to the tempo: the length of one cycle (4 BARS, 2 BARS, 1 BAR, 1/2, 1/4, 1/8, 1/16, 1/32 of a note)
 *   AMOUNT 0 to 100 %: 0 leaves the sound alone (exactly)
 *   SHAPE  SINE, TRIANGLE, RAMP UP, RAMP DOWN, SQUARE, RANDOM (a new value each cycle)
 *   PHASE  where in the cycle it starts, 0 to 345 degrees (15 a click)
 * The shapes run from -1 to +1 and start at their phase when the sequencer starts (the LFO runs on while it is stopped). Volume: +1 is the
 * sound as it is, -1 is AMOUNT less of it (RAMP UP at 0 degrees ducks on the beat and swells back: a pump). Pan: +1 is right, -1 left,
 * AMOUNT how far out; a balance, not a pan law: the centre and the near channel stay at unity (nothing gets louder or quieter at rest).
 * The pan only moves what is above 100 Hz (it was 300: the toms and congas hardly moved): a Linkwitz-Riley crossover (two Butterworth low-passes and two high-passes in series, 24 dB an
 * octave) splits each channel, the low part goes through as it is (the bass stays in the middle) and the high part gets the pan; the two
 * parts add back to the sound (flat in level, a phase turn only). It starts and stops with a short fade, so engaging it does not click.
 * The LFOs are read once per block of CTL frames and the gains ramp inside it, so a square shape does not click. */
#define LFO_NP 4u                          /* RATE AMOUNT SHAPE PHASE */
#define LFO_RATE 0u
#define LFO_AMT 1u
#define LFO_SHAPE 2u
#define LFO_PHASE 3u
#define LFO_NRATE 8u
#define LFO_NSHAPE 6u
#define LFO_NPHASE 24u                     /* 15 degrees each */
static const char *const LFO_RATE_NAME[LFO_NRATE] = {"4 BARS", "2 BARS", "1 BAR", "1/2", "1/4", "1/8", "1/16", "1/32"};
static const char *const LFO_SHAPE_NAME[LFO_NSHAPE] = {"SINE", "TRIANGLE", "RAMP UP", "RAMP DOWN", "SQUARE", "RANDOM"};
static const uint8_t LFO_MAXV[LFO_NP] = {LFO_NRATE - 1u, 100, LFO_NSHAPE - 1u, LFO_NPHASE - 1u};
static const uint8_t LFO_CYCLE8[LFO_NRATE] = {128, 64, 32, 16, 8, 4, 2, 1};     /* a cycle, in eighths of a beat */
#define LFO_INC_Q8 3324343u                /* 2^32 x 8 x 256 / (FS x 60): the phase step of a block of 1 sample at 1 BPM and a cycle of 1/8 beat */

static struct {
    uint8_t p[2][LFO_NP];                  /* the settings (the UI) */
    volatile uint8_t restart;              /* the sequencer started: from the phase's start (ISR) */
    uint32_t ph[2];                        /* the phases (ISR), a cycle = 2^32 */
    uint32_t rng[2];
    int32_t held[2];                       /* RANDOM's value, Q15 */
    int32_t gv, gl, gr;                    /* the gains now, Q12 (the volume, the left, the right) */
    int32_t tgv, tgl, tgr, tpt;            /* ... and what this block asks (dbx_lfo_tick) */
    int32_t pm;                            /* how much of the split sound the output takes, Q12 (0: the split is off and not computed) */
    int32_t ic[2][4][2];                   /* its filters' states: per channel, the two low-passes and the two high-passes */
    int32_t dcs[2];                        /* the slow average (Q8) of what the split adds to the sound: the integer filters' rounding leaves a DC
                                            * of about -54 dBFS (measured with a split at 300 Hz), taken out here */
    tsvf_t cf;                             /* the crossover's coefficients (LFO_SPLIT_HZ) */
    uint8_t cf_ok;
} dbx_lfo = {{{5, 0, 0, 0}, {3, 0, 0, 0}}, 0, {0, 0}, {0x1234567u, 0x7654321u}, {0, 0}, 4096, 4096, 4096};

static void dbx_lfo_restart(void) { dbx_lfo.restart = 1; }

/* At every step the sequencer starts (ISR), the LFOs' cycles are set to where the steps are: a cycle of RATE lasts cycle8 / 8 beats, that is
 * cycle8 x div / 32 steps, so after pos steps from the start it is pos x 32 / (cycle8 x div) cycles in. Between two steps the phase runs on
 * with the tempo. This keeps the LFOs on the bars whatever happened since PLAY (RATE or tempo turned, a loop held, the computer's clock
 * drifting). Stopped, they run free. */
static void dbx_lfo_sync(uint32_t pos)
{
    uint32_t k;
    for (k = 0; k < 2u; k++) {
        uint32_t den = (uint32_t)LFO_CYCLE8[dbx_lfo.p[k][LFO_RATE]] * dbx_seq.div;
        dbx_lfo.ph[k] = (pos * 32u % den) * (0xFFFFFFFFu / den);
    }
}
#ifndef LFO_SPLIT_HZ
#define LFO_SPLIT_HZ 100u                  /* the pan acts above this: the kick's body stays in the middle, the toms, congas and snare move */
#endif
static uint32_t dbx_lfo_xorshift(uint32_t x) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; }
static int32_t dbx_lfo_value_of(uint32_t x) { return (int32_t)((x >> 8) & 0xFFFFu) - 32768; }   /* RANDOM's value, Q15 */

/* the shape at a phase (2^32 = a cycle): -32767 .. 32767 */
static int32_t dbx_lfo_shape(uint32_t shape, uint32_t ph, int32_t held)
{
    int32_t r = (int32_t)(ph >> 17), v;    /* 0 .. 32767 over the cycle */
    switch (shape) {
    case 0:                                /* SINE: from 0, rising */
        return SINE[(ph >> 22) & 1023u];
    case 1:                                /* TRIANGLE: from 0, rising to the quarter, down to the three quarters, up again */
        v = r < 8192 ? r * 4 : r < 24576 ? 65536 - r * 4 : r * 4 - 131072;
        return v > 32767 ? 32767 : v < -32767 ? -32767 : v;
    case 2:                                /* RAMP UP: from -1 */
        return r * 2 - 32767;
    case 3:                                /* RAMP DOWN: from +1 */
        return 32767 - r * 2;
    case 4:                                /* SQUARE: the first half high */
        return r < 16384 ? 32767 : -32767;
    default:                               /* RANDOM */
        return held;
    }
}

/* one block, first: the LFOs run on, and what they ask of the volume (tgv) and the pan (tgl, tgr: the channel's gain; tpt: the split's
 * share, 4096 when the pan LFO is on) is set; dbx_lfo_volume and dbx_lfo_pan then ramp to it over the block */
static void dbx_lfo_tick(uint32_t n)
{
    uint32_t k, bpm = dbx_seq.bpm;         /* (the measured tempo when following the computer) */
    dbx_lfo.tgv = dbx_lfo.tgl = dbx_lfo.tgr = 4096;
    dbx_lfo.tpt = dbx_lfo.p[1][LFO_AMT] != 0u ? 4096 : 0;
    if (dbx_lfo.restart) {
        dbx_lfo.restart = 0;
        dbx_lfo.ph[0] = dbx_lfo.ph[1] = 0;
    }
    for (k = 0; k < 2u; k++) {
        const uint8_t *p = dbx_lfo.p[k];
        uint32_t q8 = bpm * LFO_INC_Q8 / LFO_CYCLE8[p[LFO_RATE]];          /* the phase step of a frame, Q8 */
        uint32_t inc = (q8 >> 8) * n + (((q8 & 255u) * n) >> 8);
        uint32_t ph = dbx_lfo.ph[k] + (uint32_t)p[LFO_PHASE] * 178956971u;   /* (2^32 / 24 a click: 15 degrees) */
        int32_t v, amt = p[LFO_AMT];
        uint32_t old = dbx_lfo.ph[k];
        dbx_lfo.ph[k] += inc;
        if (old > 0xC0000000u && dbx_lfo.ph[k] < 0x40000000u) {   /* a new cycle (not the small step back a step's resync can make): RANDOM takes a new value */
            dbx_lfo.rng[k] = dbx_lfo_xorshift(dbx_lfo.rng[k]);
            dbx_lfo.held[k] = dbx_lfo_value_of(dbx_lfo.rng[k]);
        }
        if (amt == 0)
            continue;
        v = dbx_lfo_shape(p[LFO_SHAPE], ph + (uint32_t)((inc >> 1)), dbx_lfo.held[k]);   /* (read at the middle of the block) */
        if (k == 0u) {                             /* the volume: +1 as it is, -1 AMOUNT lower */
            int32_t u = (v + 32767) >> 4;          /* 0 .. 4095 */
            dbx_lfo.tgv = 4096 - amt * (4095 - u) / 100;
        } else {                                   /* the pan: the far channel falls as a quarter of a cosine */
            int32_t x = v * amt / 100, ax = x < 0 ? -x : x;
            int32_t g = SINE[256 + (ax >> 7)] >> 3;   /* cos(ax x pi / 2), Q12: (ax / 32768 x 256) steps up the sine's top */
            if (x > 0)
                dbx_lfo.tgl = g;
            else
                dbx_lfo.tgr = g;
        }
    }
}

/* LFO 1, before the effects: the volume of the drums' mono sum (so the effects' tails are not pumped) */
static void dbx_lfo_volume(int32_t *m, int32_t *s, uint32_t n)
{
    uint32_t i;
    int32_t tv = dbx_lfo.tgv;
    if (tv == 4096 && dbx_lfo.gv == 4096)
        return;                                    /* at rest: the sound goes through untouched */
    for (i = 0; i < n; i++) {
        int32_t gv = dbx_lfo.gv + (tv - dbx_lfo.gv) * (int32_t)(i + 1u) / (int32_t)n;
        m[i] = (m[i] >> 3) * gv >> 9;              /* (|m| up to 2^17: >> 3, x 2^12, >> 9 keeps 32 bits) */
        s[i] = (s[i] >> 3) * gv >> 9;              /* (what the lanes' pans add goes with it) */
    }
    dbx_lfo.gv = tv;
}

/* LFO 2, where the chain turns stereo (after the PAN effect, before FLANGER, DELAY, REPEAT, REVERB: they carry the motion): the pan, on what
 * is above the crossover */
static void dbx_lfo_pan(int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i, k;
    int32_t pt = dbx_lfo.tpt, tl = dbx_lfo.tgl, tr = dbx_lfo.tgr, pm0 = dbx_lfo.pm, pm1;
    int32_t *ch[2];
    if (pt == 0 && pm0 == 0)
        return;                                    /* at rest: the sound goes through untouched */
    if (!dbx_lfo.cf_ok) {                          /* the crossover: Butterworth (damping 1.414) at LFO_SPLIT_HZ, set up once */
        uint32_t c = 0;
        while (c < 127u && CUTOFF_HZ[c] < LFO_SPLIT_HZ)
            c++;
        tsvf_coef_k(&dbx_lfo.cf, (int32_t)c << 8, 5793);
        dbx_lfo.cf_ok = 1;
    }
    if (pt && pm0 == 0)                            /* starting: clean states */
        for (k = 0; k < 16u; k++)
            ((int32_t *)dbx_lfo.ic)[k] = 0;
    if (pt && pm0 == 0)
        dbx_lfo.dcs[0] = dbx_lfo.dcs[1] = 0;
    pm1 = pt > pm0 ? (pm0 + 512 > pt ? pt : pm0 + 512) : (pm0 - 512 < pt ? pt : pm0 - 512);   /* ~6 ms to fade in or out */
    ch[0] = l;
    ch[1] = r;
    for (i = 0; i < n; i++) {
        int32_t f = (int32_t)(i + 1u), g[2], pm, s;
        g[0] = dbx_lfo.gl + (tl - dbx_lfo.gl) * f / (int32_t)n;
        g[1] = dbx_lfo.gr + (tr - dbx_lfo.gr) * f / (int32_t)n;
        pm = pm0 + (pm1 - pm0) * f / (int32_t)n;
        for (s = 0; s < 2; s++) {
            int32_t x = ch[s][i];
            int32_t (*ic)[2] = dbx_lfo.ic[s];
            int32_t lo = tsvf_lp(&dbx_lfo.cf, tsvf_lp(&dbx_lfo.cf, x, &ic[0][0], &ic[0][1]), &ic[1][0], &ic[1][1]);
            int32_t hi = dbx_svf_hp(&dbx_lfo.cf, 5793, dbx_svf_hp(&dbx_lfo.cf, 5793, x, ic[2]), ic[3]);
            int32_t d = lo + (hi * g[s] >> 12) - x;                            /* what the split changes */
            dbx_lfo.dcs[s] += ((d << 8) - dbx_lfo.dcs[s]) >> 10;
            d -= dbx_lfo.dcs[s] >> 8;                                          /* (without its DC) */
            ch[s][i] = x + (d * pm >> 12);                                     /* (|d| up to 2^18, x 2^12: 32 bits) */
        }
    }
    dbx_lfo.gl = tl;
    dbx_lfo.gr = tr;
    dbx_lfo.pm = pm1;
}

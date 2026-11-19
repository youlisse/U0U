/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (dsp.c, the filters and tables it builds on) */
/* DrumBox step effects: eleven effects on the master bus, one per black key on the FX page. The signal goes through
 *   HALFTIME, BITCRUSH, SATURATE 1 | PAN, FLANGER, DELAY, REPEAT, REVERB, SATURATE 2 | LOW PASS, BAND PASS
 * The first three work on the mono sum of the drums, the next six on the stereo that PAN makes of it, and the two filters are last, on the stereo output:
 * a filter shapes everything that is playing then, the echoes, the tails and what the lanes' pans add to the sound (they sat ahead of the delay: it
 * echoed what they had not filtered, and the pans' side went round them).
 * Each effect has 16 steps (a pattern's, dbx_seq.fx) and eight parameters: four of its own, then ATTACK, RELEASE,
 * CONTINUOUS (on at every step) and MIX. A step that is on opens the effect: its amount follows a gate through an
 * attack / release envelope, so a run of steps in a row attacks at the first and releases after the last. The amount
 * is that envelope x the step's own mix (SELECT) x the effect's MIX x the global FX MIX (GLO page 2): LOW PASS and BAND
 * PASS sweep their cutoff from FROM to the target with it, the others fade their wet sound in over the dry one, DELAY
 * and REVERB open their send (their tails ring on after it closes).
 * All of it runs in the audio ISR, once per CTL block, in fixed point; an effect with nothing to do costs a test. */
#define DBX_NFXP 8u
enum { FX_HALF, FX_CRUSH, FX_SAT1, FX_LP, FX_BP, FX_PAN, FX_FLANGE, FX_DELAY, FX_REPEAT, FX_REVERB, FX_SAT2 };
enum { FP_ATK = 4, FP_REL = 5, FP_CONT = 6, FP_MIX = 7 };
enum { PK_PCT, PK_CUT, PK_COUNT3, PK_BITS, PK_HOLD, PK_SPEED, PK_GRAIN, PK_GAIN, PK_DRIVE, PK_PAN, PK_HZ, PK_ONOFF, PK_SLOPE,
       PK_SHAPE, PK_DTIME, PK_PRE, PK_ATK, PK_REL };
typedef struct { const char *name; uint8_t kind, opts, def; } dbx_fxp_t;     /* opts: the number of choices, 0 = a range */

static const char *const DBX_FX_NAME[DBX_NFX] = {"HALFTIME", "BITCRUSH", "SATURATE 1", "LOW PASS", "BAND PASS", "PAN", "FLANGER",
                                                 "DELAY", "REPEAT", "REVERB", "SATURATE 2"};
static const dbx_fxp_t DBX_FXP[DBX_NFX][4] = {                                  /* the first page; ATK REL CONT MIX are common */
    {{"SPEED", PK_SPEED, 0, 85}, {"GRAIN", PK_GRAIN, 0, 64}, {"TONE", PK_PCT, 0, 127}, {"GAIN", PK_GAIN, 0, 64}},
    {{"BITS", PK_BITS, 0, 67}, {"RATE", PK_HOLD, 0, 12}, {"NOISE", PK_PCT, 0, 20}, {"TONE", PK_PCT, 0, 127}},
    {{"DRIVE", PK_DRIVE, 0, 60}, {"BIAS", PK_PCT, 0, 20}, {"TONE", PK_PCT, 0, 127}, {"LEVEL", PK_GAIN, 0, 40}},
    {{"CUTOFF", PK_CUT, 0, 50}, {"RESO", PK_PCT, 0, 30}, {"FROM", PK_CUT, 0, 127}, {"SLOPE", PK_SLOPE, 2, 0}},
    {{"CENTER", PK_CUT, 0, 80}, {"WIDTH", PK_PCT, 0, 50}, {"FROM", PK_CUT, 0, 40}, {"GAIN", PK_GAIN, 0, 64}},
    {{"POSITION", PK_PAN, 0, 0}, {"RATE", PK_HZ, 0, 0}, {"DEPTH", PK_PCT, 0, 64}, {"SHAPE", PK_SHAPE, 3, 0}},
    {{"RATE", PK_HZ, 0, 30}, {"DEPTH", PK_PCT, 0, 80}, {"FEEDBACK", PK_PCT, 0, 60}, {"SPREAD", PK_PCT, 0, 60}},
    {{"TIME", PK_DTIME, 6, 32}, {"FEEDBACK", PK_PCT, 0, 60}, {"PING PONG", PK_ONOFF, 2, 96}, {"SPREAD", PK_PCT, 0, 100}},
    {{"DIV", PK_COUNT3, 3, 64}, {"DECAY", PK_PCT, 0, 100}, {"TONE", PK_PCT, 0, 127}, {"REVERSE", PK_ONOFF, 2, 0}},
    {{"SIZE", PK_PCT, 0, 26}, {"DAMP", PK_PCT, 0, 50}, {"PRE", PK_PRE, 0, 10}, {"WIDTH", PK_PCT, 0, 26}},      /* SIZE and WIDTH start at 20 % (26 of 127) */
    {{"DRIVE", PK_DRIVE, 0, 50}, {"FOLD", PK_PCT, 0, 64}, {"TONE", PK_PCT, 0, 127}, {"LEVEL", PK_GAIN, 0, 40}},
};
static const dbx_fxp_t DBX_FXP2[4] = {{"ATTACK", PK_ATK, 0, 0}, {"RELEASE", PK_REL, 0, 0}, {"CONTINUOUS", PK_ONOFF, 2, 0},
                                      {"MIX", PK_PCT, 0, 127}};
static const uint8_t DBX_FX_ATKREL[DBX_NFX][2] = {{10, 40}, {2, 16}, {2, 16}, {40, 50}, {40, 50}, {2, 10}, {10, 30}, {0, 40},
                                                  {4, 20}, {0, 60}, {2, 16}};
/* the mix a step of each effect starts at (SELECT moves it): the sends are placed at a good level, not full */
static const uint8_t DBX_FX_STEPMIX[DBX_NFX] = {100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100};   /* a step starts at full: the amount is the effect's MIX */
static const uint8_t DBX_FX_MIXDEF[DBX_NFX] = {127, 127, 127, 127, 127, 127, 127, 64, 127, 26, 127};      /* ... the delay's at 50 %, the reverb's at 20 % */
static const char *const DBX_DTIME_NAME[6] = {"1/16", "1/8", "3/16", "1/4", "3/8", "1/2"};
static const uint8_t DBX_DTIME_NUM[6] = {1, 2, 3, 4, 6, 8};                    /* in quarters of a beat */

static inline const dbx_fxp_t *dbx_fxp(uint32_t e, uint32_t i) { return i < 4u ? &DBX_FXP[e][i] : &DBX_FXP2[i - 4u]; }

static struct {
    uint8_t p[DBX_NFX][DBX_NFXP];          /* the parameters, 0..127 (a choice: its option as the middle of its share) */
    uint8_t master;                        /* FX MIX, GLO page 2: 0..100 % of every effect */
    int32_t env[DBX_NFX];                  /* the envelope now, Q16 */
    int32_t am[DBX_NFX];                   /* the amount the last block ended on, Q16 */
    int32_t up[DBX_NFX], dn[DBX_NFX];      /* the envelope's step in a block, going up (ATTACK) and down (RELEASE) */
} dbx_fx;

static void dbx_mst_init(void);                         /* (the master stage, at the end) */

/* ------------------------------------------------------------ parameters --- */
static inline uint32_t dbx_opt(uint32_t v, uint32_t n) { return v * n / 128u; }           /* the choice a value is in */
static inline int32_t dbx_tone_k(uint32_t v) { return 40 + (int32_t)(v * v * 4056u / 16129u); }   /* a one-pole's, Q12 */
static inline int32_t dbx_gain_q10(uint32_t v) { return (int32_t)v * 16; }                   /* GAIN: 64 = 1.0, up to ~2 */
static uint32_t dbx_atk_ms(uint32_t v) { return 1u + v * v / 40u; }                          /* 1 .. 404 ms */
static uint32_t dbx_rel_ms(uint32_t v) { return 5u + v * v / 8u; }                           /* 5 .. 2020 ms */

static void dbx_fx_rates(uint32_t e)                   /* ATTACK and RELEASE as the envelope's step in a block */
{
    uint32_t a = dbx_atk_ms(dbx_fx.p[e][FP_ATK]) * 441u / 10u, r = dbx_rel_ms(dbx_fx.p[e][FP_REL]) * 441u / 10u;
    dbx_fx.up[e] = (int32_t)(CTL * 65536u / (a < 1u ? 1u : a));
    dbx_fx.dn[e] = (int32_t)(CTL * 65536u / (r < 1u ? 1u : r));
}

static void dbx_fx_defaults(uint8_t (*p)[DBX_NFXP])        /* every effect's parameters as they start */
{
    uint32_t e, i;
    for (e = 0; e < DBX_NFX; e++) {
        for (i = 0; i < 4u; i++)
            p[e][i] = DBX_FXP[e][i].def;
        p[e][FP_ATK] = DBX_FX_ATKREL[e][0];
        p[e][FP_REL] = DBX_FX_ATKREL[e][1];
        p[e][FP_CONT] = 0;
        p[e][FP_MIX] = DBX_FX_MIXDEF[e];
    }
}

static void dbx_fx_init(void)
{
    uint32_t e;
    dbx_fx_defaults(dbx_fx.p);
    for (e = 0; e < DBX_NFX; e++) {
        dbx_fx_rates(e);
        dbx_fx.env[e] = dbx_fx.am[e] = 0;
    }
    dbx_fx.master = 100;
    dbx_mst_init();
}

/* clicks of a knob on parameter i of effect e: a range moves 2 a click, a choice one choice a click */
static void dbx_fx_step(uint32_t e, uint32_t i, int32_t clicks)
{
    const dbx_fxp_t *d = dbx_fxp(e, i);
    int32_t v = dbx_fx.p[e][i];
    if (d->opts) {
        int32_t k = (int32_t)dbx_opt((uint32_t)v, d->opts);
        k = clamp(k + clicks, 0, (int32_t)d->opts - 1);
        v = (k * 128 + 64) / (int32_t)d->opts;
    } else
        v = clamp(v + clicks * 2, 0, 127);
    dbx_fx.p[e][i] = (uint8_t)v;
    if (i == FP_ATK || i == FP_REL)
        dbx_fx_rates(e);
}

/* ---------------------------------------------------------------- helpers --- */
static inline int32_t dbx_c16(int32_t x) { return x < -32768 ? -32768 : x > 32767 ? 32767 : x; }
static inline int32_t dbx_c17(int32_t x) { return x < -131000 ? -131000 : x > 131000 ? 131000 : x; }
/* x and the wet y, a (Q16) of the way: x + (y - x) * a (|y - x| < 2^18) */
static inline int32_t dbx_xfade(int32_t x, int32_t y, int32_t a) { return x + ((((y - x) >> 2) * (a >> 4)) >> 10); }
static inline int32_t dbx_lp1(int32_t *st, int32_t x, int32_t k)            /* a one-pole, k Q12 */
{
    *st += ((x - *st) * k) >> 12;
    return *st;
}
/* The ceiling: full scale, 0 dBFS, and nothing over it. */
static inline int32_t dbx_hard(int32_t s) { return s < -32767 ? -32767 : s > 32767 ? 32767 : s; }

/* The soft limiter: linear up to half scale (-6 dBFS), a soft knee above that ends exactly on full scale. The knee is
 * softclip (tanh) over the top half of the range, stretched so that its end (the table stops at 31588 of 32767) lands on
 * 32767: 0 dBFS, never over it, and a slope of 1 where it starts, so there is no step in the sound. */
static inline int32_t dbx_limit(int32_t s)
{
    int32_t a = s < 0 ? -s : s;
    if (a > 16384) {
        a = 16384 + ((softclip(((a - 16384) * 63181) >> 15) * 16996) >> 15);
        return s < 0 ? -a : a;
    }
    return s;
}

/* a step's length in samples, and a beat's (a quarter note) */
static inline uint32_t dbx_step_samples(void) { return dbx_seq.step_q8 >> 8; }
static inline uint32_t dbx_beat_samples(void) { return dbx_step_samples() * dbx_seq.div / 4u; }

/* ------------------------------------------------------------ REPEAT --- */
/* A beat repeat, on the stereo sound. When the effect opens at a step it takes the next slice of the sound (the step
 * divided by DIV: 1, 2 or 3), that one goes through as it is, and then it is played over and over, each pass quieter by
 * DECAY, through TONE, forwards or REVERSE. With CONTINUOUS it takes a new slice at every step, so it stutters whatever
 * is playing. */
#define REP_LEN 12288u
static int16_t rep_hold[2][REP_LEN] __attribute__((section(".pool")));  /* the slice being repeated (up to 0.28 s) */
static struct { uint32_t len, pos, cap, steps; int32_t gain, lp[2]; uint8_t on, playing; } rep;

static void rep_begin(uint32_t div)                    /* start taking a slice */
{
    uint32_t sl = dbx_step_samples() / div;
    rep.len = sl < 32u ? 32u : sl > REP_LEN - 1u ? REP_LEN - 1u : sl;
    rep.cap = 0;
    rep.playing = 0;
}

static void fx_repeat(int32_t *l, int32_t *r, uint32_t n, int32_t a0, int32_t a1)
{
    const uint8_t *p = dbx_fx.p[FX_REPEAT];
    uint32_t i, div = 1u + dbx_opt(p[0], 3u), rev = p[3] >= 64u, steps = dbx_seq.steps_done;
    int32_t a = a0, da = (a1 - a0) / (int32_t)n, decay = 13107 + (int32_t)p[1] * 19660 / 127, k = dbx_tone_k(p[2]);
    if (p[FP_CONT] >= 64u && !dbx_seq.playing) {       /* CONTINUOUS is for the steps going by: stopped, it lets the sound be */
        rep.on = rep.playing = 0;
        return;
    }
    if (a1 > 0 && !rep.on) {                           /* opens: the slice from this step on */
        rep_begin(div);
        rep.on = 1;
        rep.steps = steps;
    } else if (rep.on && p[FP_CONT] >= 64u && rep.steps != steps) {               /* CONTINUOUS: a new slice at each step */
        rep_begin(div);
        rep.steps = steps;
    }
    if (!rep.on) {
        if (a0 == 0 && a1 == 0)
            rep.playing = 0;
        return;
    }
    for (i = 0; i < n; i++, a += da) {
        if (!rep.playing) {                            /* taking the slice: it goes through as it is */
            rep_hold[0][rep.cap] = (int16_t)dbx_c16(l[i]);
            rep_hold[1][rep.cap] = (int16_t)dbx_c16(r[i]);
            if (++rep.cap >= rep.len) {
                rep.playing = 1;
                rep.pos = 0;
                rep.gain = (32767 * decay) >> 15;      /* the first repeat is one pass quieter */
            }
        } else {
            uint32_t at = rev ? rep.len - 1u - rep.pos : rep.pos;
            int32_t yl = dbx_lp1(&rep.lp[0], (rep_hold[0][at] * rep.gain) >> 15, k);
            int32_t yr = dbx_lp1(&rep.lp[1], (rep_hold[1][at] * rep.gain) >> 15, k);
            if (++rep.pos >= rep.len) {
                rep.pos = 0;
                rep.gain = (rep.gain * decay) >> 15;
            }
            l[i] = dbx_xfade(l[i], yl, a);
            r[i] = dbx_xfade(r[i], yr, a);
        }
    }
    if (a0 == 0 && a1 == 0) {
        rep.on = 0;
        rep.playing = 0;
    }
}

/* ----------------------------------------------------------- HALFTIME --- */
/* A delay-line pitch shifter: two taps a half grain apart read the last grain at SPEED (1 down to a quarter), each
 * fading in and out over the grain, so it slows without a click. */
#define HALF_LEN 8192u
static int16_t half_buf[HALF_LEN] __attribute__((section(".pool")));
static struct { uint32_t w, ph; int32_t lp; } half;

static inline int32_t half_tap(uint32_t ph)            /* the sample `ph` (Q16, samples) behind the writing head */
{
    uint32_t lag = ph >> 16, f = (ph >> 8) & 255u, i0 = (half.w - 1u - lag) & (HALF_LEN - 1u), i1 = (i0 - 1u) & (HALF_LEN - 1u);
    return half_buf[i0] + (((half_buf[i1] - half_buf[i0]) * (int32_t)f) >> 8);
}

static void fx_half(int32_t *m, uint32_t n, int32_t a0, int32_t a1)
{
    const uint8_t *p = dbx_fx.p[FX_HALF];
    uint32_t i, inc = (uint32_t)p[0] * 49152u / 127u, W = 512u + (uint32_t)p[1] * 28u, Wq = W << 16;
    int32_t a = a0, da = (a1 - a0) / (int32_t)n, k = dbx_tone_k(p[2]), g = dbx_gain_q10(p[3]);
    for (i = 0; i < n; i++, a += da) {
        int32_t x = m[i];
        half_buf[half.w & (HALF_LEN - 1u)] = (int16_t)dbx_c16(x);
        if (a0 == 0 && a1 == 0) {
            half.ph = 0;
            half.w++;
            continue;
        }
        {
            uint32_t ph2 = half.ph + (Wq >> 1), x1;
            int32_t w1, w2, y;
            if (ph2 >= Wq)
                ph2 -= Wq;
            x1 = half.ph / W;                           /* the grain's phase, Q16 */
            w1 = (int32_t)(65536u - (x1 > 32768u ? 2u * x1 - 65536u : 65536u - 2u * x1)) >> 1;   /* a triangle, Q15 */
            w2 = 32768 - w1;
            y = (half_tap(half.ph) * w1 + half_tap(ph2) * w2) >> 15;
            half.w++;
            half.ph += inc;
            if (half.ph >= Wq)
                half.ph -= Wq;
            y = dbx_lp1(&half.lp, y, k);
            m[i] = dbx_xfade(x, dbx_c17((y * g) >> 10), a);
        }
    }
}

/* ----------------------------------------------------------- BITCRUSH --- */
static struct { int32_t held, lp, rng; uint32_t cnt; } crush = {0, 0, 0x2545F491, 0};

static void fx_crush(int32_t *m, uint32_t n, int32_t a0, int32_t a1)
{
    const uint8_t *p = dbx_fx.p[FX_CRUSH];
    uint32_t i, sh = (uint32_t)p[0] * 15u / 127u, hold = 1u + (uint32_t)p[1] * 31u / 127u;
    int32_t a = a0, da = (a1 - a0) / (int32_t)n, k = dbx_tone_k(p[3]), nz = (int32_t)p[2], half = (int32_t)((1u << sh) >> 1);
    if (a0 == 0 && a1 == 0) {
        crush.cnt = 0;
        return;
    }
    for (i = 0; i < n; i++, a += da) {
        int32_t x = m[i];
        if (crush.cnt == 0u) {                         /* a new held sample: dither, then the coarse steps */
            int32_t d = ((int32_t)noise32(&crush.rng) >> 16) * nz / 127;       /* +-32768 x NOISE */
            int32_t v = dbx_c16(x + ((d * (half ? half : 1)) >> 15));
            crush.held = ((v >> sh) << sh) + half;
            crush.cnt = hold;
        }
        crush.cnt--;
        m[i] = dbx_xfade(x, dbx_lp1(&crush.lp, crush.held, k), a);
    }
}

/* ---------------------------------------------------- SATURATE 1 and 2 --- */
static int32_t sat_lp[3];                                /* SATURATE 1's, then SATURATE 2's two sides */

static inline int32_t sat_drive_q8(uint32_t v) { return 256 + (int32_t)(v * v * 7936u / 16129u); }     /* x1 .. x32 */

static void fx_sat1(int32_t *m, uint32_t n, int32_t a0, int32_t a1)      /* a tube: tanh, a bias for the even harmonics */
{
    const uint8_t *p = dbx_fx.p[FX_SAT1];
    uint32_t i;
    int32_t a = a0, da = (a1 - a0) / (int32_t)n, g = sat_drive_q8(p[0]), bias = (int32_t)p[1] * 12000 / 127,
            sb = softclip(bias), k = dbx_tone_k(p[2]), lv = dbx_gain_q10(p[3]);
    if (a0 == 0 && a1 == 0)
        return;
    for (i = 0; i < n; i++, a += da) {
        int32_t x = m[i], y = softclip(((x * g) >> 8) + bias) - sb;
        y = dbx_lp1(&sat_lp[0], y, k);
        m[i] = dbx_xfade(x, (y * lv) >> 10, a);
    }
}

static void fx_sat2(int32_t *l, int32_t *r, uint32_t n, int32_t a0, int32_t a1)   /* hard: a clip, and past it the wave folds back */
{
    const uint8_t *p = dbx_fx.p[FX_SAT2];
    uint32_t i, ch;
    int32_t a = a0, da = (a1 - a0) / (int32_t)n, g = sat_drive_q8(p[0]), fold = p[1], k = dbx_tone_k(p[2]), lv = dbx_gain_q10(p[3]);
    int32_t *side[2];
    side[0] = l;
    side[1] = r;
    if (a0 == 0 && a1 == 0)
        return;
    for (i = 0; i < n; i++, a += da)
        for (ch = 0; ch < 2u; ch++) {
            int32_t x = side[ch][i], t = (x * g) >> 8, hc = t < -32767 ? -32767 : t > 32767 ? 32767 : t, u = (t + 32768) & 131071,
                    fo, y;
            fo = u < 65536 ? u - 32768 : 98304 - u;                                  /* the triangle fold */
            y = hc + (((fo - hc) * fold) >> 7);
            y = dbx_lp1(&sat_lp[1 + ch], y, k);
            side[ch][i] = dbx_xfade(x, (y * lv) >> 10, a);
        }
}

/* ------------------------------------------------- LOW PASS, BAND PASS --- */
static struct { int32_t ic[2][2][2]; } lpf;                 /* the channel, the pole pair, its two states */
static int32_t bpf[2][2];                                  /* the channel, its two states */

static void fx_lp(int32_t *l, int32_t *r, uint32_t n, int32_t a0, int32_t a1)
{
    const uint8_t *p = dbx_fx.p[FX_LP];
    uint32_t i, ch;
    int32_t am = (a0 + a1) >> 1, to = (int32_t)p[0] << 8, from = (int32_t)p[2] << 8, cut;
    tsvf_t c;
    if (a0 == 0 && a1 == 0) {
        for (ch = 0; ch < 2u; ch++)
            lpf.ic[ch][0][0] = lpf.ic[ch][0][1] = lpf.ic[ch][1][0] = lpf.ic[ch][1][1] = 0;
        return;
    }
    cut = from + ((((to - from) >> 4) * (am >> 4)) >> 8);                         /* the sweep: FROM -> CUTOFF */
    tsvf_coef(&c, cut, p[1]);
    for (ch = 0; ch < 2u; ch++) {
        int32_t *x = ch ? r : l;
        for (i = 0; i < n; i++) {
            int32_t y = tsvf_lp(&c, x[i], &lpf.ic[ch][0][0], &lpf.ic[ch][0][1]);
            if (dbx_opt(p[3], 2u))                                                /* 24 dB: a second pole pair */
                y = tsvf_lp(&c, y, &lpf.ic[ch][1][0], &lpf.ic[ch][1][1]);
            x[i] = dbx_c17(y);
        }
    }
}

static void fx_bp(int32_t *l, int32_t *r, uint32_t n, int32_t a0, int32_t a1)
{
    const uint8_t *p = dbx_fx.p[FX_BP];
    uint32_t i, ch;
    int32_t am = (a0 + a1) >> 1, to = (int32_t)p[0] << 8, from = (int32_t)p[2] << 8, cut, k = 600 + (int32_t)(p[1] * p[1] * 6000u / 16129u),
            da = (a1 - a0) / (int32_t)n, g = dbx_gain_q10(p[3]);
    tsvf_t c;
    if (a0 == 0 && a1 == 0) {
        bpf[0][0] = bpf[0][1] = bpf[1][0] = bpf[1][1] = 0;
        return;
    }
    cut = from + ((((to - from) >> 4) * (am >> 4)) >> 8);
    tsvf_coef_k(&c, cut, k);
    for (ch = 0; ch < 2u; ch++) {
        int32_t *m = ch ? r : l, a = a0;
        for (i = 0; i < n; i++, a += da) {
            int32_t x = m[i], y = (dv_bp(&c, x, bpf[ch]) * k) >> 12;
            y = dbx_c17((y * g) >> 10);
            m[i] = dbx_xfade(x, y, a);
        }
    }
}

/* ---------------------------------------------------------------- PAN --- */
static uint32_t pan_ph;

static void fx_pan(const int32_t *m, int32_t *l, int32_t *r, uint32_t n, int32_t a0, int32_t a1)
{
    const uint8_t *p = dbx_fx.p[FX_PAN];
    uint32_t i, inc = p[1] ? LFO_INC[p[1]] / CTL : 0u, shape = dbx_opt(p[3], 3u);
    int32_t a = a0, da = (a1 - a0) / (int32_t)n, base = ((int32_t)p[0] - 64) * 512, depth = p[2];
    for (i = 0; i < n; i++, a += da) {
        int32_t x = m[i], lfo = 0, pos, gl = 32768, gr = 32768;
        if (inc) {
            lfo = shape == 0u ? sine_i(pan_ph) : shape == 1u ? osc_tri(pan_ph) : (pan_ph >> 31 ? -32767 : 32767);
            pan_ph += inc;
        }
        pos = clamp(base + ((lfo * depth) >> 7), -32768, 32768);
        pos = (pos * (a >> 1)) >> 15;                                              /* the effect's amount */
        if (pos > 0)
            gl = 32768 - pos;
        else
            gr = 32768 + pos;
        l[i] = (x * (gl >> 3)) >> 12;                                              /* (x up to 2^17: stays in 32 bits) */
        r[i] = (x * (gr >> 3)) >> 12;
    }
}

/* ------------------------------------------------------------ FLANGER --- */
#define FL_LEN 1024u
static int16_t fl_buf[2][FL_LEN] __attribute__((section(".pool")));
static struct { uint32_t w, ph; } fl;

static inline int32_t fl_read(uint32_t ch, uint32_t ph, int32_t base_q8, int32_t span)
{
    int32_t u = osc_tri(ph) + 32768, dq = base_q8 + ((u * span) >> 8);                    /* Q8 samples back */
    uint32_t d = (uint32_t)dq >> 8, f = (uint32_t)dq & 255u, i0 = (fl.w - d) & (FL_LEN - 1u), i1 = (i0 - 1u) & (FL_LEN - 1u);
    return fl_buf[ch][i0] + (((fl_buf[ch][i1] - fl_buf[ch][i0]) * (int32_t)f) >> 8);
}

static void fx_flanger(int32_t *l, int32_t *r, uint32_t n, int32_t a0, int32_t a1)
{
    const uint8_t *p = dbx_fx.p[FX_FLANGE];
    uint32_t i, inc = LFO_INC[p[0]] / CTL, off = (uint32_t)p[3] << 24;
    int32_t a = a0, da = (a1 - a0) / (int32_t)n, span = (int32_t)p[1] * 264 / 127, base = 66 << 8, fb = (int32_t)p[2] * 24000 / 127;
    for (i = 0; i < n; i++, a += da) {
        int32_t xl = l[i], xr = r[i], wl, wr;
        if (a0 == 0 && a1 == 0) {
            fl_buf[0][fl.w & (FL_LEN - 1u)] = (int16_t)dbx_c16(xl);
            fl_buf[1][fl.w & (FL_LEN - 1u)] = (int16_t)dbx_c16(xr);
            fl.w++;
            continue;
        }
        wl = fl_read(0, fl.ph, base, span);
        wr = fl_read(1, fl.ph + off, base, span);
        fl_buf[0][fl.w & (FL_LEN - 1u)] = (int16_t)dbx_c16(xl + ((wl * fb) >> 15));
        fl_buf[1][fl.w & (FL_LEN - 1u)] = (int16_t)dbx_c16(xr + ((wr * fb) >> 15));
        fl.w++;
        fl.ph += inc;
        l[i] = dbx_c17(xl + ((((wl * (a >> 1)) >> 15) * 3) >> 2));
        r[i] = dbx_c17(xr + ((((wr * (a >> 1)) >> 15) * 3) >> 2));
    }
}

/* -------------------------------------------------------------- DELAY --- */
/* A tempo-synced stereo delay, written from nothing. The send (the effect's amount: the step's and the MIX) feeds the mono sum into a line; the echoes come back
 * at half the line's level and run on after the effect closes. PING PONG on: the echoes alternate left and right, SPREAD is how far apart; off: the same echo
 * on both sides, the right one SPREAD later (up to half a step). The feedback goes through a fixed gentle low-pass and a soft knee (a line this size cannot take
 * what a busy mix with feedback asks of it, and a hard clamp in a loop folds).
 * Its level: the send is scaled by sqrt((1 - FEEDBACK^2) (1 - FEEDBACK)): the echoes of a sound that does not repeat add the same energy whatever the feedback
 * (the first factor: more of it only makes them last), and those of a sound that repeats on the delay's own time (a kick on every step) pile up in step, which
 * the second factor takes back. The dry sound comes down a little as the send goes up (DLY_DRY, at most 1.5 dB), so the delay makes the whole no louder. Nothing here changes the gain of the whole
 * sound from block to block. The time glides (a tempo change bends the echoes, as a tape does) and is read with a linear interpolation: no clicks. */
#define DLY_LEN 49152u
#define DLY_TONE_K 2077                                /* the feedback's one-pole, Q12 (about half open) */
#define DLY_DRY 80                                     /* how far the dry comes down at the full send: 80 / 256 of the echoes' energy (Q8) */
static int16_t dly_buf[2][DLY_LEN] __attribute__((section(".pool")));      /* the line, at half the level: the chain above it runs to four times an int16 */
static struct { uint32_t w; int32_t dt[2], flp[2], act; } dly;               /* (dt: the delay in samples, Q8) */

static inline uint32_t dly_isqrt(uint32_t x)                                      /* the integer square root */
{
    uint32_t r = 0, b = 1u << 30;
    while (b > x)
        b >>= 2;
    while (b) {
        if (x >= r + b) {
            x -= r + b;
            r = (r >> 1) + b;
        } else
            r >>= 1;
        b >>= 2;
    }
    return r;
}

static inline int32_t dly_read(const int16_t *b, uint32_t w, int32_t dq)   /* the line as it was dq / 256 samples ago, at full level */
{
    uint32_t di = (uint32_t)dq >> 8, i0 = w >= di ? w - di : w + DLY_LEN - di, i1 = i0 == 0u ? DLY_LEN - 1u : i0 - 1u;
    int32_t fr = dq & 255;
    return ((b[i0] * (256 - fr) + b[i1] * fr) >> 8) * 2;
}

static void fx_delay(int32_t *l, int32_t *r, uint32_t n, int32_t a0, int32_t a1)
{
    const uint8_t *p = dbx_fx.p[FX_DELAY];
    uint32_t i, ch, opt = dbx_opt(p[0], 6u), tgt = dbx_beat_samples() * DBX_DTIME_NUM[opt] / 4u, pk = 0, pong = p[2] >= 64u;
    int32_t a = a0, da = (a1 - a0) / (int32_t)n, fb = (int32_t)p[1] * 29000 / 127, sp = p[3], tq[2], f2 = (fb * fb) >> 15, nf, ae;
    if (a0 == 0 && a1 == 0 && dly.act < 8)
        return;
    nf = (int32_t)dly_isqrt((uint32_t)(((32768 - f2) * (32768 - fb)) >> 15) << 15);   /* sqrt((1 - fb^2) (1 - fb)), Q15 */
    if (tgt > DLY_LEN - 2u - 4096u)
        tgt = DLY_LEN - 2u - 4096u;
    if (tgt < 64u)
        tgt = 64u;
    tq[0] = (int32_t)tgt << 8;
    tq[1] = ((int32_t)tgt + (pong ? 0 : (int32_t)(dbx_step_samples() / 2u) * sp / 127)) << 8;   /* off: the right side later */
    for (ch = 0; ch < 2u; ch++)
        if (dly.dt[ch] == 0)
            dly.dt[ch] = tq[ch];
    for (i = 0; i < n; i++, a += da) {
        int32_t x = l[i], y = r[i], rl, rr, fl_, fr_, wl, wr, ol, or_, in, d, w2;
        for (ch = 0; ch < 2u; ch++) {                                                  /* the time glides to its target (and gets there) */
            int32_t e = tq[ch] - dly.dt[ch];
            dly.dt[ch] += (e + (e >= 0 ? 1023 : -1023)) / 1024;
        }
        rl = dly_read(dly_buf[0], dly.w, dly.dt[0]);
        rr = dly_read(dly_buf[1], dly.w, dly.dt[1]);
        fl_ = dbx_lp1(&dly.flp[0], rl, DLY_TONE_K);
        fr_ = dbx_lp1(&dly.flp[1], rr, DLY_TONE_K);
        ae = ((a >> 4) * nf) >> 15;                                                   /* the send, Q12 */
        in = ((((x + y) >> 1) >> 3) * ae) >> 9;                                       /* (the mean of the two sides, at the send) */
        if (pong) {                                                                   /* the input in the left line, each echo into the other */
            wl = in + ((fr_ * fb) >> 15);
            wr = (fl_ * fb) >> 15;
            ol = ((rl + rr) >> 1) + (((rl - rr) >> 1) * sp) / 127;                    /* SPREAD: from the middle to apart */
            or_ = ((rl + rr) >> 1) - (((rl - rr) >> 1) * sp) / 127;
        } else {
            wl = in + ((fl_ * fb) >> 15);
            wr = in + ((fr_ * fb) >> 15);
            ol = rl;
            or_ = rr;
        }
        dly_buf[0][dly.w] = (int16_t)dbx_hard(dbx_limit((wl + 1) >> 1));
        dly_buf[1][dly.w] = (int16_t)dbx_hard(dbx_limit((wr + 1) >> 1));
        if (++dly.w >= DLY_LEN)
            dly.w = 0;
        w2 = ((a >> 4) * (a >> 4)) >> 12;                                             /* the send squared, Q12 */
        d = 4096 - ((w2 * DLY_DRY) >> 8);                                             /* the dry's gain, Q12 */
        l[i] = ((x >> 3) * d >> 9) + (ol >> 1);                                       /* (|x| up to 2^17: >> 3, x 2^12, >> 9 keeps 32 bits) */
        r[i] = ((y >> 3) * d >> 9) + (or_ >> 1);
        if ((uint32_t)(rl < 0 ? -rl : rl) > pk)
            pk = (uint32_t)(rl < 0 ? -rl : rl);
        if ((uint32_t)(rr < 0 ? -rr : rr) > pk)
            pk = (uint32_t)(rr < 0 ? -rr : rr);
    }
    dly.act = (int32_t)pk;
}

/* ------------------------------------------------------------- REVERB --- */
/* a stereo Freeverb: four combs and two allpasses a side, SIZE sets the feedback, DAMP darkens it, PRE delays it */
#define RV_MAXC 1400u
#define RV_PRE 4410u
static const uint16_t RV_C[2][4] = {{1116, 1188, 1277, 1356}, {1139, 1211, 1300, 1379}};
static const uint16_t RV_A[2][2] = {{556, 441}, {579, 464}};
static int16_t rv_cb[2][4][RV_MAXC] __attribute__((section(".pool")));
static int16_t rv_ab[2][2][600] __attribute__((section(".pool")));
static int16_t rv_pre[RV_PRE] __attribute__((section(".pool")));
static struct { uint16_t ci[2][4], ai[2][2]; int32_t cl[2][4]; uint32_t pw; int32_t act; } rv;

static void fx_reverb(int32_t *l, int32_t *r, uint32_t n, int32_t a0, int32_t a1)
{
    const uint8_t *p = dbx_fx.p[FX_REVERB];
    uint32_t i, ch, c, pre = (uint32_t)p[2] * (RV_PRE - 1u) / 127u, pk = 0;
    int32_t a = a0, da = (a1 - a0) / (int32_t)n, fb = 22937 + (int32_t)p[0] * 9175 / 127, ko = 4096 - (int32_t)p[1] * 3600 / 127,
            wd = p[3];
    if (a0 == 0 && a1 == 0 && rv.act < 8)
        return;
    for (i = 0; i < n; i++, a += da) {
        int32_t in = (((l[i] + r[i]) >> 3) * (a >> 1)) >> 13, o[2];
        rv_pre[rv.pw] = (int16_t)dbx_c16(in);
        in = rv_pre[(rv.pw + RV_PRE - pre) % RV_PRE] >> 2;
        if (++rv.pw >= RV_PRE)
            rv.pw = 0;
        for (ch = 0; ch < 2u; ch++) {
            int32_t sum = 0;
            for (c = 0; c < 4u; c++) {
                int32_t y = rv_cb[ch][c][rv.ci[ch][c]];
                rv.cl[ch][c] += ((y - rv.cl[ch][c]) * ko) >> 12;
                rv_cb[ch][c][rv.ci[ch][c]] = (int16_t)dbx_c16(in + ((rv.cl[ch][c] * fb) >> 15));
                if (++rv.ci[ch][c] >= RV_C[ch][c])
                    rv.ci[ch][c] = 0;
                sum += y;
            }
            for (c = 0; c < 2u; c++) {                                              /* the allpasses */
                int32_t b = rv_ab[ch][c][rv.ai[ch][c]], y = b - sum;
                rv_ab[ch][c][rv.ai[ch][c]] = (int16_t)dbx_c16(sum + (b >> 1));
                if (++rv.ai[ch][c] >= RV_A[ch][c])
                    rv.ai[ch][c] = 0;
                sum = y;
            }
            o[ch] = sum;
        }
        {   /* the width: mid and side */
            int32_t mid = (o[0] + o[1]) >> 1, side = ((o[0] - o[1]) >> 1) * wd / 127;
            l[i] += mid + side;
            r[i] += mid - side;
            if ((uint32_t)(mid < 0 ? -mid : mid) > pk)
                pk = (uint32_t)(mid < 0 ? -mid : mid);
        }
    }
    rv.act = (int32_t)pk;
}

/* ----------------------------------------------------- the master stage --- */
/* The last stage, on the SCL page: HPF (off at the left), LPF (off at the right), OTT (a three-band compressor that
 * also lifts what is quiet, KNOB 3 its dry / wet) and a soft limiter / clipper (KNOB 4 is the gain into it). Every
 * knob at its end leaves the sound as it was. */
static struct {
    uint8_t p[4];                          /* HPF 0 = off, LPF 127 = off, OTT 0 = dry, CLIP 0 = 0 dB */
    int32_t clip_q8;                       /* CLIP as a gain, Q8 */
    int32_t ic[2][4][2];                   /* the filters' states per side: HPF, LPF, OTT low, OTT high */
    int32_t env[3], gain[3];               /* OTT: each band's level now, and the gain it ended the last block on, Q12 */
    uint32_t lo_idx, hi_idx;               /* OTT's two crossover points as cutoff indexes */
} dbx_mst = {{0, 127, 0, 0}, 256, {{{0}}}, {0, 0, 0}, {4096, 4096, 4096}, 0, 0};

static const uint16_t DBX_CLIP_DB[19] = {256, 287, 322, 362, 406, 455, 511, 573, 643, 721, 810, 908, 1019, 1144, 1283, 1440, 1615, 1812, 2033};   /* 0 .. 18 dB, Q8 */

static inline uint32_t dbx_hpf_idx(uint32_t v) { return 8u + (v - 1u) * 72u / 126u; }          /* HPF 1..127 -> 40 Hz .. 2 kHz */
static inline uint32_t dbx_lpf_idx(uint32_t v) { return 40u + v * 70u / 126u; }                /* LPF 0..126 -> 180 Hz .. 12 kHz */

static void dbx_mst_changed(void)
{
    uint32_t v = (uint32_t)dbx_mst.p[3] * 18u * 256u / 127u, i = v >> 8, f = v & 255u;      /* dB, with 8 bits of fraction */
    int32_t a = DBX_CLIP_DB[i > 17u ? 17u : i], b = DBX_CLIP_DB[i > 17u ? 18u : i + 1u];
    dbx_mst.clip_q8 = a + (((b - a) * (int32_t)f) >> 8);
}

static void dbx_mst_reset(void)                         /* the master page back to its state 0: HPF and LPF off, OTT dry, no soft clip */
{
    dbx_mst.p[0] = 0;
    dbx_mst.p[1] = 127;
    dbx_mst.p[2] = 0;
    dbx_mst.p[3] = 0;
    dbx_mst_changed();
}

static void dbx_mst_init(void)
{
    uint32_t i;
    dbx_mst.p[0] = 0;
    dbx_mst.p[1] = 127;
    dbx_mst.p[2] = 0;
    dbx_mst.p[3] = 0;
    for (i = 0; i < 128u; i++) {
        if (!dbx_mst.lo_idx && CUTOFF_HZ[i] >= 120u)
            dbx_mst.lo_idx = i;
        if (!dbx_mst.hi_idx && CUTOFF_HZ[i] >= 2500u)
            dbx_mst.hi_idx = i;
    }
    dbx_mst_changed();
}

/* one side of the HPF: the input minus the band and the low-pass of a state variable filter (damping kq, Q12) */
static inline int32_t dbx_svf_hp(const tsvf_t *c, int32_t kq, int32_t x, int32_t *ic)
{
    int32_t v3 = x - ic[1], v1 = (c->a1 * ic[0] + c->a2 * v3) >> 13, v2 = ic[1] + ((c->a2 * ic[0] + c->a3 * v3) >> 13);
    ic[0] = clamp(2 * v1 - ic[0], -150000, 150000);
    ic[1] = clamp(2 * v2 - ic[1], -150000, 150000);
    return x - ((kq * v1) >> 12) - v2;
}

static void dbx_master_process(int32_t *l, int32_t *r, uint32_t n)
{
    int32_t *ch[2];
    uint32_t i, s, b;
    ch[0] = l;
    ch[1] = r;
    if (dbx_mst.p[0]) {                                                       /* HPF */
        tsvf_t c;
        int32_t kq = 8192 - 10 * 7600 / 127;
        tsvf_coef_k(&c, (int32_t)dbx_hpf_idx(dbx_mst.p[0]) << 8, kq);
        for (s = 0; s < 2u; s++)
            for (i = 0; i < n; i++)
                ch[s][i] = dbx_c17(dbx_svf_hp(&c, kq, ch[s][i], dbx_mst.ic[s][0]));
    }
    if (dbx_mst.p[1] < 127u) {                                                /* LPF */
        tsvf_t c;
        tsvf_coef(&c, (int32_t)dbx_lpf_idx(dbx_mst.p[1]) << 8, 10);
        for (s = 0; s < 2u; s++)
            for (i = 0; i < n; i++)
                ch[s][i] = dbx_c17(tsvf_lp(&c, ch[s][i], &dbx_mst.ic[s][1][0], &dbx_mst.ic[s][1][1]));
    }
    if (dbx_mst.p[2]) {                                                       /* OTT: three bands, each pulled towards a level */
        static const int32_t TARGET[3] = {6000, 5000, 3500};
        static const int32_t KREL[3] = {25, 37, 74};                          /* per block, Q12: 120, 80 and 40 ms */
        tsvf_t cl, ch_;
        int32_t band[2][3][CTL], peak[3] = {0, 0, 0}, g0[3], g1[3], mix = (int32_t)dbx_mst.p[2] * 4096 / 127;
        tsvf_coef(&cl, (int32_t)dbx_mst.lo_idx << 8, 0);
        tsvf_coef(&ch_, (int32_t)dbx_mst.hi_idx << 8, 0);
        for (s = 0; s < 2u; s++)
            for (i = 0; i < n; i++) {
                int32_t x = ch[s][i], lo = tsvf_lp(&cl, x, &dbx_mst.ic[s][2][0], &dbx_mst.ic[s][2][1]);
                int32_t hi = x - tsvf_lp(&ch_, x, &dbx_mst.ic[s][3][0], &dbx_mst.ic[s][3][1]);
                band[s][0][i] = lo;
                band[s][1][i] = x - lo - hi;
                band[s][2][i] = hi;
                for (b = 0; b < 3u; b++) {
                    int32_t a = band[s][b][i] < 0 ? -band[s][b][i] : band[s][b][i];
                    if (a > peak[b])
                        peak[b] = a;
                }
            }
        for (b = 0; b < 3u; b++) {                                            /* the level of each band, then its gain */
            int32_t k = peak[b] > dbx_mst.env[b] ? 1365 : KREL[b], tg;
            dbx_mst.env[b] += ((peak[b] - dbx_mst.env[b]) * k) >> 12;
            tg = 4096 + (((TARGET[b] << 12) / (dbx_mst.env[b] + 400) - 4096) * 3 >> 2);        /* 3/4 of the way to the level */
            g0[b] = dbx_mst.gain[b];
            g1[b] = clamp(tg, 1024, 24576);
            dbx_mst.gain[b] = g1[b];
        }
        for (s = 0; s < 2u; s++)
            for (i = 0; i < n; i++) {
                int32_t w = 0, x = ch[s][i];
                for (b = 0; b < 3u; b++)
                    w += (band[s][b][i] >> 2) * (g0[b] + (((g1[b] - g0[b]) * (int32_t)i) / (int32_t)n)) >> 10;
                ch[s][i] = dbx_xfade(x, dbx_c17(w), mix * 16);
            }
    } else {
        dbx_mst.env[0] = dbx_mst.env[1] = dbx_mst.env[2] = 0;
        dbx_mst.gain[0] = dbx_mst.gain[1] = dbx_mst.gain[2] = 4096;
    }
}

/* ---------------------------------------------------------- the chain --- */
/* the envelope of every effect for this block: its gate (the step that is on, or CONTINUOUS), the step's mix, the
 * attack going up and the release going down; the amount is that x the effect's MIX x the global FX MIX */
static void dbx_fx_amounts(int32_t *a0, int32_t *a1)
{
    uint32_t e, cur = dbx_seq.cur, playing = dbx_seq.playing;
    const dbx_fxpat_t *fp = DBX_FXV((uint32_t)dbx_seq.cp + DBX_NPAT * dbx_seq.cs);       /* (the effects' steps of the section playing: the second one's too) */
    for (e = 0; e < DBX_NFX; e++) {
        int32_t tgt = 0, env = dbx_fx.env[e];
        if (dbx_fx.p[e][FP_CONT] >= 64u)
            tgt = 65536;
        else if (playing && ((fp->on[e] >> cur) & 1u))
            tgt = (int32_t)fp->mix[e][cur] * 65536 / 100;
        if (tgt > env)
            env = env + dbx_fx.up[e] > tgt ? tgt : env + dbx_fx.up[e];
        else
            env = env - dbx_fx.dn[e] < tgt ? tgt : env - dbx_fx.dn[e];
        dbx_fx.env[e] = env;
        a0[e] = dbx_fx.am[e];
        a1[e] = dbx_fx.am[e] = (int32_t)(((uint32_t)env * dbx_fx.p[e][FP_MIX] / 127u) * dbx_fx.master / 100u);
    }
}

static void dbx_lfo_volume(int32_t *m, int32_t *s, uint32_t n);                 /* dbx_lfo.c: LFO 1 ahead of the chain, LFO 2 where it turns stereo */
static void dbx_lfo_pan(int32_t *l, int32_t *r, uint32_t n);

/* one block: the drums' mono sum in, the stereo out (both limited to the output's soft knee) */
static void dbx_fx_process(const int32_t *mix, const int32_t *side, int32_t *l, int32_t *r, uint32_t n)
{
    int32_t m[CTL], sd[CTL], a0[DBX_NFX], a1[DBX_NFX], bl[CTL], br[CTL];
    uint32_t i, duck = 1;
    dbx_fx_amounts(a0, a1);
    for (i = 0; i < n; i++) {
        m[i] = dbx_limit(mix[i]);
        sd[i] = side[i];
    }
    dbx_lfo_volume(m, sd, n);                           /* LFO 1: the volume, before every effect (of the middle and of what the lanes' pans add) */
    fx_half(m, n, a0[FX_HALF], a1[FX_HALF]);
    fx_crush(m, n, a0[FX_CRUSH], a1[FX_CRUSH]);
    fx_sat1(m, n, a0[FX_SAT1], a1[FX_SAT1]);
    if (a0[FX_PAN] == 0 && a1[FX_PAN] == 0)
        for (i = 0; i < n; i++)
            l[i] = r[i] = m[i];
    else
        fx_pan(m, l, r, n, a0[FX_PAN], a1[FX_PAN]);
    for (i = 0; i < n; i++) {                           /* the lanes' pans: what the mono effects did not take rejoins (left = middle + side, right = middle - side) */
        l[i] += sd[i];
        r[i] -= sd[i];
    }
    dbx_lfo_pan(l, r, n);                               /* LFO 2: the pan, before the effects that are stereo */
    if (dbx_env.p[EV_TGT] != EVT_DRY && (dbx_env.g0 != 4096 || dbx_env.g1 != 4096))
        for (i = 0; i < n; i++) {                       /* ENV: the wet sound of the next four is ducked: what they add to the sound, below */
            bl[i] = l[i];
            br[i] = r[i];
        }
    else
        duck = 0;
    fx_flanger(l, r, n, a0[FX_FLANGE], a1[FX_FLANGE]);
    fx_delay(l, r, n, a0[FX_DELAY], a1[FX_DELAY]);
    fx_repeat(l, r, n, a0[FX_REPEAT], a1[FX_REPEAT]);
    fx_reverb(l, r, n, a0[FX_REVERB], a1[FX_REVERB]);
    if (duck)
        for (i = 0; i < n; i++) {
            int32_t g = dbx_env.g0 + (dbx_env.g1 - dbx_env.g0) * (int32_t)(i + 1u) / (int32_t)n;
            l[i] = bl[i] + ((((l[i] - bl[i]) >> 3) * g) >> 9);        /* (|wet| up to 2^18: >> 3, x 2^12, >> 9 keeps 32 bits) */
            r[i] = br[i] + ((((r[i] - br[i]) >> 3) * g) >> 9);
        }
    fx_sat2(l, r, n, a0[FX_SAT2], a1[FX_SAT2]);
    fx_lp(l, r, n, a0[FX_LP], a1[FX_LP]);               /* the filters last: they shape the whole sound, the echoes and tails too */
    fx_bp(l, r, n, a0[FX_BP], a1[FX_BP]);
}

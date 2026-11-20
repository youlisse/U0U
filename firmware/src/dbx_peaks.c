/* SPDX-License-Identifier: GPL-3.0-only */
/* PEAKS: the drum models of Mutable Instruments Peaks (peaks/drums: bass_drum, snare_drum, high_hat, fm_drum, excitation and svf),
 * Copyright 2013 Emilie Gillet, MIT licence (the notice is in CREDITS.md), ported from C++ to C and to U0U's 44.1 kHz and lane parameters.
 * The models are integer already (Peaks runs on a chip with no floating point), which is why they fit here:
 *   BD  an 808-style bass drum: three pulses (a push, a pull 1 ms later, and a 4 ms chirp of the pitch) hit a resonant band-pass that rings at the
 *       pitch; the "punch" bends the filter with its own level; a low-pass takes the top off
 *   SD  an 808-style snare: two resonators a fifth-ish apart (the note and an octave over) struck by pulses, and noise through a band-pass
 *   HH  an 808-style hi-hat: six square waves into a band-pass, only the positive half passes (the 808's VCA), an envelope, a high-pass
 *   FM  a sine whose pitch falls with an envelope (FM), under an amplitude envelope, with its own output fed back into the pitch, and a noise or an overdrive
 * A lane's TUNE moves the pitch, DECAY the ring, TONE what each model calls its tone (BD the low-pass, SD the balance of the two bodies, HH the band,
 * FM the depth of the fall), EXTRA its punch (BD), snappy (SD), the noise or overdrive (FM; 64: neither). The tables: tools/gen_peaks.py. */
#include "dbx_peaks_tab.h"

enum { PKM_BD, PKM_SD, PKM_HH, PKM_FM };

#define PK_CLIP(x) ((x) < -32768 ? -32768 : (x) > 32767 ? 32767 : (x))
#define PK_MUL(a, b) ((int32_t)((uint32_t)(a) * (uint32_t)(b)))      /* (the low 32 bits of a product: what the chip it came from does when it overflows) */

typedef struct { uint32_t delay, decay; int32_t counter, state, level; } pk_exc_t;           /* a pulse that decays (Excitation) */
typedef struct { int32_t lp, bp, f, damp, punch, freq, res; uint8_t dirty; } pk_svf_t;       /* a state-variable filter (Svf) */

typedef struct {
    uint8_t model, live, trig, cnt;
    uint32_t quiet, rng;
    pk_exc_t e[4];
    pk_svf_t v[3];
    int32_t lp;                            /* BD: the low-pass; FM: the last sample */
    uint32_t ph[6];                        /* HH: the six squares; FM: the phase, the three envelopes' phases and the phase increment */
} pk_t;

static void pk_exc_set(pk_exc_t *e, uint32_t delay, uint32_t decay) { e->delay = delay; e->decay = decay; }
static inline void pk_exc_trig(pk_exc_t *e, int32_t level) { e->level = level; e->counter = (int32_t)e->delay + 1; }
static inline int32_t pk_exc(pk_exc_t *e)
{
    e->state = (int32_t)(((uint32_t)e->state * e->decay) >> 12);
    if (e->counter > 0 && --e->counter == 0)
        e->state += e->level < 0 ? -e->level : e->level;
    return e->level < 0 ? -e->state : e->state;
}
static inline int32_t pk_i824(const int16_t *t, int32_t x)       /* a table of 257 by a value of 15 bits (the note, or the resonance) */
{
    int32_t i = x >> 7, a = t[i], b = t[i + 1];
    return a + (((b - a) * ((x & 127) << 9)) >> 16);
}
static inline int32_t pk_i824u(const uint16_t *t, int32_t x)
{
    int32_t i = x >> 7, a = t[i], b = t[i + 1];
    return a + (((b - a) * ((x & 127) << 9)) >> 16);
}

static void pk_svf_init(pk_svf_t *s) { s->lp = s->bp = s->punch = 0; s->freq = 33 << 7; s->res = 16384; s->dirty = 1; }
static inline void pk_svf_freq(pk_svf_t *s, int32_t f) { s->dirty |= s->freq != f; s->freq = f; }
static inline void pk_svf_res(pk_svf_t *s, int32_t r) { s->res = r; s->dirty = 1; }
static inline int32_t pk_svf(pk_svf_t *s, int32_t in, uint32_t mode)   /* mode 0: low-pass, 1: band-pass, 2: high-pass */
{
    int32_t f, damp, notch, hp;
    if (s->dirty) {
        s->f = pk_i824(PK_SVF_F, s->freq < 0 ? 0 : s->freq > 32767 ? 32767 : s->freq);
        s->damp = pk_i824u(PK_SVF_D, s->res < 0 ? 0 : s->res > 32767 ? 32767 : s->res);
        s->dirty = 0;
    }
    f = s->f;
    damp = s->damp;
    if (s->punch) {
        int32_t ps = s->lp > 4096 ? s->lp : 2048;
        f += ((ps >> 4) * s->punch) >> 9;
        damp += (ps - 2048) >> 3;
    }
    notch = in - (PK_MUL(s->bp, damp) >> 15);
    s->lp += PK_MUL(f, s->bp) >> 15;
    s->lp = PK_CLIP(s->lp);
    hp = notch - s->lp;
    s->bp += PK_MUL(f, hp) >> 15;
    s->bp = PK_CLIP(s->bp);
    return mode == 1u ? s->bp : mode == 2u ? hp : s->lp;
}
static inline int32_t pk_noise(pk_t *s) { s->rng = s->rng * 1664525u + 1013904223u; return (int32_t)(int16_t)(s->rng >> 16); }

static void pk_init(pk_t *s, uint32_t model)
{
    uint32_t i;
    for (i = 0; i < sizeof *s / 4u; i++)
        ((uint32_t *)s)[i] = 0;
    s->model = (uint8_t)model;
    s->rng = 0x2545F491u + model;
    for (i = 0; i < 3u; i++)
        pk_svf_init(&s->v[i]);
    switch (model) {
    case PKM_BD:
        pk_exc_set(&s->e[0], 0, 3340);                          /* the push */
        pk_exc_set(&s->e[1], 44, 3072);                         /* the pull, 1 ms later */
        pk_exc_set(&s->e[2], 176, 4093);                        /* the chirp, 4 ms */
        break;
    case PKM_SD:
        pk_exc_set(&s->e[0], 0, 1536);
        pk_exc_set(&s->e[1], 44, 3072);
        pk_exc_set(&s->e[2], 44, 1200);
        pk_exc_set(&s->e[3], 0, 4093);
        pk_svf_res(&s->v[2], 2000);
        break;
    case PKM_HH:
        pk_svf_freq(&s->v[0], 105 << 7);
        pk_svf_res(&s->v[0], 24000);
        pk_svf_freq(&s->v[1], 110 << 7);
        pk_svf_res(&s->v[1], 0);
        pk_exc_set(&s->e[0], 0, 0);
        break;
    default:                                                    /* FM: the envelopes start at their end */
        s->ph[1] = s->ph[2] = s->ph[3] = 0xFFFFFFFFu;
        break;
    }
}

/* the parameters of one block: note16 the pitch (1/16 semitone, TUNE and the hit's own included), decay tone extra 0..127 */
typedef struct { int32_t note16, decay, tone, extra, p3, p4; } pk_par_t;     /* (p3, p4: the lane's second page, 0..15, 8 as Peaks has it) */

static void pk_trigger(pk_t *s, const pk_par_t *p)
{
    uint32_t sn;
    switch (s->model) {
    case PKM_BD:
        pk_exc_trig(&s->e[0], 27525 * (p->p4 + 2));                /* the push: CLICK (8: Peaks' 275251) */
        pk_exc_trig(&s->e[1], -13763);
        pk_exc_trig(&s->e[2], 18000);
        break;
    case PKM_SD:
        pk_exc_trig(&s->e[0], 15 * 32768);
        pk_exc_trig(&s->e[1], -32768);
        pk_exc_trig(&s->e[2], 13107);
        sn = (uint32_t)p->extra * 516u >> 1;                    /* snappy */
        pk_exc_trig(&s->e[3], 512 + (int32_t)(sn >= 28672u ? 28672u : sn));
        break;
    case PKM_HH:
        s->e[0].state = 30720 << 4;                             /* the envelope, Q4 (Peaks' 15 x 32768 >> 4) */
        break;
    default:
        s->ph[0] = 0;                                           /* the envelopes start (the phase of Peaks' 0x3fff x depth >> 16 is nothing) */
        s->ph[1] = s->ph[2] = s->ph[3] = 0;
        break;
    }
    s->live = 1;
    s->quiet = 0;
}

static inline uint32_t pk_env_inc(uint32_t decay)
{
    uint32_t a = PK_ENV_INC[decay >> 8], b = PK_ENV_INC[(decay >> 8) + 1];
    return a - (((a - b) * (decay & 0xFFu)) >> 8);
}
static inline uint32_t pk_osc_inc(int32_t pitch)
{
    int32_t ref;
    uint32_t a, b, inc, shifts = 0;
    if (pitch >= 16384)
        pitch = 16383;
    ref = pitch - 116 * 128;
    while (ref < 0) {
        ref += 12 * 128;
        shifts++;
    }
    a = PK_OSC_INC[ref >> 4];
    b = PK_OSC_INC[(ref >> 4) + 1];
    inc = a + (uint32_t)(((int32_t)(b - a) * (ref & 15)) >> 4);
    return shifts > 31u ? 0u : inc >> shifts;
}
static inline int32_t pk_sine(uint32_t phase)                    /* Interpolate1022 */
{
    uint32_t i = phase >> 22;
    int32_t a = PK_SINE[i], b = PK_SINE[i + 1];
    return a + (((b - a) * (int32_t)((phase >> 6) & 0xFFFFu)) >> 16);
}
static inline int32_t pk_overdrive(uint32_t phase)
{
    uint32_t i = phase >> 22;
    int32_t a = PK_OVERDRIVE[i], b = PK_OVERDRIVE[i + 1];
    return a + (((b - a) * (int32_t)((phase >> 6) & 0xFFFFu)) >> 16);
}
static inline uint32_t pk_expo(uint32_t phase)                   /* the exponential curve's table at a 32-bit phase: 65535 - it is the envelope (Interpolate824) */
{
    uint32_t i = phase >> 24;
    int32_t a = PK_ENV_EXPO[i], b = PK_ENV_EXPO[i + 1];
    return (uint32_t)(a + (((b - a) * (int32_t)((phase >> 8) & 0xFFFFu)) >> 16));
}
static inline int32_t pk_mix(int32_t a, int32_t b, int32_t bal) { return (a * (65535 - bal) + b * bal) >> 16; }

/* a block (n <= CTL) of the model into y (int16 range): the caller scales it */
static void pk_run(pk_t *s, const pk_par_t *p, int32_t *y, uint32_t n)
{
    uint32_t k;
    if (s->trig) {
        s->trig = 0;
        pk_trigger(s, p);
    }
    if (!s->live) {
        for (k = 0; k < n; k++)
            y[k] = 0;
        return;
    }
    {
        int32_t note = p->note16 * 8;                            /* 1/128 semitone */
        uint32_t d16 = (uint32_t)p->decay * 516u, t16 = (uint32_t)p->tone * 516u, x16 = (uint32_t)p->extra * 516u;
        int32_t g1 = 0, g2 = 0, lpc = 0, fm_amt = 0, noise_a = 0, od = 0, strength = 0;
        uint32_t am_inc = 0, fm_inc = 0;
        if (s->model == PKM_BD) {
            uint32_t sc = 65535u - d16, sq = sc * sc >> 16, pu;
            sc = sq * sc >> 18;
            pk_svf_res(&s->v[0], 32768 - 128 - (int32_t)sc);
            lpc = 512 + (int32_t)((t16 * t16 >> 16) >> 2) * 3;
            pu = (x16 * x16 >> 16);
            s->v[0].punch = (int32_t)(pu * pu >> 24);
        } else if (s->model == PKM_SD) {
            int32_t base = note;
            pk_svf_freq(&s->v[0], base);
            pk_svf_freq(&s->v[1], base + ((4 + p->p3) << 7));                              /* the second body: BODY (8: an octave over) */
            pk_svf_freq(&s->v[2], base + ((36 + p->p4 * 3 / 2) << 7));                     /* the noise's band: NOISE (8: 48 semitones over) */
            pk_svf_res(&s->v[0], 29000 + (int32_t)(d16 >> 5));
            pk_svf_res(&s->v[1], 26500 + (int32_t)(d16 >> 5));
            s->e[3].decay = 4092u + (d16 >> 14);
            g1 = 22000 - (int32_t)(t16 >> 2);
            g2 = 22000 + (int32_t)(t16 >> 2);
        } else if (s->model == PKM_HH) {
            uint32_t tau = 150u << (p->decay >> 4);                                      /* DECAY: the envelope's time constant, 150 samples (3 ms) to 38000 (0.9 s) */
            tau += tau * (uint32_t)(p->decay & 15) >> 4;
            pk_svf_freq(&s->v[0], (105 << 7) + (int32_t)(t16 >> 5) - 1024);              /* TONE moves the band by +-8 semitones */
            pk_svf_res(&s->v[0], 16000 + p->p4 * 1000);                                  /* METAL: how tonal the band is (8: Peaks' 24000) */
            pk_svf_freq(&s->v[1], (100 + p->p3 * 5 / 4) << 7);                           /* BRIGHT: the high-pass (8: note 110) */
            s->e[0].decay = (1u << 20) / tau;                                            /* (Q20 of the fall a sample) */
        } else {
            int32_t fl = note - (24 << 7);
            uint32_t f = fl < 0 ? 0u : (uint32_t)fl * 65536u / (72u << 7);
            f = f > 65535u ? 65535u : f;
            strength = f <= 16384u ? 1024 : f <= 32768u ? 2048 - (int32_t)(f >> 4) : 0;
            strength = strength * p->p4 / 8;                                         /* SWEEP (8: as Peaks) */
            fm_amt = (int32_t)(((t16 >> 2) * 3u) >> 2);
            am_inc = pk_env_inc(16384u + (d16 >> 1));
            fm_inc = pk_env_inc(8192u + (d16 >> 2));
            if (x16 >= 32768u)
                noise_a = (int32_t)(((((x16 - 32768u) * (x16 - 32768u)) >> 15) >> 2) * 5u);
            else
                od = (int32_t)(((32767u - x16) * (32767u - x16)) >> 14);
        }
        for (k = 0; k < n; k++) {
            int32_t o = 0;
            switch (s->model) {
            case PKM_BD: {
                int32_t ex = pk_exc(&s->e[0]), res, fq;
                ex += s->e[1].counter != 0 ? 16384 : 0;
                ex += pk_exc(&s->e[1]);
                pk_exc(&s->e[2]);
                fq = note + (s->e[2].counter == 0 ? 0 : (p->p3 * 2 + 1) << 7);     /* the chirp: CHIRP semitones (8: Peaks' 17) */
                pk_svf_freq(&s->v[0], fq);
                res = (ex >> 4) + pk_svf(&s->v[0], ex, 1u);
                s->lp += PK_MUL(res - s->lp, lpc) >> 15;
                o = PK_CLIP(s->lp);
                break;
            }
            case PKM_SD: {
                int32_t e1 = pk_exc(&s->e[0]), e2, b1, b2, nz, ne;
                e1 += pk_exc(&s->e[1]);
                e1 += s->e[1].counter != 0 ? 2621 : 0;
                b1 = pk_svf(&s->v[0], e1, 1u) + (e1 >> 4);
                e2 = pk_exc(&s->e[2]);
                e2 += s->e[2].counter != 0 ? 13107 : 0;
                b2 = pk_svf(&s->v[1], e2, 1u) + (e2 >> 4);
                nz = pk_svf(&s->v[2], pk_noise(s), 1u);
                ne = pk_exc(&s->e[3]);
                o = (b1 * g1 >> 15) + (b2 * g2 >> 15) + (ne * nz >> 15);
                o = PK_CLIP(o);
                break;
            }
            case PKM_HH: {
                int32_t nz, fn, vca, hh = 0;
                uint32_t dl;
                s->ph[0] += 52591436u; s->ph[1] += 77913239u; s->ph[2] += 40320100u;
                s->ph[3] += 59116669u; s->ph[4] += 72069746u; s->ph[5] += 102261125u;
                nz = (int32_t)(s->ph[0] >> 31) + (int32_t)(s->ph[1] >> 31) + (int32_t)(s->ph[2] >> 31) + (int32_t)(s->ph[3] >> 31) +
                     (int32_t)(s->ph[4] >> 31) + (int32_t)(s->ph[5] >> 31);
                nz <<= 12;
                fn = pk_svf(&s->v[0], nz, 1u);
                fn += pk_svf(&s->v[0], nz, 1u);          /* (the filter runs twice a sample, as Peaks' does, for its stability) */
                fn = fn < 0 ? 0 : fn > 32767 ? 32767 : fn;       /* the 808's VCA passes only the positive half */
                dl = ((uint32_t)s->e[0].state * s->e[0].decay >> 20) + 1u;
                s->e[0].state = (uint32_t)s->e[0].state > dl ? s->e[0].state - (int32_t)dl : 0;
                vca = ((s->e[0].state >> 4) * fn) >> 14;
                vca = PK_CLIP(vca);
                hh += pk_svf(&s->v[1], vca, 2u);
                hh += pk_svf(&s->v[1], vca, 2u);
                hh <<= 1;
                o = PK_CLIP(hh);
                break;
            }
            default: {
                uint32_t am_env;
                int32_t mix;
                s->ph[1] += fm_inc;
                if (s->ph[1] < fm_inc)
                    s->ph[1] = 0xFFFFFFFFu;
                s->ph[3] += 4473924u * 48000u / 44100u;
                if (s->ph[3] < 4473924u)
                    s->ph[3] = 0xFFFFFFFFu;
                if ((s->cnt++ & 3u) == 0) {                       /* the pitch, every fourth sample */
                    uint32_t ax = 65535u - pk_expo(s->ph[3]), fx = 65535u - pk_expo(s->ph[1]);
                    s->ph[4] = pk_osc_inc(note + (int32_t)(fx * (uint32_t)fm_amt >> 16) + (int32_t)(ax * (uint32_t)strength >> 15) + ((s->lp * p->p3) >> 9));            /* FEEDBK: the output back into the pitch (8: Peaks' >> 6) */
                }
                s->ph[0] += s->ph[4];
                mix = pk_sine(s->ph[0]);
                if (noise_a)
                    mix = pk_mix(mix, pk_noise(s), noise_a);
                s->ph[2] += am_inc;
                if (s->ph[2] < am_inc)
                    s->ph[2] = 0xFFFFFFFFu;
                am_env = 65535u - pk_expo(s->ph[2]);
                mix = mix * (int32_t)am_env >> 16;
                if (od)
                    mix = pk_mix(mix, pk_overdrive(((uint32_t)mix << 16) + (1u << 31)), od);
                s->lp = mix;
                o = mix;
                break;
            }
            }
            y[k] = o;
        }
    }
    {   /* silence: the voice rests after a tenth of a second of nothing (a bass drum rings long, and its zero crossings are short) */
        int32_t big = 0;
        for (k = 0; k < n; k++)
            big |= (y[k] < 0 ? -y[k] : y[k]) > 24;
        s->quiet = big ? 0u : s->quiet + n;
        if (s->quiet > 4410u && s->e[0].counter == 0)
            s->live = 0;
    }
}

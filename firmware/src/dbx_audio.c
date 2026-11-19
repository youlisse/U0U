/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (audio.c, from which this is built) */
/* DrumBox audio: the I2S half buffers and the audio ISR. Per half, blocks of CTL samples:
 * dbx_mix_block (step clock -> 11 drum lanes -> sum -> step effects (LFO 1 at the head, LFO 2 where it turns stereo) -> HPF, LPF, OTT -> clipper -> master) -> 24-bit stereo.
 * registers: bsp/boards/fm1/fm1_audio.h */
#define HALF_WORDS (HALF_FRAMES * 2u)
#define OUT_SHIFT 8                        /* Q15 -> 24-bit: 32767 is 0 dBFS, the ceiling of dbx_limit (Felucca's audio.c kept it
                                            * at -6 dBFS, 7) */

static int32_t abuf[2u * HALF_WORDS] __attribute__((aligned(4)));
#ifndef DBX_ISR_ENTER                      /* the FM-1 build (drumbox.c) tells TIMER5 that the render runs (USB waits) */
#define DBX_ISR_ENTER()
#define DBX_ISR_LEAVE()
#endif

/* The oscilloscope's samples (dbx_ui.c dbx_draw_scope): the left channel of what goes out, one in four, the last 512 (46 ms). */
#define SCOPE_N 512u
static int16_t scope_buf[SCOPE_N];
static uint32_t scope_w;

static struct {
    uint32_t master_q12;                   /* the MASTER pot: 4096 = unity */
    uint32_t vol_q12;                      /* the global volume (GLO + KNOB 2): 4096 = unity, at 100 of 0..127 */
    int32_t g_cur;                         /* the gain now: glides to master x vol, so a click of a knob is not heard */
    uint32_t over;                         /* the samples that went past 0 dBFS before the ceiling since it was last read (the FM-1's load line) */
    uint32_t halves;
} dbx_audio = {2048, 4092, 2048 * 4092 / 4096, 0};

/* The output limiter, with a block of lookahead (CTL samples, 0.7 ms): nothing goes past -0.2 dBFS and nothing is clipped. The finished block waits one block; the
 * gain for it comes down to what the block after it needs (C / its peak, the two channels together) by the time that block starts, in a straight ramp, and
 * goes back up over about 90 ms. Under the ceiling the gain is 1 and the sound is as it was (one block later); over it, the sound is turned down for a moment
 * (the loud mixes of a high VOLUME or MASTER, and what a delay adds to them) instead of being cut flat. */
#define DBX_LIM_CEIL 32000
static struct { int32_t l[CTL], r[CTL], req, g; } dbx_lim = {{0}, {0}, 4096, 4096};

static uint32_t dbx_vol_q12(uint32_t v) { return (v * v * 419u) >> 10; }     /* v 0..127: square law, 100 = unity */
static inline uint32_t dbx_master_of_pot(uint32_t k10) { return (k10 * k10 * 181u) >> 16; }   /* the MASTER pot, a square law that tops at -3 dB (2890 of 4096): a loop that sits near 0 dBFS gets 3 dB of room for what the effects add (a delay adds 1 to 4 dB), as fx.c master_of_pot (which tops at unity) */

static void dbx_mix_block(int32_t *out, uint32_t n)       /* n <= CTL stereo frames, Q15 */
{
    int32_t mix[CTL], side[CTL], ds[CTL], rs[CTL], l[CTL], r[CTL], nl[CTL], nr[CTL], pk = 0, req, ge, gs = dbx_lim.g, gq, dg;
    int32_t gt = (int32_t)((dbx_audio.master_q12 * dbx_audio.vol_q12) >> 12), d = gt - dbx_audio.g_cur, g;
    uint32_t i;
    dbx_audio.g_cur += d > -16 && d < 16 ? d : d >> 4;        /* (a glide of ~12 ms, and exactly there at the end) */
    g = dbx_audio.g_cur;
    dbx_seq_block(n);                                   /* the step clock: the steps starting in this block hit */
    for (i = 0; i < n; i++)
        mix[i] = side[i] = ds[i] = rs[i] = 0;
    dbx_voices_render(mix, side, ds, rs, n);
    dbx_lfo_tick(n);                                    /* the LFOs run on; the effects chain takes LFO 1 at its head and LFO 2 where it is stereo */
    dbx_fx_process(mix, side, ds, rs, l, r, n);                       /* the step effects: mono sum in, stereo out */
    dbx_master_process(l, r, n);                        /* the SCL page: HPF, LPF, OTT */
    for (i = 0; i < n; i++) {
        int32_t cl = l[i], cr = r[i], a;
        if (dbx_mst.p[3]) {                             /* the soft clipper (CLIP is the gain into it); at 0 it is off */
            cl = dbx_limit((cl * dbx_mst.clip_q8) >> 8);
            cr = dbx_limit((cr * dbx_mst.clip_q8) >> 8);
        }
        cl = ((cl >> 3) * g) >> 9;                             /* the master gain ... */
        cr = ((cr >> 3) * g) >> 9;                             /* (a lane or an effect up to 2^17 x 6600 / 8 fits 32 bits) */
        a = cl < 0 ? -cl : cl;
        if (cr > a || -cr > a)
            a = cr < 0 ? -cr : cr;
        pk = a > pk ? a : pk;
        if (a > 32767)
            dbx_audio.over++;                                  /* (counted: the help's third sheet shows it: the samples the limiter had to turn down) */
        nl[i] = cl;
        nr[i] = cr;
    }
    req = pk > DBX_LIM_CEIL ? DBX_LIM_CEIL * 4096 / pk : 4096;     /* ... then the limiter: this block's need, and the one waiting */
    ge = req < dbx_lim.req ? req : dbx_lim.req;
    if (ge >= gs) {
        int32_t up = gs + ((4096 - gs + 127) >> 7);            /* (back up over about 90 ms) */
        ge = ge < up ? ge : up;
    }
    dg = ((ge - gs) << 8) / (int32_t)n;
    gq = gs << 8;
    for (i = 0; i < n; i++) {
        gq += dg;
        out[2u * i] = dbx_hard((dbx_lim.l[i] * (gq >> 8)) >> 12);
        out[2u * i + 1u] = dbx_hard((dbx_lim.r[i] * (gq >> 8)) >> 12);
        dbx_lim.l[i] = nl[i];
        dbx_lim.r[i] = nr[i];
        if (!(i & 3u))
            scope_buf[scope_w++ & (SCOPE_N - 1u)] = (int16_t)out[2u * i];
    }
    dbx_lim.g = ge;
    dbx_lim.req = req;
}

static void audio_block(int32_t *out, uint32_t n)         /* mix, then Q15 -> 24 bit */
{
    uint32_t i;
    dbx_mix_block(out, n);
    for (i = 0; i < n; i++) {
        out[2u * i] *= 1 << OUT_SHIFT;
        out[2u * i + 1u] *= 1 << OUT_SHIFT;
    }
}

void fm1_alnk0_irq(void)                       /* via isr_alnk0 (bsp/chip/fm1_isr.S) */
{
    uint8_t p;
    DBX_ISR_ENTER();
    p = fm1_audio_pending();
    fm1_audio_ack_aux(p);
    if (p & FM1_AUDIO_HALF) {
        uint32_t half = fm1_audio_free_half(), b;
        int32_t *o = &abuf[half * HALF_WORDS];
        for (b = 0; b < HALF_FRAMES; b += CTL)
            audio_block(o + 2u * b, CTL);
        fm1_audio_ack_half();
        dbx_audio.halves++;
    }
    DBX_ISR_LEAVE();
}
extern void isr_alnk0(void);

static void audio_init(void)                   /* codec and ALNK0 bring-up (fm1_audio.h) */
{
    uint32_t i;
    for (i = 0; i < 2u * HALF_WORDS; i++)
        abuf[i] = 0;
    fm1_audio_init(abuf, HALF_WORDS, isr_alnk0, 3);
}

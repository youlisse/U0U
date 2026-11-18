/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (seq.c, whose step clock this follows) */
/* DrumBox sequencer: one pattern of 11 lanes x 16 steps, a transport and a step clock.
 * The clock runs in the audio ISR once per CTL block (dbx_mix_block): a step length in Q8 samples and a fractional
 * carry, so the tempo never drifts. The UI sets the flags and the pattern (single writer: 8 and 16-bit writes); the ISR
 * is the only one that moves the step and the queue.
 * Subdivision (the step): 1/8, 1/16 or 1/32 note, so 16 steps span 8, 4 or 2 beats.
 * Clock: our own (BPM), or the computer's MIDI clock (24 pulses a beat; a step is 96 / DIV pulses). The MIDI side
 * only counts pulses (dbx_midi_realtime); the ISR takes them at the next block.
 * The pattern bank: 16 patterns of 11 lanes x 16 steps. One is edited (pe), one plays (pp); while playing, a pattern picked
 * to play waits (pq) for the end of the pass. The kit (the lanes' sound) and the tempo are shared by all of them.
 * SWING (global, 0 to 100) delays every second step (the odd ones: 2nd, 4th ...) by up to half a step, on any subdivision.
 * Each step has, besides its velocity: NUDGE (late / early, 1/48 of a step a notch, half a step each way), SUBDIVISION (1 to
 * 8 hits spread over the step), CHANCE (0 to 100 %: the share of passes where the step plays) and PITCH (semitones).
 * Because a nudge can put a hit before its step, the steps are scheduled one step ahead: at each step boundary the next
 * step's hits are queued, each with the time it plays at (relative to the block start). The queue is also how a step
 * is previewed (dbx_preview: a request the ISR takes). */
#define DBX_BPM_MIN 30u
#define DBX_BPM_MAX 300u
#define DBX_NUDGE_MAX 24                   /* a notch is 1/48 of a step: 24 = half a step */
#define DBX_RATCHET_MAX 8u
#define DBX_PITCH_MAX 12
static void dbx_snap_sync(void);           /* (dbx_snap.c: the sound of the pattern that plays) */
static void dbx_snap_copy(uint32_t src, uint32_t dst);
static void dbx_snap_remove(uint32_t p);
static void dbx_lfo_sync(uint32_t pos);    /* (... and at every step, their cycles are set to where the steps are) */
static void dbx_lfo_restart(void);         /* (dbx_lfo.c: the sequencer started, the LFOs start their cycles) */
#define DBX_NPEND 64u
static const uint8_t DBX_DIV[3] = {8, 16, 32};     /* the subdivisions, slowest first */

typedef struct { int32_t t_q8; uint8_t lane, vel, used; int8_t semi; dbx_hk_t k; } dbx_pend_t;     /* a hit waiting to play */

#define DBX_NPAT 16u
#define DBX_NFX 11u                        /* the step effects: one per black key (dbx_fx.c) */
typedef struct {                           /* a pattern's effect steps: 198 bytes (all 16 are one flash object, dbx_store.c) */
    uint16_t on[DBX_NFX];                  /* bit s: the effect is on at step s */
    uint8_t mix[DBX_NFX][DBX_NSTEP];       /* how much of it, 0..100 % (SELECT on the FX page) */
} dbx_fxpat_t;
typedef struct {                           /* a pattern: 902 bytes (stored 4 to a flash object) */
    uint16_t steps[DBX_NLANE];             /* bit s: the lane hits on step s */
    uint8_t vel[DBX_NLANE][DBX_NSTEP];     /* its velocity, 1..127 (SELECT edits the step just placed) */
    int8_t nudge[DBX_NLANE][DBX_NSTEP];    /* -24 (early) .. +24 (late), 1/48 step a notch */
    uint8_t ratchet[DBX_NLANE][DBX_NSTEP]; /* 1..8 hits in the step */
    uint8_t chance[DBX_NLANE][DBX_NSTEP];  /* 0..100 %: how often it plays */
    int8_t semi[DBX_NLANE][DBX_NSTEP];     /* -12..+12 semitones from the lane's TUNE */
} dbx_pat_t;

static struct {
    dbx_pat_t pat[DBX_NPAT];
    dbx_fxpat_t fx[DBX_NPAT];              /* and its effects' steps */
    uint8_t pe;                            /* the pattern the UI shows and edits */
    volatile uint8_t pp;                   /* the pattern that plays */
    volatile uint8_t pq;                   /* the one that plays from the next pass (255: none) */
    uint16_t bpm;                          /* the tempo: ours, or measured from the computer's clock */
    uint8_t div;                           /* 8, 16 or 32: the step is a 1/div note */
    uint8_t ext;                           /* 1: follow the computer's MIDI clock */
    uint8_t swing;                         /* 0..100: every second step comes late, up to half a step */
    volatile uint8_t playing;
    volatile uint8_t start_req;            /* UI -> ISR: restart from step 0 */
    volatile uint8_t cur;                  /* the step playing (ISR); meaningful while playing */
    volatile uint32_t steps_done;          /* the steps begun since power-on (ISR): the effects count them */
    uint8_t first, running;                /* the first step after a start; the transport was running (ISR) */
    volatile uint8_t rep_req;              /* UI -> ISR: SEQ is held: loop the last beat */
    volatile uint8_t rep_r;                /* the loop's own subdivision: 0 two beats, 1 a beat, 2 half a beat, 3 a quarter */
    uint8_t rep, rep_s, rep_l, rep_i;      /* the loop (ISR): on, its first step, its length, the index of the step now in it */
    uint32_t pos;                          /* the steps begun since the start (ISR): the LFOs keep to it */
    volatile uint32_t pass;                /* the passes begun since power-on (ISR; the loops of SEQ do not count): the mutation counts them */
    uint8_t nxq, tl;                       /* the step queued to start at the next boundary (what sounds); the step the pattern
                                            * itself would be on (the loop does not stop it) */
    uint32_t acc_q8, step_q8;              /* own clock: samples (Q8) into the step, and a step's length */
    volatile uint32_t ext_rx;              /* MIDI clock pulses received (MIDI side) ... */
    uint32_t ext_seen;                     /* ... and taken (ISR) */
    uint8_t ext_acc;                       /* pulses into the current step */
    uint32_t pl_per, pl_v, pl_off;         /* the clock locked to the computer's (see dbx_midi_realtime): a pulse's length in samples (Q8), the pulses it has
                                            * counted, and the ones of them it let by (what it fired is pl_v - pl_off) */
    int32_t pl_acc;                        /* ... the samples (Q8) since its last pulse */
    uint8_t pl_lock, pl_hold, pl_skip, pl_chg;   /* it fires the steps (else the computer's own pulses do); pulses it lets by to fall back in step; the computer's
                                            * pulses to let by after it let go; beats the tempo has moved on */
    uint32_t bt_ms[5], m_n, last_ms;       /* the tempo measure: when the last beats began (a ring), the pulses counted since the clock began, the last pulse */
    uint32_t rnd;                          /* CHANCE's random numbers */
    volatile uint8_t pv_req, pv_pat, pv_lane, pv_step;   /* UI -> ISR: play this step now (a preview) */
    volatile uint16_t flush_mask;          /* UI -> ISR: drop the queued hits of these lanes (bit i) */
    volatile uint16_t lv_req;              /* UI -> ISR: play these lanes now (bit i), as REC does for the black keys */
    dbx_pend_t pend[DBX_NPEND];
} dbx_seq;
#define DBX_E (dbx_seq.pat[dbx_seq.pe])    /* the pattern the UI works on */

/* a step's own DECAY and TONE (EDIT 2), each a signed nibble (-8..7, a unit is 8 of the lane's own 0..127) per lane and step, 88 bytes a parameter a pattern;
 * 16 patterns are 2816 bytes, one flash object (DBXL) */
/* the SONG: a chain of up to 16 slots, each a pattern played REPEAT passes (1, 2, 4 or 8) before the next; the end is the first slot with no pattern (255);
 * ON: PLAY starts from the first slot and the end of each pass takes the song on (a pattern picked while it plays comes in for one pass first) */
static struct { uint8_t pat[DBX_NPAT], rep[DBX_NPAT], again[DBX_NPAT], on, cur, done, sel; } dbx_song = {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {0}, {0}, 0, 0, 0, 0};
static uint32_t dbx_song_len(void)
{
    uint32_t n = 0;
    while (n < DBX_NPAT && dbx_song.pat[n] < DBX_NPAT)
        n++;
    return n;
}
static uint32_t dbx_song_active(void) { return dbx_song.on && dbx_song_len(); }    /* the song plays what it plans: no pattern is picked to play */
#define DBX_L_MAGIC 0x4C584244u                /* "DBXL" */
static struct { uint32_t magic; uint8_t d[DBX_NPAT][2][88]; } dbx_lk;
/* ... and its own EXTRA and PAN (DBXK, the same way: 2816 bytes, one more flash object, OBJ_AUTOSAVE's pair, free in this firmware); a unit is 8, 8, 8 and 4 */
#define DBX_K_MAGIC 0x4B584244u                /* "DBXK" */
static struct { uint32_t magic; uint8_t d[DBX_NPAT][2][88]; } dbx_lk2;
static const uint8_t DBX_LK_UNIT[4] = {8, 8, 8, 4};      /* the unit of which: 0 DECAY, 1 TONE, 2 EXTRA, 3 PAN */
static uint8_t *dbx_lk_cell(uint32_t pat, uint32_t which, uint32_t n) { return which < 2u ? &dbx_lk.d[pat][which][n >> 1] : &dbx_lk2.d[pat][which - 2u][n >> 1]; }
static int32_t dbx_lk_get(uint32_t pat, uint32_t which, uint32_t lane_i, uint32_t step)
{
    uint32_t n = lane_i * DBX_NSTEP + step;
    int32_t v = (*dbx_lk_cell(pat, which, n) >> ((n & 1u) * 4u)) & 15;
    return v >= 8 ? v - 16 : v;
}
static void dbx_lk_set(uint32_t pat, uint32_t which, uint32_t lane_i, uint32_t step, int32_t v)
{
    uint32_t n = lane_i * DBX_NSTEP + step, sh = (n & 1u) * 4u;
    uint8_t *b = dbx_lk_cell(pat, which, n);
    v = v < -8 ? -8 : v > 7 ? 7 : v;
    *b = (uint8_t)((*b & ~(15u << sh)) | (((uint32_t)v & 15u) << sh));
}

/* what the flash holds is behind what is in memory: the settings (the kit, the tempo, the pattern shown), the four
 * pattern objects (bit k: patterns 4k..4k+3) and the effects (bit 4: their parameters and every pattern's steps), saved a
 * moment after the last change, never while playing */
static struct {
    uint32_t t_ms, saved_ms;               /* the last change; the last save */
    uint8_t set, pats;
    uint8_t now;                           /* SAVE held: write what waits at once, playing or not */
} dbx_store;
static void dbx_touch_set(void) { dbx_store.set = 1; dbx_store.pats |= 0x40u; dbx_store.t_ms = fm1_ms; }
static void dbx_touch_x(void) { dbx_store.pats |= 0x40u; dbx_store.t_ms = fm1_ms; }          /* (ENV: it is in the patterns' sounds) */
static void dbx_touch_lk(void) { dbx_store.pats |= 0x20u; dbx_store.t_ms = fm1_ms; }         /* (the steps' own DECAY and TONE: their object) */
static void dbx_touch_lk2(void) { dbx_store.pats |= 0x80u; dbx_store.t_ms = fm1_ms; }        /* (the steps' own EXTRA and PAN: theirs) */
static void dbx_touch_fx(void) { dbx_store.pats |= 0x50u; dbx_store.t_ms = fm1_ms; }       /* (0x10 the effects' object, 0x40 the patterns' sounds) */
static void dbx_touch_pat(uint32_t p) { dbx_store.pats |= (uint8_t)(1u << (p / 4u)); dbx_store.t_ms = fm1_ms; }

/* a step as it is placed */
static void dbx_step_defaults(dbx_pat_t *p, uint32_t i, uint32_t s)
{
    p->vel[i][s] = DBX_VEL_DEF;
    p->nudge[i][s] = 0;
    p->ratchet[i][s] = 1;
    p->chance[i][s] = 100;
    p->semi[i][s] = 0;
    dbx_lk_set((uint32_t)(p - dbx_seq.pat), 0, i, s, 0);
    dbx_lk_set((uint32_t)(p - dbx_seq.pat), 1, i, s, 0);
    dbx_lk_set((uint32_t)(p - dbx_seq.pat), 2, i, s, 0);
    dbx_lk_set((uint32_t)(p - dbx_seq.pat), 3, i, s, 0);
}

/* a step length in samples (Q8) from the tempo: FS * 60 * 4 / (BPM * DIV) (FS * 60 * 256 * 4 fits 32 bits) */
static void dbx_seq_tempo(void)
{
    dbx_seq.step_q8 = (uint32_t)FS * 60u * 256u * 4u / (dbx_seq.bpm * dbx_seq.div);
}

/* a pattern with nothing in it: no step, no effect step, every step as it is placed (and its own DECAY, TONE, EXTRA, PAN at 0) */
static void dbx_pat_empty(uint32_t p)
{
    uint32_t i, k;
    for (i = 0; i < DBX_NFX; i++) {
        dbx_seq.fx[p].on[i] = 0;
        for (k = 0; k < DBX_NSTEP; k++)
            dbx_seq.fx[p].mix[i][k] = 100;
    }
    for (i = 0; i < DBX_NLANE; i++) {
        dbx_seq.pat[p].steps[i] = 0;     /* empty patterns: the user's to make */
        for (k = 0; k < DBX_NSTEP; k++)
            dbx_step_defaults(&dbx_seq.pat[p], i, k);
    }
}

static void dbx_seq_init(void)
{
    uint32_t p;
    dbx_seq.bpm = 120;
    dbx_seq.div = 16;
    dbx_seq.ext = 0;
    dbx_seq.swing = 0;
    dbx_seq.pe = dbx_seq.pp = 0;
    dbx_seq.pq = 255;
    dbx_seq.rep_r = 1;
    dbx_seq.rnd = 0x2545F491u;
    dbx_seq_tempo();
    for (p = 0; p < DBX_NPAT; p++)
        dbx_pat_empty(p);
}

static uint32_t dbx_pat_used(uint32_t p)    /* the pattern has a step */
{
    uint32_t i, any = 0;
    for (i = 0; i < DBX_NLANE; i++)
        any |= dbx_seq.pat[p].steps[i];
    for (i = 0; i < DBX_NFX; i++)
        any |= dbx_seq.fx[p].on[i];
    return any != 0u;
}

/* the pattern to show and edit; it plays at once when stopped, and from the next pass when playing (picking the one
 * that plays again takes the wait back). With the song on (and a slot in it) the song plays what it plans, and what is picked is only the one
 * shown and edited (stopped, its sound is the live one, as ever, until PLAY starts the song) */
static void dbx_pat_select(uint32_t p)
{
    if (p >= DBX_NPAT)
        return;
    dbx_seq.pe = (uint8_t)p;
    if (dbx_seq.playing && dbx_song_active())
        ;
    else if (dbx_seq.playing)
        dbx_seq.pq = p == dbx_seq.pp ? 255u : (uint8_t)p;
    else {
        dbx_seq.pp = (uint8_t)p;
        dbx_seq.pq = 255;
    }
    dbx_touch_set();
}

static void dbx_pat_copy(uint32_t src, uint32_t dst)
{
    if (src >= DBX_NPAT || dst >= DBX_NPAT || src == dst)
        return;
    dbx_seq.pat[dst] = dbx_seq.pat[src];
    dbx_seq.fx[dst] = dbx_seq.fx[src];
    for (uint32_t w = 0; w < 2u; w++)
        for (uint32_t b = 0; b < 88u; b++)
            dbx_lk.d[dst][w][b] = dbx_lk.d[src][w][b];
    for (uint32_t w = 0; w < 2u; w++)
        for (uint32_t b = 0; b < 88u; b++)
            dbx_lk2.d[dst][w][b] = dbx_lk2.d[src][w][b];
    dbx_touch_lk();
    dbx_touch_lk2();
    dbx_snap_copy(src, dst);
    dbx_touch_pat(dst);
    dbx_touch_fx();
}

static void dbx_seq_flush(uint32_t mask);

/* a pattern deleted: the ones after it move down by one and the 16th is a new one; the song loses its slots of it and its numbers follow;
 * the one shown stays at its number (it now shows the next). What plays follows its pattern (the one deleted: the next, as its place is taken) */
static void dbx_pat_delete(uint32_t p)
{
    uint32_t q, w, b;
    if (p >= DBX_NPAT)
        return;
    if (p == dbx_seq.pp)
        dbx_seq_flush(0xFFFFu);                          /* (what was queued of it does not play) */
    if (dbx_seq.pp > p && dbx_seq.pp < DBX_NPAT)
        dbx_seq.pp--;
    if (dbx_seq.pq > p && dbx_seq.pq < DBX_NPAT)
        dbx_seq.pq--;
    if (dbx_seq.pq == dbx_seq.pp)
        dbx_seq.pq = 255;
    for (q = p; q + 1u < DBX_NPAT; q++) {
        dbx_seq.pat[q] = dbx_seq.pat[q + 1u];
        dbx_seq.fx[q] = dbx_seq.fx[q + 1u];
        for (w = 0; w < 2u; w++)
            for (b = 0; b < 88u; b++) {
                dbx_lk.d[q][w][b] = dbx_lk.d[q + 1u][w][b];
                dbx_lk2.d[q][w][b] = dbx_lk2.d[q + 1u][w][b];
            }
    }
    dbx_pat_empty(DBX_NPAT - 1u);
    for (q = w = 0; q < DBX_NPAT; q++) {                 /* the song: its slots of this pattern go, the later patterns' numbers go down */
        uint8_t sp = dbx_song.pat[q];
        if (sp == p)
            continue;
        dbx_song.pat[w] = sp > p && sp < DBX_NPAT ? (uint8_t)(sp - 1u) : sp;
        dbx_song.rep[w] = dbx_song.rep[q];
        dbx_song.again[w] = dbx_song.again[q];
        w++;
    }
    for (; w < DBX_NPAT; w++) {
        dbx_song.pat[w] = 255;
        dbx_song.rep[w] = dbx_song.again[w] = 0;
    }
    dbx_snap_remove(p);
    for (q = p; q < DBX_NPAT; q++)
        dbx_touch_pat(q);
    dbx_touch_lk();
    dbx_touch_lk2();
    dbx_touch_fx();
}

static void dbx_seq_play(uint32_t on)
{
    if (on) {
        dbx_seq.start_req = 1;
        dbx_seq.playing = 1;
    } else
        dbx_seq.playing = 0;
}

static void dbx_seq_set_bpm(int32_t bpm)
{
    if (dbx_seq.ext)
        return;                            /* the computer's clock sets it */
    dbx_seq.bpm = (uint16_t)clamp(bpm, (int32_t)DBX_BPM_MIN, (int32_t)DBX_BPM_MAX);
    dbx_seq_tempo();
}

static void dbx_seq_set_div(uint32_t i)    /* index in DBX_DIV */
{
    dbx_seq.div = DBX_DIV[i > 2u ? 2u : i];
    dbx_seq_tempo();
}

static uint32_t dbx_seq_div_index(void) { return dbx_seq.div == 8u ? 0u : dbx_seq.div == 16u ? 1u : 2u; }

static void dbx_seq_set_ext(uint32_t on)
{
    dbx_seq.ext = on ? 1u : 0u;
    dbx_seq.m_n = 0;
    dbx_seq.last_ms = fm1_ms - 1000u;      /* not locked until pulses come */
    if (!on)
        dbx_seq_tempo();
}

/* UI side: play a step now (its velocity, ratchet and pitch; no nudge, no chance), and drop what a lane had queued */
static void dbx_preview(uint32_t lane_i, uint32_t step)
{
    dbx_seq.pv_pat = dbx_seq.pe;
    dbx_seq.pv_lane = (uint8_t)lane_i;
    dbx_seq.pv_step = (uint8_t)step;
    dbx_seq.pv_req = 1;
}

/* REC: the step a hit played now belongs to: the one playing if it began less than half a step ago, else the one that comes next */
static uint32_t dbx_rec_step(void)
{
    uint32_t late;
    if (dbx_seq.ext)
        late = (uint32_t)dbx_seq.ext_acc * 2u >= 96u / dbx_seq.div;
    else
        late = dbx_seq.acc_q8 * 2u >= dbx_seq.step_q8;
    return late ? dbx_seq.nxq : dbx_seq.cur;
}

static void dbx_seq_flush(uint32_t mask) { dbx_seq.flush_mask = (uint16_t)(dbx_seq.flush_mask | mask); }

/* The computer's MIDI real-time messages (the MIDI side: USB, in the browser the page). Only while following it:
 * Start plays from step 0 on the next clock, Continue resumes, Stop stops; each Clock is counted.
 * Over USB the pulses come a few ms early or late each (and the tempo with them, and the steps if each pulse fired its own), so the steps are not
 * fired by the pulses but by a clock of our own, locked to them (the same as a hardware sequencer's phase-locked loop): the first beats the pulses fire the
 * steps, as they come, and the time of the last 4 beats gives the tempo; then the loop takes over at a beat, its pulses a sample-exact stream at that tempo,
 * and each pulse of the computer's pulls its phase (a part of the error, 1/8) and its length (1/256 of it) toward its own, so what it drifts by is put back
 * at every pulse and the tempo it runs at is the long average. A pulse or more off (a stall, a burst) puts the count right: it lets pulses by, or fires them
 * at once. The loop is the sequencer's whole clock, the beat is the pulses' count, so Start (and every pass of the pattern) stays on the computer's bar. */
static volatile uint32_t dbx_pk_n, dbx_clk_n;           /* what arrived from the host: MIDI packets (the FM-1: dbx_main.c), clock pulses (GLO 1 shows both while it waits) */
#define DBX_PL_MS_Q8 11290u                             /* a ms in samples, Q8 (FS 44100) */
#define DBX_PL_X10 282240000u                           /* 10 x 60 x FS x 256 / 24: the tempo x 10 is this over a pulse's length (Q8) */

static void dbx_pl_unlock(uint32_t h)                   /* the loop lets go: h is the computer's pulses counted (this one too): what was fired is put right with it */
{
    int32_t need = (int32_t)(h - (dbx_seq.pl_v - dbx_seq.pl_off));
    if (need > 1)
        dbx_seq.ext_seen -= (uint32_t)(need > 64 ? 63 : need - 1);      /* (pulses it did not fire: they fire now) */
    else
        dbx_seq.pl_skip = (uint8_t)(1 - need > 64 ? 64 : 1 - need);     /* (this pulse, and the ones it fired ahead: they are not fired twice) */
    dbx_seq.pl_lock = 0;
    dbx_seq.pl_hold = 0;
}

static void dbx_pl_detect(uint32_t age)                 /* a pulse of the computer's came (age: Q8 samples ago): how far off the loop is, and the correction */
{
    int32_t per = (int32_t)dbx_seq.pl_per, cnt = (int32_t)(dbx_seq.pl_v - dbx_seq.pl_off - dbx_seq.ext_rx), e, n;
    if (cnt > 64 || cnt < -64) {
        dbx_pl_unlock(dbx_seq.ext_rx);
        return;
    }
    e = cnt * per + dbx_seq.pl_acc - (int32_t)age;       /* > 0: the loop is ahead */
    if (e >= per || e <= -per) {                         /* whole pulses off: let them by (ahead), or fire them at once (behind) */
        n = e / per;
        if (n > 0) {
            dbx_seq.pl_hold = (uint8_t)(dbx_seq.pl_hold + n);
            dbx_seq.pl_off += (uint32_t)n;
        } else
            dbx_seq.pl_acc += -n * per;
        e -= n * per;
    }
    dbx_seq.pl_acc -= e / 8;
    if (dbx_seq.pl_acc < 0)
        dbx_seq.pl_acc = 0;
    per += e / 256;
    dbx_seq.pl_per = (uint32_t)(per < 94080 ? 94080 : per > 940800 ? 940800 : per);     /* (30 to 300 BPM) */
}

static void dbx_midi_realtime(uint8_t status, uint32_t ms)       /* ms: when it arrived (the FM-1: its USB poll's; the browser: now) */
{
    if (status == 0xF8u)
        dbx_clk_n++;
    if (!dbx_seq.ext)
        return;
    switch (status) {
    case 0xF8u: {
        uint32_t lim = 6250u / (dbx_seq.bpm ? dbx_seq.bpm : 120u), age = (fm1_ms - ms) * DBX_PL_MS_Q8, fresh = 0;
        if (dbx_seq.m_n == 0u || ms - dbx_seq.last_ms > (lim < 60u ? 60u : lim)) {    /* a first pulse, or after a stall (more than 2.5 pulses' time, 60 ms at least) */
            if (dbx_seq.pl_lock) {
                if (ms - dbx_seq.last_ms > 250u)        /* (a long silence is a clock that stopped, not one that is late: it begins again from the pulses) */
                    dbx_seq.pl_lock = dbx_seq.pl_hold = dbx_seq.pl_skip = 0;
                else
                    dbx_pl_unlock(dbx_seq.ext_rx + 1u);
            }
            dbx_seq.m_n = 0;
        }
        dbx_seq.ext_rx++;
        if (dbx_seq.m_n % 24u == 0u) {     /* a beat: its time is kept, and the tempo is the last 4 beats' (fewer at first) */
            uint32_t beat = dbx_seq.m_n / 24u, span = beat < 4u ? beat : 4u, per = 0, per1 = 0;
            dbx_seq.bt_ms[beat % 5u] = ms;
            if (span && ms != dbx_seq.bt_ms[(beat - span) % 5u])
                per = (ms - dbx_seq.bt_ms[(beat - span) % 5u]) * 4704u / (span * 10u);       /* a pulse's length, samples Q8 */
            if (beat && ms != dbx_seq.bt_ms[(beat - 1u) % 5u])
                per1 = (ms - dbx_seq.bt_ms[(beat - 1u) % 5u]) * 4704u;
            per1 /= 10u;
            if (dbx_seq.pl_lock) {          /* locked: a tempo that moved (2 % off for two beats) is taken at once, the shown tempo is the loop's */
                if (per1 && (per1 > dbx_seq.pl_per ? per1 - dbx_seq.pl_per : dbx_seq.pl_per - per1) * 50u > dbx_seq.pl_per) {
                    if (++dbx_seq.pl_chg >= 2u && per1 >= 94080u && per1 <= 940800u) {
                        dbx_seq.pl_per = per1;
                        dbx_seq.pl_chg = 0;
                    }
                } else
                    dbx_seq.pl_chg = 0;
                per = dbx_seq.pl_per;
            }
            if (per >= 94080u && per <= 940800u) {
                uint32_t x10 = DBX_PL_X10 / per;
                int32_t d = (int32_t)x10 - (int32_t)dbx_seq.bpm * 10;
                if (d > 6 || d < -6) {      /* (a whole BPM only when it is 0.6 off: it does not flicker) */
                    dbx_seq.bpm = (uint16_t)((x10 + 5u) / 10u);
                    dbx_seq_tempo();        /* (a step's length, for the nudges and ratchets) */
                }
                if (!dbx_seq.pl_lock && dbx_seq.playing && span >= 2u) {      /* two beats heard: the loop takes over, this pulse is its first */
                    dbx_seq.pl_per = per;
                    dbx_seq.pl_acc = (int32_t)(age + per);
                    dbx_seq.pl_v = dbx_seq.ext_rx - 1u;
                    dbx_seq.pl_off = 0;
                    dbx_seq.pl_hold = dbx_seq.pl_skip = dbx_seq.pl_chg = 0;
                    dbx_seq.pl_lock = 1;
                    fresh = 1;
                }
            }
        }
        if (dbx_seq.pl_lock && !fresh)
            dbx_pl_detect(age);
        dbx_seq.m_n++;
        dbx_seq.last_ms = ms;
        break;
    }
    case 0xFAu:
        dbx_seq_play(1);
        break;
    case 0xFBu:
        dbx_seq.playing = 1;
        dbx_seq.pl_lock = dbx_seq.pl_skip = 0;
        break;
    case 0xFCu:
        dbx_seq.playing = 0;
        dbx_seq.pl_lock = dbx_seq.pl_skip = 0;
        break;
    default:
        break;
    }
}

static uint32_t dbx_seq_locked(void)       /* following the clock, and it is arriving */
{
    return dbx_seq.ext && fm1_ms - dbx_seq.last_ms < 500u;
}

/* ------------------------------------------------------------ the queue --- */
static uint32_t dbx_rand100(void)          /* 0..99 */
{
    uint32_t x = dbx_seq.rnd;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    dbx_seq.rnd = x;
    return (x >> 8) % 100u;
}

static void dbx_push(uint32_t lane_i, uint32_t vel, int32_t semi, const dbx_hk_t *hk, int32_t t_q8)
{
    uint32_t k;
    for (k = 0; k < DBX_NPEND; k++)
        if (!dbx_seq.pend[k].used) {
            dbx_pend_t *p = &dbx_seq.pend[k];
            p->t_q8 = t_q8;
            p->lane = (uint8_t)lane_i;
            p->vel = (uint8_t)vel;
            p->semi = (int8_t)semi;
            p->k = *hk;
            p->used = 1;                   /* last: the entry is whole */
            return;
        }
}

/* the hits of step s of a lane, base_q8 after the block start: SUBDIVISION hits spread over the step, from the NUDGE */
static void dbx_sched(const dbx_pat_t *p, uint32_t i, uint32_t s, int32_t base_q8, uint32_t use_nudge)
{
    int32_t len = (int32_t)dbx_seq.step_q8, n = p->ratchet[i][s], gap, k;
    int32_t at = base_q8 + (use_nudge ? (int32_t)p->nudge[i][s] * (len / 48) : 0);
    uint32_t pat = (uint32_t)(p - dbx_seq.pat);
    dbx_hk_t hk = {(int8_t)(dbx_lk_get(pat, 0, i, s) * DBX_LK_UNIT[0]), (int8_t)(dbx_lk_get(pat, 1, i, s) * DBX_LK_UNIT[1]),      /* (the step's own DECAY, TONE, EXTRA */
                   (int8_t)(dbx_lk_get(pat, 2, i, s) * DBX_LK_UNIT[2]), (int8_t)(dbx_lk_get(pat, 3, i, s) * DBX_LK_UNIT[3])};         /* and PAN, in the lane's units) */
    if (n < 1)
        n = 1;
    gap = len / n;
    for (k = 0; k < n; k++) {
        int32_t t = at + k * gap;
        dbx_push(i, p->vel[i][s], p->semi[i][s], &hk, t < 0 ? 0 : t);
    }
}

static uint32_t dbx_plays(const dbx_pat_t *p, uint32_t i, uint32_t s)   /* the step is on, and CHANCE lets it play this pass */
{
    uint32_t c = p->chance[i][s];
    if (!((p->steps[i] >> s) & 1u))
        return 0;
    return c >= 100u || (c > 0u && dbx_rand100() < c);
}

/* the end of a pass while the song plays: the slot's passes counted, then the next slot (round) unless the slot's AGAIN (0 to 100 %) says that it
 * plays its passes once more; its pattern is the one that plays */
static void dbx_song_advance(void)
{
    uint32_t len = dbx_song_len();
    if (dbx_song.cur >= len) {
        dbx_song.cur = 0;
        dbx_song.done = 0;
    } else if (++dbx_song.done >= (1u << dbx_song.rep[dbx_song.cur])) {
        dbx_song.done = 0;
        if (!(dbx_song.again[dbx_song.cur] && dbx_rand100() < dbx_song.again[dbx_song.cur]))
            dbx_song.cur = (uint8_t)((dbx_song.cur + 1u) % len);
    }
    dbx_seq.pp = dbx_song.pat[dbx_song.cur];
}

/* PLAY with the song on: from its first slot */
static void dbx_song_start(void)
{
    if (dbx_song_active()) {
        dbx_song.cur = 0;
        dbx_song.done = 0;
        dbx_seq.pp = dbx_song.pat[0];
    }
}

/* a step boundary tb_q8 after the block start: step s begins; the hits of the next step are queued (and, after a
 * start, the first step's own). The pattern that waits takes over when the next step is the first of a pass. */
static void dbx_seq_step(int32_t tb_q8)
{
    uint32_t i, s = dbx_seq.nxq, nx, held = dbx_seq.rep_req && dbx_seq.playing, l = (dbx_seq.div / 2u) >> dbx_seq.rep_r;   /* (a beat is 2, 4 or 8 steps) */
    if (l == 0u)
        l = 1u;
    int32_t len = (int32_t)dbx_seq.step_q8;
    const dbx_pat_t *ps = &dbx_seq.pat[dbx_seq.pp], *pn;
    dbx_seq.tl = (uint8_t)((dbx_seq.tl + 1u) % DBX_NSTEP);
    if (dbx_seq.rep && !held)                       /* let go: the step now was the loop's, the one after is the pattern's */
        dbx_seq.rep = 0;
    else if (dbx_seq.rep && dbx_seq.rep_l == l)     /* looping: the loop's next step */
        dbx_seq.rep_i = (uint8_t)((dbx_seq.rep_i + 1u) % dbx_seq.rep_l);
    else if (held && !dbx_seq.first) {              /* SEQ held (or its length changed under it): the last beat, up to the step
                                                     * now, loops */
        dbx_seq.rep = 1;
        dbx_seq.rep_l = (uint8_t)l;
        dbx_seq.rep_s = (uint8_t)((s + DBX_NSTEP - (l - 1u)) % DBX_NSTEP);
        dbx_seq.rep_i = (uint8_t)(l - 1u);
    }
    nx = dbx_seq.rep ? ((uint32_t)dbx_seq.rep_s + (dbx_seq.rep_i + 1u) % dbx_seq.rep_l) % DBX_NSTEP
                     : (dbx_seq.tl + 1u) % DBX_NSTEP;
    dbx_seq.cur = (uint8_t)s;
    dbx_seq.nxq = (uint8_t)nx;
    dbx_seq.steps_done++;
    dbx_lfo_sync(++dbx_seq.pos);                    /* (0 at the first step after a start) */
    if (s == 0u && !dbx_seq.rep) {
        dbx_seq.pass++;
        dbx_snap_sync();                                /* a new pass: the pattern that comes in brings its sound */
    }
    if (nx == 0u && !dbx_seq.rep) {                 /* (the pattern that waits comes in with the pattern's own pass; or the song's next) */
        if (dbx_seq.pq != 255u) {
            dbx_seq.pp = dbx_seq.pq;
            dbx_seq.pq = 255;
        } else if (dbx_song_active())
            dbx_song_advance();
    }
    pn = &dbx_seq.pat[dbx_seq.pp];
    for (i = 0; i < DBX_NLANE; i++) {
        if (dbx_seq.first && dbx_plays(ps, i, s))
            dbx_sched(ps, i, s, tb_q8, 1);
        if (dbx_plays(pn, i, nx))
            dbx_sched(pn, i, nx, tb_q8 + len + ((nx & 1u) ? len * (int32_t)dbx_seq.swing / 200 : 0), 1);   /* (SWING: the
                                                                         * second step of each pair, up to half a step late) */
    }
    dbx_seq.first = 0;
}

/* the queued hits: those due in this block (nq_q8 long) play, the others come closer */
static void dbx_pend_run(int32_t nq_q8)
{
    uint32_t k;
    for (k = 0; k < DBX_NPEND; k++) {
        dbx_pend_t *p = &dbx_seq.pend[k];
        if (!p->used)
            continue;
        if (p->t_q8 < nq_q8) {
            dbx_hit_lk(p->lane, p->vel, p->semi, &p->k);
            p->used = 0;
        } else
            p->t_q8 -= nq_q8;
    }
}

/* one block of the clock (ISR): the steps that start in these n samples (or with the pulses that came) queue their
 * hits, and the hits that are due play */
static void dbx_seq_block(uint32_t n)
{
    uint32_t pulses, k;
    int32_t nq = (int32_t)(n << 8);
#ifdef DBX_MIDI_DRAIN
    DBX_MIDI_DRAIN();                      /* (the FM-1: the host's MIDI queue, here every block, so a clock pulse lands within 3 ms, not a screen frame) */
#endif
    pulses = dbx_seq.ext_rx - dbx_seq.ext_seen;
    dbx_seq.ext_seen += pulses;
    if (!dbx_seq.playing)                  /* stopped: the sound of the pattern picked is the live one at once */
        dbx_snap_sync();
    if (dbx_seq.flush_mask) {
        uint32_t m = dbx_seq.flush_mask;
        dbx_seq.flush_mask = 0;
        for (k = 0; k < DBX_NPEND; k++)
            if (dbx_seq.pend[k].used && ((m >> dbx_seq.pend[k].lane) & 1u))
                dbx_seq.pend[k].used = 0;
    }
    if (dbx_seq.lv_req) {                  /* a live hit (REC): the lane as it is set, at the velocity a step starts with */
        uint32_t m = dbx_seq.lv_req, l;
        dbx_seq.lv_req = 0;
        for (l = 0; l < DBX_NLANE; l++)
            if ((m >> l) & 1u)
                dbx_hit(l, DBX_VEL_DEF, 0);
    }
    if (dbx_seq.pv_req) {                  /* a preview: that step, now, as it is set (no nudge, no chance) */
        dbx_seq.pv_req = 0;
        dbx_sched(&dbx_seq.pat[dbx_seq.pv_pat], dbx_seq.pv_lane, dbx_seq.pv_step, 0, 0);
    }
    if (dbx_seq.playing) {
        dbx_seq.running = 1;
        if (dbx_seq.ext) {
            uint32_t pps = 96u / dbx_seq.div;
            if (dbx_seq.start_req) {       /* from the top: the next pulse starts step 1 */
                dbx_seq.start_req = 0;
                dbx_seq.cur = DBX_NSTEP - 1u;
                dbx_seq.tl = DBX_NSTEP - 1u;
                dbx_seq.nxq = 0;
                dbx_seq.rep = 0;
                dbx_lfo_restart();
                dbx_seq.pos = 0xFFFFFFFFu;
                dbx_seq.ext_acc = (uint8_t)(pps - 1u);
                dbx_seq.pl_lock = dbx_seq.pl_skip = 0;      /* (from the top: the computer's pulses fire the first beats again) */
                dbx_seq.first = 1;
                dbx_seq.pp = dbx_seq.pe;
                dbx_seq.pq = 255;
                dbx_song_start();
                dbx_snap_sync();                /* (its sound) */
            }
            if (dbx_seq.pl_lock) {          /* the locked loop is the clock: its pulses (the computer's are counted, they only pull it) */
                uint32_t lim = 6250u / (dbx_seq.bpm ? dbx_seq.bpm : 120u);
                if (fm1_ms - dbx_seq.last_ms <= (lim < 60u ? 60u : lim)) {     /* (no pulse for 2.5 pulses' time: it waits, it does not run on ahead) */
                    dbx_seq.pl_acc += nq;
                    while (dbx_seq.pl_acc >= (int32_t)dbx_seq.pl_per) {
                        int32_t at;
                        dbx_seq.pl_acc -= (int32_t)dbx_seq.pl_per;
                        dbx_seq.pl_v++;
                        if (dbx_seq.pl_hold) {
                            dbx_seq.pl_hold--;
                            continue;
                        }
                        at = nq - dbx_seq.pl_acc;
                        if (++dbx_seq.ext_acc >= pps) {
                            dbx_seq.ext_acc = 0;
                            dbx_seq_step(at < 0 ? 0 : at);
                        }
                    }
                }
            } else
                while (pulses--) {
                    if (dbx_seq.pl_skip) {
                        dbx_seq.pl_skip--;
                        continue;
                    }
                    if (++dbx_seq.ext_acc >= pps) {
                        dbx_seq.ext_acc = 0;
                        dbx_seq_step(0);
                    }
                }
        } else {
            if (dbx_seq.start_req) {       /* from the top: the first step starts with this block */
                dbx_seq.start_req = 0;
                dbx_seq.cur = DBX_NSTEP - 1u;
                dbx_seq.tl = DBX_NSTEP - 1u;
                dbx_seq.nxq = 0;
                dbx_seq.rep = 0;
                dbx_lfo_restart();
                dbx_seq.pos = 0xFFFFFFFFu;
                dbx_seq.acc_q8 = dbx_seq.step_q8;
                dbx_seq.first = 1;
                dbx_seq.pp = dbx_seq.pe;                 /* it plays the pattern that is shown */
                dbx_seq.pq = 255;
                dbx_song_start();
                dbx_snap_sync();                         /* (and its sound) */
            }
            dbx_seq.acc_q8 += (uint32_t)nq;
            while (dbx_seq.acc_q8 >= dbx_seq.step_q8) {
                dbx_seq.acc_q8 -= dbx_seq.step_q8;
                dbx_seq_step(nq - (int32_t)dbx_seq.acc_q8);   /* (the boundary was acc_q8 before the block's end) */
            }
        }
    } else if (dbx_seq.running) {          /* stopped: what was queued does not play */
        dbx_seq.running = 0;
        dbx_seq.rep = 0;
        for (k = 0; k < DBX_NPEND; k++)
            dbx_seq.pend[k].used = 0;
    }
    dbx_pend_run(nq);
}

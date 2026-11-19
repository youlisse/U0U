/* SPDX-License-Identifier: GPL-3.0-only */
/* DrumBox ARP: two generators for the selected lane, on the ARP button's two pages.
 * Page 1, EUCLID: HITS (0 to 16) spread as evenly as they can over LENGTH steps (2 to 16, the pattern repeating to fill the 16), turned by ROTATE
 *   steps; ACCENT (0 to 100 %) softens the hits that are off the beat (steps 1, 5, 9, 13) of the ones it adds. Any knob turned writes the lane at
 *   once: a step that is on and stays keeps everything it has, a step that goes off goes, a step that comes on starts as a step does.
 * Page 2, MUTATE: from the next pass on, every EVERY passes (1, 2, 4, 8) the pattern playing moves some of the hits of a lane to other steps, as many
 *   as AMOUNT says (100 %: four), and DRIFT (0 to 100 %) moves every setting of every hit of the lane at random: the velocity (up to +-25 at 100 %),
 *   the nudge (+-6), the subdivision (+-1), the chance (+-25), the pitch (+-2), the decay, tone, extra and pan of EDIT 2 (+-2 notches). Each lane has its own AMOUNT, EVERY and DRIFT:
 *   the knobs write the selected lane's (SCOPE ALL: every lane's, to the same values), and a lane keeps mutating while another is selected.
 *   The mutation is the pattern's own (dbx_arp.m[pattern]): another pattern has its own AMOUNT, EVERY and DRIFT, and mutates its second section too.
 *   The hits move with everything they have (velocity, nudge, subdivision, chance, pitch); the number of hits stays. It is the pattern that changes
 *   (and is saved): AMOUNT back to 0 keeps what it came to.
 * None of it is saved but its result: the pattern. */
enum { AP_HITS, AP_ROT, AP_LEN, AP_ACC, AM_AMT, AM_EVERY, AM_SCOPE, AM_DRIFT, AP_NP };
static struct {
    uint8_t p[AP_NP];                      /* page 1, and SCOPE (what the knobs of page 2 write: this lane, or all) */
    uint8_t m[DBX_NPAT][DBX_NLANE][4];     /* each pattern's, each lane's mutation: AMOUNT, EVERY, DRIFT at AM_* - AM_AMT (the third is not used: SCOPE is p's) */
    uint32_t seen;                         /* the pass counted (dbx_seq.pass) */
    uint32_t n;                            /* the passes counted since the mutation settings were last touched */
    uint32_t rng;
} dbx_arp = {{4, 0, 16, 0, 0, 0, 0, 0}, {{{0}}}, 0, 0, 0x6D2B79F5u};

#define DBX_M(lane_i) (dbx_arp.m[dbx_seq.pe][(lane_i)])         /* the pattern shown's mutation of a lane */
static void dbx_arp_copy(uint32_t src, uint32_t dst)
{
    uint32_t i, k;
    for (i = 0; i < DBX_NLANE; i++)
        for (k = 0; k < 4u; k++)
            dbx_arp.m[dst][i][k] = dbx_arp.m[src][i][k];
}
static void dbx_arp_shift(uint32_t p)                         /* a pattern deleted: the later ones' settings move down, the last has none */
{
    uint32_t q, i, k;
    for (q = p; q + 1u < DBX_NPAT; q++)
        for (i = 0; i < DBX_NLANE; i++)
            for (k = 0; k < 4u; k++)
                dbx_arp.m[q][i][k] = dbx_arp.m[q + 1u][i][k];
    for (i = 0; i < DBX_NLANE; i++)
        for (k = 0; k < 4u; k++)
            dbx_arp.m[DBX_NPAT - 1u][i][k] = 0;
}

static uint32_t dbx_arp_rand(void)
{
    uint32_t x = dbx_arp.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    dbx_arp.rng = x;
    return x >> 8;
}

/* is step s (0..n-1) of the Euclidean pattern of k hits in n steps, turned right by r, a hit: (i k) mod n < k, from the start of the pattern */
static uint32_t dbx_euclid_on(uint32_t k, uint32_t n, uint32_t r, uint32_t s)
{
    uint32_t i = (s + n - r % n) % n;
    return k != 0u && (i * k) % n < k;
}

static void dbx_arp_apply(uint32_t lane_i)         /* page 1: write the lane */
{
    uint32_t s, k = dbx_arp.p[AP_HITS], n = dbx_arp.p[AP_LEN] < 2u ? 2u : dbx_arp.p[AP_LEN], r = dbx_arp.p[AP_ROT], acc = dbx_arp.p[AP_ACC];
    for (s = 0; s < DBX_NSTEP; s++) {
        uint32_t on = dbx_euclid_on(k > n ? n : k, n, r, s % n), was = (DBX_E.steps[lane_i] >> s) & 1u;
        if (on && !was) {
            dbx_step_defaults(&DBX_E, lane_i, s);
            if (s % 4u != 0u)
                DBX_E.vel[lane_i][s] = (uint8_t)(DBX_VEL_DEF - acc * 50u / 100u);
            DBX_E.steps[lane_i] |= (uint16_t)(1u << s);
        } else if (!on && was)
            DBX_E.steps[lane_i] &= (uint16_t)~(1u << s);
    }
    dbx_seq_flush(1u << lane_i);                   /* (what was queued of the lane does not play) */
    dbx_touch_pat(DBX_EV);
}

static void dbx_arp_move(dbx_pat_t *p, uint32_t lane_i, uint32_t from, uint32_t to)       /* a hit and all it has, from one step to another */
{
    p->vel[lane_i][to] = p->vel[lane_i][from];
    p->nudge[lane_i][to] = p->nudge[lane_i][from];
    p->ratchet[lane_i][to] = p->ratchet[lane_i][from];
    p->chance[lane_i][to] = p->chance[lane_i][from];
    p->semi[lane_i][to] = p->semi[lane_i][from];
    {
        uint32_t pat = dbx_pat_idx(p);
        dbx_lk_set(pat, 0, lane_i, to, dbx_lk_get(pat, 0, lane_i, from));
        dbx_lk_set(pat, 1, lane_i, to, dbx_lk_get(pat, 1, lane_i, from));
        dbx_lk_set(pat, 2, lane_i, to, dbx_lk_get(pat, 2, lane_i, from));
        dbx_lk_set(pat, 3, lane_i, to, dbx_lk_get(pat, 3, lane_i, from));
    }
    p->steps[lane_i] = (uint16_t)((p->steps[lane_i] | (1u << to)) & ~(1u << from));
}

/* v moved by up to +-(range x drift / 100), at random (a fraction of a unit is a chance of one more), held in lo..hi: every hit's settings wander, a little at
 * each mutation */
static int32_t dbx_arp_wander(int32_t v, uint32_t range, uint32_t drift, int32_t lo, int32_t hi)
{
    uint32_t m = range * drift / 100u, frac = range * drift % 100u;
    if (frac && dbx_arp_rand() % 100u < frac)
        m++;
    if (m == 0u)
        return v;
    return clamp(v + (int32_t)(dbx_arp_rand() % (2u * m + 1u)) - (int32_t)m, lo, hi);
}

static void dbx_arp_mutate_lane(dbx_pat_t *p, const uint8_t *m, uint32_t lane_i)
{
    uint32_t t, s, w, n_on = 0, swaps = ((uint32_t)m[AM_AMT - AM_AMT] * 4u + 99u) / 100u, drift = m[AM_DRIFT - AM_AMT], pat = dbx_pat_idx(p);
    for (s = 0; s < DBX_NSTEP; s++)
        n_on += (p->steps[lane_i] >> s) & 1u;
    if (n_on == 0u || n_on == DBX_NSTEP)
        swaps = 0;                                          /* (no rest to move to) */
    for (t = 0; t < swaps; t++) {
        uint32_t from = dbx_arp_rand() % n_on, to = dbx_arp_rand() % (DBX_NSTEP - n_on), a = 0, b = 0;
        for (s = 0; s < DBX_NSTEP; s++)                     /* the from-th hit, the to-th rest */
            if ((p->steps[lane_i] >> s) & 1u) {
                if (from == 0u)
                    a = s;
                from--;
            } else {
                if (to == 0u)
                    b = s;
                to--;
            }
        dbx_arp_move(p, lane_i, a, b);
    }
    if (drift)                                              /* DRIFT: every setting of every hit of EDIT 1 and EDIT 2 (and its velocity) wanders; the range is at 100 % */
        for (s = 0; s < DBX_NSTEP; s++)
            if ((p->steps[lane_i] >> s) & 1u) {
                p->vel[lane_i][s] = (uint8_t)dbx_arp_wander(p->vel[lane_i][s], 25, drift, 1, 127);
                p->nudge[lane_i][s] = (int8_t)dbx_arp_wander(p->nudge[lane_i][s], 6, drift, -DBX_NUDGE_MAX, DBX_NUDGE_MAX);
                p->ratchet[lane_i][s] = (uint8_t)dbx_arp_wander(p->ratchet[lane_i][s], 1, drift, 1, DBX_RATCHET_MAX);
                p->chance[lane_i][s] = (uint8_t)dbx_arp_wander(p->chance[lane_i][s], 25, drift, 0, 100);
                p->semi[lane_i][s] = (int8_t)dbx_arp_wander(p->semi[lane_i][s], 2, drift, -DBX_PITCH_MAX, DBX_PITCH_MAX);
                for (w = 0; w < 4u; w++)                    /* DECAY, TONE, EXTRA, PAN: by up to two of their notches */
                    dbx_lk_set(pat, w, lane_i, s, dbx_arp_wander(dbx_lk_get(pat, w, lane_i, s), 2, drift, -8, 7));
            }
}

/* the main loop, each frame: a pass has gone by while playing (the sequencer counts them, not the loops of SEQ) */
static void dbx_arp_poll(void)
{
    uint32_t pass = dbx_seq.pass, i, any = 0, pp = dbx_seq.pp, v = pp + DBX_NPAT * dbx_seq.cs;
    dbx_pat_t *p = DBX_PATV(v);                    /* (the section that has begun: its first step started the pass) */
    if (pass == dbx_arp.seen)
        return;
    dbx_arp.seen = pass;
    if (!dbx_seq.playing)
        return;
    dbx_arp.n++;
    for (i = 0; i < DBX_NLANE; i++) {              /* every lane on its own: its AMOUNT or DRIFT on, and its EVERY come round */
        const uint8_t *m = dbx_arp.m[pp][i];
        if ((m[AM_AMT - AM_AMT] || m[AM_DRIFT - AM_AMT]) && dbx_arp.n % (1u << m[AM_EVERY - AM_AMT]) == 0u) {
            dbx_arp_mutate_lane(p, m, i);
            any = 1;
        }
    }
    if (any)
        dbx_touch_pat(v);
}

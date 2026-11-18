/* SPDX-License-Identifier: GPL-3.0-only */
/* DrumBox sounds per pattern: each of the 16 patterns has its own SOUND, a snapshot of what the kit, the effects and the master page are set to:
 *   the kit (CLASSIC, ANALOG ...) and every lane's TUNE, DECAY, TONE, EXTRA, LEVEL and mute,
 *   every effect's 8 parameters and the FX MIX of GLO page 2,
 *   the master page (HPF, LPF, OTT, CLIP),
 *   the two LFOs (RATE, AMOUNT, SHAPE, PHASE) and ENV (its 8 values).
 * The tempo, the subdivision, the volume, the swing and ARP are the project's and are not here.
 * The sound that plays is the live one (the parameters the pages show and the audio reads); a snapshot is where a pattern's sound waits.
 *   - the live sound is the PLAYING pattern's (stopped: the one shown): the knobs set it, and it is taken into the pattern's snapshot when the
 *     pattern is left (and when it is saved);
 *   - the pattern that plays changes (a pass ends and the waiting one comes in, a pattern is picked while stopped, PLAY from the shown one):
 *     the live sound is taken into the snapshot of the one that leaves and the snapshot of the one that comes becomes the live sound. That is done
 *     by the audio ISR, at the step where the new pattern's first step begins (stopped: at once), so that the last hits of the old pattern keep their
 *     sound and the first of the new one has its own;
 *   - copying a pattern copies its sound; resetting the one shown resets its sound;
 *   - what is saved before this (one sound for all) is read as the same sound in all 16.
 * The effects' own state (the delay line, the reverb) is not part of it: a tail rings out across the change. A change of kit starts the lanes'
 * voices over (the drums change), as picking a kit does. */
typedef struct { int16_t tune; uint8_t decay, tone, extra, level, accent; int8_t pan; uint8_t choke, pad; } dbx_snap_lane_t;   /* 10 */
typedef struct {
    dbx_snap_lane_t k[DBX_NLANE];          /* 110 */
    uint8_t fx[DBX_NFX][DBX_NFXP];         /* 88 */
    uint8_t mst[4];
    uint8_t kit, fxmaster, pad[2];
    uint8_t lfo[2 * LFO_NP];               /* 8 */
    uint8_t env[EV_NP];                    /* 8: 222 bytes */
} dbx_snap_t;
#define DBX_S_MAGIC 0x55584244u               /* "DBXU": the flash object that holds the 16 sounds, 222 bytes each (OBJ_UPFM6's pair, free in this firmware), then the song */
#define DBX_S_MAGIC_T 0x54584244u             /* "DBXT": the sounds with the LFOs and ENV, 200 bytes each (lanes without ACCENT, PAN, CHOKE) */
#define DBX_S_MAGIC_S 0x53584244u             /* "DBXS": the first ones, 184 bytes each (no LFOs, no ENV either) */
static struct {
    uint32_t magic;                        /* (the first of what is saved: the magic, the 16 snapshots, then the song) */
    dbx_snap_t s[DBX_NPAT];
    uint8_t song[40];                      /* the chain of patterns (dbx_song.c) */
    uint8_t live;                          /* the pattern whose sound is the live one (255: none yet) */
    volatile uint8_t reload;               /* the live sound is to be replaced by the playing pattern's snapshot, as it is (a copy onto it) */
} dbx_snap = {0, {{{{0}}}}, {0}, 255, 0};
#define DBX_SNAP_BYTES (4u + (uint32_t)sizeof(dbx_snap_t) * DBX_NPAT + 40u)

static void dbx_snap_take(dbx_snap_t *d)                    /* the live sound into d */
{
    uint32_t i, e, k;
    for (i = 0; i < DBX_NLANE; i++) {
        d->k[i].tune = lane[i].p.tune;
        d->k[i].decay = lane[i].p.decay;
        d->k[i].tone = lane[i].p.tone;
        d->k[i].extra = lane[i].p.extra;
        d->k[i].level = lane[i].p.level;
        d->k[i].accent = lane[i].p.accent;
        d->k[i].pan = lane[i].pan;
        d->k[i].choke = lane[i].choke;                     /* (the mutes are not a pattern's: see dbx_ui.c) */
        d->k[i].pad = 0;
    }
    for (e = 0; e < DBX_NFX; e++)
        for (k = 0; k < DBX_NFXP; k++)
            d->fx[e][k] = dbx_fx.p[e][k];
    for (k = 0; k < 4u; k++)
        d->mst[k] = dbx_mst.p[k];
    d->kit = dbx_kit;
    d->fxmaster = dbx_fx.master;
    d->pad[0] = d->pad[1] = 0;
    for (k = 0; k < LFO_NP; k++) {
        d->lfo[k] = dbx_lfo.p[0][k];
        d->lfo[LFO_NP + k] = dbx_lfo.p[1][k];
    }
    for (k = 0; k < EV_NP; k++)
        d->env[k] = dbx_env.p[k];
}

static void dbx_snap_give(const dbx_snap_t *s)               /* s as the live sound */
{
    uint32_t i, e, k;
    if (s->kit != dbx_kit && s->kit < DBX_NKIT) {            /* another kit: the lanes are set up for its drums (here: the audio ISR) ... */
        dbx_kit = s->kit;
        dbx_kit_run = s->kit;
        dbx_kit_apply(s->kit, 0);
    }
    for (i = 0; i < DBX_NLANE; i++) {                        /* ... then their parameters, as the pattern has them */
        lane[i].p.tune = (int16_t)clamp(s->k[i].tune, -192, 192);
        lane[i].p.decay = (uint8_t)clamp(s->k[i].decay, 0, 127);
        lane[i].p.tone = (uint8_t)clamp(s->k[i].tone, 0, 127);
        lane[i].p.extra = (uint8_t)clamp(s->k[i].extra, 0, 127);
        lane[i].p.level = (uint8_t)clamp(s->k[i].level, 0, 127);
        lane[i].p.accent = (uint8_t)clamp(s->k[i].accent, 0, 127);
        lane[i].pan = (int8_t)clamp(s->k[i].pan, -32, 32);
        lane[i].choke = DBX_CHOKE_DEF[i];                  /* (no page sets it: what an older save holds is not read) */
    }
    for (e = 0; e < DBX_NFX; e++) {
        for (k = 0; k < DBX_NFXP; k++)
            dbx_fx.p[e][k] = s->fx[e][k] > 127u ? dbx_fx.p[e][k] : s->fx[e][k];
        dbx_fx_rates(e);
    }
    for (k = 0; k < 4u; k++)
        dbx_mst.p[k] = s->mst[k] > 127u ? dbx_mst.p[k] : s->mst[k];
    dbx_mst_changed();
    dbx_fx.master = s->fxmaster > 100u ? 100u : s->fxmaster;
    for (k = 0; k < LFO_NP; k++) {
        dbx_lfo.p[0][k] = s->lfo[k] > LFO_MAXV[k] ? dbx_lfo.p[0][k] : s->lfo[k];
        dbx_lfo.p[1][k] = s->lfo[LFO_NP + k] > LFO_MAXV[k] ? dbx_lfo.p[1][k] : s->lfo[LFO_NP + k];
    }
    for (k = 0; k < EV_NP; k++)
        dbx_env.p[k] = s->env[k] > EV_MAX[k] ? dbx_env.p[k] : s->env[k];
}

static void dbx_snap_default(dbx_snap_t *d)                  /* a sound at its state 0: the kit CLASSIC, the drums as designed, the effects, the master page */
{
    uint32_t i;
    dbx_fx_defaults(d->fx);
    for (i = 0; i < DBX_NLANE; i++) {
        dv_param_t p;
        dv_default(&p, DBX_TYPE[i]);
        d->k[i].tune = 0;
        d->k[i].decay = p.decay;
        d->k[i].tone = p.tone;
        d->k[i].extra = p.extra;
        d->k[i].level = 100;
        d->k[i].accent = 0;
        d->k[i].pan = 0;
        d->k[i].choke = DBX_CHOKE_DEF[i];
        d->k[i].pad = 0;
    }
    d->mst[0] = 0;
    d->mst[1] = 127;
    d->mst[2] = 0;
    d->mst[3] = 0;
    d->kit = 0;
    d->fxmaster = 100;
    d->pad[0] = d->pad[1] = 0;
    for (i = 0; i < 2u * LFO_NP; i++)
        d->lfo[i] = i % LFO_NP == 0u ? (i < LFO_NP ? 5u : 3u) : 0u;          /* (RATE 1/8 and 1/2, the others at 0: off) */
    for (i = 0; i < EV_NP; i++)
        d->env[i] = EV_DEF[i];
}

/* the live sound is the one of the pattern that plays: if that changed, swap (the audio ISR, see above) */
static void dbx_snap_sync(void)
{
    uint32_t pp = dbx_seq.pp;
    if (pp >= DBX_NPAT)
        return;
    if (dbx_snap.reload) {
        dbx_snap.reload = 0;
        dbx_snap_give(&dbx_snap.s[pp]);
        dbx_snap.live = (uint8_t)pp;
        return;
    }
    if (dbx_snap.live == pp)
        return;
    if (dbx_snap.live < DBX_NPAT)
        dbx_snap_take(&dbx_snap.s[dbx_snap.live]);
    dbx_snap_give(&dbx_snap.s[pp]);
    dbx_snap.live = (uint8_t)pp;
}

static void dbx_snap_flush(void)                             /* the live sound into its pattern's snapshot (before saving, or copying), and the song beside them */
{
    uint32_t k;
    for (k = 0; k < DBX_NPAT; k++) {
        dbx_snap.song[k] = dbx_song.pat[k] < DBX_NPAT ? (uint8_t)(dbx_song.pat[k] + 1u) : 0u;      /* (a pattern + 1; 0: the end, as a flash of zeros reads) */
        dbx_snap.song[16u + k] = (uint8_t)(dbx_song.rep[k] | (dbx_song.again[k] / 5u) << 2);      /* (the passes in 2 bits, the AGAIN in fives above them) */
    }
    dbx_snap.song[32] = dbx_song.on;
    if (dbx_snap.live < DBX_NPAT)
        dbx_snap_take(&dbx_snap.s[dbx_snap.live]);
}

static void dbx_snap_copy(uint32_t src, uint32_t dst)        /* a pattern copied: its sound too */
{
    dbx_snap_flush();
    dbx_snap.s[dst] = dbx_snap.s[src];
    if (dst == dbx_snap.live)
        dbx_snap.reload = 1;                                 /* (the copy is to be heard) */
}

static void dbx_snap_remove(uint32_t p)                      /* a pattern deleted: the sounds after it move down, the 16th is at state 0 */
{
    uint32_t q;
    dbx_snap_flush();                                        /* (the live sound into its pattern's first) */
    for (q = p; q + 1u < DBX_NPAT; q++)
        dbx_snap.s[q] = dbx_snap.s[q + 1u];
    dbx_snap_default(&dbx_snap.s[DBX_NPAT - 1u]);
    dbx_snap.live = 255;                                     /* (the numbers moved: the live sound is the playing pattern's snapshot, as it now is) */
    dbx_snap.reload = 1;
}

/* a sound of an older object (entry bytes: 184 from DBXS, 200 from DBXT) as today's: the lanes get no ACCENT, no PAN and the hats' choke group */
static void dbx_snap_from_old(dbx_snap_t *d, const uint8_t *o, uint32_t entry)
{
    uint32_t i, k;
    for (i = 0; i < DBX_NLANE; i++) {
        const uint8_t *q = o + 8u * i;
        d->k[i].tune = (int16_t)(q[0] | (q[1] << 8));
        d->k[i].decay = q[2];
        d->k[i].tone = q[3];
        d->k[i].extra = q[4];
        d->k[i].level = q[5];
        d->k[i].accent = 0;
        d->k[i].pan = 0;
        d->k[i].choke = DBX_CHOKE_DEF[i];
        d->k[i].pad = 0;
    }
    for (i = 0; i < DBX_NFX; i++)
        for (k = 0; k < DBX_NFXP; k++)
            d->fx[i][k] = o[88u + i * DBX_NFXP + k];
    for (k = 0; k < 4u; k++)
        d->mst[k] = o[176u + k];
    d->kit = o[180];
    d->fxmaster = o[181];
    d->pad[0] = d->pad[1] = 0;
    if (entry >= 200u) {
        for (k = 0; k < 2u * LFO_NP; k++)
            d->lfo[k] = o[184u + k];
        for (k = 0; k < EV_NP; k++)
            d->env[k] = o[192u + k];
    }                                                      /* (a DBXS sound: the LFOs and ENV as they are, already in d) */
}

static void dbx_snap_song_load(void)                         /* the song saved, checked */
{
    uint32_t k;
    for (k = 0; k < DBX_NPAT; k++) {
        dbx_song.pat[k] = dbx_snap.song[k] >= 1u && dbx_snap.song[k] <= DBX_NPAT ? (uint8_t)(dbx_snap.song[k] - 1u) : 255u;
        dbx_song.rep[k] = dbx_snap.song[16u + k] & 3u;
        dbx_song.again[k] = (uint8_t)((dbx_snap.song[16u + k] >> 2) <= 20u ? (dbx_snap.song[16u + k] >> 2) * 5u : 0u);
    }
    dbx_song.on = dbx_snap.song[32] == 1u ? 1u : 0u;
    dbx_song.cur = dbx_song.done = 0;
}

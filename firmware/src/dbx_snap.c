/* SPDX-License-Identifier: GPL-3.0-only */
/* DrumBox sound: one for the whole project, the same under every pattern: the kit (CLASSIC, ANALOG ...) and every lane's TUNE, DECAY, TONE, EXTRA, LEVEL
 * and mute, every effect's 8 parameters and the FX MIX of GLO page 2, the master page (HPF, LPF, OTT, CLIP), the two LFOs (RATE, AMOUNT, SHAPE, PHASE) and
 * ENV (its 8 values); with the tempo, the subdivision, the volume, the swing and ARP's own settings. What belongs to a pattern is its steps (and their
 * effects' steps and own values) and its mutation. The live sound (the parameters the pages show and the audio reads) is saved as one snapshot.
 * The object that holds it was 16 snapshots, one a pattern (before the sound was the project's): it is still written that way (the same one 16 times), so
 * that what is read is the same either way, and an older save is read as its pattern shown's sound. */
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
} dbx_snap = {0, {{{{0}}}}, {0}};
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
        d->k[i].pad = lane[i].px ^ 0x88u;                  /* (the second page's two knobs of a Peaks lane, from their defaults: an older save has 0, the defaults) */
    }
    for (e = 0; e < DBX_NFX; e++)
        for (k = 0; k < DBX_NFXP; k++)
            d->fx[e][k] = dbx_fx.p[e][k];
    for (k = 0; k < 4u; k++)
        d->mst[k] = dbx_mst.p[k];
    d->kit = DBX_KIT_ID[dbx_kit];
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
    if (dbx_kit_pos(s->kit) != dbx_kit) {                    /* another kit (its id; one that is no longer listed: CLASSIC): the lanes are set up for its drums (here: the audio ISR) ... */
        dbx_kit = (uint8_t)dbx_kit_pos(s->kit);
        dbx_kit_run = dbx_kit;
        dbx_kit_apply(dbx_kit, 0);
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
        lane[i].px = (uint8_t)(s->k[i].pad ^ 0x88u);
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
    d->kit = KIT_CLASSIC;
    d->fxmaster = 100;
    d->pad[0] = d->pad[1] = 0;
    for (i = 0; i < 2u * LFO_NP; i++)
        d->lfo[i] = i % LFO_NP == 0u ? (i < LFO_NP ? 5u : 3u) : 0u;          /* (RATE 1/8 and 1/2, the others at 0: off) */
    for (i = 0; i < EV_NP; i++)
        d->env[i] = EV_DEF[i];
}

static void dbx_snap_flush(void)                             /* the live sound into the snapshots (the same 16 times), and the song beside them */
{
    uint32_t k;
    for (k = 0; k < DBX_NPAT; k++) {
        dbx_snap.song[k] = dbx_song.pat[k] < DBX_NPAT ? (uint8_t)(dbx_song.pat[k] + 1u) : 0u;      /* (a pattern + 1; 0: the end, as a flash of zeros reads) */
        dbx_snap.song[16u + k] = (uint8_t)(dbx_song.rep[k] | (dbx_song.again[k] / 5u) << 2);      /* (the passes in 2 bits, the AGAIN in fives above them) */
    }
    dbx_snap.song[32] = dbx_song.on;
    dbx_snap_take(&dbx_snap.s[0]);
    for (k = 1; k < DBX_NPAT; k++)
        dbx_snap.s[k] = dbx_snap.s[0];
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

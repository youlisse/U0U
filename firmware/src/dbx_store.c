/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (storage.c, which keeps the objects) */
/* DrumBox storage: the settings (the kit, the tempo, the subdivision, the volume, the pattern shown) and the 16 patterns,
 * in storage.c's A/B sector objects (a save goes to the other copy, the commit record last: a power cut leaves the
 * previous one). Four patterns to an object (OBJ_PROJECT0..3, 0x97000..0x9EFFF); the settings in OBJ_SETTINGS (0xFC000).
 * Both start with a magic of their own, so what an earlier firmware left in those sectors is not read as ours.
 * Saving erases a sector, which stops the audio for a moment on the FM-1: it is done a moment after the last change
 * (dbx_touch_*), one object a call, and never while playing. */
#define DBX_SET_MAGIC 0x31584244u                      /* "DBX1" */
#define DBX_PAT_MAGIC 0x50584244u                      /* "DBXP" */
#define DBX_FX_MAGIC 0x49584244u                       /* "DBXI": the delay and the reverb with their amount in the effect's MIX (steps at 100) */
#define DBX_FX_MAGIC_H 0x48584244u                     /* "DBXH": their amount was in each step (50 and 30), the effect's MIX at 100 */
#define DBX_FX_MAGIC_F 0x46584244u                     /* "DBXF": the first order of the effects (REPEAT first, SATURATE 2 fifth) */
#define DBX_FX_MAGIC_G 0x47584244u                     /* "DBXG": REPEAT before the REVERB, SATURATE 2 still fifth */
static const uint8_t DBX_FX_F2NEW[DBX_NFX] = {8, 0, 1, 2, 10, 3, 4, 5, 6, 7, 9};   /* where an effect of the first order went */
static const uint8_t DBX_FX_G2NEW[DBX_NFX] = {0, 1, 2, 10, 3, 4, 5, 6, 7, 8, 9};   /* ... and one of the second */
#define DBX_X_MAGIC 0x58584244u                        /* "DBXX": the extras (ENV; what comes next has its room), in the second spare object */
#define OBJ_DBX_X (OBJ_UPRESET0 + 1)
#define OBJ_DBX_K OBJ_AUTOSAVE                         /* (project.c's autosave pair, which this firmware does not use) */
#define DBX_PAT_PER_OBJ 4u
#define DBX_SAVE_WAIT_MS 1500u                         /* after the last change */

typedef struct {
    uint32_t magic;
    uint16_t bpm;
    uint8_t div, vol, pe, swing, pad[2];       /* (swing: a byte that was padding: older saves read it as 0; pad[0] the FX mix + 1, pad[1] the kit) */
    struct { int16_t tune; uint8_t decay, tone, extra, level, mute, pad; } k[DBX_NLANE];     /* the kit */
} dbx_setobj_t;
typedef struct { uint32_t magic, pad; dbx_pat_t p[DBX_PAT_PER_OBJ]; } dbx_patobj_t;
typedef struct { uint32_t magic, pad; uint8_t p[DBX_NFX][DBX_NFXP]; dbx_fxpat_t pat[DBX_NPAT]; } dbx_fxobj_t;   /* the effects: 3264 bytes */
_Static_assert(sizeof(dbx_fxobj_t) <= ST_PAYLOAD_MAX, "the effects fit a storage object");
_Static_assert(sizeof(dbx_patobj_t) <= ST_PAYLOAD_MAX, "four patterns fit a storage object");
_Static_assert(sizeof dbx_lk2 <= ST_PAYLOAD_MAX, "the steps' own EXTRA and PAN fit a storage object");
_Static_assert(DBX_NPAT == 4u * DBX_PAT_PER_OBJ / 1u && DBX_NPAT / DBX_PAT_PER_OBJ == 4u, "four pattern objects");

static dbx_setobj_t dbx_sobj;
static dbx_patobj_t dbx_pobj;
static dbx_fxobj_t dbx_fobj;

static void dbx_pat_sanitize(dbx_pat_t *p)             /* what was stored may be anything: keep every value in range */
{
    uint32_t i, s;
    for (i = 0; i < DBX_NLANE; i++)
        for (s = 0; s < DBX_NSTEP; s++) {
            p->vel[i][s] = (uint8_t)clamp(p->vel[i][s], 1, 127);
            p->nudge[i][s] = (int8_t)clamp(p->nudge[i][s], -DBX_NUDGE_MAX, DBX_NUDGE_MAX);
            p->ratchet[i][s] = (uint8_t)clamp(p->ratchet[i][s], 1, (int32_t)DBX_RATCHET_MAX);
            p->chance[i][s] = (uint8_t)clamp(p->chance[i][s], 0, 100);
            p->semi[i][s] = (int8_t)clamp(p->semi[i][s], -DBX_PITCH_MAX, DBX_PITCH_MAX);
        }
}

/* power-on (after dbx_seq_init and dbx_voices_init): what the flash holds, where it is whole and ours */
static void dbx_store_load(void)
{
    uint32_t i, k;
    if (st_load(OBJ_SETTINGS, &dbx_sobj, sizeof dbx_sobj) == (int)sizeof dbx_sobj && dbx_sobj.magic == DBX_SET_MAGIC) {
        dbx_seq.bpm = (uint16_t)clamp(dbx_sobj.bpm, (int32_t)DBX_BPM_MIN, (int32_t)DBX_BPM_MAX);
        dbx_seq.div = dbx_sobj.div == 8u || dbx_sobj.div == 32u ? dbx_sobj.div : 16u;
        dbx_seq_tempo();
        dbx_ui.vol = (uint8_t)clamp(dbx_sobj.vol, 0, 127);
        dbx_audio.vol_q12 = dbx_vol_q12(dbx_ui.vol);
        dbx_seq.pe = dbx_seq.pp = dbx_sobj.pe < DBX_NPAT ? dbx_sobj.pe : 0u;
        dbx_seq.swing = dbx_sobj.swing <= 100u ? dbx_sobj.swing : 0u;
        dbx_fx.master = dbx_sobj.pad[0] == 0u || dbx_sobj.pad[0] > 101u ? 100u : (uint8_t)(dbx_sobj.pad[0] - 1u);   /* (stored + 1) */
        dbx_kit = dbx_sobj.pad[1] < DBX_NKIT ? dbx_sobj.pad[1] : 0u;     /* (an older save left 0: STD) */
        dbx_kit_apply(dbx_kit, 0);                                       /* (the lanes' drums first, then the parameters saved with them) */
        dbx_kit_run = dbx_kit;
        for (i = 0; i < DBX_NLANE; i++) {
            lane[i].p.tune = (int16_t)clamp(dbx_sobj.k[i].tune, -192, 192);
            lane[i].p.decay = (uint8_t)clamp(dbx_sobj.k[i].decay, 0, 127);
            lane[i].p.tone = (uint8_t)clamp(dbx_sobj.k[i].tone, 0, 127);
            lane[i].p.extra = (uint8_t)clamp(dbx_sobj.k[i].extra, 0, 127);
            lane[i].p.level = (uint8_t)clamp(dbx_sobj.k[i].level, 0, 127);
            lane[i].mute = 0;                                          /* (a mute is for playing: not kept) */
        }
        for (i = 0; i < 2u * LFO_NP; i++) {                              /* the LFOs: in the first eight lanes' spare bytes (stored + 1: */
            static const uint8_t MAXV[LFO_NP] = {LFO_NRATE - 1u, 100, LFO_NSHAPE - 1u, LFO_NPHASE - 1u};   /* 0: never saved, as it is) */
            uint32_t v = dbx_sobj.k[i].pad;
            if (v)
                dbx_lfo.p[i / LFO_NP][i % LFO_NP] = (uint8_t)clamp((int32_t)v - 1, 0, (int32_t)MAXV[i % LFO_NP]);
        }
    }
    for (k = 0; k < DBX_NPAT / DBX_PAT_PER_OBJ; k++)
        if (st_load(OBJ_PROJECT0 + k, &dbx_pobj, sizeof dbx_pobj) == (int)sizeof dbx_pobj && dbx_pobj.magic == DBX_PAT_MAGIC)
            for (i = 0; i < DBX_PAT_PER_OBJ; i++) {
                dbx_seq.pat[k * DBX_PAT_PER_OBJ + i] = dbx_pobj.p[i];
                dbx_pat_sanitize(&dbx_seq.pat[k * DBX_PAT_PER_OBJ + i]);
            }
    if (st_load(OBJ_UPRESET0, &dbx_fobj, sizeof dbx_fobj) == (int)sizeof dbx_fobj &&
        (dbx_fobj.magic == DBX_FX_MAGIC || dbx_fobj.magic == DBX_FX_MAGIC_H || dbx_fobj.magic == DBX_FX_MAGIC_F || dbx_fobj.magic == DBX_FX_MAGIC_G)) {
        const uint8_t *to = dbx_fobj.magic == DBX_FX_MAGIC_F ? DBX_FX_F2NEW : dbx_fobj.magic == DBX_FX_MAGIC_G ? DBX_FX_G2NEW : 0;
        uint32_t old = to != 0;                                                /* (an old save: its effects in an old order) */
        dbx_mst.p[0] = (uint8_t)clamp(dbx_fobj.pad & 255u, 0, 127);                 /* the master page: HPF, LPF (stored as 127 - it, */
        dbx_mst.p[1] = (uint8_t)(127 - clamp((dbx_fobj.pad >> 8) & 255u, 0, 127));  /* so a save from before reads as off), OTT, CLIP */
        dbx_mst.p[2] = (uint8_t)clamp((dbx_fobj.pad >> 16) & 255u, 0, 127);
        dbx_mst.p[3] = (uint8_t)clamp((dbx_fobj.pad >> 24) & 255u, 0, 127);
        dbx_mst_changed();
        for (i = 0; i < DBX_NFX; i++) {
            uint32_t e = old ? to[i] : i;
            for (k = 0; k < DBX_NFXP; k++)
                dbx_fx.p[e][k] = dbx_fobj.p[i][k] > 127u ? dbx_fx.p[e][k] : dbx_fobj.p[i][k];
            dbx_fx_rates(e);
        }
        for (k = 0; k < DBX_NPAT; k++)
            for (i = 0; i < DBX_NFX; i++) {
                uint32_t e = old ? to[i] : i, s;
                dbx_seq.fx[k].on[e] = dbx_fobj.pat[k].on[i];
                for (s = 0; s < DBX_NSTEP; s++)
                    dbx_seq.fx[k].mix[e][s] = dbx_fobj.pat[k].mix[i][s] > 100u ? 100 : dbx_fobj.pat[k].mix[i][s];
            }
        if (dbx_fobj.magic != DBX_FX_MAGIC) {                                  /* an older save: the delay's and the reverb's amount moves from the */
            static const uint8_t E[2] = {7, 9}, OLD[2] = {50, 30}, MIX[2] = {64, 39};   /* steps (as placed: 50, 30) to the effect's MIX (50 %, 30 %: what those saves had) */
            for (i = 0; i < 2u; i++) {
                for (k = 0; k < DBX_NPAT; k++) {
                    uint32_t s;
                    for (s = 0; s < DBX_NSTEP; s++)
                        if (dbx_seq.fx[k].mix[E[i]][s] == OLD[i])
                            dbx_seq.fx[k].mix[E[i]][s] = 100;
                }
                if (dbx_fx.p[E[i]][FP_MIX] == 127u)
                    dbx_fx.p[E[i]][FP_MIX] = MIX[i];
            }
            dbx_store.pats |= 0x10u;
        }
        if (old)
            dbx_store.pats |= 0x10u;                                           /* (written again in the new order) */
    }
    {                                                                    /* the steps' own DECAY and TONE (DBXL); or the extras of before (DBXX, its 8 bytes of ENV) */
        int rc = st_load(OBJ_DBX_X, &dbx_lk, sizeof dbx_lk);
        if (!(rc == (int)sizeof dbx_lk && dbx_lk.magic == DBX_L_MAGIC)) {
            if (rc == 32 && dbx_lk.magic == DBX_X_MAGIC) {
                const uint8_t *e = (const uint8_t *)&dbx_lk + 4;
                for (i = 0; i < EV_NP; i++)
                    dbx_env.p[i] = e[i] <= EV_MAX[i] ? e[i] : EV_DEF[i];     /* (it goes into the patterns' sounds below) */
            }
            for (i = 0; i < DBX_NPAT * 2u * 88u; i++)
                ((uint8_t *)dbx_lk.d)[i] = 0;
            dbx_lk.magic = DBX_L_MAGIC;
        }
    }
    if (!(st_load(OBJ_DBX_K, &dbx_lk2, sizeof dbx_lk2) == (int)sizeof dbx_lk2 && dbx_lk2.magic == DBX_K_MAGIC)) {   /* the steps' own EXTRA and PAN (DBXK) */
        for (i = 0; i < DBX_NPAT * 2u * 88u; i++)
            ((uint8_t *)dbx_lk2.d)[i] = 0;
        dbx_lk2.magic = DBX_K_MAGIC;
    }
    {                                                                    /* the patterns' sounds: what is saved, or (an older save) the sound as it is, in all */
        uint32_t p;
        if (st_load(OBJ_UPFM6, &dbx_snap, DBX_SNAP_BYTES) != (int)DBX_SNAP_BYTES || dbx_snap.magic != DBX_S_MAGIC) {
            dbx_snap_take(&dbx_snap.s[0]);                               /* (no sounds: the sound as it is, in all 16 ... */
            for (p = 1; p < DBX_NPAT; p++)
                dbx_snap.s[p] = dbx_snap.s[0];
            dbx_snap.magic = DBX_S_MAGIC;                                /* ... not written until something is) */
            {                                                            /* ... but the older sounds, DBXT (200 bytes each) or DBXS (184), as they were */
                int rc = st_load(OBJ_UPFM6, &dbx_pobj, sizeof dbx_pobj);
                uint32_t mg = rc >= 4 ? *(uint32_t *)&dbx_pobj : 0u, entry = mg == DBX_S_MAGIC_T ? 200u : mg == DBX_S_MAGIC_S ? 184u : 0u;
                if (entry && rc == (int)(4u + entry * DBX_NPAT)) {
                    for (p = 0; p < DBX_NPAT; p++)
                        dbx_snap_from_old(&dbx_snap.s[p], (const uint8_t *)&dbx_pobj + 4u + entry * p, entry);
                    dbx_store.pats |= 0x40u;
                }
            }
        }
        dbx_snap_song_load();
        dbx_snap.live = 255;
        dbx_snap_sync();                                                 /* (the sound of the pattern shown: the live one) */
    }
    dbx_store.set = 0;
    dbx_store.pats = (uint8_t)(dbx_store.pats & 0x50u);        /* (only the converted effects and sounds wait to be saved) */
    dbx_store.t_ms = fm1_ms;
    dbx_store.saved_ms = fm1_ms - 5000u;                   /* (no "SAVED" at power-on) */
}

/* the main loop, each frame: one object a call, once the changes have been still for a moment */
static void dbx_store_poll(void)
{
    uint32_t i, k;
    int rc;
    if (dbx_store.now && !dbx_store.set && !dbx_store.pats) {      /* SAVE held, and all of it is written: that is it (the header says SAVED) */
        dbx_store.now = 0;
        dbx_store.saved_ms = fm1_ms;
        return;
    }
    if (dbx_seq.playing && !dbx_store.now) {               /* not while playing, unless SAVE is held; the wait starts again when it stops (the */
        if (dbx_store.set || dbx_store.pats)               /* last hits ring out before the flash stops the audio) */
            dbx_store.t_ms = fm1_ms;
        return;
    }
    if ((!dbx_store.set && !dbx_store.pats) || (!dbx_store.now && fm1_ms - dbx_store.t_ms < DBX_SAVE_WAIT_MS))
        return;
    if (dbx_store.set) {
        dbx_store.set = 0;
        dbx_sobj.magic = DBX_SET_MAGIC;
        dbx_sobj.bpm = dbx_seq.bpm;
        dbx_sobj.div = dbx_seq.div;
        dbx_sobj.vol = dbx_ui.vol;
        dbx_sobj.pe = dbx_seq.pe;
        dbx_sobj.swing = dbx_seq.swing;
        dbx_sobj.pad[0] = (uint8_t)(dbx_fx.master + 1u);
        dbx_sobj.pad[1] = dbx_kit;
        for (i = 0; i < DBX_NLANE; i++) {
            dbx_sobj.k[i].tune = lane[i].p.tune;
            dbx_sobj.k[i].decay = lane[i].p.decay;
            dbx_sobj.k[i].tone = lane[i].p.tone;
            dbx_sobj.k[i].extra = lane[i].p.extra;
            dbx_sobj.k[i].level = lane[i].p.level;
            dbx_sobj.k[i].mute = lane[i].mute;
            dbx_sobj.k[i].pad = i < 2u * LFO_NP ? (uint8_t)(dbx_lfo.p[i / LFO_NP][i % LFO_NP] + 1u) : 0u;
        }
        rc = st_save(OBJ_SETTINGS, &dbx_sobj, sizeof dbx_sobj);
    } else if (dbx_store.pats & 0x40u) {                   /* the 16 patterns' sounds (the live one into its pattern's first) */
        dbx_store.pats &= (uint8_t)~0x40u;
        dbx_snap_flush();
        dbx_snap.magic = DBX_S_MAGIC;
        rc = st_save(OBJ_UPFM6, &dbx_snap, DBX_SNAP_BYTES);
    } else if (dbx_store.pats & 0x80u) {                   /* the steps' own EXTRA and PAN */
        dbx_store.pats &= (uint8_t)~0x80u;
        dbx_lk2.magic = DBX_K_MAGIC;
        rc = st_save(OBJ_DBX_K, &dbx_lk2, sizeof dbx_lk2);
    } else if (dbx_store.pats & 0x20u) {                   /* the steps' own DECAY and TONE */
        dbx_store.pats &= (uint8_t)~0x20u;
        dbx_lk.magic = DBX_L_MAGIC;
        rc = st_save(OBJ_DBX_X, &dbx_lk, sizeof dbx_lk);
    } else if (dbx_store.pats & 0x10u) {                   /* the effects: their parameters and every pattern's steps */
        dbx_store.pats &= (uint8_t)~0x10u;
        dbx_fobj.magic = DBX_FX_MAGIC;
        dbx_fobj.pad = (uint32_t)dbx_mst.p[0] | ((uint32_t)(127 - dbx_mst.p[1]) << 8) | ((uint32_t)dbx_mst.p[2] << 16) |
                       ((uint32_t)dbx_mst.p[3] << 24);
        for (i = 0; i < DBX_NFX; i++)
            for (k = 0; k < DBX_NFXP; k++)
                dbx_fobj.p[i][k] = dbx_fx.p[i][k];
        for (k = 0; k < DBX_NPAT; k++)
            dbx_fobj.pat[k] = dbx_seq.fx[k];
        rc = st_save(OBJ_UPRESET0, &dbx_fobj, sizeof dbx_fobj);
    } else {
        for (k = 0; !((dbx_store.pats >> k) & 1u); k++)
            ;
        dbx_store.pats &= (uint8_t)~(1u << k);
        dbx_pobj.magic = DBX_PAT_MAGIC;
        dbx_pobj.pad = 0;
        for (i = 0; i < DBX_PAT_PER_OBJ; i++)
            dbx_pobj.p[i] = dbx_seq.pat[k * DBX_PAT_PER_OBJ + i];
        rc = st_save(OBJ_PROJECT0 + k, &dbx_pobj, sizeof dbx_pobj);
    }
    if (rc == 0)
        dbx_store.saved_ms = fm1_ms;
}

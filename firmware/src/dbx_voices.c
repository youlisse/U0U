/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (drum_voice.c, from which this is built) */
/* DrumBox voices: the 11 lanes, one per black key, played by Felucca's drum_voice.c.
 * A lane is one drum (mono: a new hit restarts it). The audio ISR renders them and sums them (dbx_voices_render);
 * the UI only sets parameters (lane[i].p) and calls dbx_hit. As eng_drum.c does, the coefficients are set up in the
 * render when a parameter moved (never from the main loop, which would race the ISR). */
#define DBX_NLANE 11u
#define DBX_NSTEP 16u
#define DBX_NKEY 27u                       /* the note keys: bit k of fm1_in.notes = F3 + k semitones */
#include "dbx_peaks.c"                    /* the Peaks drum models (the kits 808 and FM) */

enum { L_KICK, L_SNARE, L_CLAP, L_HATC, L_HATO, L_TOM, L_CONGA, L_RIM, L_CLAVE, L_BELL, L_CYM };
static const uint8_t DBX_TYPE[DBX_NLANE] = {DVT_PUNCH, DVT_SNARE, DVT_CLAP, DVT_HATC, DVT_HATO, DVT_TOM,
                                            DVT_CONGA, DVT_RIM, DVT_CLAVE, DVT_BELL, DVT_CYM};
static const char *const DBX_NAME[DBX_NLANE] = {"KICK", "SNARE", "CLAP", "HAT CL", "HAT OP", "TOM",
                                                "CONGA", "RIM", "CLAVE", "BELL", "CYM"};

/* The kits: CLASSIC (Felucca's own drums, STD in drum_voice.c; the lanes' types above), TIGHT (one of drum_voice.c's model kits, DV_KIT: every lane that kit's own
 * piece) and the kits of the other engines below. The list is what the KIT knob walks (dbx_kit is the place in it); each kit has an id that is what is saved
 * (so that a kit keeps its place in older saves and the list can grow): CLASSIC 0, TIGHT 7 (its DV_KIT number), then the new ones. The old model kits
 * (ANALOG MINI VINTAGE METAL DISCO DEEP PERC) are not listed any more: a save that has one starts on CLASSIC. A model kit has 8 lanes (KICK SNARE CLAP
 * HAT CL HAT OP TOM RIM BELL), some of them another drum (DBX_SWAP, as eng_drum.c's DK_SWAP: 1 its TOM is a conga, 2 its RIM claves, 4 its BELL a cymbal):
 * the lane of that name takes it, and the lane it leaves keeps Felucca's own drum. */
enum { KIT_CLASSIC = 0, KIT_TIGHT = 7, KIT_BREAK = 9, KIT_808 = 10, KIT_FM = 11 };
#define DBX_NKIT 4u
static const uint8_t DBX_KIT_ID[DBX_NKIT] = {KIT_808, KIT_CLASSIC, KIT_FM, KIT_TIGHT};      /* (the order the KIT knob walks) */
static const char *const DBX_KIT_NAME[DBX_NKIT] = {"808", "CLASSIC", "FM", "TIGHT"};
static uint32_t dbx_kit_pos(uint32_t id)                 /* the place of a saved id in the list (an id that is not listed: CLASSIC's place, which is where a fresh start and a reset are too) */
{
    uint32_t k;
    for (k = 0; k < DBX_NKIT; k++)
        if (DBX_KIT_ID[k] == id)
            return k;
    return id == KIT_CLASSIC ? 0u : dbx_kit_pos(KIT_CLASSIC);
}
static const uint8_t DBX_SWAP[DV_NKIT] = {0, 4, 1, 0, 2 | 4, 4, 0, 1 | 2};

/* The kits 808 and FM: lanes played by dbx_peaks.c (m: PKM_* + 1; 0: the lane keeps Felucca's own drum), with the lane's note as designed, DECAY TONE and
 * EXTRA as the kit starts them, and gain (Q12, at LEVEL 100: evens the models out with the other drums). The 808: Peaks' bass drum (the kick, and the toms
 * tuned up and short), snare and hi-hat (the closed one, the open one, the cymbal a very long one); the FM: Peaks' FM drum all over, its hats the 808's. */
typedef struct { uint8_t m, decay, tone, extra; int16_t note16; uint16_t gain; } dbx_pkl_t;
static const dbx_pkl_t DBX_PK_KIT[2][DBX_NLANE] = {
    {   /* 808 */
        {1 + PKM_BD, 62, 64, 96, 31 * 16, 2500}, {1 + PKM_SD, 64, 64, 64, 52 * 16, 4500}, {0, 0, 0, 0, 0, 0},
        {1 + PKM_HH, 24, 64, 0, 0, 7500}, {1 + PKM_HH, 72, 64, 0, 0, 4000}, {1 + PKM_BD, 40, 90, 40, 40 * 16, 4200},
        {1 + PKM_BD, 24, 110, 30, 50 * 16, 5000}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0},
        {1 + PKM_HH, 120, 40, 0, 0, 2600}},
    {   /* FM */
        {1 + PKM_FM, 70, 70, 64, 26 * 16, 2000}, {1 + PKM_FM, 45, 100, 105, 60 * 16, 2550}, {0, 0, 0, 0, 0, 0},
        {1 + PKM_HH, 20, 100, 0, 0, 9600}, {1 + PKM_HH, 64, 100, 0, 0, 4400}, {1 + PKM_FM, 65, 80, 64, 45 * 16, 2550},
        {1 + PKM_FM, 40, 60, 64, 56 * 16, 3600}, {1 + PKM_FM, 8, 120, 90, 84 * 16, 2250}, {1 + PKM_FM, 5, 30, 64, 79 * 16, 3000},
        {1 + PKM_FM, 70, 127, 64, 77 * 16, 1100}, {1 + PKM_HH, 110, 100, 0, 0, 2200}},
};
/* the names of a Peaks model's knobs on the lane's second page: EXTRA, then the two of its own (BD SD HH FM) */
static const char *const DBX_PK_X[4] = {"PUNCH", "SNAPPY", "SIZZLE", "NOISE"};
static const char *const DBX_PK_P3[4] = {"CHIRP", "BODY", "BRIGHT", "FEEDBK"};
static const char *const DBX_PK_P4[4] = {"CLICK", "NOISE", "METAL", "SWEEP"};
static const dbx_pkl_t *dbx_pk_of(uint32_t pos, uint32_t i)   /* the Peaks lane of the kit at that place, or 0 */
{
    uint32_t id = DBX_KIT_ID[pos];
    return (id == KIT_808 || id == KIT_FM) && DBX_PK_KIT[id - KIT_808][i].m ? &DBX_PK_KIT[id - KIT_808][i] : 0;
}
static uint32_t dbx_type_of(uint32_t i, uint32_t pos)   /* the drum lane i plays in the kit at that place (id 0: STD): a DVT_*, or a model kit's DV_KTYPE */
{
    uint32_t kit = DBX_KIT_ID[pos], sw = kit && kit <= DV_NKIT ? DBX_SWAP[kit - 1u] : 0u, kl;
    switch (i) {
    case L_KICK:  kl = 0; break;
    case L_SNARE: kl = 1; break;
    case L_CLAP:  kl = 2; break;
    case L_HATC:  kl = 3; break;
    case L_HATO:  kl = 4; break;
    case L_TOM:   kl = sw & 1u ? 8u : 5u; break;
    case L_CONGA: kl = sw & 1u ? 5u : 8u; break;
    case L_RIM:   kl = sw & 2u ? 8u : 6u; break;
    case L_CLAVE: kl = sw & 2u ? 6u : 8u; break;
    case L_BELL:  kl = sw & 4u ? 8u : 7u; break;
    default:      kl = sw & 4u ? 7u : 8u; break;       /* L_CYM */
    }
    return kit && kit <= DV_NKIT && kl < 8u ? DV_KTYPE(kit, kl) : DBX_TYPE[i];     /* (the other engines' kits: the lane's own type, for its pitch range and name) */
}

/* The keys from F3: the 11 black ones are the lanes, the 16 white ones the steps (-1: the other kind) */
static const int8_t DBX_LANE_OF_KEY[DBX_NKEY] = {-1, 0, -1, 1, -1, 2, -1, -1, 3, -1, 4, -1, -1, 5,
                                                 -1, 6, -1, 7, -1, -1, 8, -1, 9, -1, -1, 10, -1};
static const int8_t DBX_STEP_OF_KEY[DBX_NKEY] = {0, -1, 1, -1, 2, -1, 3, 4, -1, 5, -1, 6, 7, -1,
                                                 8, -1, 9, -1, 10, 11, -1, 12, -1, 13, 14, -1, 15};
static uint8_t DBX_KEY_OF_LANE[DBX_NLANE], DBX_KEY_OF_STEP[DBX_NSTEP];   /* the inverse, from dbx_voices_init */
static uint32_t DBX_WHITE_MASK;                                          /* bit k of fm1_in.notes: the 16 white keys */

typedef struct { int8_t ld, lt, le, lp; } dbx_hk_t;   /* a hit's own settings (its step's, EDIT 2), added to the lane's: DECAY, TONE, EXTRA, PAN */
static const dbx_hk_t DBX_HK0 = {0, 0, 0, 0};

typedef struct {
    dv_param_t p, key;                     /* wanted, and as dv_setup has them */
    dv_coef_t c;
    dv_voice_t v;
    dv_metal_t mb;                         /* the lane's metal source (hats, cymbal) */
    uint16_t vg;                           /* the last hit's velocity as a gain, Q12 (dbx_vel_gain) */
    int8_t semi;                           /* the last hit's pitch, in semitones from the lane's TUNE (a step's PITCH) */
    uint8_t mute;
    int8_t pan;                            /* -32 (left) .. +32 (right): the lane's place in the stereo field (ALGORITHM, EDIT page 2) */
    dbx_hk_t hk;                           /* the last hit's own settings (its step's, EDIT 2): added to the lane's */
    uint8_t choked;                        /* a hit of its group has cut it: it fades out in two blocks (1.5 ms) and stops */
    int16_t cg;                            /* ... and the gain of that fade, Q12 */
    uint8_t choke;                         /* its choke group, 0 (none) or 1: a hit cuts the voices of the others of its group (the two hats; no page sets it) */
    uint32_t age;                          /* samples since the last hit (the ENV page's attack and gate follow it) */
    uint8_t pkm;                           /* the kits 808 and FM: the Peaks model the lane plays + 1 (dbx_peaks.c), 0: its own drum */
    uint8_t px;                            /* ... and its two parameters of the lane's second page, a nibble each (the low one the third knob, the high one the fourth): 8 and 8 as Peaks has it */
    pk_t pk;
    uint8_t smp;                           /* the kit BREAK: the lane plays its slice of the break, not drum_voice.c */
    uint32_t bpos;                         /* ... where in the slice, in source samples, Q16 */
    int32_t blp;                           /* ... and the TONE's low-pass */
} dbx_lane_t;

#define DBX_VEL_DEF 100u                   /* the velocity of a step as it is placed */
static inline uint32_t dbx_vel_gain(uint32_t v) { return (v * v * 416u) >> 10; }   /* v 0..127: Q12, 100 -> 4062 */

static const uint8_t DBX_CHOKE_DEF[DBX_NLANE] = {0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0};     /* the two hats are a group; the others have none */
static dbx_lane_t lane[DBX_NLANE];
static int32_t dbx_metal[CTL];
static uint8_t dbx_kit;                    /* the kit wanted (the UI; 0: STD) */
static volatile uint8_t dbx_kit_run = 255; /* the kit the lanes are set up for (the render takes the change up, never the UI: the voices are
                                            * the ISR's); 255: none yet */
static volatile uint8_t dbx_kit_say;       /* the UI wants the kick heard in the new kit (when stopped) */
static void dbx_hit(uint32_t i, uint32_t vel, int32_t semi);
static void dbx_hit_lk(uint32_t i, uint32_t vel, int32_t semi, const dbx_hk_t *k);

/* every lane takes the drum of a kit, with that drum's designed sound (LEVEL and the mute are kept) */
static volatile uint8_t dbx_kit_reset;     /* the UI asks for the lanes to start over (HOME held): the kit's drums as designed, LEVEL 100, no mute */
static void dbx_kit_apply(uint32_t kit, uint32_t reset)
{
    uint32_t i;
    for (i = 0; i < DBX_NLANE; i++) {
        dbx_lane_t *l = &lane[i];
        uint8_t lvl = l->p.level;
        uint32_t t = dbx_type_of(i, kit);
        dv_default(&l->p, t);
        const dbx_pkl_t *pk = dbx_pk_of(kit, i);
        l->pkm = pk ? pk->m : 0u;
        l->px = 0x88;
        if (pk) {                          /* 808, FM: a Peaks model (the lane's drum only gives its range and its name) */
            pk_init(&l->pk, (uint32_t)pk->m - 1u);
            l->p.decay = pk->decay;
            l->p.tone = pk->tone;
            l->p.extra = pk->extra;
        }
        l->smp = DBX_KIT_ID[kit] == KIT_BREAK;
        if (l->smp) {                      /* BREAK: the whole slice, bright, from its start (the lane's drum only gives its pitch range and name) */
            l->p.decay = 127;
            l->p.tone = 127;
            l->p.extra = 0;
        }
        if (!reset && dbx_kit_run != 255u)
            l->p.level = lvl;
        if (reset) {
            l->mute = 0;
            l->pan = 0;
            l->choke = DBX_CHOKE_DEF[i];
        }
        l->key = l->p;
        l->key.type = 0xFFu;               /* never equal: the next hit sets the lane up */
        dv_init(&l->v, t);
    }
}

static void dbx_voices_init(void)
{
    uint32_t i, k;
    for (k = 0; k < DBX_NKEY; k++) {
        if (DBX_LANE_OF_KEY[k] >= 0)
            DBX_KEY_OF_LANE[DBX_LANE_OF_KEY[k]] = (uint8_t)k;
        if (DBX_STEP_OF_KEY[k] >= 0) {
            DBX_KEY_OF_STEP[DBX_STEP_OF_KEY[k]] = (uint8_t)k;
            DBX_WHITE_MASK |= 1u << k;
        }
    }
    for (i = 0; i < DBX_NLANE; i++) {
        dbx_lane_t *l = &lane[i];
        dv_default(&l->p, DBX_TYPE[i]);
        l->key = l->p;
        l->key.type = 0xFFu;               /* never equal: the first hit sets the lane up */
        dv_init(&l->v, DBX_TYPE[i]);
        dbx_kit = (uint8_t)dbx_kit_pos(KIT_CLASSIC);
        dbx_kit_run = dbx_kit;
        l->vg = dbx_vel_gain(DBX_VEL_DEF);
        l->px = 0x88;
        l->semi = 0;
        l->mute = 0;
        l->pan = 0;
        l->choke = DBX_CHOKE_DEF[i];
    }
}

/* ---- ENV: the DUCKING of the sound by one lane's hits (page 1), and an attack, a decay and a gate for every lane (page 2).
 * Page 1: AMOUNT (0 to 100 %), RELEASE (30 ms to 0.9 s), SOURCE (the lane whose hits duck) and TARGET (EFFECTS: the wet sound of FLANGER, DELAY,
 *   REPEAT and REVERB; DRY: every lane but the source; BOTH). Each hit of the source that really plays (chance, subdivisions and pitch have had their
 *   say: dbx_hit) takes the gain down by AMOUNT (a hit softer than a step's first velocity, 100, by less), and it comes back in a straight line over RELEASE.
 * Page 2: KICK ATTACK (0: as designed; up: the kick's first 6 ms louder, to x3), ATTACK (64: as designed; below, every lane fades in over up to
 *   20 ms; above, its first 8 ms are louder, to x2), DECAY (64: as designed; each lane's own DECAY moved by the difference), GATE (127: off;
 *   below, a hit is cut after 20 ms to 5 s). Left alone (the defaults) nothing is computed: the sound is as it was. */
enum { EV_AMT, EV_REL, EV_SRC, EV_TGT, EV_KATK, EV_ATK, EV_DEC, EV_GATE, EV_NP };
enum { EVT_FX, EVT_DRY, EVT_BOTH };
static const char *const EV_TGT_NAME[3] = {"EFFECTS", "DRY", "BOTH"};
static const uint8_t EV_DEF[EV_NP] = {0, 50, 0, EVT_BOTH, 0, 64, 64, 127};     /* (the duck's TARGET starts on BOTH: the effects and the dry sound) */
static const uint8_t EV_MAX[EV_NP] = {100, 127, DBX_NLANE - 1u, 2, 127, 127, 127, 127};
static struct {
    uint8_t p[EV_NP];                      /* the settings (the UI) */
    volatile uint8_t trig;                 /* the source's last hit's velocity, not taken yet (the hits and the tick are the ISR's) */
    uint8_t shape;                         /* an attack or a gate is set (this block) */
    int32_t env;                           /* the duck, Q16: 65536 just after a full hit */
    int32_t g0, g1;                        /* the duck's gain at the start and at the end of this block, Q12 (4096: none) */
} dbx_env = {{0, 50, 0, EVT_BOTH, 0, 64, 64, 127}, 0, 0, 0, 4096, 4096};

static uint32_t dbx_env_rel_ms(uint32_t v) { return (30u << (v / 26u)) * (26u + v % 26u) / 26u; }          /* 30 ms .. 905 ms */
static uint32_t dbx_env_gate_samples(uint32_t v) { return (882u << (v >> 4)) * (16u + (v & 15u)) / 16u; }  /* 20 ms .. 4.8 s */

/* once a block, first: the source's hit ducks, the duck comes back, the gains of this block */
static void dbx_env_tick(uint32_t n)
{
    int32_t rel = (int32_t)(dbx_env_rel_ms(dbx_env.p[EV_REL]) * 441u / 10u);
    uint32_t t = dbx_env.trig;
    dbx_env.g0 = dbx_env.g1;
    if (t) {
        dbx_env.trig = 0;
        dbx_env.env = t >= DBX_VEL_DEF ? 65536 : (int32_t)(t * 65536u / DBX_VEL_DEF);     /* (a hit at the velocity a step starts with, or above: all of AMOUNT; softer, less) */
    }
    dbx_env.g1 = 4096 - (int32_t)((uint32_t)dbx_env.p[EV_AMT] * (uint32_t)(dbx_env.env >> 4) / 100u);
    dbx_env.env -= (int32_t)(65536u * n) / (rel < 1 ? 1 : rel);
    if (dbx_env.env < 0)
        dbx_env.env = 0;
    dbx_env.shape = dbx_env.p[EV_ATK] != 64u || dbx_env.p[EV_KATK] != 0u || dbx_env.p[EV_GATE] != 127u;
}

/* the gain of lane i's hit, age samples after it began, from the attack, the kick's attack and the gate (Q12) */
static int32_t dbx_env_shape(uint32_t i, uint32_t age)
{
    int32_t g = 4096, a = dbx_env.p[EV_ATK], k = i == L_KICK ? dbx_env.p[EV_KATK] : 0, ga = dbx_env.p[EV_GATE];
    if (a < 64) {
        uint32_t ta = (uint32_t)(64 - a) * 14u;
        if (age < ta)
            g = (int32_t)(4096u * age / ta);
    } else if (a > 64 && age < 353u)
        g += (int32_t)((uint32_t)(a - 64) * 65u * (353u - age) / 353u);
    if (k && age < 265u)
        g = g * (int32_t)(4096u + (uint32_t)k * 64u * (265u - age) / 265u) >> 12;
    if (ga < 127) {
        uint32_t tg = dbx_env_gate_samples((uint32_t)ga);
        if (age >= tg + 132u)
            g = 0;
        else if (age >= tg)
            g = g * (int32_t)(tg + 132u - age) / 132;
    }
    return g;
}

/* A hit on lane i at velocity 1..127 and a pitch of semi semitones from the lane's TUNE (the step clock, or the
 * control thread: only the gain, the pitch and the trigger are written). The velocity is a gain on the whole hit,
 * square law, 100 = the drum as designed (up to +4 dB at 127). */
static void dbx_hit_lk(uint32_t i, uint32_t vel, int32_t semi, const dbx_hk_t *k)
{
    dbx_lane_t *l = &lane[i];
    l->vg = dbx_vel_gain(vel);
    l->semi = (int8_t)semi;
    l->hk = *k;
    l->age = 0;
    l->choked = 0;                         /* (a new hit is sounding again) */
    l->cg = 4096;
    if (i == dbx_env.p[EV_SRC] && dbx_env.p[EV_AMT])
        dbx_env.trig = (uint8_t)vel;       /* (the duck) */
    if (l->choke) {                        /* a hit cuts the voices of the others of its group (the hats: a closed hat stops the open one, and the other way) */
        uint32_t j;
        for (j = 0; j < DBX_NLANE; j++)
            if (j != i && lane[j].choke == l->choke) {
                dv_choke(&lane[j].v);          /* (the hats' own short release) ... */
                lane[j].choked = 1;            /* ... and for every drum a fade-out of the lane itself */
            }
    }
    dv_trigger(&l->v);
}

static void dbx_hit(uint32_t i, uint32_t vel, int32_t semi) { dbx_hit_lk(i, vel, semi, &DBX_HK0); }      /* (a hit that is not a step's: no DECAY or TONE of its own) */

static int dbx_param_same(const dv_param_t *a, const dv_param_t *b)
{
    return a->type == b->type && a->decay == b->decay && a->tone == b->tone && a->extra == b->extra &&
           a->level == b->level && a->accent == b->accent && a->tune == b->tune;
}

/* a lane's pan as two gains, Q12: a balance, the near side and the middle at unity, the far side a quarter of a cosine down */
static void dbx_pan_gains(int32_t p, int32_t *gl, int32_t *gr)
{
    *gl = *gr = 4096;
    if (p > 0)
        *gl = SINE[256 + (p > 32 ? 32 : p) * 8] >> 3;
    else if (p < 0)
        *gr = SINE[256 + (p < -32 ? 32 : -p) * 8] >> 3;
}

/* ---- BREAK: the kit whose lanes play the slices of a break (dbx_break.h: tools/gen_break.py), 22.05 kHz, 16 bit, read from flash.
 * TUNE is a varispeed (1/16 semitone a step, from the break's own pitch: a pitch and a speed together, as a sampler's), DECAY how much of the slice plays (127: all of
 * it, then a fade of 64 source samples), TONE a low-pass (127: none), EXTRA where in the slice it starts (0: the start, 127: half way in), LEVEL and the accent as
 * for the other drums. A hit restarts the slice; the choke, the velocity, the pan, the ENV page and the effects are the lane's, as for any drum. */
#include "dbx_break.h"

static void brk_run(dbx_lane_t *l, uint32_t i, const dv_param_t *p, int32_t *y, uint32_t n)
{
    const int16_t *s = BRK_PCM + BRK_SLICE[i].off;
    uint32_t len = BRK_SLICE[i].len, end = len * ((uint32_t)p->decay + 1u) / 128u, k;
    int32_t out = (int32_t)((uint32_t)p->level * 4096u / 100u), tk = ((int32_t)p->tone + 1) * 32;     /* the low-pass's coefficient, Q12 (4096: none) */
    uint32_t step = dv_exp2((uint32_t)(p->tune * 4096 / 192 + 8192)) >> 3;                              /* source samples a sample out, Q16: 2^(tune / 192) x 22050 / 44100 */
    if (l->v.trig) {
        l->v.trig = 0;
        l->v.live = 1;
        l->bpos = (len >> 1) * (uint32_t)p->extra / 127u << 16;
        l->blp = 0;
    }
    out += (out * (int32_t)p->accent * 75) >> 14;
    end = end < 64u ? 64u : end;
    for (k = 0; k < n; k++) {
        uint32_t ip = l->bpos >> 16;
        int32_t x = 0;
        if (l->v.live && ip + 1u < end && ip + 1u < len) {
            int32_t a = s[ip], b = s[ip + 1u], fr = (int32_t)((l->bpos >> 4) & 4095u);
            x = a + (((b - a) * fr) >> 12);
            if (end - ip < 64u)
                x = x * (int32_t)(end - ip) >> 6;
            if (tk < 4096)
                x = l->blp += ((x - l->blp) * tk) >> 12;
            l->bpos += step;
        } else
            l->v.live = 0;
        y[k] = (x * out) >> 12;
    }
}

/* adds the lanes' block (n <= CTL) into mix (the middle: what the mono effects take) and side (what a pan adds: left = mix + side, right = mix - side), Q15 */
static void dbx_voices_render(int32_t *mix, int32_t *side, uint32_t n)
{
    int32_t y[CTL], o[CTL];
    uint32_t i, k;
    if (dbx_kit_run != dbx_kit || dbx_kit_reset) {          /* a kit was picked (or a reset asked): the lanes change drums, and the kick says how it sounds */
        uint32_t rs = dbx_kit_reset;
        dbx_kit_reset = 0;
        dbx_kit_run = dbx_kit;
        dbx_kit_apply(dbx_kit, rs);
        if (dbx_kit_say) {
            dbx_kit_say = 0;
            dbx_hit(L_KICK, DBX_VEL_DEF, 0);
        }
    }
    dbx_env_tick(n);
    for (i = 0; i < DBX_NLANE; i++) {
        dbx_lane_t *l = &lane[i];
        dv_param_t pe;
        if (!l->v.live && !l->v.trig)
            continue;                      /* rung out and not struck: nothing to compute */
        pe = l->p;
        pe.tune = (int16_t)(pe.tune + l->semi * 16);       /* the hit's own pitch (dv_setup holds it in the type's range) */
        if (dbx_env.p[EV_DEC] != 64u || l->hk.ld)          /* ENV: every lane's DECAY moved, and the step's own */
            pe.decay = (uint8_t)clamp((int32_t)pe.decay + (int32_t)dbx_env.p[EV_DEC] - 64 + l->hk.ld, 0, 127);
        if (l->hk.lt)
            pe.tone = (uint8_t)clamp((int32_t)pe.tone + l->hk.lt, 0, 127);
        if (l->hk.le)
            pe.extra = (uint8_t)clamp((int32_t)pe.extra + l->hk.le, 0, 127);
        if (!dbx_param_same(&pe, &l->key)) {
            dv_setup(&l->c, &pe);          /* (a parameter moved, a hit of another pitch, or the first hit) */
            l->key = pe;
            if (dv_uses_metal(l->c.type))
                dv_metal_tune(&l->mb, &l->c);
        }
        if (l->pkm) {
            const dbx_pkl_t *pk = dbx_pk_of(dbx_kit_run, i);
            pk_par_t pp;
            int32_t out;
            uint32_t k2;
            if (l->v.trig) {
                l->v.trig = 0;
                l->pk.trig = 1;
            }
            pp.note16 = (pk ? pk->note16 : 0) + pe.tune;
            pp.decay = pe.decay;
            pp.tone = pe.tone;
            pp.extra = pe.extra;
            pp.p3 = l->px & 15;
            pp.p4 = l->px >> 4;
            pk_run(&l->pk, &pp, y, n);
            l->v.live = l->pk.live;
            out = (int32_t)((pk ? pk->gain : 0u) * (uint32_t)pe.level / 100u);
            out += (out * (int32_t)pe.accent * 75) >> 14;
            for (k2 = 0; k2 < n; k2++)
                y[k2] = (y[k2] * out) >> 12;
        } else if (l->smp)
            brk_run(l, i, &pe, y, n);
        else {
            if (dv_uses_metal(l->c.type))
                dv_metal_run(&l->mb, dbx_metal, n);
            dv_run(&l->c, &l->v, dbx_metal, y, n);
        }
        if (!l->mute) {
            int32_t ga = 4096, gb = 4096, pan;
            if (l->choked) {                                   /* a choke: down to nothing in two blocks */
                int32_t c1 = l->cg - 2048 > 0 ? l->cg - 2048 : 0;
                ga = l->cg;
                gb = c1;
                l->cg = (int16_t)c1;
                if (c1 == 0) {
                    l->choked = 0;
                    l->v.live = 0;                             /* (and the voice is done) */
                    l->v.trig = 0;
                    l->pk.live = 0;
                }
            }
            if (dbx_env.shape) {                               /* ENV: the attack and the gate */
                ga = ga * dbx_env_shape(i, l->age) >> 12;
                gb = gb * dbx_env_shape(i, l->age + n) >> 12;
            }
            if (dbx_env.p[EV_TGT] != EVT_FX && i != dbx_env.p[EV_SRC] && (dbx_env.g0 != 4096 || dbx_env.g1 != 4096)) {   /* ... and the duck */
                ga = ga * dbx_env.g0 >> 12;
                gb = gb * dbx_env.g1 >> 12;
            }
            if (ga == 4096 && gb == 4096)
                for (k = 0; k < n; k++)
                    o[k] = (y[k] * (int32_t)l->vg) >> 12;           /* (|y| <= 32179, vg <= 6600: fits 32 bits) */
            else
                for (k = 0; k < n; k++) {
                    int32_t g = ga + (gb - ga) * (int32_t)(k + 1u) / (int32_t)n;
                    o[k] = ((y[k] * g >> 12) * (int32_t)l->vg) >> 12;     /* (|y| x g <= 32179 x 12224: 32 bits) */
                }
            pan = clamp((int32_t)l->pan + l->hk.lp, -32, 32);   /* (the lane's, and the step's own) */
            if (pan == 0)
                for (k = 0; k < n; k++)
                    mix[k] += o[k];
            else {                                             /* a pan: the middle goes to the effects, the difference of the sides rejoins after the mono ones */
                int32_t gl, gr;
                dbx_pan_gains(pan, &gl, &gr);
                for (k = 0; k < n; k++) {
                    int32_t a = o[k] * gl >> 12, b = o[k] * gr >> 12;      /* (|o| < 2^18: 32 bits) */
                    mix[k] += (a + b) >> 1;
                    side[k] += (a - b) >> 1;
                }
            }
        }
        if (l->age < 0x40000000u)
            l->age += n;
    }
}

/* The lane's pitch as a note. drum_voice.c keeps it as a TUNE offset (1/16 semitone) from the type's designed pitch
 * (DV_DEF p16 = 16 x MIDI note), held in the type's range lo..hi (a model kit's lane: its own designed pitch, +-12 semitones).
 * The UI shows and steps the note itself. */
static void dbx_range(uint32_t t, int32_t *p16, int32_t *lo, int32_t *hi)   /* a type's designed pitch and its range */
{
    if (DV_KITOF(t) != 0u) {
        *p16 = DV_KIT[DV_KITOF(t) - 1u][t & 7u].p16;
        *lo = *p16 - 192;
        *hi = *p16 + 192;
    } else {
        *p16 = DV_DEF[t].p16;
        *lo = DV_DEF[t].lo;
        *hi = DV_DEF[t].hi;
    }
}

static int32_t dbx_note16(uint32_t i)                   /* the pitch now, 1/16 semitone (16 x MIDI note) */
{
    int32_t p16, lo, hi;
    dbx_range(lane[i].p.type, &p16, &lo, &hi);
    return clamp(p16 + lane[i].p.tune, lo, hi);
}

/* s clicks of the knob: whole semitones; from a pitch between two notes the first click snaps to the next one */
static void dbx_note_step(uint32_t i, int32_t s)
{
    int32_t p16, rlo, rhi, n = dbx_note16(i), lo, hi;
    dbx_range(lane[i].p.type, &p16, &rlo, &rhi);
    lo = (rlo + 15) / 16 * 16;
    hi = rhi / 16 * 16;
    if (n % 16) {
        n = s > 0 ? (n + 15) / 16 * 16 : n / 16 * 16;
        s += s > 0 ? -1 : 1;
    }
    n = clamp(n + s * 16, lo, hi);
    lane[i].p.tune = (int16_t)(n - p16);
}

/* TUNE of a lane: the kick is set as a note (dbx_note_step); the others keep the offset from the designed pitch,
 * a quarter semitone a click, held where the voice can go so that what the screen shows is what plays */
static void dbx_tune_range(uint32_t i, int32_t *lo, int32_t *hi)
{
    int32_t p16, rlo, rhi;
    dbx_range(lane[i].p.type, &p16, &rlo, &rhi);
    *lo = clamp(rlo - p16, -192, 0);
    *hi = clamp(rhi - p16, 0, 192);
}

static void dbx_tune_step(uint32_t i, int32_t s)
{
    int32_t lo, hi;
    if (i == L_KICK) {
        dbx_note_step(i, s);
        return;
    }
    dbx_tune_range(i, &lo, &hi);
    lane[i].p.tune = (int16_t)clamp(lane[i].p.tune + s * 4, lo, hi);
}

/* the pitch a hit plays at (for the tests): the type's designed pitch, TUNE and the hit's semitones, in its range */
static int32_t dbx_eff16(uint32_t i)
{
    int32_t p16, lo, hi;
    dbx_range(lane[i].p.type, &p16, &lo, &hi);
    return clamp(p16 + lane[i].p.tune + lane[i].semi * 16, lo, hi);
}

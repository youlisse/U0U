/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (gfx.c and the HAL it draws on) */
/* DrumBox UI, M1.
 *   black key      selects the lane for editing (11 lanes); it makes no sound
 *   white key      toggles that step (1..16) of the selected lane; a step turned on while stopped plays the drum
 *   PLAY           starts / stops the pattern
 *   SELECT         the velocity of the step just placed; on the FX pages the mix of the effect's step
 *   ALGORITHM      selects the previous / next lane
 *   PRESETS        scrolls through the steps already placed on the lane: each becomes the one SELECT / EDIT work on
 *   KNOB 1..4      TUNE (the kick: a note; the others: semitones), DECAY, TONE, LEVEL of the selected lane
 *   GLO            the global pages (a tap opens the first, the next the second, the next closes): 1: KNOB 1 tempo,
 *                  KNOB 2 volume, KNOB 3 PC clock on / off, KNOB 4 subdivision; 2: KNOB 1 swing, KNOB 2 FX mix and
 *                  KNOB 3 kit (STD and 8 model kits), KNOB 4 reset (a full turn, a warning from half: the pattern shown and every parameter of the sound)
 *   EDIT           the step page (tap to open, tap again to close): a white key picks the step (a step that is off is
 *                  placed); KNOB 1 NUDGE (early / late), KNOB 2 SUBDIVISION (1..8 hits), KNOB 3 CHANCE, KNOB 4 PITCH.
 *                  Pressing the picked step again turns it off. EDIT held 0.6 s clears the selected lane.
 *   FX             the effect pages (a tap opens the first, the next the second, the next closes): the black keys pick one
 *                  of the 11 effects, the white keys its 16 steps; KNOB 1..4 its four parameters, on the second page
 *                  ATTACK, RELEASE, CONTINUOUS (on at every step) and MIX; ALGORITHM picks the effect too
 *   SCL            the master page (a tap opens it, a tap closes it): KNOB 1 HPF (off at the left), KNOB 2 LPF (off at the
 *                  right), KNOB 3 OTT dry / wet, KNOB 4 the gain into a soft limiter / clipper
 *   SAVE           the pattern page (tap to open, tap again to close): the 16 white keys are the 16 patterns; a pattern
 *                  picked while playing plays from the next pass; one key held and another pressed copies the held
 *                  pattern to the other; EDIT held 0.6 s deletes the pattern shown (the next ones move down).
 *                  What you change is saved a moment after, never while playing.
 *   HOME           closes a page; held 0.6 s it clears the whole pattern shown
 *   MASTER         volume (the pot)
 * The LEDs: the selected lane's black key; the white keys of its steps (the playhead inverts its key); PLAY's green
 * while playing; GLO or EDIT while their page is open. */
#define DBX_HOLD_MS 600u                   /* how long EDIT / HOME are held to clear */
#define DIRTY_ALL 1u
#define DIRTY_LANE 2u                      /* the page below the grid (no full clear: the panel takes ~80 ms for a whole
                                            * screen) */
enum { PAGE_LANE, PAGE_GLO, PAGE_EDIT, PAGE_PAT, PAGE_GLO2, PAGE_FX1, PAGE_FX2, PAGE_SCL, PAGE_LFO1, PAGE_LFO2, PAGE_ENV1, PAGE_ENV2, PAGE_ARP1, PAGE_ARP2, PAGE_EDIT2, PAGE_EDIT3 /* (gone into EDIT 2: its number stays) */, PAGE_SONG, PAGE_HELP };
#define DBX_RED_LIGHT 0xFB6Du                   /* ERASE: a light red of its own, in every palette (255, 110, 110) ... */
#define DBX_RED_WARN 0xF9E7u                    /* ... and a stronger one from half the turn (255, 60, 60) */
#define DBX_GREEN 0xC7B1u                       /* #c5f78b (197, 247, 139): the colour of everything that is shown (the text, the values,
                                                 * the bars, the wave) */
#define DBX_RED_PLAY 0xFA28u                    /* #ff4646 (255, 70, 70): the cell playing */
#define DBX_VIOLET 0xB47Fu                      /* #b48cff (180, 140, 255): what moves: PLAY, a pattern waiting, a mark */
#define DBX_FW_NAME "U0U"                        /* the firmware's name, and under it the boot screen's two lines: */
#define DBX_FW_BY "By"
#define DBX_FW_WHO "@Y_oulisse"
#define DBX_SPLASH_MS 900u                      /* ... shown from power-on for this long (the keys and knobs work under it) */
static uint32_t dbx_ms0;                                /* ... counted from here: 0 in the browser; on the FM-1 fm1_ms already holds the boot (dbx_main.c sets it) */
#define DBX_ERASE_CLICKS 20u                   /* a full turn of a knob (the page draws 20 detents a turn); the warning is from half
                                                 * of it, and it is forgotten after 1.5 s without a click */

/* the palette: near black, the green for everything shown, the violet (the "accent") for what moves. The panels are the
 * black with a twelfth of the green; the grey labels and rules the palette derives are the green's too. */
static void dbx_palette(void)
{
    ux_cust = (ui_pal_t){"U0U", 0x0841, 0x1923, DBX_GREEN, DBX_GREEN, DBX_VIOLET};
    palette_set(NPALETTES);
}

static uint32_t dbx_save_t;                             /* SAVE: when it went down */
static uint8_t dbx_save_done;                           /* ... and its hold was taken */
static uint32_t dbx_glo_t;                              /* GLO: when it went down (held a quarter of a second, the mixer shows) */
static uint8_t dbx_glo_used;                            /* GLO: a key was pressed while it was held (mute, solo), so its letting go is not a tap */
static uint32_t dbx_mute_t[DBX_NLANE];                  /* a lane's key pressed with GLO held: when ... */
static uint8_t dbx_mute_st[DBX_NLANE];                  /* ... and where it is: 1 waiting (let go: mute; held 0.5 s: solo), 2 solo done */
static uint32_t dbx_seq_t;                              /* SEQ: when it went down (a tap is the help, held a quarter of a second the loop) */
static uint8_t dbx_seq_used;                            /* ... a knob was turned with it: not a tap */
static uint8_t dbx_help;                                /* the help page shown, 0..2 */
static uint8_t dbx_rec;                                 /* REC is armed: the black keys sound, and while playing they are written */

static struct {
    uint8_t sel;                           /* the selected lane */
    uint8_t dirty;
    uint8_t page;                          /* PAGE_*: what the rows below the grid show */
    uint8_t vol;                           /* the global volume 0..127 (100: as designed) */
    uint8_t t_on, t_lane, t_step;          /* the step just placed or picked: SELECT's and the EDIT page's target */
    uint32_t sig, psig;                    /* what the grid and the header showed last; what the pattern page did */
    int32_t master_knob;                   /* the MASTER pot x16, smoothed */
    uint32_t hold_t[2], hold_done[2];      /* EDIT, HOME: when pressed, and held long enough already */
    uint8_t er;                            /* ERASE: the clicks of the turn so far */
    uint32_t er_t, erased_ms;              /* its last click; when a pattern was erased or everything reset (the header says ERASED or RESET) */
    uint8_t fxsel;                         /* the effect picked on the FX pages */
    uint8_t t_fx;                          /* the target is an effect's step (t_lane: the effect), not a drum's */
    uint32_t scope_t;                      /* when the oscilloscope was drawn last */
} dbx_ui = {0, DIRTY_ALL, PAGE_LANE, 100, 0, 0, 0, 0, 0, 512 * 16, {0, 0}, {1, 1}, 0, 0, 0, 0, 0, 0};
#define DBX_FXPAGE (dbx_ui.page == PAGE_FX1 || dbx_ui.page == PAGE_FX2)
#define DBX_F (dbx_seq.fx[dbx_seq.pe])     /* the effects' steps of the pattern shown */

/* ------------------------------------------------------------ input --- */
static void dbx_master_poll(void)
{
    int32_t a = fm1_adc_read(FM1_ADC_MASTER);
    if (a >= 0) {
        dbx_ui.master_knob += (a * 16 - dbx_ui.master_knob) / 8;
        dbx_audio.master_q12 = dbx_master_of_pot((uint32_t)(dbx_ui.master_knob / 16));
    }
}

/* the battery: the divider's reading (ADC 3; the stock's 531 / 561 / 591 make 1, 2 and 3 bars), read four times a second and smoothed (the
 * reading sags with the load), a bar changing only 3 counts past its threshold; a host on USB powers the box (dbx_usb_powered: dbx_main.c) */
static uint32_t dbx_usb_powered(void);
static struct { int32_t acc; uint32_t t_ms; uint8_t lvl; } dbx_batt;           /* acc: the reading x 32 */
static void dbx_batt_poll(void)
{
    static const int16_t THR[3] = {531, 561, 591};
    int32_t b, k;
    uint32_t first = !dbx_batt.acc;
    if (dbx_batt.t_ms && fm1_ms - dbx_batt.t_ms < 250u)
        return;
    dbx_batt.t_ms = fm1_ms ? fm1_ms : 1u;
    b = fm1_adc_read(FM1_ADC_BATT);
    if (b <= 0)
        return;
    dbx_batt.acc = first ? b << 5 : dbx_batt.acc + ((b << 5) - dbx_batt.acc) / 8;
    b = dbx_batt.acc >> 5;
    for (k = 0; k < 3 && b >= THR[k]; k++)
        ;
    if (first)
        dbx_batt.lvl = (uint8_t)k;
    else if (k > (int32_t)dbx_batt.lvl && b >= THR[dbx_batt.lvl] + 3)
        dbx_batt.lvl = (uint8_t)k;
    else if (k < (int32_t)dbx_batt.lvl && b < THR[dbx_batt.lvl - 1u] - 3)
        dbx_batt.lvl = (uint8_t)k;
}

static void dbx_clear_steps(uint32_t lane_i, uint32_t all)
{
    uint32_t i;
    for (i = 0; i < DBX_NLANE; i++)
        if (all || i == lane_i)
            DBX_E.steps[i] = 0;
    dbx_seq_flush(all ? 0xFFFFu : 1u << lane_i);        /* (what was queued does not play) */
    dbx_touch_pat(dbx_seq.pe);
    if (all) {                                          /* a whole pattern: its effects' steps too */
        for (i = 0; i < DBX_NFX; i++)
            DBX_F.on[i] = 0;
        dbx_touch_fx();
    }
}

static uint8_t dbx_reset_note;                          /* the header's note: 0 ERASED (a pattern cleared), 1 RESET (a full reset), 2 DELETED (a pattern taken out) */

/* HOME held: the pattern is cleared (its steps and its effects' steps) and every parameter of the sound goes back to its state 0: the kit CLASSIC
 * with its drums as designed (every lane's TUNE, DECAY, TONE, LEVEL, no mute), every effect's parameters and the FX MIX, the master page, the swing,
 * the LFOs, ENV and ARP. The tempo, the subdivision, the volume and the clock are the transport's and stay. All of it is saved (the autosave takes
 * it a moment later, when stopped), so it is what the next start loads. */
static void dbx_reset_all(void)
{
    uint32_t pe = dbx_seq.pe, k;
    dbx_clear_steps(0, 1);
    for (k = 0; k < DBX_NLANE; k++)
        lane[k].mute = 0;                               /* (the mutes are the project's, not a pattern's sound: all back) */
    if (pe == dbx_snap.live) {                          /* the sound of the pattern shown is the live one: the kit, the lanes, the effects, the master page, the LFOs, ENV */
        dbx_fx_init();
        dbx_kit = 0;
        dbx_kit_reset = 1;                              /* (the render starts the lanes over) */
        dbx_lfo.p[0][LFO_RATE] = 5;
        dbx_lfo.p[1][LFO_RATE] = 3;
        dbx_lfo.p[0][LFO_AMT] = dbx_lfo.p[1][LFO_AMT] = 0;
        dbx_lfo.p[0][LFO_SHAPE] = dbx_lfo.p[1][LFO_SHAPE] = 0;
        dbx_lfo.p[0][LFO_PHASE] = dbx_lfo.p[1][LFO_PHASE] = 0;
        for (k = 0; k < EV_NP; k++)
            dbx_env.p[k] = EV_DEF[k];
    } else
        dbx_snap_default(&dbx_snap.s[pe]);               /* (the pattern waiting: its snapshot) */
    {
        static const uint8_t ARP_DEF[AP_NP] = {4, 0, 16, 0, 0, 0, 0, 0};          /* (the project's: ARP and the swing) */
        for (k = 0; k < AP_NP; k++)
            dbx_arp.p[k] = ARP_DEF[k];
        for (k = 0; k < DBX_NLANE; k++)
            dbx_arp.m[k][0] = dbx_arp.m[k][1] = dbx_arp.m[k][3] = 0;
    }
    dbx_seq.swing = 0;
    dbx_touch_set();
    dbx_touch_fx();
    dbx_touch_x();
    dbx_ui.t_on = 0;
    dbx_ui.erased_ms = fm1_ms;
    dbx_reset_note = 1;
    dbx_ui.dirty |= DIRTY_LANE;
}

/* SOLO: the lane alone; on the one that is already alone, every lane again */
static void dbx_solo(uint32_t l)
{
    uint32_t i, alone = !lane[l].mute;
    for (i = 0; i < DBX_NLANE; i++)
        if (i != l && !lane[i].mute)
            alone = 0;
    for (i = 0; i < DBX_NLANE; i++)
        lane[i].mute = alone ? 0u : (i != l);
}

static void dbx_set_page(uint32_t p)
{
    if (dbx_ui.page != p) {
        if (dbx_ui.page == PAGE_HELP || p == PAGE_HELP)
            dbx_ui.dirty |= DIRTY_ALL;                  /* (the help is a whole screen of its own: in, and out, the frame is drawn again) */
        dbx_ui.page = (uint8_t)p;
        dbx_ui.er = 0;                                  /* (a half-done erase is forgotten) */
        dbx_ui.dirty |= DIRTY_LANE;
    }
}

/* the step SELECT and the EDIT page work on: the one just placed or picked on the selected lane (while it is on) */
static int dbx_target(uint32_t *step)
{
    if (!dbx_ui.t_on)
        return 0;
    if (DBX_FXPAGE) {                                   /* on the FX pages: the effect's step */
        if (dbx_ui.t_fx && dbx_ui.t_lane == dbx_ui.fxsel && ((DBX_F.on[dbx_ui.fxsel] >> dbx_ui.t_step) & 1u)) {
            *step = dbx_ui.t_step;
            return 1;
        }
    } else if (!dbx_ui.t_fx && dbx_ui.t_lane == dbx_ui.sel && ((DBX_E.steps[dbx_ui.sel] >> dbx_ui.t_step) & 1u)) {
        *step = dbx_ui.t_step;
        return 1;
    }
    return 0;
}

static void dbx_set_target(uint32_t lane_or_fx, uint32_t step)    /* the step SELECT and the EDIT page work on */
{
    dbx_ui.t_on = 1;
    dbx_ui.t_fx = DBX_FXPAGE ? 1u : 0u;
    dbx_ui.t_lane = (uint8_t)lane_or_fx;
    dbx_ui.t_step = (uint8_t)step;
}

static void dbx_ui_input(void)
{
    uint32_t released, btn = fm1_input_edges(&released), pressed = fm1_input_note_edges(), k, h, st = 0;
    uint32_t i = dbx_ui.sel, fxp;
    int32_t s_sel, s_algo, s_pre, s_k[4];
    dbx_lane_t *l;
    if (btn & PANEL_BTN(B_GLO)) {
        dbx_glo_used = 0;
        dbx_glo_t = fm1_ms;
    } else if ((fm1_in.buttons & PANEL_BTN(B_GLO)) && fm1_ms - dbx_glo_t >= 250u)
        dbx_glo_used = 1;                               /* (held: the mixer is up, and letting go is not a tap) */
    if ((released & PANEL_BTN(B_GLO)) && !dbx_glo_used)  /* GLO let go with no key pressed meanwhile (a tap): the pages, two, then it closes */
        dbx_set_page(dbx_ui.page == PAGE_GLO ? PAGE_GLO2 : dbx_ui.page == PAGE_GLO2 ? PAGE_LANE : PAGE_GLO);
    if (btn & PANEL_BTN(B_SAVE)) {                      /* SAVE: a tap (let go) opens the pattern page; held 0.6 s it writes at once, playing or not */
        dbx_save_t = fm1_ms;
        dbx_save_done = 0;
    } else if ((fm1_in.buttons & PANEL_BTN(B_SAVE)) && !dbx_save_done && fm1_ms - dbx_save_t >= DBX_HOLD_MS) {
        dbx_save_done = 1;
        dbx_store.now = 1;
        dbx_store.saved_ms = fm1_ms;                    /* (the header says SAVED at once, and when each object is written) */
    }
    if ((released & PANEL_BTN(B_SAVE)) && !dbx_save_done)
        dbx_set_page(dbx_ui.page == PAGE_PAT ? PAGE_SONG : dbx_ui.page == PAGE_SONG ? PAGE_LANE : PAGE_PAT);      /* the patterns, the song, then it closes */
    if (btn & PANEL_BTN(B_SCL))
        dbx_set_page(dbx_ui.page == PAGE_SCL ? PAGE_LANE : PAGE_SCL);
    if (btn & PANEL_BTN(B_LFO))                         /* LFO: the volume's, the pan's, then it closes */
        dbx_set_page(dbx_ui.page == PAGE_LFO1 ? PAGE_LFO2 : dbx_ui.page == PAGE_LFO2 ? PAGE_LANE : PAGE_LFO1);
    if (btn & PANEL_BTN(B_ENV))                         /* ENV: the ducking, the attack, decay and gate, then it closes */
        dbx_set_page(dbx_ui.page == PAGE_ENV1 ? PAGE_ENV2 : dbx_ui.page == PAGE_ENV2 ? PAGE_LANE : PAGE_ENV1);
    if (btn & PANEL_BTN(B_ARP))                         /* ARP: the Euclidean generator, the mutation, then it closes */
        dbx_set_page(dbx_ui.page == PAGE_ARP1 ? PAGE_ARP2 : dbx_ui.page == PAGE_ARP2 ? PAGE_LANE : PAGE_ARP1);
    if (btn & PANEL_BTN(B_FX))                          /* FX: two pages of parameters, then it closes */
        dbx_set_page(dbx_ui.page == PAGE_FX1 ? PAGE_FX2 : dbx_ui.page == PAGE_FX2 ? PAGE_LANE : PAGE_FX1);
    fxp = DBX_FXPAGE;
    if (btn & PANEL_BTN(B_REC)) {                       /* REC: armed, or not */
        dbx_rec ^= 1u;
        dbx_ui.dirty |= DIRTY_LANE;
        dbx_ui.sig = ~dbx_ui.sig;
    }
    if (btn & (PANEL_BTN(B_OCTUP) | PANEL_BTN(B_OCTDN))) {   /* OCT-, OCT+: the previous, the next pattern, round (it waits for the pass while playing) */
        dbx_pat_select((dbx_seq.pe + ((btn & PANEL_BTN(B_OCTUP)) ? 1u : DBX_NPAT - 1u)) % DBX_NPAT);
        dbx_ui.t_on = 0;                                        /* (its steps are others) */
        dbx_ui.dirty |= DIRTY_LANE;
    }
    if (btn & PANEL_BTN(B_SEQ)) {
        dbx_seq_t = fm1_ms;
        dbx_seq_used = 0;
    }
    dbx_seq.rep_req = ((fm1_in.buttons & PANEL_BTN(B_SEQ)) && fm1_ms - dbx_seq_t >= 250u) ? 1u : 0u;   /* SEQ held a quarter of a second while playing: the last beat loops */
    if ((released & PANEL_BTN(B_SEQ)) && !dbx_seq_used && fm1_ms - dbx_seq_t < 250u) {     /* a tap of SEQ: the help, its three pages, then it closes */
        if (dbx_ui.page == PAGE_HELP && dbx_help < 2u)
            dbx_help++;
        else if (dbx_ui.page == PAGE_HELP)
            dbx_set_page(PAGE_LANE);
        else {
            dbx_help = 0;
            dbx_set_page(PAGE_HELP);
        }
        dbx_ui.dirty |= DIRTY_LANE;
    }
    for (h = 0; h < 2u; h++) {                          /* EDIT, HOME: a tap, or held 0.6 s to clear */
        uint32_t m = PANEL_BTN(h ? B_HOME : B_EDIT);
        if (btn & m) {
            dbx_ui.hold_t[h] = fm1_ms;
            dbx_ui.hold_done[h] = 0;
        } else if ((fm1_in.buttons & m) && !dbx_ui.hold_done[h] && fm1_ms - dbx_ui.hold_t[h] >= DBX_HOLD_MS) {
            dbx_ui.hold_done[h] = 1;
            if (fxp && h == 0u) {                       /* EDIT held on the FX pages: the effect's steps */
                DBX_F.on[dbx_ui.fxsel] = 0;
                dbx_touch_fx();
                dbx_ui.dirty |= DIRTY_LANE;
            } else if (dbx_ui.page == PAGE_PAT && h == 0u) {   /* ... on the pattern page: the pattern shown, deleted (the next ones move down) */
                dbx_pat_delete(dbx_seq.pe);
                dbx_ui.t_on = 0;
                dbx_ui.erased_ms = fm1_ms;
                dbx_reset_note = 2;
                dbx_ui.dirty |= DIRTY_LANE;
            } else if (dbx_ui.page == PAGE_SCL && h == 0u) {   /* EDIT held on the master page: its state 0 */
                dbx_mst_reset();
                dbx_touch_fx();
                dbx_ui.dirty |= DIRTY_LANE;
            } else if ((dbx_ui.page == PAGE_LFO1 || dbx_ui.page == PAGE_LFO2) && h == 0u) {   /* ... on an LFO page: that LFO, off, as it starts */
                uint32_t q = dbx_ui.page == PAGE_LFO2;
                dbx_lfo.p[q][LFO_RATE] = q ? 3u : 5u;
                dbx_lfo.p[q][LFO_AMT] = dbx_lfo.p[q][LFO_SHAPE] = dbx_lfo.p[q][LFO_PHASE] = 0;
                dbx_touch_set();
                dbx_ui.dirty |= DIRTY_LANE;
            } else if ((dbx_ui.page == PAGE_ENV1 || dbx_ui.page == PAGE_ENV2) && h == 0u) {   /* ... on an ENV page: its four values */
                uint32_t q = dbx_ui.page == PAGE_ENV2 ? 4u : 0u, n;
                for (n = 0; n < 4u; n++)
                    dbx_env.p[q + n] = EV_DEF[q + n];
                dbx_touch_x();
                dbx_ui.dirty |= DIRTY_LANE;
            } else if ((dbx_ui.page == PAGE_ARP1 || dbx_ui.page == PAGE_ARP2) && h == 0u) {   /* ... on an ARP page: its four values (the lane is not touched) */
                static const uint8_t ARP_DEF[AP_NP] = {4, 0, 16, 0, 0, 0, 0, 0};
                uint32_t q = dbx_ui.page == PAGE_ARP2 ? 4u : 0u, n, all = dbx_arp.p[AM_SCOPE];
                for (n = 0; n < 4u; n++)
                    dbx_arp.p[q + n] = ARP_DEF[q + n];
                for (n = 0; q && n < DBX_NLANE; n++)            /* (MUTATE: this lane's, or every lane's if SCOPE was ALL; SCOPE goes back to this lane) */
                    if (all || n == i)
                        dbx_arp.m[n][0] = dbx_arp.m[n][1] = dbx_arp.m[n][3] = 0;
                dbx_arp.n = 0;
                dbx_ui.dirty |= DIRTY_LANE;
            } else if (h == 1u)
                dbx_reset_all();                        /* HOME: all, and every parameter back to its state 0 */
            else
                dbx_clear_steps(i, h);                  /* EDIT: the lane */
        }
        if ((released & m) && !dbx_ui.hold_done[h]) {   /* a tap */
            if (h == 0u)
                dbx_set_page(dbx_ui.page == PAGE_EDIT ? PAGE_EDIT2 : dbx_ui.page == PAGE_EDIT2 ? PAGE_LANE : PAGE_EDIT);   /* EDIT: the step's page, the step's sound, then it closes */
            else
                dbx_set_page(PAGE_LANE);
        }
    }
    for (k = 0; k < DBX_NKEY; k++) {
        if (!((pressed >> k) & 1u))
            continue;
        if (DBX_LANE_OF_KEY[k] >= 0 && !fxp && (fm1_in.buttons & PANEL_BTN(B_GLO))) {   /* GLO held, a lane's key: MUTE (let go), or SOLO (held) */
            dbx_glo_used = 1;
            dbx_mute_t[DBX_LANE_OF_KEY[k]] = fm1_ms;
            dbx_mute_st[DBX_LANE_OF_KEY[k]] = 1;
            continue;
        }
        if (DBX_LANE_OF_KEY[k] >= 0) {                  /* a black key: its lane selected (it makes no sound), */
            if (fxp)                                    /* on the FX pages its effect */
                dbx_ui.fxsel = (uint8_t)DBX_LANE_OF_KEY[k];
            else {
                dbx_ui.sel = (uint8_t)DBX_LANE_OF_KEY[k];
                i = dbx_ui.sel;
                if (dbx_ui.page == PAGE_EDIT || dbx_ui.page == PAGE_EDIT2)
                    dbx_ui.t_on = 0;                    /* (on the step pages a lane picked is the whole lane: a white key then picks a step of it) */
                if (dbx_rec) {                          /* REC: it sounds, and while playing it is written (an overdub: a step already on stays as it is) */
                    dbx_seq.lv_req |= (uint16_t)(1u << i);
                    if (dbx_seq.playing) {
                        uint32_t s = dbx_rec_step();
                        if (!((DBX_E.steps[i] >> s) & 1u)) {
                            DBX_E.steps[i] |= (uint16_t)(1u << s);
                            dbx_step_defaults(&DBX_E, i, s);
                            dbx_touch_pat(dbx_seq.pe);
                        }
                        dbx_set_target(i, s);           /* (SELECT sets its velocity) */
                    }
                }
            }
            dbx_ui.dirty |= DIRTY_LANE;
            continue;
        }
        st = (uint32_t)DBX_STEP_OF_KEY[k];              /* a white key: a step of the selected lane */
        if (dbx_ui.page == PAGE_SONG) {                 /* the song page: the key is a slot */
            dbx_song.sel = (uint8_t)st;
            dbx_ui.dirty |= DIRTY_LANE;
            continue;
        }
        if (dbx_ui.page == PAGE_PAT) {                  /* the pattern page: the key is a pattern */
            uint32_t held = fm1_in.notes & DBX_WHITE_MASK & ~(1u << k);
            if (held) {                                 /* another key is down: its pattern is copied here */
                uint32_t src = 0;
                while (!((held >> src) & 1u))
                    src++;
                dbx_pat_copy((uint32_t)DBX_STEP_OF_KEY[src], st);
            }
            dbx_pat_select(st);
            dbx_ui.t_on = 0;                            /* (its steps are others) */
            dbx_ui.dirty |= DIRTY_LANE;
            continue;
        }
        if (fxp) {                                      /* the FX pages: a step of the effect picked, on or off */
            uint32_t e = dbx_ui.fxsel;
            if ((DBX_F.on[e] >> st) & 1u)
                DBX_F.on[e] &= (uint16_t)~(1u << st);
            else {
                DBX_F.on[e] |= (uint16_t)(1u << st);
                DBX_F.mix[e][st] = DBX_FX_STEPMIX[e];   /* (the sends start low) */
                dbx_set_target(e, st);                  /* SELECT sets its mix */
            }
            dbx_touch_fx();
            dbx_ui.dirty |= DIRTY_LANE;
            continue;
        }
        if ((dbx_ui.page == PAGE_EDIT || dbx_ui.page == PAGE_EDIT2) && ((DBX_E.steps[i] >> st) & 1u) &&
            !(dbx_ui.t_on && dbx_ui.t_lane == i && dbx_ui.t_step == st)) {
            dbx_set_target(i, st);                      /* EDIT: pick a step that is on */
            dbx_ui.dirty |= DIRTY_LANE;
            if (!dbx_seq.playing)
                dbx_preview(i, st);
        } else if ((DBX_E.steps[i] >> st) & 1u) {     /* on: off (EDIT: the picked step again) */
            DBX_E.steps[i] &= (uint16_t)~(1u << st);
            dbx_touch_pat(dbx_seq.pe);
            dbx_ui.dirty |= DIRTY_LANE;
        } else {                                        /* off: on, as a new step, and the one SELECT edits */
            DBX_E.steps[i] |= (uint16_t)(1u << st);
            dbx_step_defaults(&DBX_E, i, st);
            dbx_touch_pat(dbx_seq.pe);
            dbx_set_target(i, st);
            dbx_ui.dirty |= DIRTY_LANE;
            if (!dbx_seq.playing)
                dbx_preview(i, st);                     /* (a preview of what the step will play) */
        }
    }
    for (k = 0; k < DBX_NLANE; k++) {                   /* the lane keys pressed with GLO held: let go, a mute; held 0.5 s, a solo (or all back) */
        uint32_t down = (fm1_in.notes >> DBX_KEY_OF_LANE[k]) & 1u;
        if (dbx_mute_st[k] == 1u && down && fm1_ms - dbx_mute_t[k] >= 500u) {
            dbx_solo(k);
            dbx_mute_st[k] = 2;
            dbx_ui.dirty |= DIRTY_LANE;
        } else if (!down && dbx_mute_st[k]) {
            if (dbx_mute_st[k] == 1u) {
                lane[k].mute ^= 1u;
                dbx_ui.dirty |= DIRTY_LANE;
            }
            dbx_mute_st[k] = 0;
        }
    }
    if (btn & PANEL_BTN(B_PLAY))
        dbx_seq_play(!dbx_seq.playing);
    s_sel = panel_enc(EN_SELECT);
    s_algo = panel_enc(EN_ALGO);
    s_pre = panel_enc(EN_PRESET);
    for (k = 0; k < 4u; k++)
        s_k[k] = panel_enc(EN_K1 + k);
    if ((fm1_in.buttons & PANEL_BTN(B_SEQ)) && s_k[0] != 0) {   /* SEQ held, KNOB 1: the loop's own subdivision (the tempo grid is not touched) */
        dbx_seq.rep_r = (uint8_t)clamp((int32_t)dbx_seq.rep_r + s_k[0], 0, 3);   /* clockwise: shorter */
        dbx_ui.scope_t = 0;                             /* (the scope says it at once) */
        dbx_seq_used = 1;                               /* (and it is not a tap) */
        s_k[0] = 0;
    }
    if (s_pre != 0 && (fxp ? DBX_F.on[dbx_ui.fxsel] : DBX_E.steps[i])) {   /* PRESETS: through the steps placed on this lane or
                                                         * effect, round */
        uint32_t on = fxp ? DBX_F.on[dbx_ui.fxsel] : DBX_E.steps[i], n, at = dbx_target(&st) ? st : (s_pre > 0 ? DBX_NSTEP - 1u : 0u);
        int32_t dir = s_pre > 0 ? 1 : -1, left = s_pre > 0 ? s_pre : -s_pre;
        while (left-- > 0)                              /* (none picked yet: the first, or the last, is a click away) */
            for (n = 0; n < DBX_NSTEP; n++) {
                at = (at + DBX_NSTEP + (uint32_t)dir) % DBX_NSTEP;
                if ((on >> at) & 1u)
                    break;
            }
        dbx_set_target(fxp ? dbx_ui.fxsel : i, at);
        dbx_ui.dirty |= DIRTY_LANE;
        if (!dbx_seq.playing && !fxp)
            dbx_preview(i, at);                         /* (heard) */
    }
    if (s_sel != 0 && fxp && dbx_target(&st)) {         /* the mix of the effect's step just placed */
        uint8_t *v = &DBX_F.mix[dbx_ui.fxsel][st];
        *v = (uint8_t)clamp((int32_t)*v + s_sel * 5, 0, 100);
        dbx_touch_fx();
    } else if (s_sel != 0 && dbx_target(&st)) {         /* the velocity of the step just placed */
        uint8_t *v = &DBX_E.vel[i][st];
        *v = (uint8_t)clamp((int32_t)*v + s_sel * 4, 1, 127);
        dbx_touch_pat(dbx_seq.pe);
        if (!dbx_seq.playing)
            dbx_preview(i, st);                         /* (heard at its new level) */
    }
    if (s_algo != 0 && fxp) {                           /* the effect before / after */
        dbx_ui.fxsel = (uint8_t)(((int32_t)dbx_ui.fxsel + s_algo % (int32_t)DBX_NFX + (int32_t)DBX_NFX) % (int32_t)DBX_NFX);
        dbx_ui.dirty |= DIRTY_LANE;
    } else if (s_algo != 0 && (dbx_ui.page == PAGE_LANE || dbx_ui.page == PAGE_EDIT || dbx_ui.page == PAGE_EDIT2)) {
        lane[i].pan = (int8_t)clamp((int32_t)lane[i].pan + s_algo, -32, 32);     /* ALGORITHM on a lane's pages: its PAN (the black keys pick the lane) */
        dbx_touch_set();
        dbx_ui.dirty |= DIRTY_LANE;
    } else if (s_algo != 0) {                           /* (the other pages: the previous, the next lane) */
        dbx_ui.sel = (uint8_t)(((int32_t)dbx_ui.sel + s_algo % (int32_t)DBX_NLANE + (int32_t)DBX_NLANE) % (int32_t)DBX_NLANE);
        dbx_ui.dirty |= DIRTY_LANE;
        i = dbx_ui.sel;
    }
    if (fxp) {                                          /* the effect pages: its parameters, four a page */
        uint32_t base = dbx_ui.page == PAGE_FX2 ? 4u : 0u;
        if (s_k[0] | s_k[1] | s_k[2] | s_k[3]) {
            for (k = 0; k < 4u; k++)
                if (s_k[k])
                    dbx_fx_step(dbx_ui.fxsel, base + k, s_k[k]);
            dbx_touch_fx();
            dbx_ui.dirty |= DIRTY_LANE;
        }
        return;
    }
    if (dbx_ui.page == PAGE_GLO) {                      /* the global page */
        if (s_k[0])
            dbx_seq_set_bpm((int32_t)dbx_seq.bpm + s_k[0]);
        if (s_k[1]) {
            dbx_ui.vol = (uint8_t)clamp((int32_t)dbx_ui.vol + s_k[1] * 2, 0, 127);
            dbx_audio.vol_q12 = dbx_vol_q12(dbx_ui.vol);
        }
        if (s_k[2])
            dbx_seq_set_ext(s_k[2] > 0);                /* clockwise: follow the computer, back: our own */
        if (s_k[3])
            dbx_seq_set_div((uint32_t)clamp((int32_t)dbx_seq_div_index() + s_k[3], 0, 2));
        if (s_k[0] | s_k[1] | s_k[2] | s_k[3]) {
            dbx_ui.dirty |= DIRTY_LANE;
            dbx_touch_set();
        }
        return;
    }
    if (dbx_ui.er && fm1_ms - dbx_ui.er_t > 1500u) {    /* an erase that stopped halfway is forgotten */
        dbx_ui.er = 0;
        dbx_ui.dirty |= DIRTY_LANE;
    }
    if (dbx_ui.page == PAGE_GLO2) {                     /* the second global page: swing, (FX mix, kit), erase */
        if (s_k[0]) {
            dbx_seq.swing = (uint8_t)clamp((int32_t)dbx_seq.swing + s_k[0] * 5, 0, 100);
            dbx_touch_set();
        }
        if (s_k[1]) {                                   /* FX MIX: how much of every effect */
            dbx_fx.master = (uint8_t)clamp((int32_t)dbx_fx.master + s_k[1] * 5, 0, 100);
            dbx_touch_set();
        }
        if (s_k[2]) {                                   /* KIT: the drums of the 11 lanes (the render takes it up and plays the kick) */
            dbx_kit_say = !dbx_seq.playing;
            dbx_kit = (uint8_t)clamp((int32_t)dbx_kit + s_k[2], 0, (int32_t)DBX_NKIT - 1);
            dbx_touch_set();
        }
        if (s_k[3]) {                                   /* a full turn erases the pattern shown; turning back undoes it */
            int32_t n = clamp((int32_t)dbx_ui.er + s_k[3], 0, (int32_t)DBX_ERASE_CLICKS);
            dbx_ui.er = (uint8_t)n;
            dbx_ui.er_t = fm1_ms;
            if (n >= (int32_t)DBX_ERASE_CLICKS) {       /* ... and the same reset as HOME held: the pattern and every parameter of the sound */
                dbx_reset_all();
                dbx_ui.er = 0;
            }
        }
        if (s_k[0] | s_k[1] | s_k[2] | s_k[3])
            dbx_ui.dirty |= DIRTY_LANE;
        return;
    }
    if (dbx_ui.page == PAGE_ARP1 || dbx_ui.page == PAGE_ARP2) {      /* ARP: EUCLID (HITS ROTATE LENGTH ACCENT), MUTATE (AMOUNT EVERY SCOPE DRIFT) */
        uint8_t *q = dbx_arp.p + (dbx_ui.page == PAGE_ARP2 ? 4 : 0);
        if (s_k[0] | s_k[1] | s_k[2] | s_k[3]) {
            if (dbx_ui.page == PAGE_ARP1) {
                q[AP_HITS] = (uint8_t)clamp((int32_t)q[AP_HITS] + s_k[0], 0, 16);
                q[AP_ROT] = (uint8_t)(((int32_t)q[AP_ROT] + s_k[1] % 16 + 16) % 16);               /* (round) */
                q[AP_LEN] = (uint8_t)clamp((int32_t)q[AP_LEN] + s_k[2], 2, 16);
                q[AP_ACC] = (uint8_t)clamp((int32_t)q[AP_ACC] + s_k[3] * 5, 0, 100);
                dbx_arp_apply(i);
                dbx_ui.t_on = 0;
            } else {
                uint8_t *m = dbx_arp.m[i], a = (uint8_t)clamp((int32_t)m[0] + s_k[0] * 5, 0, 100), e = (uint8_t)clamp((int32_t)m[1] + s_k[1], 0, 3),
                        d = (uint8_t)clamp((int32_t)m[3] + s_k[3] * 5, 0, 100);
                q[2] = (uint8_t)clamp((int32_t)q[2] + s_k[2], 0, 1);                                /* SCOPE: this lane, or all */
                for (k = 0; k < DBX_NLANE; k++)                                                     /* AMOUNT, EVERY, DRIFT: the lane's own */
                    if (q[2] || k == i) {
                        dbx_arp.m[k][0] = a;
                        dbx_arp.m[k][1] = e;
                        dbx_arp.m[k][3] = d;
                    }
                dbx_arp.n = 0;
            }
            dbx_ui.dirty |= DIRTY_LANE;
        }
        return;
    }
    if (dbx_ui.page == PAGE_ENV1 || dbx_ui.page == PAGE_ENV2) {      /* ENV: DUCK (AMOUNT RELEASE SOURCE TARGET), SHAPE (KICK ATK ATTACK DECAY GATE) */
        uint8_t *q = dbx_env.p + (dbx_ui.page == PAGE_ENV2 ? 4 : 0);
        if (s_k[0] | s_k[1] | s_k[2] | s_k[3]) {
            if (dbx_ui.page == PAGE_ENV1) {
                q[EV_AMT] = (uint8_t)clamp((int32_t)q[EV_AMT] + s_k[0] * 5, 0, 100);
                q[EV_REL] = (uint8_t)clamp((int32_t)q[EV_REL] + s_k[1] * 2, 0, 127);
                q[EV_SRC] = (uint8_t)clamp((int32_t)q[EV_SRC] + s_k[2], 0, (int32_t)DBX_NLANE - 1);
                q[EV_TGT] = (uint8_t)clamp((int32_t)q[EV_TGT] + s_k[3], 0, 2);
            } else {
                q[0] = (uint8_t)clamp((int32_t)q[0] + s_k[0] * 4, 0, 127);          /* KICK ATTACK */
                q[1] = (uint8_t)clamp((int32_t)q[1] + s_k[1] * 2, 0, 127);          /* ATTACK */
                q[2] = (uint8_t)clamp((int32_t)q[2] + s_k[2] * 2, 0, 127);          /* DECAY */
                q[3] = (uint8_t)clamp((int32_t)q[3] + s_k[3] * 2, 0, 127);          /* GATE */
            }
            dbx_touch_x();
            dbx_ui.dirty |= DIRTY_LANE;
        }
        return;
    }
    if (dbx_ui.page == PAGE_LFO1 || dbx_ui.page == PAGE_LFO2) {      /* an LFO: RATE, AMOUNT, SHAPE, PHASE */
        uint8_t *q = dbx_lfo.p[dbx_ui.page == PAGE_LFO2];
        if (s_k[0] | s_k[1] | s_k[2] | s_k[3]) {
            q[LFO_RATE] = (uint8_t)clamp((int32_t)q[LFO_RATE] + s_k[0], 0, (int32_t)LFO_NRATE - 1);
            q[LFO_AMT] = (uint8_t)clamp((int32_t)q[LFO_AMT] + s_k[1] * 5, 0, 100);
            q[LFO_SHAPE] = (uint8_t)clamp((int32_t)q[LFO_SHAPE] + s_k[2], 0, (int32_t)LFO_NSHAPE - 1);
            q[LFO_PHASE] = (uint8_t)(((int32_t)q[LFO_PHASE] + s_k[3] % (int32_t)LFO_NPHASE + (int32_t)LFO_NPHASE) % (int32_t)LFO_NPHASE);   /* (round) */
            dbx_touch_set();
            dbx_ui.dirty |= DIRTY_LANE;
        }
        return;
    }
    if (dbx_ui.page == PAGE_SCL) {                      /* the master page: HPF, LPF, OTT, CLIP */
        if (s_k[0] | s_k[1] | s_k[2] | s_k[3]) {
            for (k = 0; k < 4u; k++)
                dbx_mst.p[k] = (uint8_t)clamp((int32_t)dbx_mst.p[k] + s_k[k] * 2, 0, 127);
            dbx_mst_changed();
            dbx_touch_fx();
            dbx_ui.dirty |= DIRTY_LANE;
        }
        return;
    }
    if (dbx_ui.page == PAGE_SONG) {                     /* the song: the slot picked (a white key): PATTERN, REPEAT (passes), AGAIN (chance of one more round), SONG on or off */
        uint32_t s = dbx_song.sel;
        if (s_k[0]) {
            int32_t v = dbx_song.pat[s] < DBX_NPAT ? (int32_t)dbx_song.pat[s] : -1;
            v = clamp(v + s_k[0], -1, (int32_t)DBX_NPAT - 1);
            dbx_song.pat[s] = v < 0 ? 255u : (uint8_t)v;
        }
        if (s_k[1])
            dbx_song.rep[s] = (uint8_t)clamp((int32_t)dbx_song.rep[s] + s_k[1], 0, 3);
        if (s_k[2])
            dbx_song.again[s] = (uint8_t)clamp((int32_t)dbx_song.again[s] + s_k[2] * 5, 0, 100);
        if (s_k[3])
            dbx_song.on = s_k[3] > 0 ? 1u : 0u;
        if (s_k[0] | s_k[1] | s_k[2] | s_k[3]) {
            dbx_touch_x();
            dbx_ui.dirty |= DIRTY_LANE;
        }
        return;
    }
    if (dbx_ui.page == PAGE_EDIT2) {                    /* the step's sound: its own DECAY, TONE, EXTRA and PAN, added to the lane's (no step picked: all the lane's steps) */
        if (s_k[0] | s_k[1] | s_k[2] | s_k[3]) {
            uint32_t one = (uint32_t)dbx_target(&st), s, first = DBX_NSTEP;
            for (s = 0; s < DBX_NSTEP; s++) {
                if (one ? s != st : !((DBX_E.steps[i] >> s) & 1u))
                    continue;
                if (first == DBX_NSTEP)
                    first = s;
                for (k = 0; k < 4u; k++)
                    dbx_lk_set(dbx_seq.pe, k, i, s, dbx_lk_get(dbx_seq.pe, k, i, s) + s_k[k]);
            }
            if (first != DBX_NSTEP) {
                if (s_k[0] | s_k[1])
                    dbx_touch_lk();
                if (s_k[2] | s_k[3])
                    dbx_touch_lk2();
                dbx_ui.dirty |= DIRTY_LANE;
                if (!dbx_seq.playing)
                    dbx_preview(i, first);              /* (heard as it will play) */
            }
        }
        return;
    }
    if (dbx_ui.page == PAGE_EDIT) {                     /* the step page: its timing, its hits, its chance, its pitch; with no step picked, all the lane's steps by the same clicks */
        if (s_k[0] | s_k[1] | s_k[2] | s_k[3]) {
            uint32_t one = (uint32_t)dbx_target(&st), s, first = DBX_NSTEP;
            for (s = 0; s < DBX_NSTEP; s++) {
                if (one ? s != st : !((DBX_E.steps[i] >> s) & 1u))
                    continue;
                if (first == DBX_NSTEP)
                    first = s;
                DBX_E.nudge[i][s] = (int8_t)clamp(DBX_E.nudge[i][s] + s_k[0], -DBX_NUDGE_MAX, DBX_NUDGE_MAX);
                DBX_E.ratchet[i][s] = (uint8_t)clamp((int32_t)DBX_E.ratchet[i][s] + s_k[1], 1, (int32_t)DBX_RATCHET_MAX);
                DBX_E.chance[i][s] = (uint8_t)clamp((int32_t)DBX_E.chance[i][s] + s_k[2] * 5, 0, 100);
                DBX_E.semi[i][s] = (int8_t)clamp(DBX_E.semi[i][s] + s_k[3], -DBX_PITCH_MAX, DBX_PITCH_MAX);
            }
            if (first != DBX_NSTEP) {
                dbx_touch_pat(dbx_seq.pe);
                dbx_ui.dirty |= DIRTY_LANE;
                if (!dbx_seq.playing)
                    dbx_preview(i, first);              /* (heard as it will play: its hits and its pitch) */
            }
        }
        return;
    }
    l = &lane[i];
    if (s_k[0] | s_k[1] | s_k[2] | s_k[3])
        dbx_touch_set();                                /* the kit is saved too */
    if (s_k[0]) {
        dbx_tune_step(i, s_k[0]);
        dbx_ui.dirty |= DIRTY_LANE;
    }
    if (s_k[1]) {
        l->p.decay = (uint8_t)clamp((int32_t)l->p.decay + s_k[1] * 2, 0, 127);
        dbx_ui.dirty |= DIRTY_LANE;
    }
    if (s_k[2]) {
        l->p.tone = (uint8_t)clamp((int32_t)l->p.tone + s_k[2] * 2, 0, 127);
        dbx_ui.dirty |= DIRTY_LANE;
    }
    if (s_k[3]) {
        l->p.level = (uint8_t)clamp((int32_t)l->p.level + s_k[3] * 2, 0, 127);
        dbx_ui.dirty |= DIRTY_LANE;
    }
}

/* -------------------------------------------------------------- LEDs --- */
static void dbx_led_key(uint8_t *arr, uint32_t id)      /* set the LED of key / button id (0..40) in a column array */
{
    uint32_t c, r;
    for (c = 0; c < FM1_NCOL; c++)
        for (r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][c] == (int8_t)id)
                arr[c] |= (uint8_t)(1u << r);
}

static void dbx_ui_leds(void)
{
    uint8_t lit[FM1_NCOL] = {0}, dim[FM1_NCOL] = {0}, mid[FM1_NCOL] = {0}, breath[FM1_NCOL] = {0};
    uint32_t k, c, playing = dbx_seq.playing, cur = dbx_seq.cur;
    for (k = 0; k < DBX_NKEY; k++) {
        uint32_t on = (fm1_in.notes >> k) & 1u;
        dbx_led_key(dim, 14u + k);                      /* the idle glow */
        if (DBX_LANE_OF_KEY[k] >= 0) {
            on |= DBX_LANE_OF_KEY[k] == (int8_t)(DBX_FXPAGE ? dbx_ui.fxsel : dbx_ui.sel);
            if (!DBX_FXPAGE && lane[DBX_LANE_OF_KEY[k]].mute)           /* a muted lane: its key breathes */
                dbx_led_key(breath, 14u + k);
        } else {
            uint32_t st = (uint32_t)DBX_STEP_OF_KEY[k];
            if (dbx_ui.page == PAGE_SONG) {             /* the song: the slot picked lit, the slots in the chain at the mid level, the one playing breathing */
                if (st == dbx_song.sel)
                    on = 1;
                else if (dbx_song.on && dbx_seq.playing && st == dbx_song.cur)
                    dbx_led_key(breath, 14u + k);
                else if (st < dbx_song_len())
                    dbx_led_key(mid, 14u + k);
            } else if (dbx_ui.page == PAGE_PAT) {       /* the patterns: the one shown lit, the one that waits breathing, */
                if (st == dbx_seq.pe)                   /* the others that hold something at the steady mid level */
                    on = 1;
                else if (st == dbx_seq.pq || (dbx_seq.playing && st == dbx_seq.pp))   /* (the song plays others than the one shown) */
                    dbx_led_key(breath, 14u + k);
                else if (dbx_pat_used(st))
                    dbx_led_key(mid, 14u + k);
            } else {
                uint32_t step_on = ((DBX_FXPAGE ? DBX_F.on[dbx_ui.fxsel] : DBX_E.steps[dbx_ui.sel]) >> st) & 1u;
                on |= step_on ^ (playing && cur == st); /* the playhead inverts its step */
            }
        }
        if (on)
            dbx_led_key(lit, 14u + k);
    }
    if (dbx_ui.page == PAGE_GLO || dbx_ui.page == PAGE_GLO2)
        dbx_led_key(lit, panel.btn[B_GLO]);
    if (dbx_ui.page == PAGE_EDIT || dbx_ui.page == PAGE_EDIT2)
        dbx_led_key(lit, panel.btn[B_EDIT]);
    if (dbx_ui.page == PAGE_PAT || dbx_ui.page == PAGE_SONG)
        dbx_led_key(lit, panel.btn[B_SAVE]);
    if (DBX_FXPAGE)
        dbx_led_key(lit, panel.btn[B_FX]);
    if (dbx_ui.page == PAGE_SCL)
        dbx_led_key(lit, panel.btn[B_SCL]);
    if (dbx_ui.page == PAGE_LFO1 || dbx_ui.page == PAGE_LFO2)
        dbx_led_key(lit, panel.btn[B_LFO]);
    if (dbx_ui.page == PAGE_ENV1 || dbx_ui.page == PAGE_ENV2)
        dbx_led_key(lit, panel.btn[B_ENV]);
    if (dbx_ui.page == PAGE_ARP1 || dbx_ui.page == PAGE_ARP2)
        dbx_led_key(lit, panel.btn[B_ARP]);
    if (dbx_seq.rep)
        dbx_led_key(lit, panel.btn[B_SEQ]);
    if (dbx_rec)
        dbx_led_key(lit, panel.btn[B_REC]);
    if (playing)
        lit[LED_PLAY_GREEN >> 3] |= (uint8_t)(1u << (LED_PLAY_GREEN & 7u));
    for (c = 0; c < FM1_NCOL; c++) {
        fm1_led[c] = lit[c];
        fm1_led_dim[c] = dim[c];
        fm1_led_mid[c] = mid[c];
        fm1_led_breath[c] = breath[c];
    }
}

/* ------------------------------------------------------------ screen --- */
static uint32_t dbx_fmt_int(char *d, int32_t v)         /* d: at least 6 bytes; returns the length */
{
    char t[6];
    uint32_t n = 0, i = 0, u = v < 0 ? (uint32_t)-v : (uint32_t)v;
    do {
        t[n++] = (char)('0' + u % 10u);
        u /= 10u;
    } while (u && n < 4u);
    if (v < 0)
        d[i++] = '-';
    while (n)
        d[i++] = t[--n];
    d[i] = 0;
    return i;
}

static void dbx_cat(char *d, const char *s)             /* append s to d */
{
    while (*d)
        d++;
    while ((*d++ = *s++) != 0)
        ;
}

static void dbx_cat_int(char *d, int32_t v)
{
    char t[8];
    dbx_fmt_int(t, v);
    dbx_cat(d, t);
}

static void dbx_cat_signed(char *d, int32_t v)          /* "+3", "-3", "0" */
{
    if (v > 0)
        dbx_cat(d, "+");
    dbx_cat_int(d, v);
}

static void dbx_cat_pan(char *d, int32_t p)                      /* "C", "L 25%", "R 100%" */
{
    if (p == 0) {
        dbx_cat(d, "C");
        return;
    }
    dbx_cat(d, p < 0 ? "L " : "R ");
    dbx_cat_int(d, (p < 0 ? -p : p) * 100 / 32);
    dbx_cat(d, "%");
}

static void dbx_fmt_tune(char *d, int32_t tune)         /* 1/16 semitone -> "+1.5 ST" */
{
    uint32_t a = (uint32_t)(tune < 0 ? -tune : tune), i = 0;
    uint32_t st = a / 16u, tenth = (a % 16u) * 10u / 16u;
    d[i++] = tune < 0 ? '-' : '+';
    if (st >= 10u)
        d[i++] = (char)('0' + st / 10u);
    d[i++] = (char)('0' + st % 10u);
    d[i++] = '.';
    d[i++] = (char)('0' + tenth);
    d[i++] = ' ';
    d[i++] = 'S';
    d[i++] = 'T';
    d[i] = 0;
}

static void dbx_fmt_note(char *d, int32_t n16)          /* 16 x MIDI note -> "G1", or "G1 +37c" off the note */
{
    static const char *const NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    int32_t note = (n16 + 8) / 16, cents = (n16 - note * 16) * 100 / 16;
    const char *n = NAME[note % 12];
    uint32_t i = 0;
    while (*n)
        d[i++] = *n++;
    i += dbx_fmt_int(d + i, note / 12 - 1);                /* C4 = 60: C0 = 12 */
    if (cents) {
        d[i++] = ' ';
        d[i++] = cents < 0 ? '-' : '+';
        i += dbx_fmt_int(d + i, cents < 0 ? -cents : cents);
        d[i++] = 'c';
    }
    d[i] = 0;
}

/* The screen, 240 x 240, top to bottom: the header, the title (H1), the oscilloscope, one row of the 16 steps (four
 * groups of four), and the four rows of the page. */
#define HEAD_Y 4u
#define TITLE_Y 20u
#define SCOPE_X 12u
#define SCOPE_Y 58u
#define SCOPE_W 216u
#define SCOPE_H 72u
#define GRID_Y 134u
#define GRID_H 22u
#define GCELL 11u                                       /* a step: 11 px, 1 px between the four of a group, 9 px between groups */
#define ROW_Y 160u                                      /* the four rows of the page: a line of text, a thin bar under it */
#define ROW_PITCH 19u
#define PCELL_W 24u                                     /* the pattern page's cells */
#define PCELL_H 20u
static inline uint32_t dbx_cell_x(uint32_t s) { return 12u + (s / 4u) * 56u + (s % 4u) * 12u; }

static uint32_t dbx_saved_now(void) { return fm1_ms - dbx_store.saved_ms < 1200u; }     /* "SAVED" shows a moment */
static uint32_t dbx_erased_now(void) { return dbx_ui.erased_ms && fm1_ms - dbx_ui.erased_ms < 1500u; }

#define BATT_X 152u                                     /* the battery's cell, left of the tempo (which ends at 228) */
static void dbx_draw_batt(void)                         /* the battery, left of the tempo: 3 bars (red at one or none), a bolt when USB powers the box */
{
    static const uint8_t BOLT[6] = {0x30, 0x18, 0x3E, 0x1C, 0x0C, 0x04};   /* (6 rows, 6 columns; the lowest bit is the left) */
    uint32_t usb = dbx_usb_powered(), l = dbx_batt.lvl, n, r;
    uint16_t ink = usb ? T_THEME : l > 1u ? T_TEXT : DBX_RED_LIGHT, line = usb || l > 1u ? T_MID : DBX_RED_LIGHT;
    lcd_fill(BATT_X, 4, 21, 11, T_BG);
    lcd_fill(BATT_X + 1u, 5, 18, 1, line);
    lcd_fill(BATT_X + 1u, 12, 18, 1, line);
    lcd_fill(BATT_X + 1u, 5, 1, 8, line);
    lcd_fill(BATT_X + 18u, 5, 1, 8, line);
    lcd_fill(BATT_X + 19u, 7, 2, 4, line);
    if (usb) {
        for (r = 0; r < 6u; r++)
            for (n = 0; n < 6u; n++)
                if ((BOLT[r] >> n) & 1u)
                    lcd_fill(BATT_X + 7u + n, 6u + r, 1, 1, ink);
        return;
    }
    for (n = 0; n < l; n++)                                  /* (empty: the outline alone, red) */
        lcd_fill(BATT_X + 3u + n * 5u, 7, 4, 4, ink);
}

static void dbx_draw_head(void)                         /* the pattern, PLAY / STOP / SAVED, the tempo */
{
    char b[16] = "", t[16] = "PATTERN ";
    if (dbx_seq.ext && !dbx_seq_locked())
        dbx_cat(b, "PC WAIT");
    else {
        if (dbx_seq.ext)
            dbx_cat(b, "PC ");
        dbx_cat_int(b, dbx_seq.bpm);
        if (!dbx_seq.ext)
            dbx_cat(b, " BPM");
    }
    dbx_cat_int(t, (int32_t)dbx_seq.pe + 1);
    draw_text_line(12, HEAD_Y, 76, &AF_S, t, T_MID, T_BG, 0);
    if (dbx_erased_now()) {
        gfx_colour = 1;
        draw_text_line(90, HEAD_Y, 56, &AF_S, dbx_reset_note == 2u ? "DELETED" : dbx_reset_note ? "RESET" : "ERASED", DBX_RED_LIGHT, T_BG, 1);
        gfx_colour = 0;
    } else if (dbx_saved_now())
        draw_text_line(90, HEAD_Y, 56, &AF_S, "SAVED", T_ACCENT, T_BG, 1);
    else {
        char s[10] = "";
        dbx_cat(s, dbx_rec ? "REC" : dbx_seq.rep ? "LOOP" : dbx_seq.playing ? "PLAY" : "STOP");
        if (dbx_seq.playing && !dbx_rec && !dbx_seq.rep && dbx_seq.pp != dbx_seq.pe) {      /* (another pattern than the one shown plays: the song, or one waiting) */
            dbx_cat(s, " ");
            dbx_cat_int(s, (int32_t)dbx_seq.pp + 1);
        }
        draw_text_line(90, HEAD_Y, 56, &AF_S, s, dbx_rec ? DBX_RED_LIGHT : dbx_seq.playing ? T_ACCENT : T_MID, T_BG, 1);
    }
    draw_text_line(176, HEAD_Y, 52, &AF_S, b, T_TEXT, T_BG, 2);
    dbx_draw_batt();
}

static uint32_t dbx_edit_values(uint32_t f0, uint32_t n, int32_t *val, uint32_t *mixed);       /* (the EDIT pages, below) */

static int dbx_step_edited(uint32_t i, uint32_t s)      /* a step with something set besides its velocity */
{
    return DBX_E.nudge[i][s] != 0 || DBX_E.ratchet[i][s] != 1u || DBX_E.chance[i][s] != 100u ||
           DBX_E.semi[i][s] != 0 || dbx_lk_get(dbx_seq.pe, 0, i, s) != 0 || dbx_lk_get(dbx_seq.pe, 1, i, s) != 0 ||
           dbx_lk_get(dbx_seq.pe, 2, i, s) != 0 || dbx_lk_get(dbx_seq.pe, 3, i, s) != 0;
}

static void dbx_draw_grid(void)                         /* the selected lane's (or effect's) 16 steps: bars, playhead */
{
    uint32_t s, tstep = 0, has = (uint32_t)dbx_target(&tstep), fxp = DBX_FXPAGE, e = dbx_ui.fxsel;
    uint32_t bits = fxp ? DBX_F.on[e] : DBX_E.steps[dbx_ui.sel];
    lcd_fill(11, GRID_Y - 1u, 218, GRID_H + 6u, T_BG);                         /* (the gaps, and the mark under the step picked, clean) */
    for (s = 0; s < DBX_NSTEP; s++) {
        uint32_t x = dbx_cell_x(s), y = GRID_Y, now = dbx_seq.playing && dbx_seq.cur == s;
        lcd_fill(x, y, GCELL, GRID_H, now ? DBX_RED_PLAY : s % 4u == 0u ? T_RAISE : T_SURF);   /* the playhead's cell is lit whole (red) */
        if ((bits >> s) & 1u) {
            if (fxp) {                                                     /* an effect's step: its mix is the height */
                uint32_t h = (uint32_t)DBX_F.mix[e][s] * GRID_H / 100u;
                if (now && h > GRID_H - 5u)
                    h = GRID_H - 5u;                                       /* (a full bar leaves the playhead's cap lit) */
                lcd_fill(x, y + GRID_H - (h < 3u ? 3u : h), GCELL, h < 3u ? 3u : h, now ? ux_mix(DBX_RED_PLAY, T_BG, 65) : T_THEME);
            } else {
                uint32_t h = (uint32_t)DBX_E.vel[dbx_ui.sel][s] * GRID_H / 127u;
                int32_t n = DBX_E.nudge[dbx_ui.sel][s], off = (n * (int32_t)GCELL + (n > 0 ? 24 : n < 0 ? -24 : 0)) / 48;
                uint32_t aoff = (uint32_t)(off < 0 ? -off : off);
                if (now && h > GRID_H - 5u)
                    h = GRID_H - 5u;                                       /* (a full bar leaves the playhead's cap lit) */
                if (h < 3u)
                    h = 3u;
                lcd_fill(off > 0 ? x + aoff : x, y + GRID_H - h, GCELL - aoff, h, now ? ux_mix(DBX_RED_PLAY, T_BG, 65) : T_THEME);   /* its velocity; a late note is
                                                                                           * pushed right and cut at the left,
                                                                                           * an early one pushed left and cut at
                                                                                           * the right */
                if (dbx_step_edited(dbx_ui.sel, s))
                    lcd_fill(x + GCELL - 4u, y + 2u, 3, 3, now ? T_BG : T_ACCENT);      /* a mark: its EDIT page is not the default */
            }
        }
        if (has && tstep == s) {                                           /* the step SELECT / EDIT work on: framed in violet (a full bar is green to the
                                                                            * edge, so the frame must not be), and marked under the cell, where no bar goes */
            lcd_fill(x, y, GCELL, 1, DBX_VIOLET);
            lcd_fill(x, y + GRID_H - 1u, GCELL, 1, DBX_VIOLET);
            lcd_fill(x, y, 1, GRID_H, DBX_VIOLET);
            lcd_fill(x + GCELL - 1u, y, 1, GRID_H, DBX_VIOLET);
            lcd_fill(x, y + GRID_H + 2u, GCELL, 3, DBX_VIOLET);
        }
    }
}

/* ---- the oscilloscope: the sound that goes out, between the title and the steps. It is drawn into the canvas strip
 * (a rounded panel, a centre line, three faint beat marks, the wave filled to the centre and traced 2 px), triggered
 * on a rising zero crossing and scaled to what it holds (the scale follows a loud sound fast and a quiet one slowly).
 * The corners say what the steps do: the one SELECT works on, and the subdivision. */
static void dbx_draw_scope(void)
{
    static int16_t snap[SCOPE_N] __attribute__((section(".pool")));
    static int32_t scale = 3000;
    char b[24] = "", r[24] = "1/";
    uint32_t w = scope_w, i, trig = 0, st;
    int32_t cy = (int32_t)SCOPE_H / 2, a = cy - 7, x, py = cy, peak = 1500;
    uint16_t fillc = ux_mix(T_SURF, T_THEME, 24);
    for (i = 0; i < SCOPE_N; i++) {
        int32_t v = scope_buf[(w + i) & (SCOPE_N - 1u)];
        snap[i] = (int16_t)v;
        if (v > peak)
            peak = v;
        else if (-v > peak)
            peak = -v;
    }
    scale += (peak - scale) / (peak > scale ? 2 : 16);
    if (scale < 1500)
        scale = 1500;
    for (i = 1; i < SCOPE_N - SCOPE_W; i++)
        if (snap[i - 1] < 0 && snap[i] >= 0) {
            trig = i;
            break;
        }
    cv_begin(SCOPE_W, SCOPE_H, T_BG);
    cv_rrect(0, 0, (int32_t)SCOPE_W, (int32_t)SCOPE_H, 6, T_SURF, T_BG);
    for (x = (int32_t)SCOPE_W / 4; x < (int32_t)SCOPE_W; x += (int32_t)SCOPE_W / 4)        /* the beat marks, dotted */
        for (i = 8; i < SCOPE_H - 8u; i += 4u)
            cv_rect(x, (int32_t)i, 1, 2, T_RAISE);
    cv_rect(6, cy, (int32_t)SCOPE_W - 12, 1, T_RAISE);
    for (x = 0; x < (int32_t)SCOPE_W; x++) {
        int32_t y = cy - (int32_t)snap[trig + (uint32_t)x] * a / scale;
        if (y < 4)
            y = 4;
        if (y > (int32_t)SCOPE_H - 5)
            y = (int32_t)SCOPE_H - 5;
        if (x > 5 && x < (int32_t)SCOPE_W - 5)
            cv_rect(x, y < cy ? y : cy, 1, (y < cy ? cy - y : y - cy) + 1, fillc);       /* the wave, filled to the centre */
        if (x)
            cv_line_t(x - 1, py, x, y, T_THEME, 2);
        py = y;
    }
    if (!dbx_target(&st) && (dbx_ui.page == PAGE_EDIT || dbx_ui.page == PAGE_EDIT2) && DBX_E.steps[dbx_ui.sel])
        dbx_cat(b, "ALL STEPS");
    else if (dbx_target(&st)) {
        dbx_cat(b, "STEP ");
        dbx_cat_int(b, (int32_t)st + 1);
        if (DBX_FXPAGE) {
            dbx_cat(b, "  MIX ");
            dbx_cat_int(b, DBX_F.mix[dbx_ui.fxsel][st]);
        } else {
            dbx_cat(b, "  VEL ");
            dbx_cat_int(b, DBX_E.vel[dbx_ui.sel][st]);
        }
    }
    dbx_cat_int(r, dbx_seq.div);
    dbx_cat(r, dbx_seq.div == 8u ? "  8 BEATS" : dbx_seq.div == 16u ? "  4 BEATS" : "  2 BEATS");
    if (dbx_seq.rep_req) {                              /* SEQ held: what loops */
        static const char *const LN[4] = {"2 BEATS", "1 BEAT", "1/2 BEAT", "1/4 BEAT"};
        r[0] = 0;
        dbx_cat(r, "LOOP ");
        dbx_cat(r, LN[dbx_seq.rep_r & 3u]);
    }
    cv_text(9, 4, &AF_S, b, T_MID);
    cv_text_r((int32_t)SCOPE_W - 9, 4, &AF_S, r, dbx_seq.rep_req ? T_ACCENT : T_MID, T_SURF);
    cv_blit(SCOPE_X, SCOPE_Y);
    lcd_sync();
}

/* ---- an LFO page's picture, in the oscilloscope's place: two cycles of the LFO as it is set (its shape, shifted by PHASE, scaled by
 * AMOUNT), and the cycle's position now as a cursor (a line and a dot; in the second cycle a fainter line). LFO 1 (the volume): the gain,
 * the top is the sound as it is. LFO 2 (the pan): up is right, down is left, the middle is unmoved. RANDOM draws the value now and the next
 * one it will take. In the corners: what it moves and its value now, and its rate. */
#define LFO_X0 6                                    /* the curve's first column, and a cycle's width: two of them fill the panel */
#define LFO_CW 102
#define LFO_TOP 20
#define LFO_BOT ((int32_t)SCOPE_H - 8)
static int32_t dbx_lfo_pos(uint32_t k, uint32_t ph, int32_t held)   /* the LFO k's output now: the volume's gain Q12, or the pan's position -32767..32767 */
{
    const uint8_t *q = dbx_lfo.p[k];
    int32_t v = dbx_lfo_shape(q[LFO_SHAPE], ph + (uint32_t)q[LFO_PHASE] * 178956971u, held), amt = q[LFO_AMT];
    return k == 0u ? 4096 - amt * (4095 - ((v + 32767) >> 4)) / 100 : v * amt / 100;
}

static void dbx_draw_lfo_view(void)
{
    uint32_t k = dbx_ui.page == PAGE_LFO2, i;
    const uint8_t *q = dbx_lfo.p[k];
    char b[24] = "", r[24] = "";
    int32_t cy = (LFO_TOP + LFO_BOT) / 2, half = (LFO_BOT - LFO_TOP) / 2, x, py = k ? cy : LFO_BOT, cx, now;
    uint16_t fillc = ux_mix(T_SURF, T_THEME, 24), faint = ux_mix(T_SURF, T_ACCENT, 45);
    uint32_t ph_now = dbx_lfo.ph[k];
    int32_t held[2] = {dbx_lfo.held[k], dbx_lfo_value_of(dbx_lfo_xorshift(dbx_lfo.rng[k]))};
    cv_begin(SCOPE_W, SCOPE_H, T_BG);
    cv_rrect(0, 0, (int32_t)SCOPE_W, (int32_t)SCOPE_H, 6, T_SURF, T_BG);
    for (i = 8; i < SCOPE_H - 8u; i += 4u)                 /* the cycles' border, dotted */
        cv_rect(LFO_X0 + LFO_CW, (int32_t)i, 1, 2, T_RAISE);
    cv_rect(6, k ? cy : LFO_TOP, (int32_t)SCOPE_W - 12, 1, T_RAISE);         /* the line of rest: the middle (pan), the full gain (volume) */
    for (x = 0; x < 2 * LFO_CW; x++) {
        uint32_t cyc = (uint32_t)x / LFO_CW;
        uint32_t ph = (uint32_t)((uint32_t)x % LFO_CW) * (0xFFFFFFFFu / LFO_CW);
        int32_t p = dbx_lfo_pos(k, ph, held[cyc]), y;
        y = k ? cy - p * half / 32767 : LFO_BOT - p * (LFO_BOT - LFO_TOP) / 4096;
        cv_rect(LFO_X0 + x, y < (k ? cy : LFO_BOT) ? y : (k ? cy : LFO_BOT), 1, (y < (k ? cy : LFO_BOT) ? (k ? cy : LFO_BOT) - y : y - (k ? cy : LFO_BOT)) + 1, fillc);
        if (x)
            cv_line_t(LFO_X0 + x - 1, py, LFO_X0 + x, y, T_THEME, 2);
        py = y;
    }
    cx = (int32_t)(ph_now >> 24) * LFO_CW >> 8;           /* the cycle's position now */
    now = dbx_lfo_pos(k, ph_now, held[0]);
    for (i = 14; i < SCOPE_H - 4u; i++) {                    /* the cursor: a line through both cycles, the second fainter */
        cv_rect(LFO_X0 + cx, (int32_t)i, 1, 1, T_ACCENT);
        cv_rect(LFO_X0 + LFO_CW + cx, (int32_t)i, 1, 1, faint);
    }
    {
        int32_t y = k ? cy - now * half / 32767 : LFO_BOT - now * (LFO_BOT - LFO_TOP) / 4096;
        cv_rect(LFO_X0 + cx - 2, y - 2, 5, 5, T_ACCENT);                   /* ... and a dot on the curve */
    }
    dbx_cat(b, k ? "PAN " : "VOLUME ");
    if (q[LFO_AMT] == 0u)
        dbx_cat(b, "OFF");
    else if (k == 0u) {
        dbx_cat_int(b, now * 100 / 4096);
        dbx_cat(b, "%");
    } else if (now / 328 == 0) {
        dbx_cat(b, "C");
    } else {
        dbx_cat(b, now > 0 ? "R " : "L ");
        dbx_cat_int(b, (now < 0 ? -now : now) / 328);
        dbx_cat(b, "%");
    }
    dbx_cat(r, LFO_RATE_NAME[q[LFO_RATE]]);
    cv_text(9, 4, &AF_S, b, T_MID);
    cv_text_r((int32_t)SCOPE_W - 9, 4, &AF_S, r, T_MID, T_SURF);
    cv_blit(SCOPE_X, SCOPE_Y);
    lcd_sync();
}

static void dbx_row(uint32_t i, const char *label, const char *value, uint32_t w)      /* a labelled value and its bar */
{
    uint32_t y = ROW_Y + i * ROW_PITCH;
    lcd_fill(12, y + 13u, 216, 6, T_BG);                                   /* (the marks of another page's row) */
    draw_text_line(12, y, 100, &AF_S, label, T_MID, T_BG, 0);
    draw_text_line(128, y, 100, &AF_S, value, T_TEXT, T_BG, 2);
    lcd_fill(12, y + 15u, 216, 3, T_LINE);
    if (w)
        lcd_fill(12, y + 15u, w, 3, T_THEME);
}

static void dbx_row_bipolar(uint32_t i, const char *label, const char *value, int32_t v, int32_t range)   /* from the centre */
{
    uint32_t y = ROW_Y + i * ROW_PITCH, w = (uint32_t)(v < 0 ? -v : v) * 108u / (uint32_t)range;
    lcd_fill(12, y + 13u, 216, 6, T_BG);
    draw_text_line(12, y, 100, &AF_S, label, T_MID, T_BG, 0);
    draw_text_line(128, y, 100, &AF_S, value, T_TEXT, T_BG, 2);
    lcd_fill(12, y + 15u, 216, 3, T_LINE);
    lcd_fill(119, y + 13u, 2, 6, T_MID);                                   /* the centre */
    if (w)
        lcd_fill(v < 0 ? 120u - w : 120u, y + 15u, w, 3, T_THEME);
}

static void dbx_row_nudge(uint32_t i, const char *label, const char *value, int32_t n)
{   /* the track is the step; the note is a block as wide as the step, pushed right when late (cut at the left) and left
     * when early (cut at the right): a notch is 1/48 of a step */
    uint32_t y = ROW_Y + i * ROW_PITCH, off = (uint32_t)(n < 0 ? -n : n) * 216u / 48u;
    lcd_fill(12, y + 13u, 216, 6, T_BG);
    draw_text_line(12, y, 100, &AF_S, label, T_MID, T_BG, 0);
    draw_text_line(128, y, 100, &AF_S, value, T_TEXT, T_BG, 2);
    lcd_fill(12, y + 15u, 216, 3, T_LINE);
    lcd_fill(n > 0 ? 12u + off : 12u, y + 15u, 216u - off, 3, T_THEME);
    lcd_fill(12, y + 13u, 2, 6, T_MID);                                    /* the step's two ends */
    lcd_fill(226, y + 13u, 2, 6, T_MID);
}

static void dbx_row_choice(uint32_t i, const char *label, const char *value, uint32_t n, uint32_t at)   /* n notches */
{
    uint32_t y = ROW_Y + i * ROW_PITCH, k, w = (216u - 3u * (n - 1u)) / n;
    lcd_fill(12, y + 13u, 216, 6, T_BG);                                   /* (the gaps: not the last page's bar) */
    draw_text_line(12, y, 100, &AF_S, label, T_MID, T_BG, 0);
    draw_text_line(128, y, 100, &AF_S, value, T_TEXT, T_BG, 2);
    for (k = 0; k < n; k++)
        lcd_fill(12u + k * (w + 3u), y + 15u, w, 3, k == at ? T_THEME : T_LINE);
}

static void dbx_draw_lane_page(void)
{
    static const char *const NAME[4] = {"TUNE", "DECAY", "TONE", "LEVEL"};
    const dbx_lane_t *l = &lane[dbx_ui.sel];
    uint32_t i;
    draw_text_line(12, TITLE_Y, 216, &AF_L, DBX_NAME[dbx_ui.sel], T_THEME, T_BG, 0);
    if (lane[dbx_ui.sel].mute)
        draw_text_line(112, TITLE_Y + 10u, 56, &AF_S, "MUTED", DBX_RED_LIGHT, T_BG, 2);
    {
        char pv[16] = "PAN ";
        dbx_cat_pan(pv, lane[dbx_ui.sel].pan);
        draw_text_line(170, TITLE_Y + 10u, 58, &AF_S, pv, lane[dbx_ui.sel].pan ? T_THEME : T_MID, T_BG, 2);
    }
    for (i = 0; i < 4u; i++) {
        char v[12];
        uint32_t w;
        int32_t val = i == 1 ? l->p.decay : i == 2 ? l->p.tone : l->p.level;
        if (i == 0 && dbx_ui.sel == L_KICK) {           /* the kick: its pitch as a note */
            int32_t p16, lo, hi, n16 = dbx_note16(dbx_ui.sel);
            dbx_range(l->p.type, &p16, &lo, &hi);
            dbx_fmt_note(v, n16);
            w = (uint32_t)(n16 - lo) * 216u / (uint32_t)(hi - lo);
        } else if (i == 0) {                            /* the others: semitones from the designed pitch */
            int32_t lo, hi;
            dbx_tune_range(dbx_ui.sel, &lo, &hi);
            dbx_fmt_tune(v, l->p.tune);
            w = hi > lo ? (uint32_t)(l->p.tune - lo) * 216u / (uint32_t)(hi - lo) : 0u;
        } else {
            dbx_fmt_int(v, val);
            w = (uint32_t)val * 216u / 127u;
        }
        dbx_row(i, NAME[i], v, w);
    }
}

static void dbx_sync_wait(char *v)                       /* SYNC's value while it waits for the computer; on the FM-1 what has arrived so far */
{
    v[0] = 0;
#ifdef DBX_MIDI_DRAIN
    if (!dbx_usb_powered()) {
        dbx_cat(v, "NO USB");
        return;
    }
    dbx_cat(v, "WAIT P");                                   /* (P: the host's MIDI packets, C: its clock pulses; a thousand, then round) */
    dbx_cat_int(v, (int32_t)(dbx_pk_n % 1000u));
    dbx_cat(v, " C");
    dbx_cat_int(v, (int32_t)(dbx_clk_n % 1000u));
#else
    dbx_cat(v, "PC WAITING");
#endif
}

static void dbx_draw_global_page(void)                  /* GLO: BPM, VOLUME, SYNC, SUBDIVISION */
{
    char v[16] = "", w[24];
    draw_text_line(12, TITLE_Y, 216, &AF_L, "GLOBAL 1", T_THEME, T_BG, 0);
    if (dbx_seq.ext)
        dbx_cat(v, dbx_seq_locked() ? "PC " : "PC WAIT");
    if (!dbx_seq.ext || dbx_seq_locked())
        dbx_cat_int(v, dbx_seq.bpm);
    dbx_row(0, "BPM", v, (uint32_t)(clamp((int32_t)dbx_seq.bpm, (int32_t)DBX_BPM_MIN, (int32_t)DBX_BPM_MAX) -
                                   (int32_t)DBX_BPM_MIN) * 216u / (DBX_BPM_MAX - DBX_BPM_MIN));
    v[0] = 0;
    dbx_cat_int(v, dbx_ui.vol);
    dbx_row(1, "VOLUME", v, (uint32_t)dbx_ui.vol * 216u / 127u);
    dbx_sync_wait(w);
    dbx_row_choice(2, "SYNC", dbx_seq.ext ? (dbx_seq_locked() ? (dbx_seq.pl_lock ? "PC LOCKED" : "PC CLOCK") : w) : "INTERNAL", 2, dbx_seq.ext);
    v[0] = 0;
    dbx_cat(v, "1/");
    dbx_cat_int(v, dbx_seq.div);
    dbx_row_choice(3, "SUBDIVISION", v, 3, dbx_seq_div_index());
}

static void dbx_fx_fmt(uint32_t e, uint32_t i, char *d)   /* the value of parameter i of effect e as the screen shows it */
{
    const dbx_fxp_t *q = dbx_fxp(e, i);
    uint32_t v = dbx_fx.p[e][i], h, ms;
    d[0] = 0;
    switch (q->kind) {
    case PK_PCT:
        dbx_cat_int(d, (int32_t)(v * 100u / 127u));
        dbx_cat(d, "%");
        break;
    case PK_CUT:
        h = CUTOFF_HZ[v];
        if (h >= 1000u) {
            dbx_cat_int(d, (int32_t)(h / 1000u));
            dbx_cat(d, ".");
            dbx_cat_int(d, (int32_t)(h % 1000u / 100u));
            dbx_cat(d, " KHZ");
        } else {
            dbx_cat_int(d, (int32_t)h);
            dbx_cat(d, " HZ");
        }
        break;
    case PK_COUNT3:
        dbx_cat_int(d, 1 + (int32_t)dbx_opt(v, 3u));
        break;
    case PK_BITS:
        dbx_cat_int(d, 16 - (int32_t)(v * 15u / 127u));
        dbx_cat(d, " BIT");
        break;
    case PK_HOLD:
        dbx_cat(d, "X");
        dbx_cat_int(d, 1 + (int32_t)(v * 31u / 127u));
        break;
    case PK_SPEED:
        dbx_cat_int(d, 100 - (int32_t)(v * 75u / 127u));
        dbx_cat(d, "%");
        break;
    case PK_GRAIN:
        dbx_cat_int(d, (int32_t)((512u + v * 28u) * 10u / 441u));
        dbx_cat(d, " MS");
        break;
    case PK_GAIN:
        dbx_cat_int(d, (int32_t)(v * 100u / 64u));
        dbx_cat(d, "%");
        break;
    case PK_DRIVE:
        h = (uint32_t)sat_drive_q8(v);
        dbx_cat(d, "X");
        dbx_cat_int(d, (int32_t)(h / 256u));
        dbx_cat(d, ".");
        dbx_cat_int(d, (int32_t)(h % 256u * 10u / 256u));
        break;
    case PK_PAN: {
        int32_t pos = ((int32_t)v - 64) * 100 / 64;
        if (pos == 0)
            dbx_cat(d, "CENTER");
        else {
            dbx_cat(d, pos < 0 ? "L" : "R");
            dbx_cat_int(d, pos < 0 ? -pos : pos);
        }
        break;
    }
    case PK_HZ:
        if (v == 0u)
            dbx_cat(d, "OFF");
        else {
            h = LFO_HZ_X100[v];
            dbx_cat_int(d, (int32_t)(h / 100u));
            dbx_cat(d, ".");
            dbx_cat_int(d, (int32_t)(h % 100u / 10u));
            dbx_cat(d, " HZ");
        }
        break;
    case PK_ONOFF:
        dbx_cat(d, v >= 64u ? "ON" : "OFF");
        break;
    case PK_SLOPE:
        dbx_cat(d, dbx_opt(v, 2u) ? "24 DB" : "12 DB");
        break;
    case PK_SHAPE:
        dbx_cat(d, dbx_opt(v, 3u) == 0u ? "SINE" : dbx_opt(v, 3u) == 1u ? "TRI" : "SQUARE");
        break;
    case PK_DTIME:
        dbx_cat(d, DBX_DTIME_NAME[dbx_opt(v, 6u)]);
        break;
    case PK_PRE:
        dbx_cat_int(d, (int32_t)(v * 100u / 127u));
        dbx_cat(d, " MS");
        break;
    default:                                            /* PK_ATK, PK_REL */
        ms = q->kind == PK_ATK ? dbx_atk_ms(v) : dbx_rel_ms(v);
        if (ms >= 1000u) {
            dbx_cat_int(d, (int32_t)(ms / 1000u));
            dbx_cat(d, ".");
            dbx_cat_int(d, (int32_t)(ms % 1000u / 100u));
            dbx_cat(d, " S");
        } else {
            dbx_cat_int(d, (int32_t)ms);
            dbx_cat(d, " MS");
        }
        break;
    }
}

static void dbx_draw_fx_page(void)                      /* FX: the effect picked, four parameters a page */
{
    uint32_t e = dbx_ui.fxsel, base = dbx_ui.page == PAGE_FX2 ? 4u : 0u, k;
    char v[16];
    draw_text_line(12, TITLE_Y, 216, &AF_L, DBX_FX_NAME[e], T_THEME, T_BG, 0);
    draw_text_line(196, TITLE_Y + 10u, 32, &AF_S, base ? "2/2" : "1/2", T_MID, T_BG, 2);
    for (k = 0; k < 4u; k++) {
        const dbx_fxp_t *q = dbx_fxp(e, base + k);
        dbx_fx_fmt(e, base + k, v);
        if (q->opts)
            dbx_row_choice(k, q->name, v, q->opts, dbx_opt(dbx_fx.p[e][base + k], q->opts));
        else
            dbx_row(k, q->name, v, (uint32_t)dbx_fx.p[e][base + k] * 216u / 127u);
    }
}

static void dbx_cat_hz(char *d, uint32_t h)             /* 355 HZ, 2.5 KHZ */
{
    if (h >= 1000u) {
        dbx_cat_int(d, (int32_t)(h / 1000u));
        dbx_cat(d, ".");
        dbx_cat_int(d, (int32_t)(h % 1000u / 100u));
        dbx_cat(d, " KHZ");
    } else {
        dbx_cat_int(d, (int32_t)h);
        dbx_cat(d, " HZ");
    }
}

static void dbx_draw_scl_page(void)                     /* SCL: HPF, LPF, OTT, CLIP */
{
    char v[24];
    uint32_t hp = dbx_mst.p[0], lp = dbx_mst.p[1], ot = dbx_mst.p[2], cl = dbx_mst.p[3], db10 = cl * 180u / 127u;
    draw_text_line(12, TITLE_Y, 216, &AF_L, "MASTER", T_THEME, T_BG, 0);
    v[0] = 0;
    if (hp == 0u)
        dbx_cat(v, "OFF");
    else
        dbx_cat_hz(v, CUTOFF_HZ[dbx_hpf_idx(hp)]);
    dbx_row(0, "HPF", v, hp * 216u / 127u);
    v[0] = 0;
    if (lp >= 127u)
        dbx_cat(v, "OFF");
    else
        dbx_cat_hz(v, CUTOFF_HZ[dbx_lpf_idx(lp)]);
    dbx_row(1, "LPF", v, lp * 216u / 127u);
    v[0] = 0;
    if (ot == 0u)
        dbx_cat(v, "DRY");
    else {
        dbx_cat_int(v, (int32_t)(ot * 100u / 127u));
        dbx_cat(v, "% WET");
    }
    dbx_row(2, "OTT", v, ot * 216u / 127u);
    v[0] = 0;
    dbx_cat(v, "SOFT +");
    dbx_cat_int(v, (int32_t)(db10 / 10u));
    dbx_cat(v, ".");
    dbx_cat_int(v, (int32_t)(db10 % 10u));
    dbx_cat(v, " DB");
    dbx_row(3, "CLIP", v, cl * 216u / 127u);
}

static void dbx_draw_lfo_page(void)                     /* LFO 1 (the volume) or LFO 2 (the pan): RATE, AMOUNT, SHAPE, PHASE */
{
    uint32_t k = dbx_ui.page == PAGE_LFO2;
    const uint8_t *q = dbx_lfo.p[k];
    char v[16] = "";
    draw_text_line(12, TITLE_Y, 216, &AF_L, k ? "LFO 2 PAN" : "LFO 1 VOLUME", T_THEME, T_BG, 0);
    dbx_row(0, "RATE", LFO_RATE_NAME[q[LFO_RATE]], (uint32_t)q[LFO_RATE] * 216u / (LFO_NRATE - 1u));
    if (q[LFO_AMT] == 0u)
        dbx_cat(v, "OFF");
    else {
        dbx_cat_int(v, q[LFO_AMT]);
        dbx_cat(v, "%");
    }
    dbx_row(1, "AMOUNT", v, (uint32_t)q[LFO_AMT] * 216u / 100u);
    dbx_row(2, "SHAPE", LFO_SHAPE_NAME[q[LFO_SHAPE]], (uint32_t)q[LFO_SHAPE] * 216u / (LFO_NSHAPE - 1u));
    v[0] = 0;
    dbx_cat_int(v, (int32_t)q[LFO_PHASE] * 15);
    dbx_cat(v, " DEG");
    dbx_row(3, "PHASE", v, (uint32_t)q[LFO_PHASE] * 216u / (LFO_NPHASE - 1u));
}

static void dbx_draw_env_page(void)                     /* ENV 1 (DUCK): AMOUNT RELEASE SOURCE TARGET; ENV 2 (SHAPE): KICK ATTACK ATTACK DECAY GATE */
{
    char v[20] = "";
    const uint8_t *q = dbx_env.p;
    if (dbx_ui.page == PAGE_ENV1) {
        uint32_t ms = dbx_env_rel_ms(q[EV_REL]);
        draw_text_line(12, TITLE_Y, 216, &AF_L, "ENV 1 DUCK", T_THEME, T_BG, 0);
        if (q[EV_AMT] == 0u)
            dbx_cat(v, "OFF");
        else {
            dbx_cat_int(v, q[EV_AMT]);
            dbx_cat(v, "%");
        }
        dbx_row(0, "AMOUNT", v, (uint32_t)q[EV_AMT] * 216u / 100u);
        v[0] = 0;
        dbx_cat_int(v, (int32_t)ms);
        dbx_cat(v, " MS");
        dbx_row(1, "RELEASE", v, (uint32_t)q[EV_REL] * 216u / 127u);
        dbx_row(2, "SOURCE", DBX_NAME[q[EV_SRC]], (uint32_t)q[EV_SRC] * 216u / (DBX_NLANE - 1u));
        dbx_row(3, "TARGET", EV_TGT_NAME[q[EV_TGT]], (uint32_t)q[EV_TGT] * 216u / 2u);
        return;
    }
    draw_text_line(12, TITLE_Y, 216, &AF_L, "ENV 2 SHAPE", T_THEME, T_BG, 0);
    if (q[EV_KATK] == 0u)
        dbx_cat(v, "AS DESIGNED");
    else {
        dbx_cat(v, "+");
        dbx_cat_int(v, (int32_t)q[EV_KATK] * 100 / 127);
        dbx_cat(v, "%");
    }
    dbx_row(0, "KICK ATTACK", v, (uint32_t)q[EV_KATK] * 216u / 127u);
    v[0] = 0;
    if (q[EV_ATK] == 64u)
        dbx_cat(v, "AS DESIGNED");
    else {
        dbx_cat(v, q[EV_ATK] < 64u ? "SOFT " : "HARD ");
        dbx_cat_int(v, (q[EV_ATK] < 64u ? 64 - (int32_t)q[EV_ATK] : (int32_t)q[EV_ATK] - 64) * 100 / 64);
        dbx_cat(v, "%");
    }
    dbx_row_bipolar(1, "ATTACK", v, (int32_t)q[EV_ATK] - 64, 64);
    v[0] = 0;
    if (q[EV_DEC] == 64u)
        dbx_cat(v, "AS DESIGNED");
    else {
        dbx_cat(v, q[EV_DEC] < 64u ? "SHORTER " : "LONGER ");
        dbx_cat_int(v, (q[EV_DEC] < 64u ? 64 - (int32_t)q[EV_DEC] : (int32_t)q[EV_DEC] - 64) * 100 / 64);
        dbx_cat(v, "%");
    }
    dbx_row_bipolar(2, "DECAY", v, (int32_t)q[EV_DEC] - 64, 64);
    v[0] = 0;
    if (q[EV_GATE] >= 127u)
        dbx_cat(v, "OFF");
    else {
        uint32_t ms = dbx_env_gate_samples(q[EV_GATE]) * 10u / 441u;
        if (ms >= 1000u) {
            dbx_cat_int(v, (int32_t)(ms / 1000u));
            dbx_cat(v, ".");
            dbx_cat_int(v, (int32_t)(ms % 1000u / 100u));
            dbx_cat(v, " S");
        } else {
            dbx_cat_int(v, (int32_t)ms);
            dbx_cat(v, " MS");
        }
    }
    dbx_row(3, "GATE", v, (uint32_t)(127u - q[EV_GATE]) * 216u / 127u);
}

/* ---- an ENV page's picture, in the oscilloscope's place. ENV 1: the duck, a full hit then the way back over RELEASE (the gain at the top: none), and a
 * dot that follows the duck now. ENV 2: the first 30 ms of a hit's gain, the kick's (thick) and every other lane's (thin), the line of 1.0, and the
 * GATE as a violet line when it falls in those 30 ms. */
#define EV_TOP 20
#define EV_BOT ((int32_t)SCOPE_H - 8)
static void dbx_draw_env_view(void)
{
    uint32_t k = dbx_ui.page == PAGE_ENV2, i;
    const uint8_t *q = dbx_env.p;
    char b[24] = "", r[24] = "";
    int32_t x, py = EV_TOP, py2 = EV_BOT;
    uint16_t fillc = ux_mix(T_SURF, T_THEME, 24);
    cv_begin(SCOPE_W, SCOPE_H, T_BG);
    cv_rrect(0, 0, (int32_t)SCOPE_W, (int32_t)SCOPE_H, 6, T_SURF, T_BG);
    if (k == 0u) {
        int32_t x0 = 22, rw = 130, amt = q[EV_AMT], ymin = EV_TOP + amt * (EV_BOT - EV_TOP) / 100, xl, yl, g = dbx_env.g1;
        for (i = 8; i < SCOPE_H - 8u; i += 4u)                  /* the hit, dotted */
            cv_rect(x0, (int32_t)i, 1, 2, T_RAISE);
        for (x = 6; x < (int32_t)SCOPE_W - 6; x++) {
            int32_t y = x < x0 ? EV_TOP : x >= x0 + rw ? EV_TOP : ymin - (ymin - EV_TOP) * (x - x0) / rw;
            cv_rect(x, y, 1, EV_BOT - y + 1, fillc);
            if (x > 6)
                cv_line_t(x - 1, py, x, y, T_THEME, 2);
            py = y;
        }
        xl = x0 + (((65536 - dbx_env.env) >> 4) * rw >> 12);
        if (xl > x0 + rw)
            xl = x0 + rw;
        yl = EV_BOT - g * (EV_BOT - EV_TOP) / 4096;
        cv_rect(xl - 2, yl - 2, 5, 5, T_ACCENT);
        dbx_cat(b, "DUCK ");
        dbx_cat_int(b, (4096 - g) * 100 / 4096);
        dbx_cat(b, "%");
        dbx_cat(r, DBX_NAME[q[EV_SRC]]);
        dbx_cat(r, " > ");
        dbx_cat(r, EV_TGT_NAME[q[EV_TGT]]);
    } else {
        uint32_t tg = q[EV_GATE] < 127u ? dbx_env_gate_samples(q[EV_GATE]) : 0xFFFFFFu;
        for (x = 8; x < 208; x++) {
            uint32_t age = (uint32_t)(x - 8) * 1323u / 200u;
            int32_t gk = dbx_env_shape(L_KICK, age), go = dbx_env_shape(L_SNARE, age);
            int32_t yk = EV_BOT - (gk > 13107 ? 13107 : gk) * (EV_BOT - EV_TOP) / 13107, yo = EV_BOT - (go > 13107 ? 13107 : go) * (EV_BOT - EV_TOP) / 13107;
            if (x == 8 || (x & 3) == 0)
                cv_rect(x, EV_BOT - 4096 * (EV_BOT - EV_TOP) / 13107, 2, 1, T_RAISE);     /* the line of 1.0, dotted */
            if (x > 8) {
                cv_line_t(x - 1, py2, x, yo, T_MID, 1);
                cv_line_t(x - 1, py, x, yk, T_THEME, 2);
            }
            py = yk;
            py2 = yo;
        }
        if (tg < 1323u) {
            int32_t gx = 8 + (int32_t)(tg * 200u / 1323u);
            for (i = 12; i < SCOPE_H - 6u; i++)
                cv_rect(gx, (int32_t)i, 1, 1, T_ACCENT);
        }
        dbx_cat(b, "KICK");
        dbx_cat(r, "OTHERS  30 MS");
    }
    cv_text(9, 4, &AF_S, b, T_MID);
    cv_text_r((int32_t)SCOPE_W - 9, 4, &AF_S, r, T_MID, T_SURF);
    cv_blit(SCOPE_X, SCOPE_Y);
    lcd_sync();
}

static void dbx_draw_arp_page(void)                     /* ARP 1 (EUCLID): HITS ROTATE LENGTH ACCENT; ARP 2 (MUTATE): AMOUNT EVERY SCOPE DRIFT */
{
    char v[20] = "";
    const uint8_t *q = dbx_arp.p;
    if (dbx_ui.page == PAGE_ARP1) {
        draw_text_line(12, TITLE_Y, 216, &AF_L, "ARP 1 EUCLID", T_THEME, T_BG, 0);
        dbx_cat_int(v, q[AP_HITS]);
        dbx_row(0, "HITS", v, (uint32_t)q[AP_HITS] * 216u / 16u);
        v[0] = 0;
        dbx_cat_int(v, q[AP_ROT]);
        dbx_row(1, "ROTATE", v, (uint32_t)q[AP_ROT] * 216u / 15u);
        v[0] = 0;
        dbx_cat_int(v, q[AP_LEN]);
        dbx_cat(v, " STEPS");
        dbx_row(2, "LENGTH", v, (uint32_t)(q[AP_LEN] - 2u) * 216u / 14u);
        v[0] = 0;
        if (q[AP_ACC] == 0u)
            dbx_cat(v, "OFF");
        else {
            dbx_cat_int(v, q[AP_ACC]);
            dbx_cat(v, "%");
        }
        dbx_row(3, "ACCENT", v, (uint32_t)q[AP_ACC] * 216u / 100u);
        return;
    }
    const uint8_t *m = dbx_arp.m[dbx_ui.sel];           /* (the selected lane's own) */
    draw_text_line(12, TITLE_Y, 216, &AF_L, "ARP 2 MUTATE", T_THEME, T_BG, 0);
    if (m[0] == 0u)
        dbx_cat(v, "OFF");
    else {
        dbx_cat_int(v, m[0]);
        dbx_cat(v, "%");
    }
    dbx_row(0, "AMOUNT", v, (uint32_t)m[0] * 216u / 100u);
    v[0] = 0;
    dbx_cat_int(v, 1 << m[1]);
    dbx_cat(v, m[1] ? " PASSES" : " PASS");
    dbx_row(1, "EVERY", v, (uint32_t)m[1] * 216u / 3u);
    dbx_row(2, "SCOPE", q[AM_SCOPE] ? "ALL LANES" : "THIS LANE", q[AM_SCOPE] ? 216u : 0u);
    v[0] = 0;
    if (m[3] == 0u)
        dbx_cat(v, "OFF");
    else {
        dbx_cat_int(v, m[3]);
        dbx_cat(v, "%");
    }
    dbx_row(3, "DRIFT", v, (uint32_t)m[3] * 216u / 100u);
}

/* ---- an ARP page's picture, in the oscilloscope's place: the 16 steps of the lane on a ring (the hits full, the rests small), a line from hit to
 * hit, and the step playing in red; the mutation adds how many passes until the next one */
static void dbx_draw_arp_view(void)
{
    uint32_t s, last = DBX_NSTEP, first = DBX_NSTEP;
    int32_t cx = (int32_t)SCOPE_W / 2, cy = (int32_t)SCOPE_H / 2 + 6, rad = 25, px = 0, py = 0, fx = 0, fy = 0;
    char b[24] = "", r[24] = "";
    uint16_t line = ux_mix(T_SURF, T_THEME, 45);
    uint32_t steps = DBX_E.steps[dbx_ui.sel], n_on = 0;
    cv_begin(SCOPE_W, SCOPE_H, T_BG);
    cv_rrect(0, 0, (int32_t)SCOPE_W, (int32_t)SCOPE_H, 6, T_SURF, T_BG);
    for (s = 0; s < DBX_NSTEP; s++) {                      /* the line first, under the dots */
        int32_t x = cx + SINE[s * 64u] * rad / 32767, y = cy - SINE[(s * 64u + 256u) & 1023u] * rad / 32767;
        if ((steps >> s) & 1u) {
            if (last != DBX_NSTEP)
                cv_line_t(px, py, x, y, line, 1);
            else {
                fx = x;
                fy = y;
                first = s;
            }
            px = x;
            py = y;
            last = s;
            n_on++;
        }
    }
    if (first != DBX_NSTEP && n_on > 2u)
        cv_line_t(px, py, fx, fy, line, 1);
    for (s = 0; s < DBX_NSTEP; s++) {
        int32_t x = cx + SINE[s * 64u] * rad / 32767, y = cy - SINE[(s * 64u + 256u) & 1023u] * rad / 32767;
        uint32_t on = (steps >> s) & 1u, now = dbx_seq.playing && dbx_seq.cur == s;
        if (now)
            cv_rect(x - 4, y - 4, 9, 9, DBX_RED_PLAY);
        if (on)
            cv_rect(x - 3, y - 3, 7, 7, T_THEME);
        else
            cv_rect(x - 1, y - 1, 3, 3, now ? T_BG : T_DIM);
    }
    if (dbx_ui.page == PAGE_ARP1) {
        dbx_cat(b, DBX_NAME[dbx_ui.sel]);
        dbx_cat_int(r, (int32_t)n_on);
        dbx_cat(r, n_on == 1u ? " HIT" : " HITS");
    } else {
        const uint8_t *m = dbx_arp.m[dbx_ui.sel];
        uint32_t every = 1u << m[1], left = every - dbx_arp.n % every;
        dbx_cat(b, dbx_arp.p[AM_SCOPE] ? "ALL LANES" : DBX_NAME[dbx_ui.sel]);
        if (m[0] || m[3]) {
            dbx_cat(r, "NEXT IN ");
            dbx_cat_int(r, (int32_t)left);
            dbx_cat(r, left == 1u ? " PASS" : " PASSES");
        } else
            dbx_cat(r, "OFF");
    }
    cv_text(9, 4, &AF_S, b, T_MID);
    cv_text_r((int32_t)SCOPE_W - 9, 4, &AF_S, r, T_MID, T_SURF);
    cv_blit(SCOPE_X, SCOPE_Y);
    lcd_sync();
}

/* EDIT 2: the step's own DECAY, TONE, EXTRA (named by the drum) and PAN, added to the lane's (none picked: the lane's steps') */
static const char *const DBX_EXTRA_NAME[DVT_COUNT] = {"DRIVE", "DRIVE", "SNAPPY", "SPREAD", "SIZZLE", "SIZZLE", "BEND", "BEND", "DRIVE", "DRIVE", "STRIKE", "STRIKE"};

static void dbx_draw_edit2_page(void)
{
    int32_t val[4] = {0, 0, 0, 0};
    uint32_t mixed[4] = {0, 0, 0, 0}, has = dbx_edit_values(4, 4, val, mixed), k;
    const char *name[4] = {"DECAY", "TONE", DBX_EXTRA_NAME[dv_run_type(lane[dbx_ui.sel].p.type)], "PAN"};
    char v[12];
    draw_text_line(12, TITLE_Y, 216, &AF_L, "EDIT 2 STEP", T_THEME, T_BG, 0);
    for (k = 0; k < 4u; k++) {
        v[0] = 0;
        if (!has)
            dbx_cat(v, "--");
        else if (mixed[k])
            dbx_cat(v, "MIXED");
        else if (k == 3u) {                                         /* the pan, in % of the way to a side */
            dbx_cat_signed(v, val[k] * 100 / 32);
            dbx_cat(v, "%");
        } else
            dbx_cat_signed(v, val[k]);
        dbx_row_bipolar(k, name[k], v, has && !mixed[k] ? val[k] : 0, k == 3u ? 32 : 64);
    }
}

static void dbx_draw_global2_page(void)                 /* GLO, second page: SWING, FX MIX, KIT, ERASE */
{
    char v[32] = "";
    uint32_t y = ROW_Y + 3u * ROW_PITCH, w = (uint32_t)dbx_ui.er * 216u / DBX_ERASE_CLICKS, pct = (uint32_t)dbx_ui.er * 100u / DBX_ERASE_CLICKS;
    draw_text_line(12, TITLE_Y, 216, &AF_L, "GLOBAL 2", T_THEME, T_BG, 0);
    dbx_cat_int(v, dbx_seq.swing);
    dbx_cat(v, "%");
    dbx_row(0, "SWING", v, (uint32_t)dbx_seq.swing * 216u / 100u);
    v[0] = 0;
    dbx_cat_int(v, dbx_fx.master);
    dbx_cat(v, "%");
    dbx_row(1, "FX MIX", v, (uint32_t)dbx_fx.master * 216u / 100u);
    v[0] = 0;
    dbx_row(2, "KIT", DBX_KIT_NAME[dbx_kit], (uint32_t)dbx_kit * 216u / (DBX_NKIT - 1u));
    lcd_fill(12, y + 13u, 216, 6, T_BG);                                   /* ERASE: a full turn; from half, a warning */
    v[0] = 0;
    if (dbx_ui.er == 0u) {
        gfx_colour = 1;
        draw_text_line(12, y, 216, &AF_S, "RESET: TURN A FULL TURN", DBX_RED_LIGHT, T_BG, 0);
        gfx_colour = 0;
    } else if (pct < 50u) {
        dbx_cat(v, "RESET  ");
        dbx_cat_int(v, (int32_t)pct);
        dbx_cat(v, "%");
        gfx_colour = 1;
        draw_text_line(12, y, 216, &AF_S, v, DBX_RED_LIGHT, T_BG, 0);
        gfx_colour = 0;
    } else {
        dbx_cat(v, "RESET ALL? KEEP TURNING");
        gfx_colour = 1;
        draw_text_line(12, y, 216, &AF_S, v, DBX_RED_WARN, T_BG, 0);
        gfx_colour = 0;
    }
    lcd_fill(12, y + 15u, 216, 3, T_LINE);
    if (w)
        lcd_fill(12, y + 15u, w, 3, pct < 50u ? DBX_RED_LIGHT : DBX_RED_WARN);
}

/* the value of a step's setting f for the EDIT pages: 0 NUDGE, 1 SUBDIVISION, 2 CHANCE, 3 PITCH, 4 DECAY, 5 TONE, 6 EXTRA, 7 PAN */
static int32_t dbx_edit_field(uint32_t i, uint32_t s, uint32_t f)
{
    switch (f) {
    case 0: return DBX_E.nudge[i][s];
    case 1: return DBX_E.ratchet[i][s];
    case 2: return DBX_E.chance[i][s];
    case 3: return DBX_E.semi[i][s];
    case 4: case 5: case 6: case 7: return dbx_lk_get(dbx_seq.pe, f - 4u, i, s) * DBX_LK_UNIT[f - 4u];
    default: return 0;
    }
}

/* what the EDIT rows show of settings f0..f0+n-1: the picked step's; with none picked, the lane's steps' if they all have the same, MIXED if not (mixed[]);
 * returns 0 when there is no step to show (none picked and the lane empty) */
static uint32_t dbx_edit_values(uint32_t f0, uint32_t n, int32_t *val, uint32_t *mixed)
{
    uint32_t i = dbx_ui.sel, st, s, k, first = DBX_NSTEP, one = (uint32_t)dbx_target(&st);
    for (s = 0; s < DBX_NSTEP; s++) {
        if (one ? s != st : !((DBX_E.steps[i] >> s) & 1u))
            continue;
        for (k = 0; k < n; k++) {
            int32_t v = dbx_edit_field(i, s, f0 + k);
            if (first == DBX_NSTEP)
                val[k] = v, mixed[k] = 0;
            else if (v != val[k])
                mixed[k] = 1;
        }
        first = s;
    }
    return first != DBX_NSTEP;
}

static void dbx_draw_edit_page(void)                    /* EDIT: the picked step's NUDGE, SUBDIVISION, CHANCE, PITCH (none picked: the lane's steps') */
{
    static const char *const NAME[4] = {"NUDGE", "SUBDIVISION", "CHANCE", "PITCH"};
    char v[16];
    int32_t val[4] = {0, 0, 0, 0};
    uint32_t mixed[4] = {0, 0, 0, 0}, k;
    draw_text_line(12, TITLE_Y, 216, &AF_L, "EDIT", T_THEME, T_BG, 0);
    if (!dbx_edit_values(0, 4, val, mixed)) {           /* no step to show: say so, leave the rows empty */
        for (k = 0; k < 4u; k++)
            dbx_row(k, NAME[k], "--", 0);
        return;
    }
    if (mixed[0])
        dbx_row(0, NAME[0], "MIXED", 0);
    else {      /* the nudge in % of a step (a notch is 1/48: 24 notches = 50 %) */
        int32_t n = val[0], pct = (n * 100 + (n > 0 ? 24 : n < 0 ? -24 : 0)) / 48;
        v[0] = 0;
        dbx_cat_signed(v, pct);
        dbx_cat(v, "%");
        dbx_row_nudge(0, n < 0 ? "NUDGE  EARLY" : n > 0 ? "NUDGE  LATE" : "NUDGE", v, n);
    }
    if (mixed[1])
        dbx_row(1, NAME[1], "MIXED", 0);
    else {
        v[0] = 0;
        dbx_cat(v, "x");
        dbx_cat_int(v, val[1]);
        dbx_row(1, NAME[1], v, (uint32_t)val[1] * 216u / DBX_RATCHET_MAX);
    }
    if (mixed[2])
        dbx_row(2, NAME[2], "MIXED", 0);
    else {
        v[0] = 0;
        dbx_cat_int(v, val[2]);
        dbx_cat(v, "%");
        dbx_row(2, NAME[2], v, (uint32_t)val[2] * 216u / 100u);
    }
    if (mixed[3])
        dbx_row(3, NAME[3], "MIXED", 0);
    else {
        v[0] = 0;
        dbx_cat_signed(v, val[3]);
        dbx_cat(v, " ST");
        dbx_row_bipolar(3, NAME[3], v, val[3], DBX_PITCH_MAX);
    }
}

static uint32_t dbx_pat_sig(void)                       /* what the pattern page shows */
{
    uint32_t p, used = 0;
    for (p = 0; p < DBX_NPAT; p++)
        used |= dbx_pat_used(p) << p;
    return used * 31u + (uint32_t)dbx_seq.pe * 7u + (uint32_t)dbx_seq.pq * 3u + dbx_seq.pp + (uint32_t)dbx_seq.playing * 1000u;
}

static void dbx_draw_pat_page(void)                     /* SAVE: the 16 patterns, 2 x 8 */
{
    char t[16] = "PATTERN ", n[4];
    uint32_t p;
    dbx_cat_int(t, (int32_t)dbx_seq.pe + 1);
    draw_text_line(12, TITLE_Y, 216, &AF_L, t, T_THEME, T_BG, 0);
    for (p = 0; p < DBX_NPAT; p++) {
        uint32_t x = 12u + (p % 8u) * 27u, y = ROW_Y + (p / 8u) * 22u;
        uint32_t cur = p == dbx_seq.pe, used = dbx_pat_used(p);
        uint32_t wait = p == dbx_seq.pq || (dbx_seq.playing && p == dbx_seq.pp && !cur);   /* the one that plays, or will */
        uint16_t bg = cur ? T_THEME : used ? T_RAISE : T_SURF, fg = cur ? T_INK : used ? T_TEXT : T_DIM;
        n[0] = 0;
        dbx_cat_int(n, (int32_t)p + 1);
        lcd_fill(x - 1u, y - 1u, PCELL_W + 2u, PCELL_H + 2u, wait ? T_ACCENT : T_BG);   /* outlined: it plays, or will */
        lcd_fill(x, y, PCELL_W, PCELL_H, bg);
        draw_text_line(x, y + 3u, PCELL_W, &AF_S, n, fg, bg, 1);
    }
    {
        char m[32] = "";
        if (dbx_seq.pq != 255u) {
            dbx_cat(m, "NEXT PASS: PATTERN ");
            dbx_cat_int(m, (int32_t)dbx_seq.pq + 1);
        } else if (dbx_seq.playing) {
            dbx_cat(m, "PLAYING PATTERN ");
            dbx_cat_int(m, (int32_t)dbx_seq.pp + 1);
        }
        draw_text_line(12, 205, 108, &AF_S, "HOLD + KEY: COPY", T_MID, T_BG, 0);
        draw_text_line(120, 205, 108, &AF_S, "EDIT HELD: DELETE", T_MID, T_BG, 2);
        draw_text_line(12, 221, 216, &AF_S, m, T_THEME, T_BG, 0);
    }
}

/* ---- the help: a tap of SEQ, three pages, each a label and what it does; a tap goes on, HOME closes */
static const char *const HELP_L[3][13] = {
    {"FX", "SCL", "ENV", "LFO", "EDIT", "GLO", "SAVE", "ARP", "SEQ", "REC", "OCT -/+", "PLAY", "HOME"},
    {"HOME HELD", "EDIT HELD", "SAVE HELD", "GLO + KEY", "(HELD)", "SEQ HELD", "SEQ + K1", "FX EDIT ARP", "WHITE KEYS", "BLACK KEYS", "", "", ""},
    {"SELECT", "ALGO", "PRESETS", "K1 - K4", "MASTER", "GLO 2 K4", "GLO 1", "GLO 2", "KICK TUNE", "", "", "", ""},
};
static const char *const HELP_T[3][13] = {
    {"EFFECTS (2 PAGES)", "HPF LPF OTT CLIP", "DUCK / ATK DEC GATE", "VOLUME / PAN LFO", "STEP, LANE, STEP SND", "TEMPO.. / SWING KIT", "PATTERNS / SONG", "EUCLID / MUTATE", "TAP HELP, HOLD LOOP", "KEYS SOUND + WRITE", "PATTERN - / +", "START / STOP", "CLOSE A PAGE"},
    {"FULL RESET", "CLEAR LANE / PAGE", "SAVE NOW", "MUTE THE LANE", "SOLO THE LANE", "LOOP THE LAST BEAT", "THE LOOP'S LENGTH", "TAP: NEXT PAGE", "THE 16 STEPS", "THE 11 DRUMS", "", "", ""},
    {"VELOCITY / FX MIX", "PAN / EFFECT (FX)", "SCROLL THE STEPS", "THE 4 ROWS", "OUTPUT LEVEL", "FULL TURN: RESET", "BPM VOL SYNC SUB", "SWING MIX KIT", "SHOWN AS A NOTE", "", "", "", ""},
};
static const char *const HELP_TITLE[3] = {"BUTTONS", "HOLDS", "KNOBS"};
static void dbx_draw_help_page(void)
{
    uint32_t n, y = 52;
    lcd_fill(0, y - 2u, 240, 240u - (y - 2u), T_BG);                 /* (the lines of the sheet before) */
    draw_text_line(12, TITLE_Y, 216, &AF_L, HELP_TITLE[dbx_help], T_THEME, T_BG, 0);
    draw_text_line(196, TITLE_Y + 10u, 32, &AF_S, dbx_help == 0u ? "1/3" : dbx_help == 1u ? "2/3" : "3/3", T_MID, T_BG, 2);
    for (n = 0; n < 13u; n++) {
        if (HELP_L[dbx_help][n][0] == 0 && HELP_T[dbx_help][n][0] == 0)
            continue;
        draw_text_line(12, y + n * 14u, 84, &AF_S, HELP_L[dbx_help][n], T_THEME, T_BG, 0);
        draw_text_line(100, y + n * 14u, 128, &AF_S, HELP_T[dbx_help][n], T_TEXT, T_BG, 0);
    }
#ifdef DBX_CPU_LINE                                                 /* (the FM-1: the audio's share of the time) */
    if (dbx_help == 2u) {
        char b[24];
        DBX_CPU_LINE(b);
        draw_text_line(12, 222, 216, &AF_S, b, T_MID, T_BG, 0);
    }
#endif
}

/* ---- the mixer: GLO held a quarter of a second shows the 11 lanes with their names, pan and state (MUTE, SOLO), over the page: the keys of the lanes
 * then mute (let go) or solo (held 0.5 s); letting GLO go brings the page back */
static uint32_t dbx_mixer_sig(void)
{
    uint32_t n, h = dbx_ui.sel;
    for (n = 0; n < DBX_NLANE; n++)
        h = h * 31u + lane[n].mute * 3u + (uint32_t)(lane[n].pan + 40);
    return h;
}

static void dbx_draw_mixer(void)
{
    uint32_t n, unmuted = 0, alone = 0;
    for (n = 0; n < DBX_NLANE; n++)
        if (!lane[n].mute) {
            unmuted++;
            alone = n;
        }
    lcd_fill(0, 18, 240, 222, T_BG);
    draw_text_line(12, TITLE_Y, 216, &AF_L, "MIXER", T_THEME, T_BG, 0);
    for (n = 0; n < DBX_NLANE; n++) {
        uint32_t y = 54u + n * 15u, mute = lane[n].mute, solo = unmuted == 1u && n == alone;
        char pv[16] = "";
        dbx_cat_pan(pv, lane[n].pan);
        if (n == dbx_ui.sel)
            lcd_fill(4, y + 1u, 3, 12, DBX_VIOLET);                      /* (the lane selected) */
        draw_text_line(12, y, 84, &AF_S, DBX_NAME[n], mute ? DBX_RED_LIGHT : T_THEME, T_BG, 0);
        draw_text_line(100, y, 70, &AF_S, pv, T_MID, T_BG, 0);
        draw_text_line(172, y, 56, &AF_S, mute ? "MUTE" : solo ? "SOLO" : "", mute ? DBX_RED_LIGHT : DBX_VIOLET, T_BG, 2);
    }
    draw_text_line(12, 222, 216, &AF_S, "KEY: MUTE    KEY HELD: SOLO", T_MID, T_BG, 0);
}

static uint32_t dbx_song_sig(void)                       /* what the song page shows */
{
    uint32_t n, h = 0;
    for (n = 0; n < DBX_NPAT; n++)
        h = h * 31u + dbx_song.pat[n] * 5u + dbx_song.rep[n] + dbx_song.again[n] * 17u;
    return h + dbx_song.sel * 3u + dbx_song.on * 97u + dbx_song.cur * 11u + (dbx_seq.playing ? 7u : 0u) + dbx_song.done * 13u;
}

static void dbx_draw_song_page(void)                    /* SAVE, second page: the SONG, 16 slots 2 x 8 */
{
    static const char *const REPN[4] = {"1", "2", "4", "8"};
    char m[40] = "", n[4];
    uint32_t p, len = dbx_song_len();
    draw_text_line(12, TITLE_Y, 216, &AF_L, "SONG", dbx_song.on ? T_THEME : T_MID, T_BG, 0);
    draw_text_line(150, TITLE_Y + 10u, 78, &AF_S, dbx_song.on ? "ON" : "OFF", dbx_song.on ? T_ACCENT : T_MID, T_BG, 2);
    for (p = 0; p < DBX_NPAT; p++) {
        uint32_t x = 12u + (p % 8u) * 27u, y = ROW_Y + (p / 8u) * 22u;
        uint32_t inchain = p < len, sel = p == dbx_song.sel, now = dbx_song.on && dbx_seq.playing && p == dbx_song.cur && inchain;
        uint16_t bg = inchain ? T_RAISE : T_SURF, fg = inchain ? T_TEXT : T_DIM;
        n[0] = 0;
        if (dbx_song.pat[p] < DBX_NPAT)
            dbx_cat_int(n, (int32_t)dbx_song.pat[p] + 1);
        else
            dbx_cat(n, "-");
        lcd_fill(x - 1u, y - 1u, PCELL_W + 2u, PCELL_H + 2u, sel ? DBX_VIOLET : now ? DBX_RED_PLAY : T_BG);   /* the slot picked, or the one playing */
        lcd_fill(x, y, PCELL_W, PCELL_H, now ? DBX_RED_PLAY : bg);
        draw_text_line(x, y + 3u, PCELL_W, &AF_S, n, now ? T_BG : fg, now ? DBX_RED_PLAY : bg, 1);
    }
    dbx_cat(m, "SLOT ");
    dbx_cat_int(m, (int32_t)dbx_song.sel + 1);
    dbx_cat(m, dbx_song.pat[dbx_song.sel] < DBX_NPAT ? "  PATTERN " : "  END");
    if (dbx_song.pat[dbx_song.sel] < DBX_NPAT) {
        dbx_cat_int(m, (int32_t)dbx_song.pat[dbx_song.sel] + 1);
        dbx_cat(m, "  X");
        dbx_cat(m, REPN[dbx_song.rep[dbx_song.sel] & 3u]);
    }
    draw_text_line(12, 205, 216, &AF_S, m, T_THEME, T_BG, 0);
    m[0] = 0;
    dbx_cat_int(m, (int32_t)len);
    dbx_cat(m, len == 1u ? " SLOT" : " SLOTS");
    if (dbx_song.pat[dbx_song.sel] < DBX_NPAT) {
        dbx_cat(m, "   AGAIN ");
        dbx_cat_int(m, dbx_song.again[dbx_song.sel]);
        dbx_cat(m, "%");
    }
    draw_text_line(12, 221, 216, &AF_S, m, T_MID, T_BG, 0);
}

/* the logo of the boot screen: three discs on a trefoil, 64 x 48 px, green on the background; each disc is anti-aliased by 4 x 4 samples per pixel.
 * Centres and radius are in 1/8 px. */
static void dbx_logo(uint32_t x0, uint32_t y0)
{
    static const int16_t C[3][2] = {{256, 96}, {152, 276}, {360, 276}};         /* the top one, the lower left and right (7 px discs, 15 px from the middle) */
    int32_t x, y, sx, sy, k, r = 56;
    cv_begin(64, 48, T_BG);
    for (k = 0; k < 3; k++)
        for (y = (C[k][1] - r) / 8 - 1; y <= (C[k][1] + r) / 8 + 1; y++)
            for (x = (C[k][0] - r) / 8 - 1; x <= (C[k][0] + r) / 8 + 1; x++) {
                int32_t n = 0;
                for (sy = 0; sy < 4; sy++)
                    for (sx = 0; sx < 4; sx++) {
                        int32_t dx = x * 8 + 2 * sx + 1 - C[k][0], dy = y * 8 + 2 * sy + 1 - C[k][1];
                        n += dx * dx + dy * dy <= r * r;
                    }
                if (n)                                                              /* (the discs do not touch: a pixel is one disc's) */
                    cv_pset(x, y, n >= 16 ? DBX_GREEN : ux_mix(T_BG, DBX_GREEN, n * 100 / 16));
            }
    cv_blit(x0, y0);
    lcd_sync();
}

static void dbx_ui_draw(void)
{
    static uint8_t drawn_page, splash, seen_live = 255;
    if (fm1_ms - dbx_ms0 < DBX_SPLASH_MS) {             /* the boot screen: the logo, the name, by whom, nothing else */
        if (!splash) {
            splash = 1;
            lcd_fill(0, 0, 240, 240, T_BG);
            dbx_logo(88, 48);                                                      /* (the logo over the name, ...) */
            draw_text_box(0, 104, 240, &AF_L, DBX_FW_NAME, T_TEXT, 1);             /* (the name, large (AF_L has no lower case); */
            draw_text_box(0, 146, 240, &AF_S, DBX_FW_BY, T_MID, 1);                /* by, and who, centred under it) */
            draw_text_box(0, 164, 240, &AF_M, DBX_FW_WHO, T_TEXT, 1);
        }
        return;
    }
    {                                                   /* GLO held: the mixer over everything; let go: the page, whole */
        static uint8_t mixer_up;
        static uint32_t mixer_sig;
        uint32_t show = (fm1_in.buttons & PANEL_BTN(B_GLO)) && fm1_ms - dbx_glo_t >= 250u && dbx_ui.page != PAGE_HELP;
        if (show) {
            if (!mixer_up || mixer_sig != dbx_mixer_sig()) {
                mixer_sig = dbx_mixer_sig();
                dbx_draw_mixer();
            }
            mixer_up = 1;
            return;
        }
        if (mixer_up) {
            mixer_up = 0;
            dbx_ui.dirty |= DIRTY_ALL;
        }
    }
    if (dbx_snap.live != seen_live) {                   /* the sound of another pattern is live: the pages show its values */
        seen_live = dbx_snap.live;
        dbx_ui.dirty |= DIRTY_LANE;
    }
    if (splash) {                                       /* ... then the page, whole */
        splash = 0;
        dbx_ui.dirty |= DIRTY_ALL;
    }
    uint32_t k, st = 0, vs = 0, sig, tg = (uint32_t)dbx_target(&st), ed = 0, fxp = DBX_FXPAGE, e = dbx_ui.fxsel;
    uint32_t bits = fxp ? DBX_F.on[e] : DBX_E.steps[dbx_ui.sel];
    for (k = 0; k < DBX_NSTEP; k++)
        if ((bits >> k) & 1u)
            vs = vs * 31u + (fxp ? DBX_F.mix[e][k] : DBX_E.vel[dbx_ui.sel][k]) + k +
                 977u * (fxp ? 0u : (uint32_t)dbx_step_edited(dbx_ui.sel, k));
    ed = fxp ? 0x1000u + e * 7u : 0u;
    sig = (bits << 16) ^ ((uint32_t)dbx_seq.cur << 9) ^ ((uint32_t)dbx_seq.playing << 8) ^ ((uint32_t)dbx_seq.rep << 3) ^ ((uint32_t)dbx_rec << 4) ^
          ((uint32_t)dbx_seq.bpm << 1) ^ vs * 0x9E3779B1u ^ ((uint32_t)dbx_seq.div << 20) ^
          ((uint32_t)dbx_seq.ext << 28) ^ ((uint32_t)dbx_seq_locked() << 29) ^ (tg << 30) ^ (st << 24) ^ ed ^
          (dbx_saved_now() ? 0x80000000u : 0u) ^ ((uint32_t)dbx_seq.pe << 4) ^ (dbx_erased_now() ? 0x40000000u : 0u) ^
          ((uint32_t)dbx_batt.lvl * 0x1F3D5B79u) ^ (dbx_usb_powered() * 0x2545F491u) ^ ((uint32_t)dbx_seq.pl_lock * 0x3C6EF372u);
    if (dbx_ui.dirty) {
        uint32_t all = dbx_ui.dirty & DIRTY_ALL;
        dbx_ui.dirty = 0;
        dbx_ui.sig = ~sig;                              /* the head and the grid follow */
        dbx_ui.scope_t = 0;                             /* (the scope too, at once) */
        if (all)
            lcd_fill(0, 0, 240, 240, T_BG);
        else if (drawn_page != dbx_ui.page)             /* another page: its rows replace these */
            lcd_fill(0, ROW_Y - 6u, 240, 240u - (ROW_Y - 6u), T_BG);
        drawn_page = dbx_ui.page;
        if (dbx_ui.page == PAGE_GLO)
            dbx_draw_global_page();
        else if (dbx_ui.page == PAGE_GLO2)
            dbx_draw_global2_page();
        else if (dbx_ui.page == PAGE_HELP)
            dbx_draw_help_page();
        else if (dbx_ui.page == PAGE_SONG) {
            dbx_draw_song_page();
            dbx_ui.psig = dbx_song_sig();
        } else if (dbx_ui.page == PAGE_EDIT2)
            dbx_draw_edit2_page();
        else if (dbx_ui.page == PAGE_EDIT)
            dbx_draw_edit_page();
        else if (fxp)
            dbx_draw_fx_page();
        else if (dbx_ui.page == PAGE_SCL)
            dbx_draw_scl_page();
        else if (dbx_ui.page == PAGE_LFO1 || dbx_ui.page == PAGE_LFO2)
            dbx_draw_lfo_page();
        else if (dbx_ui.page == PAGE_ENV1 || dbx_ui.page == PAGE_ENV2)
            dbx_draw_env_page();
        else if (dbx_ui.page == PAGE_ARP1 || dbx_ui.page == PAGE_ARP2)
            dbx_draw_arp_page();
        else if (dbx_ui.page == PAGE_PAT) {
            dbx_draw_pat_page();
            dbx_ui.psig = dbx_pat_sig();
        } else
            dbx_draw_lane_page();
    }
    if (sig != dbx_ui.sig) {
        dbx_ui.sig = sig;
        dbx_draw_head();
        if (dbx_ui.page != PAGE_HELP)
            dbx_draw_grid();
        if (dbx_ui.page == PAGE_GLO && dbx_seq.ext)     /* the measured tempo and the lock follow */
            dbx_draw_global_page();
    }
#ifdef DBX_MIDI_DRAIN
    {   /* GLO 1, waiting for the computer: what has arrived follows (four times a second) */
        static uint32_t diag_t;
        if (dbx_ui.page == PAGE_GLO && dbx_seq.ext && !dbx_seq_locked() && fm1_ms - diag_t >= 250u) {
            diag_t = fm1_ms;
            dbx_draw_global_page();
        }
    }
#endif
    if (dbx_ui.page == PAGE_SONG && dbx_song_sig() != dbx_ui.psig) {
        dbx_ui.psig = dbx_song_sig();
        dbx_draw_song_page();
    }
    if (dbx_ui.page == PAGE_PAT && dbx_pat_sig() != dbx_ui.psig) {
        dbx_ui.psig = dbx_pat_sig();
        dbx_draw_pat_page();
    }
    if (dbx_ui.page != PAGE_HELP && fm1_ms - dbx_ui.scope_t >= 30u) {              /* the oscilloscope (not under the help): every other frame, ~33 times a second (the panel takes ~20 ms) */
        dbx_ui.scope_t = fm1_ms;
        if (dbx_ui.page == PAGE_LFO1 || dbx_ui.page == PAGE_LFO2)
            dbx_draw_lfo_view();                        /* (an LFO page: its picture in the oscilloscope's place) */
        else if (dbx_ui.page == PAGE_ENV1 || dbx_ui.page == PAGE_ENV2)
            dbx_draw_env_view();                        /* (so is an ENV page's) */
        else if (dbx_ui.page == PAGE_ARP1 || dbx_ui.page == PAGE_ARP2)
            dbx_draw_arp_view();                        /* (and an ARP page's) */
        else
            dbx_draw_scope();
    }
}

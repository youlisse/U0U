/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (panel.c, from which this is built) */
/* The FM-1's panel: which matrix button / encoder carries which printed label (measured on hardware, as Felucca's
 * PANEL_DEFAULT). Buttons are matrix ids 0..13, the note keys 14..40 (bit k of fm1_in.notes = key id 14 + k). */
enum { B_FX, B_SCL, B_ENV, B_LFO, B_EDIT, B_GLO, B_HOME, B_SAVE, B_ARP, B_SEQ, B_PLAY, B_REC,
       B_OCTDN, B_OCTUP, NB };
enum { EN_SELECT, EN_ALGO, EN_PRESET, EN_K1, EN_K2, EN_K3, EN_K4, NE };

typedef struct {
    uint8_t btn[NB];             /* matrix button id (0..13) per label */
    uint8_t enc[NE];             /* matrix encoder (0..6) per role */
    int8_t dir[NE];              /* +1 / -1 so that clockwise is + */
} panel_t;

static const panel_t panel = {
    {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 0, 1},   /* PLAY = 12, REC = 13 */
    {0, 1, 6, 2, 3, 4, 5},                           /* SELECT = enc 0, ALGORITHM = enc 1, PRESETS = enc 6 */
    {1, 1, 1, 1, 1, 1, 1},
};

/* steps of a role since the last call, + = clockwise */
static int32_t panel_enc(uint32_t role) { return fm1_enc_take(panel.enc[role]) * panel.dir[role]; }

/* a button label's bit in fm1_in.buttons */
#define PANEL_BTN(b) (1u << panel.btn[b])

/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (felucca_web.c, from which this is built)
 * The design (the device clock driven by the audio the worklet asks for, the exports below) follows X0X's browser
 * build, web/emu/x0x_web.c of charlesvestal/fm1-x0x (GPL-3.0), a Felucca fork. */
/* DrumBox in the browser: the firmware's own sources (the drum voices, the audio ISR, the UI), compiled to
 * WebAssembly and run in an AudioWorklet against a simulated FM-1:
 *   - the device clock: TIMER4 ticks (24 MHz) and fm1_ms advance with the audio the worklet asks for; the audio ISR
 *     (dbx_audio.c fm1_alnk0_irq, the real one) renders each I2S half (HALF_FRAMES) as the DMA would ask for it,
 *     and the main loop (web_frame below) runs every 15 ms;
 *   - the panel: buttons and encoders by their printed labels (dbx_panel.c), the keys as fm1_in.notes, the LEDs read
 *     back from fm1_led / fm1_led_dim through the key matrix;
 *   - the screen: lcd_fill / lcd_blit into a 240 x 240 RGB565 frame (the panel's byte order);
 *   - the flash: a 1 MiB NOR image the page keeps (nothing stored in it yet).
 * Not here: USB, TRS MIDI, the update path. Same page and exports as Felucca's (web/emu/index.html).
 *   web/emu/build_drumbox.sh  ->  build/emu-drumbox/{felucca.wasm, worklet.js, index.html} */
#include <stdint.h>
#include <string.h>

#define DRUMBOX_WEB 1
#define __attribute__(x)
#include "felucca_tables.h"                      /* build/gen: FS, CTL, SINE, pitch_inc, SVF_G ... */
#define NVOICE 1                                 /* felucca_engine.h wants a host: one part, one voice */
#define NPART 1
#include "../../firmware/engines/src/felucca_engine.h"
#include "../../firmware/engines/src/dsp.c"
#include "../../firmware/src/drum_voice.c"
#define HALF_FRAMES 128                          /* I2S half buffer, as core.h */
#define FM1_TICKS_PER_US 24u                     /* TIMER4: 24 MHz */

/* ------------------------------------------------------------ the clock --- */
static uint32_t web_ticks, fm1_ms;
static uint32_t fm1_ticks(void) { return web_ticks; }

/* ------------------------------------------------------ audio (ALNK0) --- */
static uint32_t web_half;                        /* the half the next render fills */
#define FM1_AUDIO_HALF 1u
static uint8_t fm1_audio_pending(void) { return FM1_AUDIO_HALF; }
static void fm1_audio_ack_aux(uint8_t p) { (void)p; }
static uint32_t fm1_audio_free_half(void) { return web_half; }
static void fm1_audio_ack_half(void) {}
static void fm1_audio_init(int32_t *buf, uint32_t words, void (*isr)(void), uint32_t prio)
{ (void)buf; (void)words; (void)isr; (void)prio; }
void isr_alnk0(void) {}

/* ------------------------------------------------------------ the panel --- */
#define FM1_NCOL 11u
static const int8_t FM1_KEYMAP[6][FM1_NCOL] = {      /* as bsp/boards/fm1/fm1_input.h: key id at (column, row bit) */
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    { 5, 11,  4, 10,  3,  9,  2,  8, -1, -1, -1},
    {34, 35, 36, 37, 38, 40, 39, 13,  7,  6, 12},
    {23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33},
    { 0,  1, 15, 14, 17, 16, 19, 18, 20, 21, 22},
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
};
static uint8_t fm1_led[FM1_NCOL], fm1_led_dim[FM1_NCOL], fm1_led_breath[FM1_NCOL], fm1_led_mid[FM1_NCOL];
#define LED_PLAY_GREEN ((8u << 3) | 1u)
static struct { volatile uint32_t notes, buttons; } fm1_in;
static uint32_t web_pressed, web_released, web_notes_pressed;
static int32_t web_enc_steps[7];
static uint32_t fm1_input_edges(uint32_t *released)     /* the buttons pressed (and released) since the last call */
{
    uint32_t p = web_pressed;
    if (released)
        *released = web_released;
    web_pressed = web_released = 0;
    return p;
}
static uint32_t fm1_input_note_edges(void) { uint32_t n = web_notes_pressed; web_notes_pressed = 0; return n; }
static int32_t fm1_enc_take(uint32_t e) { int32_t s = web_enc_steps[e % 7u]; web_enc_steps[e % 7u] = 0; return s; }

/* ----------------------------------------------------------- the screen --- */
static uint16_t web_fb[240 * 240];               /* RGB565 as the panel gets it (high byte first) */
static uint32_t web_draws, web_px_fill, web_px_blit;             /* the calls, and the pixels they wrote (the tests: what is drawn at a step) */
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    web_px_fill += w * h;
    uint16_t sw = (uint16_t)((c >> 8) | (c << 8));
    for (j = 0; j < h && y + j < 240u; j++)
        for (i = 0; i < w && x + i < 240u; i++)
            web_fb[(y + j) * 240u + x + i] = sw;
    web_draws++;
}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *p)
{
    uint32_t i, j;
    web_px_blit += w * h;
    for (j = 0; j < h && y + j < 240u; j++)
        for (i = 0; i < w && x + i < 240u; i++)
            web_fb[(y + j) * 240u + x + i] = p[j * w + i];
    web_draws++;
}
static void lcd_sync(void) {}

#include "../../firmware/src/gfx.c"

/* ------------------------------------------------------------ the device --- */
static uint32_t web_master_adc = 700;            /* MASTER, 0..1023 as the ADC reads it */
enum { FM1_ADC_BATT = 3, FM1_ADC_MASTER = 4 };
static uint32_t web_batt_adc = 620, web_usb_on;          /* the battery divider's reading (620: full), and a host on USB (the tests) */
static int32_t fm1_adc_read(uint32_t ch) { return ch == FM1_ADC_MASTER ? (int32_t)web_master_adc : (int32_t)web_batt_adc; }
static uint32_t dbx_usb_powered(void) { return web_usb_on; }

/* ------------------------------------------------------------ the flash --- */
/* the SPI NOR (erased: 0xFF) behind storage.c's three hooks (a program only clears bits); the page keeps the sectors */
static uint8_t nor[0x100000];
static uint32_t web_flash_writes;
static int st_read(uint32_t off, void *dst, uint32_t n)
{
    if (off >= sizeof nor || n > sizeof nor - off)
        return -1;
    memcpy(dst, nor + off, n);
    return 0;
}
static int st_erase(uint32_t off)
{
    if (off >= sizeof nor || 4096u > sizeof nor - off)
        return -8;
    memset(nor + (off & ~4095u), 0xFF, 4096);
    web_flash_writes++;
    return 0;
}
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    uint32_t i;
    if (off >= sizeof nor || n > sizeof nor - off)
        return -8;
    for (i = 0; i < n; i++)
        nor[off + i] &= s[i];
    web_flash_writes++;
    return 0;
}
#include "../../firmware/src/storage.c"

#include "../../firmware/src/dbx_panel.c"
#include "../../firmware/src/dbx_voices.c"
#include "../../firmware/src/dbx_seq.c"
#include "../../firmware/src/dbx_fx.c"
#include "../../firmware/src/dbx_lfo.c"
#include "../../firmware/src/dbx_snap.c"
#include "../../firmware/src/dbx_audio.c"
#include "../../firmware/src/dbx_arp.c"
#include "../../firmware/src/dbx_ui.c"
#include "../../firmware/src/dbx_store.c"

static uint32_t web_booted, web_boot_ms, web_last_frame;

static void web_power_on(void)
{
    dbx_palette();
    lcd_fill(0, 0, 240, 240, T_BG);
    dbx_voices_init();
    dbx_seq_init();
    dbx_fx_init();
    dbx_store_load();                     /* the patterns, the kit and the settings the flash holds */
    dbx_audio.master_q12 = dbx_master_of_pot(web_master_adc);
    dbx_audio.g_cur = (int32_t)((dbx_audio.master_q12 * dbx_audio.vol_q12) >> 12);   /* no glide from silence */
    dbx_ui.master_knob = (int32_t)web_master_adc * 16;
    audio_init();
    web_boot_ms = fm1_ms + 30u;
    web_booted = 1;
}

/* one pass of the main loop */
static void web_frame(void)
{
    dbx_master_poll();
    dbx_batt_poll();
    dbx_ui_input();
    dbx_arp_poll();
    dbx_store_poll();
    dbx_ui_leds();
    dbx_ui_draw();
}

/* output ring, stereo float, filled by web_ms, drained by web_render */
#define RING 4096u
static float ring[RING * 2];
static uint32_t ring_w, ring_r, web_samples_due;

/* one millisecond of the device: the clock, the audio halves due by now, the main loop */
static void web_ms(void)
{
    fm1_ms++;
    web_ticks += 1000u * FM1_TICKS_PER_US;
    web_samples_due += FS;
    while (web_samples_due >= 1000u * HALF_FRAMES) {      /* an I2S half: the audio ISR renders it */
        uint32_t k;
        const int32_t *o = &abuf[web_half * HALF_WORDS];
        web_samples_due -= 1000u * HALF_FRAMES;
        fm1_alnk0_irq();
        for (k = 0; k < HALF_FRAMES; k++) {
            uint32_t i = (ring_w++ % RING) * 2u;
            ring[i] = (float)o[2u * k] / 8388608.0f;
            ring[i + 1] = (float)o[2u * k + 1u] / 8388608.0f;
        }
        web_half ^= 1u;
    }
    if (!web_booted || (int32_t)(fm1_ms - web_boot_ms) < 0)
        return;
    if (fm1_ms - web_last_frame >= 15u) {
        web_last_frame = fm1_ms;
        web_frame();
    }
}

#define EXPORT __attribute__((used, visibility("default")))
#undef __attribute__

static float out_l[1024], out_r[1024];
EXPORT float *web_out_l(void) { return out_l; }
EXPORT float *web_out_r(void) { return out_r; }
/* n frames (<= 1024) into out_l / out_r: the device runs until they exist */
EXPORT void web_render(uint32_t n)
{
    uint32_t k;
    if (n > 1024u)
        n = 1024u;
    while (ring_w - ring_r < n)
        web_ms();
    for (k = 0; k < n; k++) {
        uint32_t i = (ring_r++ % RING) * 2u;
        out_l[k] = ring[i];
        out_r[k] = ring[i + 1];
    }
}

/* the flash: erased, then the page's saved sectors (web_nor), then boot */
EXPORT uint8_t *web_nor(void) { return nor; }
EXPORT uint32_t web_nor_size(void) { return sizeof nor; }
EXPORT void web_nor_erase(void) { memset(nor, 0xFF, sizeof nor); }
EXPORT uint32_t web_flash_writes_count(void) { return web_flash_writes; }
EXPORT void web_boot(void)
{
    if (!web_booted)
        web_power_on();
}

/* input, by the printed labels: buttons bit b = B_FX .. B_OCTUP (dbx_panel.c), keys bit k = key k (F3 .. G5),
 * encoders by role (EN_SELECT .. EN_K4), MASTER 0..1023 */
EXPORT void web_buttons(uint32_t labels)
{
    uint32_t m = 0, b, old = fm1_in.buttons;
    for (b = 0; b < NB; b++)
        if ((labels >> b) & 1u)
            m |= 1u << panel.btn[b];
    web_pressed |= m & ~old;
    web_released |= old & ~m;
    fm1_in.buttons = m;
}
EXPORT void web_keys(uint32_t mask)
{
    uint32_t old = fm1_in.notes;
    mask &= (1u << 27) - 1u;
    web_notes_pressed |= mask & ~old;
    fm1_in.notes = mask;
}
EXPORT void web_enc(uint32_t role, int32_t n)
{
    if (role < NE)
        web_enc_steps[panel.enc[role] % 7u] += n * panel.dir[role];
}
EXPORT void web_master(uint32_t v) { web_master_adc = v > 1023u ? 1023u : v; }
/* a MIDI message from the page: only the real-time ones matter (the computer's clock, Start, Continue, Stop) */
EXPORT uint32_t web_midi(uint32_t status, uint32_t d1, uint32_t d2)
{
    (void)d1; (void)d2;
    if (status >= 0xF8u)
        dbx_midi_realtime((uint8_t)status, fm1_ms);
    return 1;
}

/* the LEDs: bit b = the button labelled b lit (bit 14: PLAY's green), keys bit k; the dim glow and the breath alike */
static uint32_t led_on(const uint8_t *l, uint32_t id)
{
    uint32_t c, r;
    for (c = 0; c < FM1_NCOL; c++)
        for (r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][c] == (int8_t)id)
                return (l[c] >> r) & 1u;
    return 0;
}
static uint32_t led_buttons(const uint8_t *l)
{
    uint32_t b, m = 0;
    for (b = 0; b < NB; b++)
        m |= led_on(l, panel.btn[b]) << b;
    return m;
}
static uint32_t led_keys(const uint8_t *l)
{
    uint32_t k, m = 0;
    for (k = 0; k < 27u; k++)
        m |= led_on(l, 14u + k) << k;
    return m;
}
EXPORT uint32_t web_lit_buttons(void)
{
    return led_buttons(fm1_led) | ((fm1_led[LED_PLAY_GREEN >> 3] >> (LED_PLAY_GREEN & 7u)) & 1u) << 14;
}
EXPORT uint32_t web_lit_keys(void) { return led_keys(fm1_led); }
EXPORT uint32_t web_dim_buttons(void) { return led_buttons(fm1_led_dim); }
EXPORT uint32_t web_dim_keys(void) { return led_keys(fm1_led_dim); }
EXPORT uint32_t web_dim_level(void) { return 0; }
EXPORT uint32_t web_breath_buttons(void) { return led_buttons(fm1_led_breath); }
EXPORT uint32_t web_breath_keys(void) { return led_keys(fm1_led_breath); }
EXPORT uint32_t web_mid_keys(void) { return led_keys(fm1_led_mid); }
EXPORT uint8_t *web_anim_levels(void) { return 0; }     /* (no power-on sweep) */

EXPORT uint16_t *web_screen(void) { return web_fb; }
EXPORT uint32_t web_screen_draws(void) { return web_draws; }
EXPORT uint32_t web_now_ms(void) { return fm1_ms; }
EXPORT uint32_t web_playing(void) { return dbx_seq.playing; }
EXPORT int32_t web_note16(uint32_t lane_i) { return lane_i < DBX_NLANE ? dbx_note16(lane_i) : -1; }   /* (the tests) */
EXPORT uint32_t web_step(void) { return dbx_seq.cur; }          /* the step playing (the tests) */
EXPORT uint32_t web_steps_done(void) { return dbx_seq.steps_done; }   /* (the tests) */
EXPORT uint32_t web_loop(void) { return dbx_seq.rep; }
EXPORT uint32_t web_kit(void) { return dbx_kit; }                /* (the tests) */
EXPORT uint32_t web_lfo(uint32_t l, uint32_t p) { return l < 2u && p < LFO_NP ? dbx_lfo.p[l][p] : 0xFFu; }   /* (the tests) */
/* for the bench (web/emu/bench.mjs, bench_native.c): a heavy scene straight into the engine, as Felucca's web_test_heavy does. All 11 lanes
 * on every step at 1/32 (every voice sounding all the time); level 2: every effect always on at its full MIX; level 3: both LFOs on (the pan's
 * crossover running), the master page's HPF, LPF, OTT and soft clip all on, ENV's ducking, attack and gate on, the mutation on. PLAY is the caller's to press */
EXPORT void web_test_heavy(uint32_t level)
{
    uint32_t i, s, e;
    for (i = 0; i < DBX_NLANE; i++) {
        dbx_seq.pat[0].steps[i] = 0xFFFFu;
        for (s = 0; s < DBX_NSTEP; s++)
            dbx_step_defaults(&dbx_seq.pat[0], i, s);
    }
    dbx_seq_set_div(2);
    if (level >= 2u)
        for (e = 0; e < DBX_NFX; e++) {
            dbx_fx.p[e][FP_CONT] = 127;
            dbx_fx.p[e][FP_MIX] = 127;
            dbx_fx_rates(e);
        }
    if (level >= 3u) {
        dbx_lfo.p[0][LFO_AMT] = 80;
        dbx_lfo.p[0][LFO_SHAPE] = 0;
        dbx_lfo.p[0][LFO_RATE] = 6;
        dbx_lfo.p[1][LFO_AMT] = 100;
        dbx_lfo.p[1][LFO_SHAPE] = 0;
        dbx_lfo.p[1][LFO_RATE] = 4;
        dbx_mst.p[0] = 30;
        dbx_mst.p[1] = 100;
        dbx_mst.p[2] = 127;
        dbx_mst.p[3] = 60;
        dbx_mst_changed();
        dbx_env.p[EV_AMT] = 80;                        /* ENV: ducking (the dry sound and the effects) by the kick, a hard attack, a gate */
        dbx_env.p[EV_TGT] = EVT_BOTH;
        dbx_env.p[EV_KATK] = 90;
        dbx_env.p[EV_ATK] = 100;
        dbx_env.p[EV_GATE] = 100;
        for (e = 0; e < DBX_NLANE; e++)                /* every lane panned (the middle and side signals through the chain) */
            lane[e].pan = (int8_t)(e & 1u ? 14 : -14);
        for (e = 0; e < DBX_NLANE; e++)                /* and the mutation moving hits on every pass */
            dbx_arp.m[0][e][AM_AMT - AM_AMT] = 50;                 /* (pattern 1's) */
        dbx_arp.p[AM_SCOPE] = 1;
    }
}
EXPORT uint32_t web_env(uint32_t i) { return i < EV_NP ? dbx_env.p[i] : 0xFFu; }     /* (the tests) */
EXPORT int32_t web_duck(void) { return dbx_env.g1; }
EXPORT uint32_t web_arp(uint32_t i) { return i < AP_NP ? (i < AM_AMT || i == AM_SCOPE ? dbx_arp.p[i] : dbx_arp.m[dbx_seq.pe][dbx_ui.sel][i - AM_AMT]) : 0xFFu; }     /* the selected lane's values (the tests) */
EXPORT uint32_t web_arp_lane(uint32_t l, uint32_t i) { return l < DBX_NLANE && i < 4u ? dbx_arp.m[dbx_seq.pe][l][i] : 0xFFu; }     /* a lane's AMOUNT, EVERY, -, DRIFT (the tests) */
EXPORT uint32_t web_steps_of(uint32_t lane_i) { return lane_i < DBX_NLANE ? DBX_E.steps[lane_i] : 0u; }          /* (the section shown) */
EXPORT uint32_t web_pass(void) { return dbx_seq.pass; }
EXPORT int32_t web_lane(uint32_t i, uint32_t f)                                 /* a lane's parameter: 0 TUNE 1 DECAY 2 TONE 3 EXTRA 4 LEVEL 5 mute 6 ACCENT 7 PAN 8 CHOKE (the tests) */
{
    if (i >= DBX_NLANE)
        return -1;
    return f == 0u ? lane[i].p.tune : f == 1u ? lane[i].p.decay : f == 2u ? lane[i].p.tone : f == 3u ? lane[i].p.extra : f == 4u ? lane[i].p.level : f == 5u ? lane[i].mute : f == 6u ? lane[i].p.accent : f == 7u ? lane[i].pan : lane[i].choke;
}
EXPORT uint32_t web_px_filled(void) { uint32_t n = web_px_fill; web_px_fill = 0; return n; }   /* the pixels written by lcd_fill since the last call (the tests) */
EXPORT uint32_t web_px_blitted(void) { uint32_t n = web_px_blit; web_px_blit = 0; return n; }
EXPORT uint32_t web_two(void) { return dbx_seq.two; }                           /* bit p: pattern p has a second section (the tests) */
EXPORT uint32_t web_vs(void) { return dbx_seq.vs; }                             /* the section shown */
EXPORT uint32_t web_ps(void) { return dbx_seq.ps; }                             /* the section that comes next (ISR), */
EXPORT uint32_t web_cs(void) { return dbx_seq.cs; }                             /* the one playing */
EXPORT uint32_t web_oct_ask(void) { return dbx_oct.ask; }                       /* the question OCT asks: 0 none, 1 double the track, 2 take the second part away */
EXPORT int32_t web_lock(uint32_t which, uint32_t lane_i, uint32_t step) { return which < 4u && lane_i < DBX_NLANE && step < DBX_NSTEP ? dbx_lk_get(DBX_EV, which, lane_i, step) * DBX_LK_UNIT[which] : 0; }   /* a step's own DECAY (0), TONE (1), EXTRA (2) or PAN (3), in the lane's units (the tests) */
EXPORT uint32_t web_song(uint32_t slot, uint32_t f) { return slot < DBX_NPAT ? (f == 0u ? dbx_song.pat[slot] : f == 1u ? dbx_song.rep[slot] : dbx_song.again[slot]) : 0u; }   /* a slot's pattern (255: the end), its repeat code (the tests) */
EXPORT uint32_t web_song_on(void) { return dbx_song.on; }
EXPORT uint32_t web_song_cur(void) { return dbx_song.cur; }
EXPORT uint32_t web_help(void) { return dbx_help; }                           /* the help page shown (the tests) */
EXPORT uint32_t web_sel(void) { return dbx_ui.sel; }                       /* the lane selected (the tests) */
EXPORT uint32_t web_rec(void) { return dbx_rec; }     /* (the tests) */
EXPORT uint32_t web_lfo_phase(uint32_t k) { return k < 2u ? dbx_lfo.ph[k] : 0u; }     /* a cycle = 2^32 (the tests) */
EXPORT uint32_t web_pos(void) { return dbx_seq.pos; }
EXPORT uint32_t web_bpm(void) { return dbx_seq.bpm; }
EXPORT uint32_t web_div(void) { return dbx_seq.div; }
EXPORT uint32_t web_ext(void) { return dbx_seq.ext; }
EXPORT uint32_t web_locked(void) { return dbx_seq_locked(); }
EXPORT uint32_t web_swing(void) { return dbx_seq.swing; }
EXPORT uint32_t web_erase(void) { return dbx_ui.er; }                    /* the clicks of the erase turn so far */
EXPORT uint32_t web_erased(void) { return dbx_erased_now(); }
EXPORT void web_set_batt(uint32_t adc, uint32_t usb) { web_batt_adc = adc; web_usb_on = usb; }    /* the battery's reading and USB power (the tests) */
EXPORT uint32_t web_batt_level(void) { return dbx_batt.lvl; }
EXPORT uint32_t web_mst(uint32_t i) { return i < 4u ? dbx_mst.p[i] : 0; }                    /* the master page: HPF LPF OTT CLIP */
EXPORT uint32_t web_fxsel(void) { return dbx_ui.fxsel; }
EXPORT uint32_t web_fxmaster(void) { return dbx_fx.master; }
EXPORT int32_t web_fxamt(uint32_t e) { return e < DBX_NFX ? dbx_fx.am[e] : -1; }                  /* the amount: envelope x mixes, 0..65536 */
EXPORT int32_t web_fxenv(uint32_t e) { return e < DBX_NFX ? dbx_fx.env[e] : -1; }                 /* the envelope, 0..65536 */
EXPORT uint32_t web_fxparam(uint32_t e, uint32_t i) { return e < DBX_NFX && i < DBX_NFXP ? dbx_fx.p[e][i] : 0; }
EXPORT uint32_t web_fxstep(uint32_t p, uint32_t e, uint32_t s) { return p < DBX_NVP && e < DBX_NFX && s < DBX_NSTEP ? (DBX_FXV(p)->on[e] >> s) & 1u : 0; }      /* (p + 16: the second section) */
EXPORT uint32_t web_fxmix(uint32_t p, uint32_t e, uint32_t s) { return p < DBX_NVP && e < DBX_NFX && s < DBX_NSTEP ? DBX_FXV(p)->mix[e][s] : 0; }
EXPORT uint32_t web_pe(void) { return dbx_seq.pe; }               /* the pattern shown, the one playing, the one waiting */
EXPORT uint32_t web_pp(void) { return dbx_seq.pp; }
EXPORT uint32_t web_pq(void) { return dbx_seq.pq; }
EXPORT uint32_t web_pat_used(uint32_t p) { return p < DBX_NPAT ? dbx_pat_used(p) : 0; }
EXPORT uint32_t web_step_on(uint32_t p, uint32_t l, uint32_t s) { return p < DBX_NVP && l < DBX_NLANE && s < DBX_NSTEP ? (DBX_PATV(p)->steps[l] >> s) & 1u : 0; }      /* (p + 16: the second section) */
EXPORT uint32_t web_dirty(void) { return dbx_store.set | dbx_store.pats; }    /* something not saved yet */
EXPORT uint32_t web_saved(void) { return dbx_saved_now(); }
EXPORT uint32_t web_vol(void) { return dbx_ui.vol; }
EXPORT int32_t web_target(void) { uint32_t st = 0; return dbx_target(&st) ? (int32_t)st : -1; }   /* the step SELECT / EDIT work on */
EXPORT uint32_t web_page(void) { return dbx_ui.page; }            /* 0 lane, 1 GLO, 2 EDIT */
EXPORT int32_t web_nudge(uint32_t l, uint32_t s) { return l < DBX_NLANE && s < DBX_NSTEP ? DBX_E.nudge[l][s] : 0; }
EXPORT uint32_t web_ratchet(uint32_t l, uint32_t s) { return l < DBX_NLANE && s < DBX_NSTEP ? DBX_E.ratchet[l][s] : 0; }
EXPORT uint32_t web_chance(uint32_t l, uint32_t s) { return l < DBX_NLANE && s < DBX_NSTEP ? DBX_E.chance[l][s] : 0; }
EXPORT int32_t web_semi(uint32_t l, uint32_t s) { return l < DBX_NLANE && s < DBX_NSTEP ? DBX_E.semi[l][s] : 0; }
EXPORT int32_t web_eff16(uint32_t l) { return l < DBX_NLANE ? dbx_eff16(l) : -1; }   /* the pitch of the last hit (the tests) */
EXPORT uint32_t web_vel(uint32_t lane_i, uint32_t step) { return lane_i < DBX_NLANE && step < DBX_NSTEP ? DBX_E.vel[lane_i][step] : 0; }
EXPORT uint32_t web_sample_rate(void) { return FS; }
EXPORT uint32_t web_over(void) { uint32_t v = dbx_audio.over; dbx_audio.over = 0; return v; }
EXPORT void web_set_fxparam(uint32_t e, uint32_t i, uint32_t v) { if (e < DBX_NFX && i < DBX_NFXP) { dbx_fx.p[e][i] = (uint8_t)(v > 127u ? 127u : v); dbx_fx_rates(e); } }   /* (a test hook: a parameter moved as a knob would, with no page and no preview) */
